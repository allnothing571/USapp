"""Restricted OFFLINE initialization. Synthetic credentials; no live/repair mode."""
import uuid

from . import deletion_journal as j1
from . import deletion_journal_publication as p
from .j4c_metadata import Metadata
from .j4c_preflight import validate, report
from .j4c_publication_worker import Report
from .j4c_call_deadline import run
from .j4c_tablestore_sdk import Client, Config, ClientError


class SetupClient(Client):
    def __init__(self, *args, initial_plan, **kwargs):
        self.initial_receipt = receipt(validate(initial_plan))
        super().__init__(*args, **kwargs)

    def _key(self, kind, row_id):
        if kind == "bootstrap" and row_id == "current":
            return [("journal_id", self.config.journal_id), ("row_type", kind), ("row_id", row_id)]
        if kind == "head" and row_id == "current":
            return super()._key(kind, row_id)
        raise ClientError("setup_row_not_allowed")

    def insert(self, tx, kind, row_id, columns):
        self._key(kind, row_id)
        expected = self.initial_receipt if kind == "bootstrap" else self.initial_receipt["initial_head"]
        if columns != {"payload": j1.canonical(expected).decode("ascii")}:
            raise ClientError("setup_payload_not_allowed")
        return super().insert(tx, kind, row_id, columns)

    def advance_head(self, *_args, **_kwargs):
        raise ClientError("setup_update_not_allowed")

    def advance_operation(self, *_args, **_kwargs):
        raise ClientError("setup_update_not_allowed")

    def scan(self, *_args, **_kwargs):
        raise ClientError("setup_range_not_allowed")

    def any_partition_row(self, tx):
        self._tx(tx)
        # Do not enumerate known row types: unknown types must block initialization.
        prefix = [("journal_id", self.config.journal_id)]
        return self._call("get_range", self.config.table, "FORWARD",
                          prefix + [("row_type", self._types.INF_MIN), ("row_id", self._types.INF_MIN)],
                          prefix + [("row_type", self._types.INF_MAX), ("row_id", self._types.INF_MAX)],
                          limit=1, max_version=1, transaction_id=tx)


def receipt(plan):
    digest = report(plan)["manifest_sha256"]
    return dict(version=1, journal_id=plan["journal_id"], manifest_sha256=digest,
                initialization_id=str(uuid.uuid5(uuid.UUID(plan["journal_id"]), "initialize-v1:" + digest)),
                initial_head=p.zero_head(plan["journal_id"]))


def initialize_client(plan, client):
    """Injected-client protocol only. Caller must verify fresh lineage separately.

    COMMITTED means initialization recorded, not serving/current-state validated.
    """
    tx = None
    try:
        plan = validate(plan)
        config = client.config
        if (config.journal_id, config.instance, config.table, config.region) != \
                (plan["journal_id"], plan["instance"], plan["table"], plan["region"]):
            return p.State.INVALID
        client.preflight()
        tx = client.start()
        _, following, rows, token = client.any_partition_row(tx)
        if token or not isinstance(rows, list) or len(rows) > 1 or (following and not rows):
            raise ValueError("partial_partition_probe")
        expected = receipt(plan)
        if rows:
            _, row, partial = client.get(tx, "bootstrap", "current")
            if row is None or partial:
                client.finish(tx)
                tx = None
                return p.State.CONFLICT
            reader = Metadata(client, None)
            _, recorded, _ = reader._row(row, "bootstrap", "current")
            if recorded != expected:
                client.finish(tx)
                tx = None
                return p.State.CONFLICT
            reader._head(tx)  # Missing/corrupt current head is never repaired.
            client.finish(tx)
            tx = None
            return p.State.COMMITTED  # Original initialization facts only.
        client.insert(tx, "head", "current", {"payload": j1.canonical(expected["initial_head"]).decode("ascii")})
        client.insert(tx, "bootstrap", "current", {"payload": j1.canonical(expected).decode("ascii")})
        committing, tx = tx, None
        client.finish(committing, commit=True)
        return p.State.COMMITTED
    except Exception:
        if tx is not None:
            try:
                client.finish(tx)
            except Exception:
                pass
        return p.State.UNKNOWN


def _initialize(plan, fixture, trial_ref=None):
    config = Config(plan["region"], plan["instance"], plan["table"], plan["journal_id"],
                    "https://{}.cn-shanghai.ots.aliyuncs.com".format(plan["instance"]))
    def execute(factory=None):
        if trial_ref is not None:
            from .j4c_trial import Ledger
            from .j4c_tablestore_sdk import _BlockedPool
            factory = Ledger(*trial_ref).factory(factory or _BlockedPool)
        return initialize_client(plan, SetupClient(config, "offline-access-id", "offline-synthetic-secret",
                                                  initial_plan=plan, pool_factory=factory))
    if fixture is None:
        return execute()
    from .j4c_local_transport import Pools, restricted
    pools = Pools(fixture)
    try:
        with restricted(fixture):
            return execute(pools.factory(config.endpoint, "tables"))
    finally:
        pools.close()


def initialize(plan, timeout=5.0, *, fixture=None):
    try:
        plan = validate(plan)
        if fixture is not None:
            from .j4c_local_transport import Fixture
            if type(fixture) is not Fixture:
                raise ValueError()
            fixture.validate()
        if type(timeout) not in (int, float) or not 0 < timeout <= 20:
            raise ValueError()
    except Exception:
        return Report(p.State.INVALID)
    result = run(_initialize, args=(plan, fixture), timeout=timeout)
    return Report(result.value if isinstance(result.value, p.State) else p.State.UNKNOWN,
                  timed_out=result.timed_out, worker_reaped=result.worker_reaped)
