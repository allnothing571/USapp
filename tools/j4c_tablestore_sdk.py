"""J4c1 restricted real-SDK facade for OFFLINE client preparation.

Default transport refuses all I/O. Used by j4c_metadata's offline-tested adapter;
no credential-file reader, live runner, resource creation or production wiring.
"""
import logging
import re
from dataclasses import dataclass

from .deletion_journal import ZERO, _hash, _integer, _uuid, canonical
from .deletion_journal_batches import CHECKPOINT_FIELDS


class ClientError(Exception):
    pass


@dataclass(frozen=True)
class Config:
    region: str
    instance: str
    table: str
    journal_id: str
    endpoint: str

    def validate(self):
        if self.region != "cn-shanghai" or not isinstance(self.instance, str) or \
                not re.fullmatch(r"uj4c[0-9a-f]{8}", self.instance) or self.table != "journal" or \
                not _uuid(self.journal_id) or self.endpoint != "https://{}.cn-shanghai.ots.aliyuncs.com".format(self.instance):
            raise ClientError("invalid_test_configuration")


class _BlockedPool:
    def __init__(self, **_):
        pass

    def urlopen(self, *_args, **_kwargs):
        raise ClientError("network_not_enabled")

    def clear(self):
        pass  # This transport never owns sockets or connections.


class _Transport:
    ALLOWED = {"DescribeTable", "StartLocalTransaction", "CommitTransaction", "AbortTransaction",
               "GetRow", "GetRange", "PutRow", "UpdateRow"}

    def __init__(self, endpoint, pool, timeout, max_reply):
        self.endpoint, self.pool, self.timeout, self.max_reply = endpoint, pool, timeout, max_reply

    def send_receive(self, api, headers, body):
        if api not in {"/" + name for name in self.ALLOWED}:
            raise ClientError("operation_not_allowed")
        response = None
        complete = False
        try:
            response = self.pool.urlopen("POST", self.endpoint + api, body=body, headers=headers,
                                         redirect=False, retries=False, timeout=self.timeout,
                                         pool_timeout=1.0, preload_content=False)
            if any(k.lower() == "content-encoding" and v.lower() != "identity"
                   for k, v in response.headers.items()):
                raise ClientError("response_encoding")
            data = response.read(self.max_reply + 1, decode_content=False)
            if not isinstance(data, bytes) or len(data) > self.max_reply:
                raise ClientError("response_limit")
            result = (response.status, response.reason, dict(response.headers), data)
            complete = True
            return result
        finally:
            if response is not None:
                if complete:
                    response.release_conn()
                else:
                    response.close()


