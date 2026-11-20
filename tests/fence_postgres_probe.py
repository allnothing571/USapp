"""Disposable PostgreSQL integration evidence for the independent recovery fence."""

import os
import re
import secrets
import subprocess
import time
from pathlib import Path


DATABASE = "usapp_recovery_fence"
OWNER_LABEL = "usapp.recovery-fence-probe"
IMAGE = "postgres:17"
OWNER = secrets.token_hex(8)
BOOT_PASSWORD = secrets.token_hex(16)
ACTOR_PASSWORDS = {
    actor: secrets.token_hex(16)
    for actor in ("writer_one", "writer_two", "writer_unknown", "fence_controller")
}
JOURNAL = "00000000-0000-0000-0000-000000000001"
RACE_JOURNAL = "00000000-0000-0000-0000-000000000002"
PAUSE_FIRST_JOURNAL = "00000000-0000-0000-0000-000000000003"
ONE = "00000000-0000-0000-0000-000000000011"
TWO = "00000000-0000-0000-0000-000000000012"
OTHER = "00000000-0000-0000-0000-000000000013"
PAUSE = "00000000-0000-0000-0000-000000000021"


def docker(*arguments, check=True, timeout=25, **kwargs):
    return subprocess.run(["docker", *arguments], text=True, capture_output=True, check=check,
                          timeout=timeout, **kwargs)


def psql_args(container, actor, application="fence-probe"):
    secret = BOOT_PASSWORD if actor == "postgres" else ACTOR_PASSWORDS[actor]
    return ["exec", "-i", "-e", "PGPASSWORD=" + secret, "-e", "PGAPPNAME=" + application,
            container, "psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-h", "127.0.0.1", "-U", actor, "-d", DATABASE]


def sql(container, actor, text, expected=None):
    result = docker(*psql_args(container, actor), input=text, check=False)
    if result.returncode != 0 or (expected is not None and result.stdout.strip() != expected):
        raise AssertionError(f"unexpected SQL result for {actor}: {result.stdout.strip()} {result.stderr.strip()}")
    return result.stdout.strip()


def refuse(container, actor, text, reason):
    result = docker(*psql_args(container, actor), input=text, check=False)
    if result.returncode == 0 or reason not in result.stderr:
        raise AssertionError(f"expected SQL refusal for {actor}")


def call(function, journal, key):
    return f"SELECT recovery_fence.{function}('{journal}','{key}');"


