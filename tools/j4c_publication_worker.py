"""Bounded OFFLINE publication entry, synthetic keys only. No live mode or CLI."""
from dataclasses import dataclass

from . import deletion_journal as j1
from . import deletion_journal_publication as p
from .j4c_call_deadline import run
from .j4c_metadata import Metadata
from .j4c_oss_objects import Config as ObjectConfig, Objects
from .j4c_preflight import validate
from .j4c_tablestore_sdk import Client, Config as TableConfig


@dataclass(frozen=True)
class Report:
    state: p.State
    timed_out: bool = False
    worker_reaped: bool = True
    proven_not_applied: bool = False
    serving_allowed: bool = False


def _publish(plan, operation, phase, fixture, trial_ref=None):
    plan = validate(plan)
    table = TableConfig(plan["region"], plan["instance"], plan["table"], plan["journal_id"],
                        "https://{}.cn-shanghai.ots.aliyuncs.com".format(plan["instance"]))
    obj = ObjectConfig(plan["bucket"], plan["journal_id"])
    def execute(pools=None):
        table_factory = pools.factory(table.endpoint, "tables") if pools else None
        object_factory = pools.factory(obj.endpoint, "objects") if pools else None
        if trial_ref is not None:
            from .j4c_trial import Ledger
            from .j4c_tablestore_sdk import _BlockedPool
            ledger = Ledger(*trial_ref)
            table_factory = ledger.factory(table_factory or _BlockedPool)
            object_factory = ledger.factory(object_factory or _BlockedPool)
        client = Client(table, "offline-access-id", "offline-synthetic-secret", pool_factory=table_factory)
        try:
            client.preflight()
        except Exception:
            return p.State.UNAVAILABLE
        objects = Objects(obj, "offline-id", "offline-secret", pool_factory=object_factory)
        publisher = p.Publisher(plan["journal_id"], objects, Metadata(client, objects))
        return publisher.publish(operation, phase)
    if fixture is None:
        return execute()  # Both default clients block network; no global enable switch.
    from .j4c_local_transport import Pools, restricted
    pools = Pools(fixture)
    try:
        with restricted(fixture):
            return execute(pools)
    finally:
        pools.close()


def publish(plan, operation, phase="intent", timeout=5.0, *, fixture=None):
    try:
        plan = validate(plan)
        if phase not in ("intent", "committed", "aborted") or len(j1.canonical(operation)) > 65536:
            raise ValueError()
        operation = p.Publisher(plan["journal_id"], None, None)._operation(operation)
        if fixture is not None:
            from .j4c_local_transport import Fixture
            if type(fixture) is not Fixture:
                raise ValueError()
            fixture.validate()
        if type(timeout) not in (int, float) or not 0 < timeout <= 20:
            raise ValueError()
    except Exception:
        return Report(p.State.INVALID)
    result = run(_publish, args=(plan, operation, phase, fixture), timeout=timeout)
    state = result.value if isinstance(result.value, p.State) else p.State.UNKNOWN
    return Report(state, timed_out=result.timed_out, worker_reaped=result.worker_reaped)