class Client:
    def __init__(self, config, access_key_id, access_key_secret, pool_factory=None, max_reply=1024 * 1024):
        config.validate()
        if not isinstance(access_key_id, str) or not access_key_id or \
                not isinstance(access_key_secret, str) or not access_key_secret or \
                type(max_reply) is not int or not 1 <= max_reply <= 1024 * 1024:
            raise ClientError("invalid_client_parameters")
        # Lazy import: normal repository tests do not acquire an SDK dependency.
        import tablestore
        from tablestore.retry import NoRetryPolicy
        import certifi
        from urllib3.util import Timeout
        if tablestore.__version__ != "6.4.8":
            raise ClientError("sdk_version_mismatch")
        logger_name = "usapp.j4c.sdk.offline"
        logger = logging.getLogger(logger_name)
        logger.disabled, logger.propagate = True, False
        sdk = tablestore.OTSClient(config.endpoint, access_key_id, access_key_secret, config.instance,
                                   region=config.region, retry_policy=NoRetryPolicy(), socket_timeout=3,
                                   max_connection=1, logger_name=logger_name,
                                   enable_native=False, native_fallback=False)
        # Dispose of the unused stock pool. Our transport fixes urllib3 retry,
        # redirect, CA verification and per-socket timeouts explicitly.
        sdk.connection.pool.clear()
        timeout = Timeout(connect=2.0, read=3.0)
        factory = pool_factory or _BlockedPool
        pool = factory(num_pools=1, cert_reqs="CERT_REQUIRED", ca_certs=certifi.where(),
                       timeout=timeout, maxsize=1, block=True, retries=False)
        sdk.connection = _Transport(config.endpoint, pool, timeout, max_reply)
        self.config, self._sdk, self._types, self._active = config, sdk, tablestore, set()
        self._closed = False

    def _call(self, name, *args, **kwargs):
        if self._closed:
            raise ClientError("client_closed")
        try:
            return getattr(self._sdk, name)(*args, **kwargs)
        except Exception:
            # Unknown, not proven-unapplied. Never expose SDK body/headers/keys.
            raise ClientError("sdk_call_unknown") from None

    def close(self):
        if self._active:
            raise ClientError("transaction_still_active")
        if self._closed:
            return
        pool = self._sdk.connection.pool
        cleanup = getattr(pool, "clear", None) or getattr(pool, "close", None)
        if not callable(cleanup):
            raise ClientError("pool_cleanup_unavailable")
        cleanup()
        self._closed = True

    def _tx(self, transaction_id):
        if not isinstance(transaction_id, str) or transaction_id not in self._active:
            raise ClientError("unknown_transaction")

    def _key(self, row_type, row_id):
        if row_type not in ("head", "batch", "operation", "cycle") or not isinstance(row_id, str) or not 1 <= len(row_id) <= 256:
            raise ClientError("invalid_row")
        return [("journal_id", self.config.journal_id), ("row_type", row_type), ("row_id", row_id)]

    def describe(self):
        return self._call("describe_table", self.config.table)

    def preflight(self):
        description = self.describe()
        try:
            valid = description.table_meta.table_name == self.config.table and \
                description.table_meta.schema_of_primary_key == [("journal_id", "STRING"), ("row_type", "STRING"), ("row_id", "STRING")] and \
                description.table_options.time_to_live == -1 and description.table_options.max_version == 1 and \
                description.table_options.allow_update is True and not description.secondary_indexes and \
                description.reserved_throughput_details.capacity_unit.read == 0 and \
                description.reserved_throughput_details.capacity_unit.write == 0
        except Exception:
            raise ClientError("preflight_invalid_table") from None
        if not valid:
            raise ClientError("preflight_invalid_table")
        # SDK DescribeTable does not expose a local-transaction-enabled field.
        # A real Start/Abort is required after live access is explicitly approved.
        transaction = self.start()
        self.finish(transaction)
        return {"table_verified": True, "transaction_probe": True, "serving_allowed": False}

    def start(self):
        if self._active:
            raise ClientError("transaction_already_active")
        value = self._call("start_local_transaction", self.config.table, [("journal_id", self.config.journal_id)])
        if not isinstance(value, str) or not 0 < len(value) <= 1024:
            raise ClientError("invalid_transaction_receipt")
        self._active.add(value)
        return value

    def finish(self, transaction_id, commit=False):
        self._tx(transaction_id)
        if type(commit) is not bool:
            raise ClientError("invalid_transaction_action")
        # A lost commit reply MUST NOT trigger an automatic abort or blind retry.
        self._active.remove(transaction_id)
        return self._call("commit_transaction" if commit else "abort_transaction", transaction_id)

    def get(self, transaction_id, row_type, row_id):
        self._tx(transaction_id)
        return self._call("get_row", self.config.table, self._key(row_type, row_id),
                          max_version=1, transaction_id=transaction_id)

    def scan(self, transaction_id, row_type, start="", limit=20):
        self._tx(transaction_id)
        if row_type not in ("head", "batch", "operation", "cycle") or type(limit) is not int or not 1 <= limit <= 100 or \
                not isinstance(start, str) or len(start) > 256:
            raise ClientError("invalid_range")
        prefix = [("journal_id", self.config.journal_id), ("row_type", row_type)]
        return self._call("get_range", self.config.table, "FORWARD", prefix + [("row_id", start)],
                          prefix + [("row_id", self._types.INF_MAX)], limit=limit,
                          max_version=1, transaction_id=transaction_id)

    def insert(self, transaction_id, row_type, row_id, columns):
        self._tx(transaction_id)
        if not isinstance(columns, dict) or set(columns) != {"payload"} or \
                not isinstance(columns["payload"], str) or len(columns["payload"].encode("utf-8")) > 65536:
            raise ClientError("invalid_columns")
        # Insert only; existing immutable batch/cycle rows cannot be replaced.
        row = self._types.Row(self._key(row_type, row_id), [("payload", columns["payload"])])
        condition = self._types.Condition(self._types.RowExistenceExpectation.EXPECT_NOT_EXIST)
        return self._call("put_row", self.config.table, row, condition, transaction_id=transaction_id)

    def advance_operation(self, transaction_id, operation_id, previous, following):
        """Conditional intent-to-terminal replacement, preserving the operation binding."""
        from .deletion_journal import _decode
        self._tx(transaction_id)
        if not _uuid(operation_id) or not all(isinstance(v, str) and len(v.encode("utf-8")) <= 65536
                                              for v in (previous, following)):
            raise ClientError("invalid_operation_transition")
        try:
            old, new = (_decode(v.encode("utf-8")) for v in (previous, following))
            if any(not isinstance(v, dict) or set(v) != {"operation", "phase"} for v in (old, new)) or \
                    old["phase"] != "intent" or new["phase"] not in ("committed", "aborted") or \
                    old["operation"] != new["operation"] or old["operation"]["operation_id"] != operation_id or \
                    previous != canonical(old).decode("ascii") or following != canonical(new).decode("ascii"):
                raise ValueError()
        except Exception:
            raise ClientError("invalid_operation_transition") from None
        row = self._types.Row(self._key("operation", operation_id), {"PUT": [("payload", following)]})
        column = self._types.SingleColumnCondition("payload", previous, self._types.ComparatorType.EQUAL,
                                                   pass_if_missing=False, latest_version_only=True)
        condition = self._types.Condition(self._types.RowExistenceExpectation.EXPECT_EXIST, column)
        return self._call("update_row", self.config.table, row, condition, transaction_id=transaction_id)

    def advance_head(self, transaction_id, expected, following):
        """Encode a conditional head update only, NOT the full publication transaction/proof."""
        self._tx(transaction_id)
        for head in (expected, following):
            if not isinstance(head, dict) or set(head) != CHECKPOINT_FIELDS or type(head["version"]) is not int or head["version"] != 2 or \
                    head["journal_id"] != self.config.journal_id or \
                    not all(_integer(head[k]) and 0 <= head[k] < 2**63 for k in ("batch_seq", "event_seq")) or \
                    not all(_hash(head[k]) for k in ("batch_head", "event_head")):
                raise ClientError("invalid_head")
            if head["batch_seq"] == 0 and (head["event_seq"] != 0 or head["batch_head"] != ZERO or head["event_head"] != ZERO):
                raise ClientError("invalid_head")
            if head["batch_seq"] > 0 and (head["event_seq"] < head["batch_seq"] or head["batch_head"] == ZERO or head["event_head"] == ZERO):
                raise ClientError("invalid_head")
        if following["batch_seq"] != expected["batch_seq"] + 1 or following["event_seq"] <= expected["event_seq"] or \
                following["batch_head"] == expected["batch_head"]:
            raise ClientError("invalid_head_advance")
        previous = canonical(expected).decode("ascii")
        row = self._types.Row(self._key("head", "current"), {"PUT": [("payload", canonical(following).decode("ascii"))]})
        column = self._types.SingleColumnCondition("payload", previous, self._types.ComparatorType.EQUAL,
                                                   pass_if_missing=False, latest_version_only=True)
        condition = self._types.Condition(self._types.RowExistenceExpectation.EXPECT_EXIST, column)
        return self._call("update_row", self.config.table, row, condition, transaction_id=transaction_id)
