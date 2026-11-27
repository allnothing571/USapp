#!/usr/bin/env bash
# =====================================================================
# database/apply.sh
# USapp Linux database migration runner (Bash + Python 3 stdlib parity with apply.ps1).
# Enforces single outer BEGIN;/COMMIT; transaction boundaries, SHA-256 verification,
# advisory lock (7492000), schema_migrations registration, eraser safety check, and init-roles.sql.
# =====================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

MODE="docker"
HOST_NAME="127.0.0.1"
PORT="5432"
DATABASE="${POSTGRES_DB:-}"
USER_NAME="${POSTGRES_SUPERUSER:-}"
PASSWORD="${POSTGRES_SUPERUSER_PASSWORD:-${PGPASSWORD:-}}"
MIGRATIONS_DIR="${SCRIPT_DIR}/migrations"
COMPOSE_FILE=""
ENV_FILE=""
SERVICE="postgres"
CREATE_DATABASE="false"
INIT_ROLES="false"
DRY_RUN="false"
VALIDATE_ONLY="false"

usage() {
  cat <<'EOF'
Usage: bash database/apply.sh [options]
  --mode docker|host          Execution mode (default: docker)
  --compose-file <path>       Compose file (default: deploy/docker-compose.prod.yml if present, else deploy/docker-compose.yml)
  --env-file <path>           Env file path (default: .env in repo root if present)
  --service <name>            Postgres compose service name (default: postgres)
  --host <host>               Postgres host for host mode (default: 127.0.0.1)
  --port <port>               Postgres port for host mode (default: 5432)
  --database <db>             Target database (default: POSTGRES_DB or usapp)
  --user <user>               Superuser role (default: POSTGRES_SUPERUSER or postgres)
  --password <pass>           Superuser password (default: POSTGRES_SUPERUSER_PASSWORD or PGPASSWORD)
  --migrations-dir <dir>      Migrations directory (default: database/migrations)
  --create-database           Create target DB if missing
  --init-roles                Apply database/init-roles.sql after migrations
  --dry-run                   Check DB state and print pending migrations without modifying DB
  --validate-only             Offline check of all migration files without DB connection
  -h, --help                  Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode) MODE="${2:?}"; shift 2 ;;
    --compose-file) COMPOSE_FILE="${2:?}"; shift 2 ;;
    --env-file) ENV_FILE="${2:?}"; shift 2 ;;
    --service) SERVICE="${2:?}"; shift 2 ;;
    --host) HOST_NAME="${2:?}"; shift 2 ;;
    --port) PORT="${2:?}"; shift 2 ;;
    --database) DATABASE="${2:?}"; shift 2 ;;
    --user) USER_NAME="${2:?}"; shift 2 ;;
    --password) PASSWORD="${2:?}"; shift 2 ;;
    --migrations-dir) MIGRATIONS_DIR="${2:?}"; shift 2 ;;
    --create-database) CREATE_DATABASE="true"; shift ;;
    --init-roles) INIT_ROLES="true"; shift ;;
    --dry-run) DRY_RUN="true"; shift ;;
    --validate-only) VALIDATE_ONLY="true"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ "$MODE" != "docker" && "$MODE" != "host" ]]; then
  echo "Invalid --mode '$MODE'. Must be 'docker' or 'host'." >&2
  exit 2
fi

if [[ -z "$ENV_FILE" && -f "${REPO_ROOT}/.env" ]]; then
  ENV_FILE="${REPO_ROOT}/.env"
fi

if [[ -n "$ENV_FILE" ]]; then
  if [[ ! -f "$ENV_FILE" ]]; then
    echo "Env file not found: $ENV_FILE" >&2
    exit 1
  fi
  while IFS=$'\t' read -r k v; do
    if [[ -z "${!k:-}" ]]; then
      export "$k=$v"
    fi
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
fi

