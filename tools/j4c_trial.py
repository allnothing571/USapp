"""Conservative local trial quotas. Not billing control or a deletion journal.

Create once per manifest in an explicit local directory. No reopen/resume API:
the parent session must remain alive; workers share its durable SQLite counters.
"""
from contextlib import contextmanager
import hashlib
import multiprocessing
import os
from pathlib import Path
import secrets
import sqlite3
import time
import uuid

from . import deletion_journal as j1
from . import deletion_journal_publication as p
from .j4c_preflight import validate, report


class QuotaError(Exception):
    pass


MAX_REQUESTS, MAX_PUTS = 5000, 20
MAX_UPLOAD, MAX_WIRE, MAX_RECEIVE = 2 * 1024**2, 8 * 1024**2, 64 * 1024**2
WINDOW_NS = 2 * 60 * 60 * 10**9


class Ledger:
    def __init__(self, path, nonce, manifest):
        self.path, self.nonce, self.manifest = Path(path).absolute(), nonce, manifest
        if self.path.name != "trial-" + manifest + ".sqlite3":
            raise QuotaError("invalid_trial_ledger")

    @contextmanager
    def transaction(self):
        connection = None
        try:
            if self.path.is_symlink() or not self.path.is_file():
                raise QuotaError()
            connection = sqlite3.connect(self.path.as_uri() + "?mode=rw", uri=True, timeout=1, isolation_level=None)
            connection.row_factory = sqlite3.Row
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute("SELECT * FROM budget WHERE id=1").fetchone()
            parent = multiprocessing.parent_process()
            allowed = row is not None and (row["pid"] == os.getpid() or
                       (parent is not None and parent.pid == row["pid"] and parent.is_alive()))
            if not allowed or row["manifest"] != self.manifest or row["nonce"] != self.nonce:
                raise QuotaError()
            now = time.monotonic_ns()
            if now < row["last_ns"] or now >= row["end_ns"]:
                raise QuotaError()
            connection.execute("UPDATE budget SET last_ns=? WHERE id=1", (now,))
            yield connection, row, now
            connection.commit()
        except Exception:
            if connection is not None:
                connection.rollback()
            raise QuotaError("trial_unavailable_or_exhausted") from None
        finally:
            if connection is not None:
                connection.close()

    def request(self, method, body):
        if method not in ("POST", "GET", "PUT") or (body is not None and not isinstance(body, bytes)):
            raise QuotaError("unsupported_trial_request")
        size = len(body) if body is not None else 0
        put = int(method == "PUT")
        with self.transaction() as (db, row, _):
            if row["requests"] + 1 > MAX_REQUESTS or row["puts"] + put > MAX_PUTS or \
                    row["upload"] + size * put > MAX_UPLOAD or row["wire"] + size > MAX_WIRE:
                raise QuotaError()
            db.execute("UPDATE budget SET requests=requests+1, puts=puts+?, upload=upload+?, wire=wire+? WHERE id=1",
                       (put, size * put, size))

    def reserve_read(self, amount):
        if type(amount) is not int or not 0 < amount <= 1024**2 + 1:
            raise QuotaError("invalid_trial_read")
        reservation = uuid.uuid4().hex
        with self.transaction() as (db, row, _):
            if row["receive"] + amount > MAX_RECEIVE:
                raise QuotaError()
            db.execute("UPDATE budget SET receive=receive+? WHERE id=1", (amount,))
            db.execute("INSERT INTO reservations VALUES (?,?,NULL)", (reservation, amount))
        return reservation

    def settle_read(self, reservation, actual):
        with self.transaction() as (db, _, __):
            row = db.execute("SELECT amount, actual FROM reservations WHERE id=?", (reservation,)).fetchone()
            if row is None or row["actual"] is not None or type(actual) is not int or not 0 <= actual <= row["amount"]:
                raise QuotaError()
            db.execute("UPDATE reservations SET actual=? WHERE id=?", (actual, reservation))
            db.execute("UPDATE budget SET receive=receive-? WHERE id=1", (row["amount"] - actual,))

    def factory(self, factory):
        ledger = self
        def build(**options):
            pool = factory(**options)
            class Pool:
                def urlopen(self, method, url, **kwargs):
                    ledger.request(method, kwargs.get("body"))  # Commit before network.
                    response = pool.urlopen(method, url, **kwargs)
                    class Response:
                        def __getattr__(self, name):
                            return getattr(response, name)

                        def read(self, amount, decode_content=False):
                            reservation = ledger.reserve_read(amount)
                            data = response.read(amount, decode_content=decode_content)
                            if not isinstance(data, bytes):
                                raise QuotaError("invalid_trial_response")
                            ledger.settle_read(reservation, len(data))
                            return data
                    return Response()
            return Pool()
        return build


