"""Isolated real-SDK offline test runner. Never a cloud or resource runner."""
import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import subprocess
import sys
import uuid


@dataclass(frozen=True)
class ProcessResult:
    timed_out: bool
    exit_code: object
    output: bytes
    proven_not_applied: bool = False


def bounded_process(argv, timeout, cwd=None, env=None):
    """Test-process watchdog only. A killed caller proves nothing about a remote commit.

    This runner owns the child handle; it never discovers or terminates other PIDs.
    No live SDK worker entry point exists in this batch.
    """
    if type(timeout) not in (int, float) or not 0 < timeout <= 30:
        raise ValueError("invalid_deadline")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    child = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=flags)
    try:
        output, _ = child.communicate(timeout=timeout)
        return ProcessResult(False, child.returncode, output)
    except subprocess.TimeoutExpired:
        try:
            child.kill()
        except ProcessLookupError:
            pass
        output, _ = child.communicate(timeout=3)
        return ProcessResult(True, child.returncode, output)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-site", required=True, type=Path)
    parser.add_argument("--all-tests", action="store_true")
    parser.add_argument("--worker-tests", action="store_true", help="separate bounded worker/preflight suite")
    parser.add_argument("--worker-group", choices=("preflight", "publish", "deadline", "refusal", "initialize", "quota", "quota-stop", "credentials", "identity", "object-deadline", "live-stages", "permission-stages", "permission-verify", "reply-loss", "reply-loss-stop", "reply-loss-unit", "reply-loss-final", "terminal-unit", "terminal-commit", "terminal-read", "terminal-stop"), default="preflight")
    parser.add_argument("--local-tls-dir", type=Path, help="explicit synthetic local certificate directory only")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    site = args.sdk_site.resolve()
    if not (site / "tablestore-6.4.8.dist-info" / "METADATA").is_file():
        print("J4C1_SDK_SITE_INVALID", file=sys.stderr)
        return 2
    tls_dir = args.local_tls_dir.resolve() if args.local_tls_dir else None
    if tls_dir is not None and (not tls_dir.name.startswith("usapp-j4c-tls-") or
                               not all((tls_dir / name).is_file() for name in ("server.crt", "server.key"))):
        print("J4C_LOCAL_TLS_FIXTURE_INVALID", file=sys.stderr)
        return 2
    # Only paths, never keys/credentials, appear in child arguments. Python -I
    # ignores user site/PYTHONPATH; the selected temporary SDK directory is explicit.
    suite_directory = root / "tests" / "j4c_worker" if args.worker_tests else root / "tests"
    suite_expression = "worker_suite({!r},{!r})".format(str(suite_directory), args.worker_group) if args.worker_tests else \
        "unittest.defaultTestLoader.discover({!r},pattern={!r})".format(
            str(suite_directory), "test_*.py" if args.all_tests else "test_j4c_*.py")
    bootstrap = (
        "import os,sys,unittest; "
        "sys.path.insert(0,{!r}); sys.path.insert(1,{!r}); "
        "sys.path.insert(2,{!r}); "
        "os.environ['USAPP_J4C_OFFLINE_SDK']='1'; "
        "os.environ['USAPP_J4C_LOCAL_TLS_DIR']={!r}; "
        "from tools.j4c_test_suites import worker_suite; "
        "suite={}; "
        "result=unittest.TextTestRunner(verbosity=1).run(suite); "
        "sys.exit(not result.wasSuccessful())"
    ).format(str(site), str(root), str(root / "tests"), str(tls_dir) if tls_dir else "", suite_expression)
    env = {k: v for k, v in os.environ.items() if k.upper() in
           {"SYSTEMROOT", "WINDIR", "PATH", "TEMP", "TMP", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE"}}
    result = bounded_process([sys.executable, "-I", "-B", "-c", bootstrap], 20, cwd=str(root), env=env)
    log = root / "Claude outputs" / ("j4c-sdk-offline-" + uuid.uuid4().hex[:10] + ".log")
    with log.open("xb") as stream:
        stream.write(result.output)
    with Path(str(log) + ".exit").open("x", encoding="ascii") as stream:
        stream.write("timeout_unknown" if result.timed_out else str(result.exit_code))
    if result.timed_out:
        print("J4C1_SDK_TIMEOUT_UNKNOWN", file=sys.stderr)
        return 2
    if result.exit_code != 0:
        print("J4C1_SDK_OFFLINE_FAILED; log=" + str(log), file=sys.stderr)
        return 1
    print("J4C1_SDK_OFFLINE_PASSED; no cloud calls; log=" + str(log))
    return 0


if __name__ == "__main__":
    sys.exit(main())