DATABASE="${DATABASE:-${POSTGRES_DB:-usapp}}"
USER_NAME="${USER_NAME:-${POSTGRES_SUPERUSER:-postgres}}"
PASSWORD="${PASSWORD:-${POSTGRES_SUPERUSER_PASSWORD:-${PGPASSWORD:-}}}"

if [[ -z "$COMPOSE_FILE" ]]; then
  if [[ -f "${REPO_ROOT}/deploy/docker-compose.prod.yml" ]]; then
    COMPOSE_FILE="${REPO_ROOT}/deploy/docker-compose.prod.yml"
  else
    COMPOSE_FILE="${REPO_ROOT}/deploy/docker-compose.yml"
  fi
fi

if [[ ! -d "$MIGRATIONS_DIR" ]]; then
  echo "Migrations directory not found: $MIGRATIONS_DIR" >&2
  exit 1
fi

command -v python3 >/dev/null 2>&1 || { echo "python3 is required." >&2; exit 1; }

SQL_TOOL_PY=$(cat <<'PY'
import hashlib
import re
import sys
from pathlib import Path

DOLLAR_TAG_RE = re.compile(r'^\$([a-zA-Z0-9_]*)\$')

def parse_top_level(sql: str, filename: str):
    n = len(sql)
    i = 0
    stmts = []
    tokens = []
    start_idx = -1

    while i < n:
        c = sql[i]
        if c.isspace():
            i += 1
            continue
        if c == '-':
            if i + 1 < n and sql[i + 1] == '-':
                i += 2
                while i < n and sql[i] != '\n':
                    i += 1
                if i < n and sql[i] == '\n':
                    i += 1
                continue
            if start_idx < 0:
                start_idx = i
            tokens.append('-')
            i += 1
            continue
        if c == '/':
            if i + 1 < n and sql[i + 1] == '*':
                c_start = i
                i += 2
                closed = False
                while i + 1 < n:
                    if sql[i] == '*' and sql[i + 1] == '/':
                        i += 2
                        closed = True
                        break
                    i += 1
                if not closed:
                    raise ValueError(f"{filename}: unclosed block comment at offset {c_start}")
                continue
            if start_idx < 0:
                start_idx = i
            tokens.append('/')
            i += 1
            continue
        if c == "'":
            if start_idx < 0:
                start_idx = i
            s_start = i
            i += 1
            closed = False
            while i < n:
                if sql[i] == "'":
                    if i + 1 < n and sql[i + 1] == "'":
                        i += 2
                        continue
                    i += 1
                    closed = True
                    break
                i += 1
            if not closed:
                raise ValueError(f"{filename}: unclosed single-quoted string at offset {s_start}")
            continue
        if c == '$':
            m = DOLLAR_TAG_RE.match(sql[i:])
            if not m:
                raise ValueError(f"{filename}: unsupported '$' token at offset {i}")
            if start_idx < 0:
                start_idx = i
            tag = m.group(0)
            s_start = i
            i += len(tag)
            close_idx = sql.find(tag, i)
            if close_idx < 0:
                raise ValueError(f"{filename}: unclosed dollar-quoted block {tag} at offset {s_start}")
            i = close_idx + len(tag)
            continue
        if c == ';':
            if start_idx >= 0:
                end_idx = i + 1
                stmts.append((list(tokens), start_idx, end_idx, sql[start_idx:end_idx].strip()))
                tokens.clear()
                start_idx = -1
            i += 1
            continue
        if start_idx < 0:
            start_idx = i
        t_start = i
        while i < n and not sql[i].isspace() and sql[i] not in (';', '-', '/', "'", '$'):
            i += 1
        if i <= t_start:
            raise ValueError(f"{filename}: scanner stalled at offset {t_start}")
        tokens.append(sql[t_start:i])

    if tokens:
        stmts.append((list(tokens), start_idx, n, sql[start_idx:n].strip()))
    return stmts

def validate_migration(sql: str, filename: str):
    stmts = parse_top_level(sql, filename)
    if len(stmts) < 2:
        raise ValueError(f"{filename}: missing outer BEGIN; and COMMIT; (statements={len(stmts)})")
    first_toks, _, first_end, first_txt = stmts[0]
    if not first_toks or first_toks[0].upper() != 'BEGIN':
        raise ValueError(f"{filename}: first statement must be BEGIN;, got {first_txt!r}")
    if len(first_toks) > 2 or (len(first_toks) == 2 and first_toks[1].upper() not in ('TRANSACTION', 'WORK')):
        raise ValueError(f"{filename}: invalid BEGIN syntax {first_txt!r}")
    if not first_txt.endswith(';'):
        raise ValueError(f"{filename}: BEGIN missing semicolon")

    last_toks, last_start, _, last_txt = stmts[-1]
    if not last_toks or last_toks[0].upper() != 'COMMIT':
        raise ValueError(f"{filename}: last statement must be COMMIT;, got {last_txt!r}")
    if len(last_toks) > 2 or (len(last_toks) == 2 and last_toks[1].upper() not in ('TRANSACTION', 'WORK')):
        raise ValueError(f"{filename}: invalid COMMIT syntax {last_txt!r}")
    if not last_txt.endswith(';'):
        raise ValueError(f"{filename}: COMMIT missing semicolon")

    for idx, (m_toks, _, _, m_txt) in enumerate(stmts[1:-1], start=2):
        tok0 = m_toks[0].upper() if m_toks else ''
        if tok0 in ('BEGIN', 'COMMIT', 'ROLLBACK', 'START'):
            raise ValueError(f"{filename}: nested transaction control at statement #{idx}: {m_txt!r}")
    return first_end, last_start

def build_atomic(sql: str, version: str, checksum: str, filename: str) -> str:
    begin_end, commit_start = validate_migration(sql, filename)
    esc_ver = version.replace("'", "''")
    esc_sum = checksum.replace("'", "''")
    lock_sql = (
        "\n-- [apply.sh injected: migration advisory lock (7492000)]\n"
        "DO $usapp_mig_lock$\nBEGIN\n"
        "    IF NOT pg_try_advisory_xact_lock(7492000) THEN\n"
        "        RAISE EXCEPTION 'Failed to acquire migration lock (7492000)';\n"
        "    END IF;\n"
        "END $usapp_mig_lock$;\n\n"
    )
    ins_sql = (
        "\n-- [apply.sh injected: atomic schema_migrations record]\n"
        "INSERT INTO schema_migrations (version, checksum, applied_by, applied_at)\n"
        f"VALUES ('{esc_ver}', '{esc_sum}', current_user, now());\n\n"
    )
    return sql[:begin_end] + "\n" + lock_sql + sql[begin_end:commit_start] + "\n" + ins_sql + sql[commit_start:]

cmd = sys.argv[1]
mig_dir = Path(sys.argv[2])
files = sorted(mig_dir.glob("*.sql"))
if not files:
    raise SystemExit(f"No .sql files found in {mig_dir}")

if cmd == "validate":
    for f in files:
        raw_bytes = f.read_bytes()
        sha = hashlib.sha256(raw_bytes).hexdigest()
        sql = raw_bytes.decode("utf-8")
        if sql.startswith("\ufeff"):
            raise ValueError(f"{f.name}: unexpected UTF-8 BOM")
        atomic = build_atomic(sql, f.stem, sha, f.name)
        assert "pg_try_advisory_xact_lock(7492000)" in atomic
        print(f"{f.stem}\t{sha}")
elif cmd == "emit-atomic":
    target = Path(sys.argv[3])
    raw_bytes = target.read_bytes()
    sha = hashlib.sha256(raw_bytes).hexdigest()
    sql = raw_bytes.decode("utf-8")
    sys.stdout.write(build_atomic(sql, target.stem, sha, target.name))
PY
)

