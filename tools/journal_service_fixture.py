"""Owned synthetic integration fixture only; never a production daemon or credential factory."""
import json
from pathlib import Path
import signal
import sys
import threading
import time
from contextlib import ExitStack
from . import deletion_journal as j1
from .journal_backend import Service
from .journal_https_server import Config
from .journal_writer_fence import pause, writer
from .journal_fenced_publisher import FencedPublisher, Receipt


def main():
    if len(sys.argv) not in (2, 3):
        return 2
    directory = Path(sys.argv[1]).resolve()
    if directory.parent != Path("/tmp") or not directory.name.startswith("usapp-journal-service-") or \
            json.loads((directory / "synthetic-only.json").read_text()) != {"synthetic_only": True} or \
            any(not (directory / name).is_file() or (directory / name).is_symlink() for name in ("server.crt", "server.key", "ca.crt")):
        return 2
    # Conditional metadata facade and immutable object fakes; not vendor atomicity/durability evidence.
    from tests.test_journal_backend import Fixtures, config as backend_config
    journal = "00000000-0000-0000-0000-00000000002c"
    fixtures = Fixtures()
    backend = fixtures.build(backend_config(journal))
    service = Service(Config(str(directory / "server.crt"), str(directory / "server.key")), backend)
    from .journal_recovery_reader import CheckpointObserver, Context, RecoveryBackend
    from .journal_backend import Credential
    def read_fixture():
        store = Fixtures()
        store.data = fixtures.data

        class ReadClient(store.Client):
            def start(self):
                self.rows = fixtures.writer.rows  # A new read transaction sees current fixture state.
                return super().start()

            def finish(self, tx, commit=False):
                if commit:
                    raise AssertionError("reader_commit_forbidden")
                return super().finish(tx, False)

            def insert(self, *_args, **_kwargs):
                raise AssertionError("reader_write_forbidden")

            advance_head = insert
            advance_operation = insert

        class ReadObjects(store.Objects):
            def put_once(self, *_args, **_kwargs):
                raise AssertionError("reader_put_forbidden")

        return store, ReadClient, ReadObjects

    reader_config = backend_config(journal)
    from dataclasses import replace
    reader_config = replace(reader_config, mac_key="d" * 64)
    context = Context(journal, "00000000-0000-0000-0000-00000000002d", "00000000-0000-0000-0000-00000000002e")
    recovery_fixtures, reader_client, reader_objects = read_fixture()
    observer_fixtures, observer_client, observer_objects = read_fixture()
    reader = RecoveryBackend(reader_config, context, Credential("recovery-reader", "synthetic-reader-secret"),
                             _client_type=reader_client, _objects_type=reader_objects)
    observer = CheckpointObserver(reader_config.table, reader_config.objects, context,
                                  Credential("authority-observer", "synthetic-observer-secret"),
                                  _client_type=observer_client, _objects_type=observer_objects)
    coordinated = len(sys.argv) == 3
    coordinator_drained = False
    if coordinated:
        admission_id = sys.argv[2]

        def admit(journal_id, key):
            if (directory / "admission-committed").is_file():
                return Receipt(journal_id, key, "admitted")

        def reconcile(journal_id, key):
            material = observer.reader.read("snapshot")
            if material is not None and not material["snapshot"]["unresolved"] and not fixtures.writer.active:
                return Receipt(journal_id, key, "reconciled")

        def confirm_drained(journal_id, key):
            (directory / "drain-ready.json").write_bytes(b"true\n")
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if (directory / "drain-committed").is_file():
                    return Receipt(journal_id, key, "drained")
                time.sleep(.05)

        service = FencedPublisher(service, journal, admission_id, admit, reconcile, confirm_drained)
    recovery_service = Service(Config(str(directory / "server.crt"), str(directory / "server.key")), reader)
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    fence = directory / "publication-fence"
    fence.mkdir(mode=0o700)
    publication = ExitStack()
    ticket = publication.enter_context(writer(str(fence), "synthetic-publisher"))
    closed = None
    observation_written = False
    try:
        service.start()
        recovery_service.start()
        listener = service.service if coordinated else service
        (directory / "ready.json").write_bytes(j1.canonical(dict(synthetic_only=True, port=listener.server.port, recovery_port=recovery_service.server.port)) + b"\n")
        while not stop.wait(0.05):
            if (directory / "stop").exists():
                break
            if closed is None and (directory / "pause-publication").exists():
                closed = service.close(1)
                coordinator_drained = closed.pop("coordinator_drained", False)
                if not all(closed.values()) or fixtures.writer.active or (coordinated and not coordinator_drained):
                    raise RuntimeError("synthetic_publication_not_drained")
                ticket.confirm_drained()
                publication.close()
                with pause(str(fence)):
                    (directory / "pause-latched.json").write_bytes(b"true\n")
            if closed is not None and not observation_written and (directory / "observe-paused-checkpoint").exists():
                # The issuer asks for this after its first authoritative pause confirmation.
                observation = observer.observe(journal)
                if observation is None:
                    raise RuntimeError("checkpoint_observation_unconfirmed")
                observation["writersPausedUntil"] = observation["observedAt"] + 60
                pending = directory / "pause-observation.pending"
                pending.write_bytes(j1.canonical(observation) + b"\n")
                pending.replace(directory / "pause-observation.json")
                observation_written = True
    finally:
        try:
            recovery_closed = recovery_service.close(1)
            if closed is None:
                closed = service.close(1)
                coordinator_drained = closed.pop("coordinator_drained", False)
                if all(closed.values()) and not fixtures.writer.active:
                    ticket.confirm_drained()
        finally:
            publication.close()
            observer_closed = observer.close()
    report = dict(synthetic_only=True, closed=closed, operations=sum(kind == "operation" for kind, _ in fixtures.writer.rows),
                  batches=fixtures.writer.calls.count("commit"), serving_allowed=False, backend="conditional_metadata_fake",
                  preflights=fixtures.writer.preflights, validator_puts=len(fixtures.object_instances[1].puts),
                  recovery_closed=recovery_closed, recovery_preflights=recovery_fixtures.writer.preflights,
                  recovery_commits=recovery_fixtures.writer.calls.count("commit"), recovery_puts=len(recovery_fixtures.object_instances[0].puts),
                  observer_closed=observer_closed, observer_preflights=observer_fixtures.writer.preflights,
                  observer_commits=observer_fixtures.writer.calls.count("commit"),
                  observer_puts=len(observer_fixtures.object_instances[0].puts), coordinator_drained=coordinator_drained)
    (directory / "report.json").write_bytes(j1.canonical(report) + b"\n")
    drained = dict(network_drained=True, backend_drained=True, resources_drained=True)
    return 0 if closed == recovery_closed == drained and report["operations"] == 2 and report["batches"] == 4 and report["recovery_commits"] == report["recovery_puts"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