def until(container, predicate, message, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if sql(container, "postgres", f"SELECT EXISTS (SELECT 1 FROM pg_stat_activity WHERE {predicate});") == "t":
            return
        time.sleep(0.1)
    raise AssertionError(message)


def race(container, first_actor, first, first_result, second_actor, second, second_result):
    with subprocess.Popen(
        ["docker", *psql_args(container, first_actor, "fence-race-first"), "-c",
         f"BEGIN; {first} SELECT pg_sleep(8); COMMIT;"],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    ) as writer:
        until(container, "application_name='fence-race-first' AND wait_event='PgSleep'",
              "first transaction did not hold the journal lock", 5)
        with subprocess.Popen(
            ["docker", *psql_args(container, second_actor, "fence-race-second"), "-c", second],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ) as pauser:
            until(container, "application_name='fence-race-second' AND wait_event_type='Lock'",
                  "second transaction did not wait for the journal lock", 5)
            result, _ = pauser.communicate(timeout=12)
            assert pauser.returncode == 0 and result.strip() == second_result
        result, _ = writer.communicate(timeout=5)
        assert writer.returncode == 0 and result.strip() == first_result


def check_races(container):
    race(container, "writer_one", call("admit", RACE_JOURNAL, ONE), "admitted",
         "fence_controller", call("request_pause", RACE_JOURNAL, PAUSE), "draining")
    sql(container, "fence_controller", call("confirm_pause", RACE_JOURNAL, PAUSE), "draining")
    sql(container, "writer_one", call("confirm_drained", RACE_JOURNAL, ONE), "drained")
    sql(container, "fence_controller", call("confirm_pause", RACE_JOURNAL, PAUSE), "paused")
    print("ADMISSION_PAUSE_RACE_PASSED")
    race(container, "fence_controller", call("request_pause", PAUSE_FIRST_JOURNAL, PAUSE), "draining",
         "writer_one", call("admit", PAUSE_FIRST_JOURNAL, ONE), "refused")
    sql(container, "fence_controller", call("confirm_pause", PAUSE_FIRST_JOURNAL, PAUSE), "paused")
    print("PAUSE_ADMISSION_RACE_PASSED")


def run(container):
    role_setup = f"""
    CREATE ROLE usapp_fence_writer NOLOGIN;
    CREATE ROLE usapp_fence_authority NOLOGIN;
    CREATE ROLE writer_one LOGIN PASSWORD '{ACTOR_PASSWORDS["writer_one"]}' IN ROLE usapp_fence_writer;
    CREATE ROLE writer_two LOGIN PASSWORD '{ACTOR_PASSWORDS["writer_two"]}' IN ROLE usapp_fence_writer;
    CREATE ROLE writer_unknown LOGIN PASSWORD '{ACTOR_PASSWORDS["writer_unknown"]}' IN ROLE usapp_fence_writer;
    CREATE ROLE fence_controller LOGIN PASSWORD '{ACTOR_PASSWORDS["fence_controller"]}' IN ROLE usapp_fence_authority;
    """
    sql(container, "postgres", role_setup)
    schema = Path(__file__).resolve().parents[1] / "tools" / "recovery-fence" / "schema.sql"
    sql(container, "postgres", schema.read_text(encoding="utf-8"))
    sql(container, "postgres", f"""
    INSERT INTO recovery_fence.journals(journal_id) VALUES ('{JOURNAL}'), ('{RACE_JOURNAL}'), ('{PAUSE_FIRST_JOURNAL}');
    INSERT INTO recovery_fence.writer_roster SELECT journal_id, 'writer_one' FROM recovery_fence.journals;
    INSERT INTO recovery_fence.writer_roster SELECT journal_id, 'writer_two' FROM recovery_fence.journals;
    """)
    refuse(container, "writer_one", call("request_pause", JOURNAL, PAUSE), "permission denied")
    refuse(container, "writer_one", "DELETE FROM recovery_fence.admissions;", "permission denied")
    refuse(container, "fence_controller", call("admit", JOURNAL, ONE), "permission denied")
    refuse(container, "fence_controller",
           "BEGIN ISOLATION LEVEL REPEATABLE READ;" + call("request_pause", JOURNAL, PAUSE), "requires read committed")
    refuse(container, "writer_one", "SET synchronous_commit=off;" + call("admit", JOURNAL, ONE),
           "requires durable commits")
    sql(container, "writer_unknown", call("admit", JOURNAL, ONE), "refused")
    sql(container, "writer_one", call("admit", JOURNAL, ONE), "admitted")
    sql(container, "writer_one", call("admit", JOURNAL, ONE), "admitted")
    sql(container, "writer_one", call("admit", JOURNAL, OTHER), "refused")
    sql(container, "writer_two", call("admit", JOURNAL, TWO), "admitted")
    sql(container, "fence_controller", call("confirm_pause", JOURNAL, PAUSE), "refused")
    sql(container, "fence_controller", call("request_pause", JOURNAL, PAUSE), "draining")
    sql(container, "fence_controller", call("request_pause", JOURNAL, OTHER), "refused")
    sql(container, "writer_two", call("confirm_drained", JOURNAL, ONE), "refused")
    sql(container, "writer_one", call("confirm_drained", JOURNAL, ONE), "drained")
    sql(container, "writer_one", call("confirm_drained", JOURNAL, ONE), "drained")
    sql(container, "writer_one", call("admit", JOURNAL, OTHER), "refused")
    sql(container, "fence_controller", call("confirm_pause", JOURNAL, PAUSE), "draining")
    sql(container, "writer_two", call("confirm_drained", JOURNAL, TWO), "drained")
    sql(container, "fence_controller", call("confirm_pause", JOURNAL, PAUSE), "paused")
    sql(container, "fence_controller", call("request_pause", JOURNAL, PAUSE), "paused")
    sql(container, "writer_two", call("admit", JOURNAL, TWO), "refused")
    sql(container, "writer_two", call("admit", JOURNAL, OTHER), "refused")
    print("ROLES_REPLAY_DISCONNECT_AND_PAUSE_PASSED")
    check_races(container)


def main():
    container = docker("run", "-d", "--network", "none", "--pull", "never", "--label", OWNER_LABEL + "=" + OWNER,
                       "--tmpfs", "/var/lib/postgresql/data", "-e", "POSTGRES_DB=" + DATABASE,
                       "-e", "POSTGRES_PASSWORD", IMAGE, env={**os.environ, "POSTGRES_PASSWORD": BOOT_PASSWORD}).stdout.strip()
    if re.fullmatch(r"[0-9a-f]{64}", container) is None:
        raise RuntimeError("disposable container ID missing")
    try:
        deadline = time.monotonic() + 30
        while docker("exec", container, "pg_isready", "-h", "127.0.0.1", "-U", "postgres", "-d", DATABASE,
                     check=False).returncode:
            if time.monotonic() >= deadline:
                raise RuntimeError("disposable PostgreSQL did not start")
            time.sleep(0.25)
        run(container)
    finally:
        owner = docker("inspect", "--format", '{{index .Config.Labels "' + OWNER_LABEL + '"}}', container).stdout.strip()
        if owner != OWNER:
            raise RuntimeError("disposable container ownership mismatch")
        docker("rm", "-f", container)


if __name__ == "__main__":
    main()