echo "=========================================================="
echo "USapp Linux Migration Runner"
echo "  Mode:       $MODE"
echo "  Database:   $DATABASE"
echo "  User:       $USER_NAME"
echo "  Migrations: $MIGRATIONS_DIR"
echo "  DryRun:     $DRY_RUN"
echo "=========================================================="

echo "[check] Validating transaction boundaries across all migrations..."
MANIFEST_LINES="$(python3 -c "$SQL_TOOL_PY" validate "$MIGRATIONS_DIR")"
TOTAL_COUNT="$(printf '%s\n' "$MANIFEST_LINES" | wc -l | tr -d ' ')"
echo "[check] Strict single-transaction boundary check passed (${TOTAL_COUNT} migrations)."

if [[ "$VALIDATE_ONLY" == "true" ]]; then
  echo "[ok   ] Offline validation complete (--validate-only)."
  exit 0
fi

COMPOSE_ARGS=(compose -f "$COMPOSE_FILE")
if [[ -n "$ENV_FILE" ]]; then
  COMPOSE_ARGS+=(--env-file "$ENV_FILE")
fi

if [[ "$MODE" == "docker" ]]; then
  command -v docker >/dev/null 2>&1 || { echo "docker is required for --mode docker." >&2; exit 1; }
else
  command -v psql >/dev/null 2>&1 || { echo "psql is required for --mode host." >&2; exit 1; }
