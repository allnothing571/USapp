"""Read-only external-journal recovery material. No release, PG access, initialization or production factory.

The external metadata endpoint/identity/current-read contract and restore context MUST be independently pinned.
Transport authentication/context binding is not proof that an operator's supplied incarnation is the current one.
"""
import copy
from dataclasses import dataclass
import hashlib
import hmac
import math
import threading
import time

from . import deletion_journal as j1
from . import deletion_journal_publication as publication
from .deletion_journal_bridge import FrameDispatcher, MAX_BYTES
from .journal_backend import Backend, Config, Credential, _CheckedMetadata, validate_storage_pair
from .j4c_tablestore_sdk import Client, ClientError
from .j4c_oss_objects import Objects


@dataclass(frozen=True)
class Context:
    journal_id: str
    database_id: str
    attempt_id: str

    def validate(self):
        if any(not j1._uuid(v) for v in (self.journal_id, self.database_id, self.attempt_id)):
            raise ValueError("invalid_recovery_context")


def mac(key, direction, body):
    if direction not in ("request", "response"):
        raise ValueError("invalid_recovery_direction")
    return hmac.new(key.encode("ascii"), b"usapp-journal-recovery-v1:" + direction.encode("ascii") + b"\n" + body,
                    hashlib.sha256).hexdigest()


class ReadClient(Client):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._sdk.connection.ALLOWED = frozenset({"DescribeTable", "StartLocalTransaction", "AbortTransaction", "GetRow", "GetRange"})

    def finish(self, transaction_id, commit=False):
        if commit is not False:
            raise ClientError("recovery_write_forbidden")
        return super().finish(transaction_id, False)

    def insert(self, *_args, **_kwargs):
        raise ClientError("recovery_write_forbidden")

    advance_head = insert
    advance_operation = insert


class ReadObjects(Objects):
    def _request(self, method, key, body=None):
        if method != "GET" or body is not None:
            raise publication.Unavailable("recovery_write_forbidden")
        return super()._request(method, key)

    def put_once(self, *_args, **_kwargs):
        return publication.Receipt.CONFLICT  # No serialization/signing/network attempt for a forbidden write.


class _ReadMetadata(_CheckedMetadata):
    def commit(self, *_args, **_kwargs):
        return publication.Receipt.CONFLICT


class _Window:
    """Do not begin the next dependency once a previous synchronous call consumed the budget."""
    def __init__(self, metadata, objects, deadline):
        self.metadata, self.objects, self.deadline = metadata, objects, deadline

    def _check(self):
        if time.monotonic() >= self.deadline:
            raise publication.Unavailable("recovery_deadline")

    def read(self, journal_id):
        self._check()
        value = self.metadata.read(journal_id)
        self._check()
        return value

    def get(self, key):
        self._check()
        value = self.objects.get(key)
        self._check()
        return value


class Reader:
    """A current full-chain validation plus second uncached metadata read; no cached/empty fallback."""
    def __init__(self, context, objects, metadata, budget=2.0):
        if type(context) is not Context:
            raise ValueError("invalid_recovery_context")
        context.validate()
        if type(budget) not in (int, float) or not math.isfinite(budget) or not .05 <= budget <= 10:
            raise ValueError("invalid_recovery_budget")
        self.context, self.objects, self.metadata, self.budget = context, objects, metadata, budget
        self.guard = threading.Lock()
        self.closed = False

    def read(self, method):
        if method not in ("snapshot", "current_head") or not self.guard.acquire(False):
            return None
        deadline = time.monotonic() + self.budget
        try:
            if self.closed:
                return None
            window = _Window(self.metadata, self.objects, deadline)
            validator = publication.Publisher(self.context.journal_id, window, window)
            first, _, _, plan = validator._load()
            first = copy.deepcopy(first)
            # Metadata.read also brackets every index scan with current head reads. After object/chain validation,
            # re-read ALL current metadata; never accept changed head, missing/contradictory indexes or a stale cache.
            following = window.read(self.context.journal_id)
            if first != following or time.monotonic() >= deadline:
                return None
            checkpoint = first["checkpoint"]
            result = dict(checkpoint=copy.deepcopy(checkpoint), snapshot=None)
            if method == "snapshot":
                targets = lambda values: [dict(kind=row["target_kind"], id=row["target_id"]) for row in
                                          sorted(values, key=lambda r: (0 if r["target_kind"] == "memory" else 1, r["target_id"]))]
                result["snapshot"] = dict(
                    # Recovery Head is the v2 BATCH head, binding operation envelopes as well as logical events.
                    head=dict(journal_id=self.context.journal_id, seq=checkpoint["batch_seq"], hash=checkpoint["batch_head"]),
                    hidden=targets(plan["hidden_targets"]), purge_barriers=targets(plan["purge_barriers"]),
                    unresolved=sorted(plan["unresolved_intents"]))  # Per-subject J1 intent ids, not J2 operation ids.
            return result
        except Exception:
            return None
        finally:
            self.guard.release()


