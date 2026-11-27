#!/usr/bin/env bash
# =====================================================================
# deploy/scripts/backup-db.sh
# Daily encrypted pg_dump backup with SHA-256 & retention cleanup.
# Cron example (03:00 daily):
#   0 3 * * * cd /opt/usapp && bash deploy/scripts/backup-db.sh >> /var/log/usapp-backup.log 2>&1
# =====================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
COMPOSE_FILE="${REPO_ROOT}/deploy/docker-compose.prod.yml"
ENV_FILE="${REPO_ROOT}/.env"

[[ -f "$ENV_FILE" ]] || { echo "Missing $ENV_FILE" >&2; exit 1; }

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

if [[ -z "${BACKUP_PASSPHRASE:-}" || "${BACKUP_PASSPHRASE}" == *"replace-with"* ]]; then
  echo "BACKUP_PASSPHRASE in .env is missing or placeholder." >&2
  exit 1
fi

BACKUP_DIR="${BACKUP_DIR:-${REPO_ROOT}/backups}"
if [[ "$BACKUP_DIR" != /* ]]; then
  BACKUP_DIR="${REPO_ROOT}/${BACKUP_DIR#./}"
fi
RETENTION_DAYS="${BACKUP_RETENTION_DAYS:-30}"
DB_NAME="${POSTGRES_DB:-usapp}"
DB_USER="${POSTGRES_SUPERUSER:-postgres}"
TS="$(date -u +%Y%m%dT%H%M%SZ)"

mkdir -p "$BACKUP_DIR"
chmod 700 "$BACKUP_DIR"

OUT_FILE="${BACKUP_DIR}/usapp_${TS}.dump.enc"
TMP_FILE="${OUT_FILE}.tmp"
SHA_FILE="${OUT_FILE}.sha256"

export BACKUP_PASSPHRASE
docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" exec -T \
  -e "PGPASSWORD=${POSTGRES_SUPERUSER_PASSWORD}" postgres \
  pg_dump -U "$DB_USER" -d "$DB_NAME" --format=custom --no-owner --no-acl \
  | openssl enc -aes-256-cbc -pbkdf2 -iter 600000 -salt -pass env:BACKUP_PASSPHRASE -out "$TMP_FILE"

if [[ ! -s "$TMP_FILE" ]]; then
  rm -f "$TMP_FILE"
  echo "Backup failed: empty output file." >&2
  exit 1
fi

mv "$TMP_FILE" "$OUT_FILE"
chmod 600 "$OUT_FILE"
sha256sum "$OUT_FILE" > "$SHA_FILE"
chmod 600 "$SHA_FILE"

find "$BACKUP_DIR" -maxdepth 1 -type f \( -name 'usapp_*.dump.enc' -o -name 'usapp_*.dump.enc.sha256' \) \
  -mtime +"$RETENTION_DAYS" -delete

echo "[backup] Created ${OUT_FILE} ($(du -h "$OUT_FILE" | cut -f1)), retention=${RETENTION_DAYS}d"