class Trial:
    def __init__(self, plan, directory):
        self.plan = validate(plan)
        self.manifest = report(self.plan)["manifest_sha256"]
        self.pid, self.nonce = os.getpid(), secrets.token_hex(16)
        self._initialized = False  # No parent/session restart or implicit resume.
        directory = Path(directory).absolute()
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / ("trial-" + self.manifest + ".sqlite3")
        with path.open("xb"):
            pass  # Existing/partial ledger cannot reset quotas by restarting.
        now = time.monotonic_ns()
        with sqlite3.connect(str(path), timeout=1) as db:
            db.executescript("""
                CREATE TABLE budget(id INTEGER PRIMARY KEY, manifest TEXT, nonce TEXT, pid INTEGER,
                  last_ns INTEGER, end_ns INTEGER, requests INTEGER, puts INTEGER, upload INTEGER,
                  wire INTEGER, receive INTEGER);
                CREATE TABLE operations(id TEXT PRIMARY KEY, fingerprint TEXT);
                CREATE TABLE phases(id TEXT, phase TEXT, PRIMARY KEY(id,phase));
                CREATE TABLE reservations(id TEXT PRIMARY KEY, amount INTEGER, actual INTEGER);
            """)
            db.execute("INSERT INTO budget VALUES (1,?,?,?,?,?,0,0,0,0,0)",
                       (self.manifest, self.nonce, self.pid, now, now + WINDOW_NS))
        self.ledger = Ledger(path, self.nonce, self.manifest)

    def admit(self, operation, phase):
        if os.getpid() != self.pid or phase not in ("intent", "committed", "aborted") or \
                report(validate(self.plan))["manifest_sha256"] != self.manifest:
            raise QuotaError("invalid_trial_owner_or_phase")
        normalized = p.Publisher(self.plan["journal_id"], None, None)._operation(operation)
        fingerprint = hashlib.sha256(j1.canonical(normalized)).hexdigest()
        with self.ledger.transaction() as (db, row, now):
            known = db.execute("SELECT fingerprint FROM operations WHERE id=?", (normalized["operation_id"],)).fetchone()
            if known is not None and known[0] != fingerprint:
                raise QuotaError()
            if known is None:
                if db.execute("SELECT count(*) FROM operations").fetchone()[0] >= 10:
                    raise QuotaError()
                db.execute("INSERT INTO operations VALUES (?,?)", (normalized["operation_id"], fingerprint))
            phases = {v[0] for v in db.execute("SELECT phase FROM phases WHERE id=?", (normalized["operation_id"],))}
            if phase not in phases:
                if len(phases) >= 2:
                    raise QuotaError()
                db.execute("INSERT INTO phases VALUES (?,?)", (normalized["operation_id"], phase))
            return normalized, min(5.0, (row["end_ns"] - now) / 10**9)

    def stats(self):
        with self.ledger.transaction() as (db, row, _):
            return {**{key: row[key] for key in ("requests", "puts", "upload", "wire", "receive")},
                    "operations": db.execute("SELECT count(*) FROM operations").fetchone()[0]}

    def publish(self, operation, phase="intent", *, fixture=None):
        from .j4c_call_deadline import run
        from .j4c_publication_worker import Report, _publish
        try:
            if not self._initialized:
                raise QuotaError("trial_initialization_not_confirmed")
            normalized, timeout = self.admit(operation, phase)
            if fixture is not None:
                from .j4c_local_transport import Fixture
                if type(fixture) is not Fixture:
                    raise QuotaError()
                fixture.validate()
        except Exception:
            return Report(p.State.UNAVAILABLE)
        result = run(_publish, args=(self.plan, normalized, phase, fixture,
                                    (str(self.ledger.path), self.nonce, self.manifest)), timeout=timeout)
        return Report(result.value if isinstance(result.value, p.State) else p.State.UNKNOWN,
                      timed_out=result.timed_out, worker_reaped=result.worker_reaped)

    def initialize(self, *, fixture=None):
        from .j4c_call_deadline import run
        from .j4c_initialize import _initialize
        from .j4c_publication_worker import Report
        try:
            if os.getpid() != self.pid or report(validate(self.plan))["manifest_sha256"] != self.manifest:
                raise QuotaError()
            with self.ledger.transaction() as (_, row, now):
                timeout = min(5.0, (row["end_ns"] - now) / 10**9)
            if fixture is not None:
                from .j4c_local_transport import Fixture
                if type(fixture) is not Fixture:
                    raise QuotaError()
                fixture.validate()
        except Exception:
            return Report(p.State.UNAVAILABLE)
        result = run(_initialize, args=(self.plan, fixture,
                                       (str(self.ledger.path), self.nonce, self.manifest)), timeout=timeout)
        state = result.value if isinstance(result.value, p.State) else p.State.UNKNOWN
        if state == p.State.COMMITTED:
            self._initialized = True
        return Report(state,
                      timed_out=result.timed_out, worker_reaped=result.worker_reaped)