class RecoveryDispatcher(FrameDispatcher):
    def __init__(self, context, key, reader):
        if type(context) is not Context or not j1._hash(key) or not isinstance(reader, Reader) or reader.context != context:
            raise ValueError("invalid_recovery_dispatcher")
        context.validate()
        self.context, self.key, self.reader = context, key, reader
        self.guard = threading.Lock()
        self.active = self.closing = False

    def authenticate(self, body, signature):
        if type(body) is not bytes or len(body) > MAX_BYTES:
            return 413
        if not j1._hash(signature) or not hmac.compare_digest(mac(self.key, "request", body), signature):
            return 401
        return 200

    def handle(self, body, signature):
        auth = self.authenticate(body, signature)
        if auth != 200:
            return auth, b"", ""
        try:
            value = j1._decode(body)
            if not isinstance(value, dict) or set(value) != {"version", "journal_id", "database_id", "attempt_id", "nonce", "method"} or \
                    type(value["version"]) is not int or value["version"] != 1 or \
                    any(value[name] != getattr(self.context, name) for name in ("journal_id", "database_id", "attempt_id")) or \
                    not isinstance(value["nonce"], str) or len(value["nonce"]) != 32 or \
                    any(c not in "0123456789abcdef" for c in value["nonce"]) or value["method"] not in ("snapshot", "current_head"):
                raise ValueError()
        except (ValueError, TypeError, KeyError, RecursionError):
            return 400, b"", ""
        with self.guard:
            if self.closing or self.active:
                return 503, b"", ""
            self.active = True
        try:
            material = self.reader.read(value["method"])
            reply = dict(version=1, journal_id=self.context.journal_id, database_id=self.context.database_id,
                         attempt_id=self.context.attempt_id, nonce=value["nonce"], request_sha256=hashlib.sha256(body).hexdigest(),
                         state="verified" if material is not None else "unknown", serving_allowed=False,
                         checkpoint=material["checkpoint"] if material is not None else None,
                         snapshot=material["snapshot"] if material is not None else None)
            raw = j1.canonical(reply)
            if len(raw) > MAX_BYTES:
                return 503, b"", ""  # Refuse the WHOLE snapshot; never truncate or drop a barrier/id.
            return 200, raw, mac(self.key, "response", raw)
        except Exception:
            return 503, b"", ""
        finally:
            with self.guard:
                self.active = False


def _close_reader_clients(clients):
    remaining = []
    for client in reversed(clients):
        try:
            client.close()
        except Exception:
            remaining.append(client)
    return list(reversed(remaining))


def _open_reader(table, objects, context, credential, table_pool, objects_pool, budget, client_type, objects_type):
    context.validate()
    credential.validate()
    if context.journal_id != table.journal_id or any(f is not None and not callable(f) for f in (table_pool, objects_pool)):
        raise ValueError("invalid_recovery_backend")
    clients = []
    try:
        client = client_type(table, credential.access_key_id, credential.access_key_secret, pool_factory=table_pool)
        clients.append(client)
        store = objects_type(objects, credential.access_key_id, credential.access_key_secret, pool_factory=objects_pool)
        clients.append(store)
        return clients, Reader(context, store, _ReadMetadata(client, store), budget)
    except Exception:
        _close_reader_clients(clients)
        raise ValueError("recovery_backend_construction_failed") from None


class CheckpointObserver:
    """Signer-side read owner. The caller supplies an independent enrolled read-only identity."""
    def __init__(self, table, objects, context, credential, *, table_pool=None, objects_pool=None, budget=2.0,
                 _client_type=ReadClient, _objects_type=ReadObjects):
        validate_storage_pair(table, objects)
        if type(context) is not Context or type(credential) is not Credential:
            raise ValueError("invalid_checkpoint_observer")
        self.clients, self.reader = _open_reader(table, objects, context, credential, table_pool, objects_pool,
                                                 budget, _client_type, _objects_type)

    def observe(self, journal_id):
        if journal_id != self.reader.context.journal_id:
            return None
        observed_at = int(time.time())
        result = self.reader.read("current_head")
        if result is None:
            return None
        head = result["checkpoint"]
        return dict(checkpoint=dict(journalId=journal_id, seq=str(head["batch_seq"]), hash=head["batch_head"]),
                    observedAt=observed_at)

    def close(self):
        if not self.reader.guard.acquire(False):
            return False
        try:
            self.reader.closed = True
            self.clients = _close_reader_clients(self.clients)
            return not self.clients
        finally:
            self.reader.guard.release()


class RecoveryBackend(Backend):
    """One recovery principal, no writer credentials. Trial resource restrictions and blocked defaults retained."""
    def __init__(self, config, context, credential, *, table_pool=None, objects_pool=None, budget=2.0,
                 _client_type=ReadClient, _objects_type=ReadObjects):
        if type(config) is not Config or type(context) is not Context or type(credential) is not Credential:
            raise ValueError("invalid_recovery_backend")
        config.validate()
        self.closed, self.attached = False, False
        self.close_guard = threading.Lock()
        self.clients, reader = _open_reader(config.table, config.objects, context, credential, table_pool,
                                            objects_pool, budget, _client_type, _objects_type)
        try:
            self.dispatcher = RecoveryDispatcher(context, config.mac_key, reader)
        except Exception:
            _close_reader_clients(self.clients)
            raise ValueError("recovery_backend_construction_failed") from None

    def close(self):
        # Also protect programmatic Reader.read calls, which do not pass through the HTTP dispatcher lease.
        with self.dispatcher.guard:
            self.dispatcher.closing = True
        reader = self.dispatcher.reader
        if not reader.guard.acquire(False):
            return False
        try:
            reader.closed = True
            return super().close()
        finally:
            reader.guard.release()
