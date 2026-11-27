#!/usr/bin/env bash
# =====================================================================
# deploy/scripts/deploy-vps.sh
# Idempotent production deployment / upgrade entrypoint on Linux VPS.
# Usage: bash deploy/scripts/deploy-vps.sh
# =====================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
COMPOSE_FILE="${REPO_ROOT}/deploy/docker-compose.prod.yml"
ENV_FILE="${REPO_ROOT}/.env"

cd "$REPO_ROOT"

if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing ${ENV_FILE}. Copy deploy/env.prod.example to .env and fill real values first." >&2
  exit 1
fi

while IFS=$'\t' read -r k v; do
  export "$k=$v"
done < <(python3 - "$ENV_FILE" <<'PY'
import sys
from pathlib import Path
for raw in Path(sys.argv[1]).read_text(encoding='utf-8').splitlines():
    line = raw.strip()
    if not line or line.startswith('#') or '=' not in line:
        continue
    k, v = line.split('=', 1)
    k = k.strip()
    v = v.strip()
    if len(v) >= 2 and ((v[0] == '"' and v[-1] == '"') or (v[0] == "'" and v[-1] == "'")):
        v = v[1:-1]
    if k:
        print(f"{k}\t{v}")
PY
)

for required_var in \
  PUBLIC_DOMAIN ACME_EMAIL ALLOWED_EMAILS \
  POSTGRES_PASSWORD POSTGRES_SUPERUSER_PASSWORD POSTGRES_PUSH_PASSWORD \
  SESSION_SECRET SMTP_HOST SMTP_USERNAME SMTP_PASSWORD SMTP_FROM \
  ADMIN_BOOTSTRAP_USERNAME ADMIN_BOOTSTRAP_PASSWORD_HASH \
  PUSH_WORKER_TOKEN VAPID_PUBLIC_KEY VAPID_PRIVATE_KEY VAPID_SUBJECT \
  BACKUP_PASSPHRASE; do
  val="${!required_var:-}"
  if [[ -z "$val" || "$val" == *"replace-with"* || "$val" == *"change-me"* || "$val" == *"example.com"* ]]; then
    echo "Refusing deploy: ${required_var} in .env is missing or still uses a placeholder value." >&2
    exit 1
  fi
done

wait_container_healthy() {
  local service_name="$1"
  local timeout_sec="$2"
  local elapsed=0
  local cid status
  cid="$(docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" ps -q "$service_name")"
  if [[ -z "$cid" ]]; then
    echo "No container found for service '$service_name'" >&2
    return 1
  fi
  while (( elapsed < timeout_sec )); do
    status="$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$cid" 2>/dev/null || echo "unknown")"
    if [[ "$status" == "healthy" ]]; then
      return 0
    fi
    sleep 2
    elapsed=$((elapsed + 2))
  done
  echo "Timed out waiting for $service_name to become healthy (last status=$status)." >&2
  docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" logs --tail=50 "$service_name" || true
  return 1
}

echo "[1/6] Pulling GHCR images..."
docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" pull

echo "[2/6] Starting postgres..."
docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" up -d postgres
wait_container_healthy postgres 60

echo "[3/6] Applying SQL migrations (0000..latest) & roles..."
bash "${REPO_ROOT}/database/apply.sh" \
  --compose-file "$COMPOSE_FILE" \
  --env-file "$ENV_FILE" \
  --create-database \
  --init-roles

echo "[4/6] Starting push-worker, api, and caddy..."
docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" up -d
wait_container_healthy api 90

echo "[5/6] Waiting for bootstrap admin row and seeding catalog + push control..."
for _ in $(seq 1 10); do
  admin_ok="$(docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" exec -T \
    -e "PGPASSWORD=${POSTGRES_SUPERUSER_PASSWORD}" postgres \
    psql -U "${POSTGRES_SUPERUSER:-postgres}" -d "${POSTGRES_DB:-usapp}" -t -A \
    -c "SELECT count(*) FROM public.admin_users WHERE status = 'active';" | tr -d '[:space:]')"
  if [[ "${admin_ok:-0}" -ge 1 ]]; then
    break
  fi
  sleep 1
done

docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" exec -T \
  -e "PGPASSWORD=${POSTGRES_SUPERUSER_PASSWORD}" postgres \
  psql -U "${POSTGRES_SUPERUSER:-postgres}" -d "${POSTGRES_DB:-usapp}" \
  -v ON_ERROR_STOP=1 -X -q -f - < "${REPO_ROOT}/database/bootstrap-online.sql"

echo "[6/6] Deployment complete! Service status:"
docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" ps

echo
echo "Public PWA URL : https://${PUBLIC_DOMAIN}"
echo "Admin Tunnel   : ssh -N -L 8444:127.0.0.1:8444 <user>@<vps-ip>"
echo "Admin Console  : https://localhost:8444/admin/"