fi

run_psql_query() {
  local target_db="$1"
  local sql="$2"
  if [[ "$MODE" == "docker" ]]; then
    local extra_env=()
    if [[ -n "$PASSWORD" ]]; then
      extra_env=(-e "PGPASSWORD=${PASSWORD}")
    fi
    docker "${COMPOSE_ARGS[@]}" exec -T "${extra_env[@]}" "$SERVICE" \
      psql -U "$USER_NAME" -d "$target_db" -v ON_ERROR_STOP=1 -X -q -t -A -c "$sql"
  else
    PGPASSWORD="$PASSWORD" psql -h "$HOST_NAME" -p "$PORT" -U "$USER_NAME" -d "$target_db" \
      -v ON_ERROR_STOP=1 -X -q -t -A -c "$sql"
  fi
}

run_psql_stdin() {
  local target_db="$1"
  if [[ "$MODE" == "docker" ]]; then
    local extra_env=()
    if [[ -n "$PASSWORD" ]]; then
      extra_env=(-e "PGPASSWORD=${PASSWORD}")
    fi
    docker "${COMPOSE_ARGS[@]}" exec -T "${extra_env[@]}" "$SERVICE" \
      psql -U "$USER_NAME" -d "$target_db" -v ON_ERROR_STOP=1 -X -q -f -
  else
    PGPASSWORD="$PASSWORD" psql -h "$HOST_NAME" -p "$PORT" -U "$USER_NAME" -d "$target_db" \
      -v ON_ERROR_STOP=1 -X -q -f -
  fi
}

DB_CHECK="$(run_psql_query "postgres" "SELECT 1 FROM pg_database WHERE datname = '${DATABASE//\'/\'\'}';" | tr -d '[:space:]')"
if [[ "$DB_CHECK" != "1" ]]; then
  if [[ "$DRY_RUN" == "true" ]]; then
    echo "[dry  ] Database '$DATABASE' does not exist yet."
    while IFS=$'\t' read -r ver sha; do
      echo "[dry  ] $ver (pending, sha256: $sha)"
    done <<< "$MANIFEST_LINES"
    exit 0
  fi
  if [[ "$CREATE_DATABASE" == "true" ]]; then
    echo "[db   ] Creating database '$DATABASE'..."
    run_psql_query "postgres" "CREATE DATABASE \"${DATABASE//\"/\"\"}\";" >/dev/null
  else
    echo "Database '$DATABASE' does not exist. Re-run with --create-database." >&2
    exit 1
  fi
fi

