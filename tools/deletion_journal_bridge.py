"""Authenticated wire dispatcher with an INJECTED publisher; no HTTP server, SDK factory, CLI or release permission.

The transport must provide confidentiality, bounded calls/queues and private deployment. MACs authenticate bytes;
they do not prove durability, checkpoint freshness or permission to serve a restored database.
"""
import copy
import hashlib
import hmac
from abc import ABC, abstractmethod

from . import deletion_journal as j1
from . import deletion_journal_publication as p

MAX_BYTES = 65536
MAX_SUBJECTS = 256


def mac(key, direction, body):
    if direction not in ("request", "response"):
        raise ValueError("invalid_direction")
    return hmac.new(key.encode("ascii"), b"usapp-journal-bridge-v1:" + direction.encode("ascii") + b"\n" + body,
                    hashlib.sha256).hexdigest()


class FrameDispatcher(ABC):
    """Authenticated bounded-frame interface; separate services may use separate MAC domains and capabilities."""
    @abstractmethod
    def authenticate(self, body, signature):
        raise NotImplementedError

    @abstractmethod
    def handle(self, body, signature):
        raise NotImplementedError


class Dispatcher(FrameDispatcher):
    def __init__(self, journal_id, key, publisher):
        if not j1._uuid(journal_id) or not j1._hash(key) or not isinstance(publisher, p.Publisher) or \
                publisher.journal_id != journal_id:
            raise ValueError("invalid_bridge_configuration")
        self.journal_id, self.key, self.publisher = journal_id, key, publisher

    def authenticate(self, body, signature):
        """Pure frame authentication, also used before the HTTP service's backend admission."""
        if type(body) is not bytes or len(body) > MAX_BYTES:
            return 413
        if not j1._hash(signature) or not hmac.compare_digest(mac(self.key, "request", body), signature):
            return 401
        return 200

    def handle(self, body, signature):
        # Reauthenticate even when a protected service has already performed its own admission check.
        authenticated = self.authenticate(body, signature)
        if authenticated != 200:
            return authenticated, b"", ""
        try:
            request = j1._decode(body)
            if not isinstance(request, dict) or set(request) != {"version", "journal_id", "nonce", "method", "operation", "operation_id", "phase"} or \
                    type(request["version"]) is not int or request["version"] != 1 or request["journal_id"] != self.journal_id or \
                    not isinstance(request["nonce"], str) or len(request["nonce"]) != 32 or \
                    any(c not in "0123456789abcdef" for c in request["nonce"]) or not j1._uuid(request["operation_id"]):
                raise ValueError()
            method, op, phase = request["method"], request["operation"], request["phase"]
            if method == "lookup":
                if op is not None or phase is not None:
                    raise ValueError()
            elif method in ("ensure_intent", "settle"):
                if method == "ensure_intent" and phase != "intent" or method == "settle" and phase not in ("committed", "aborted"):
                    raise ValueError()
                if not isinstance(op, dict) or not isinstance(op.get("subjects"), list) or len(op["subjects"]) > MAX_SUBJECTS:
                    raise ValueError()
                normalized = self.publisher._operation(op)  # Pure validation; no metadata/object call.
                if normalized != op or op["operation_id"] != request["operation_id"]:
                    raise ValueError()
            else:
                raise ValueError()
        except (ValueError, TypeError, KeyError, RecursionError):
            return 400, b"", ""
        operation = None
        try:
            if method == "lookup":
                # Public lookup validates the complete referenced chain AND indexes before Missing/Found.
                state, operation = self.publisher.lookup(request["operation_id"])
            else:
                state = self.publisher.publish(op, phase).value
        except Exception:
            state, operation = "unknown", None
        allowed = {"missing", "found", "unknown"} if method == "lookup" else {s.value for s in p.State}
        if state not in allowed:
            state, operation = "unknown", None
        if state == "found":
            try:
                if len(operation["subjects"]) > MAX_SUBJECTS or self.publisher._operation(operation) != operation or \
                        operation["operation_id"] != request["operation_id"]:
                    raise ValueError()
            except (ValueError, TypeError, KeyError, RecursionError):
                state, operation = "unknown", None
        elif operation is not None:
            state, operation = "unknown", None
        response = dict(version=1, journal_id=self.journal_id, nonce=request["nonce"],
                        request_sha256=hashlib.sha256(body).hexdigest(), state=state, operation=copy.deepcopy(operation))
        try:
            raw = j1.canonical(response)
        except (ValueError, TypeError, RecursionError):
            return 503, b"", ""
        if len(raw) > MAX_BYTES:
            return 503, b"", ""
        return 200, raw, mac(self.key, "response", raw)