REG_CLASS="$(run_psql_query "$DATABASE" "SELECT COALESCE(to_regclass('public.schema_migrations')::text, '');" | tr -d '[:space:]')"
declare -A APPLIED=()
if [[ -n "$REG_CLASS" ]]; then
  while IFS='|' read -r ver sha; do
    [[ -n "$ver" ]] && APPLIED["$ver"]="$sha"
  done < <(run_psql_query "$DATABASE" "SELECT version || '|' || checksum FROM schema_migrations ORDER BY version;")
else
  echo "[info ] public.schema_migrations not present yet (will be created by 0000_schema_migrations)."
fi

ERASER_CHECK="$(run_psql_query "$DATABASE" "SELECT concat_ws(',', CASE WHEN r.rolsuper THEN 'superuser' END, CASE WHEN r.rolcanlogin THEN 'login' END, CASE WHEN r.rolcreaterole THEN 'createrole' END, CASE WHEN r.rolcreatedb THEN 'createdb' END, CASE WHEN r.rolreplication THEN 'replication' END, CASE WHEN r.rolbypassrls THEN 'bypassrls' END, CASE WHEN r.rolinherit THEN 'inherit' END, CASE WHEN r.rolconfig IS NOT NULL THEN 'role_settings' END, CASE WHEN EXISTS (SELECT 1 FROM pg_auth_members m WHERE m.roleid = r.oid) THEN 'has_members' END, CASE WHEN EXISTS (SELECT 1 FROM pg_auth_members m WHERE m.member = r.oid) THEN 'has_memberships' END, CASE WHEN EXISTS (SELECT 1 FROM pg_default_acl d WHERE d.defaclrole = r.oid) THEN 'default_privileges' END, CASE WHEN EXISTS (SELECT 1 FROM pg_class c WHERE c.relowner = r.oid) THEN 'owns_relation' END) FROM pg_roles r WHERE r.rolname = 'usapp_memory_eraser';" | tr -d '[:space:]')"
if [[ -n "$ERASER_CHECK" ]]; then
  echo "USAPP_MEMORY_ERASER_UNSAFE: $ERASER_CHECK" >&2
  exit 1
fi

SKIP_COUNT=0
PENDING_COUNT=0

while IFS=$'\t' read -r ver sha; do
  if [[ -n "${APPLIED[$ver]:-}" ]]; then
    if [[ "${APPLIED[$ver]}" != "$sha" ]]; then
      echo "Checksum mismatch for already-applied migration '$ver': DB=${APPLIED[$ver]} vs local=$sha" >&2
      exit 1
    fi
    echo "[skip ] $ver (applied, checksum verified)"
    SKIP_COUNT=$((SKIP_COUNT + 1))
    continue
  fi

  PENDING_COUNT=$((PENDING_COUNT + 1))
  if [[ "$DRY_RUN" == "true" ]]; then
    echo "[dry  ] $ver (pending, sha256: $sha)"
    continue
  fi

  echo "[apply] $ver (sha256: $sha)..."
  python3 -c "$SQL_TOOL_PY" emit-atomic "$MIGRATIONS_DIR" "${MIGRATIONS_DIR}/${ver}.sql" | run_psql_stdin "$DATABASE" >/dev/null
  echo "[ok   ] $ver applied."
done <<< "$MANIFEST_LINES"

echo "=========================================================="
echo "Migration summary: skipped=${SKIP_COUNT}, applied=${PENDING_COUNT}, dry_run=${DRY_RUN}"
echo "=========================================================="

if [[ "$DRY_RUN" == "false" && "$INIT_ROLES" == "true" ]]; then
  INIT_SQL="${SCRIPT_DIR}/init-roles.sql"
  [[ -f "$INIT_SQL" ]] || { echo "Missing $INIT_SQL" >&2; exit 1; }
  echo "[roles] Applying database/init-roles.sql..."
  run_psql_stdin "$DATABASE" < "$INIT_SQL" >/dev/null
  echo "[roles] Role privileges updated."
fi
