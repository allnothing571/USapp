"""Explicit one-session synthetic cloud trial. Dry-run by default.

No real business operations, resume, cleanup, resource creation or service wiring.
Unknown outcomes stop, retaining local evidence. Network requires explicit run.
"""
import argparse
from contextlib import nullcontext, closing
import datetime
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import sys
import uuid

from . import deletion_journal as j1
from . import deletion_journal_publication as p
from .j4c_credentials import load
from .j4c_identity_preflight import dry_run
from .j4c_call_deadline import run
from .j4c_initialize import SetupClient, initialize_client, receipt
from .j4c_metadata import Metadata
from .j4c_oss_objects import Objects, Config as ObjectConfig
from .j4c_tablestore_sdk import Client, Config
from .j4c_trial import Trial, Ledger


STEPS = ('identity-object-publisher', 'identity-checkpoint-writer', 'identity-recovery-reader',
         'bucket', 'table', 'initialize', 'intent', 'aborted', 'verify')
EXPECTED = (p.State.READY,)*5 + (p.State.COMMITTED, p.State.READY, p.State.ABORTED, p.State.ABORTED)


def operation(plan):
    namespace = uuid.UUID(plan['journal_id'])
    def value(label):
        return str(uuid.uuid5(namespace, 'j4c-synthetic-smoke-v1:'+label))
    return dict(operation_id=value('operation'), action='delete',
                request_fingerprint=hashlib.sha256(b'J4c synthetic smoke only: no business deletion').hexdigest(),
                subjects=[dict(target_kind='media', target_id=value('nonexistent-media'),
                               cycle_id=value('cycle'), intent_id=value('intent'))])


class FreshSetup(SetupClient):
    def any_partition_row(self, tx):
        result = super().any_partition_row(tx)
        if result[1] or result[2] or result[3]:
            raise ValueError('fresh_partition_required')
        return result


class ReadClient(Client):
    def _key(self, kind, row_id):
        if kind == 'bootstrap' and row_id == 'current':
            return [('journal_id',self.config.journal_id),('row_type',kind),('row_id',row_id)]
        return super()._key(kind,row_id)


class SmokeMetadata(Metadata):
    def __init__(self,client,objects,expected_step,synthetic):
        super().__init__(client,objects)
        self.expected_step,self.synthetic=expected_step,synthetic

    def read(self,journal_id):
        snapshot=super().read(journal_id)
        previous={'intent':0,'aborted':1,'verify':2}[self.expected_step]
        op=self.synthetic
        expected_operations={} if previous==0 else {
            op['operation_id']:{'operation':op,'phase':'intent' if previous==1 else 'aborted'}}
        expected_cycles=[] if previous==0 else [('media',op['subjects'][0]['target_id'],op['subjects'][0]['cycle_id'])]
        if snapshot['checkpoint']['batch_seq']!=previous or snapshot['checkpoint']['event_seq']!=previous or \
                len(snapshot['keys'])!=previous or snapshot['operations']!=expected_operations or snapshot['cycles']!=expected_cycles:
            raise ValueError('unexpected_synthetic_journal_state')
        return snapshot


def scoped_factory(plan, ledger, factory):
    """Quota before network; exact endpoint/operation whitelist; key recorded before PUT.

    Returns a factory plus its explicit cleanup. Cleanup never sends a request.
    """
    raw_pools = []
    table = 'https://{}.cn-shanghai.ots.aliyuncs.com'.format(plan['instance'])
    bucket = 'https://{}.oss-cn-shanghai.aliyuncs.com'.format(plan['bucket'])
    object_prefix = bucket+'/journals/'+plan['journal_id']+'/batches/'
    table_apis = {'DescribeTable','StartLocalTransaction','AbortTransaction','GetRow','GetRange',
                  'PutRow','UpdateRow','CommitTransaction'}
    def raw_factory(**options):
        pool = factory(**options)
        raw_pools.append(pool)
        return pool
    quota_factory = ledger.factory(raw_factory)
    def build(**options):
        pool = quota_factory(**options)
        class Scope:
            def urlopen(self,method,url,**kwargs):
                allowed = (method == 'POST' and url == 'https://sts.cn-shanghai.aliyuncs.com/') or \
                          (method == 'POST' and url in {table+'/'+api for api in table_apis}) or \
                          (method == 'GET' and url in {bucket+'/?bucketInfo',bucket+'/?versioning'})
                key = None
                if method in ('GET','PUT') and isinstance(url,str) and url.startswith(object_prefix):
                    suffix = url[len(object_prefix):]
                    allowed = re.fullmatch(r'[0-9a-f]{64}\.json',suffix) is not None
                    key = 'journals/'+plan['journal_id']+'/batches/'+suffix if allowed else None
                if not allowed:
                    raise ValueError('trial_request_scope')
                if method == 'PUT':
                    # Audit may name an absent key if quota refuses later; never an undeclared possible PUT.
                    with ledger.transaction() as (db,_,__):
                        db.execute('INSERT OR IGNORE INTO object_keys VALUES (?)',(key,))
                return pool.urlopen(method,url,**kwargs)
        return Scope()
    def close():
        for pool in raw_pools:
            clear = getattr(pool,'clear',None) or getattr(pool,'close',None)
            if clear:
                clear()
    return build,close


def execute(plan,bundle,step,trial_ref,ack=False,fixture=None):
    """Spawnable stage. No exception contents leave this worker."""
    close = lambda:None
    pools = None
    try:
        from .j4c_preflight import validate, report
        from .j4c_resource_package import POLICY_ROLES
        plan = validate(plan)
        if ack is not True or step not in STEPS or bundle.manifest_sha256 != report(plan)['manifest_sha256'] or \
                set(bundle.keys) != set(POLICY_ROLES) or datetime.datetime.now(datetime.timezone.utc) >= bundle.expires_at:
            return p.State.INVALID
        ledger = Ledger(*trial_ref)
        if fixture is None:
            from urllib3 import PoolManager
            factory = PoolManager
            context = nullcontext()
        else:
            from .j4c_local_transport import Fixture, Pools, restricted
            if type(fixture) is not Fixture:
                return p.State.INVALID
            pools = Pools(fixture)
            def factory(**options):
                # Each signed client passes a fixed endpoint to urlopen; route it by exact host.
                routes = {}
                class Route:
                    def urlopen(self,method,url,**kwargs):
                        if url.startswith('https://sts.cn-shanghai.aliyuncs.com/'):
                            endpoint,route='https://sts.cn-shanghai.aliyuncs.com','identity'
                        elif '.ots.aliyuncs.com/' in url:
                            endpoint,route='https://{}.cn-shanghai.ots.aliyuncs.com'.format(plan['instance']),'tables'
                        else:
                            endpoint,route='https://{}.oss-cn-shanghai.aliyuncs.com'.format(plan['bucket']),'objects'
                        if endpoint not in routes:
                            routes[endpoint]=pools.factory(endpoint,route)(**options)
                        return routes[endpoint].urlopen(method,url,**kwargs)
                return Route()
            context = restricted(fixture)
        guarded,close = scoped_factory(plan,ledger,factory)
        with context:
            return run_stage(plan,bundle,step,guarded)
    except Exception:
        return p.State.UNKNOWN
    finally:
        try:
            close()
        finally:
            if pools:
                pools.close()


def run_stage(plan,bundle,step,factory):
    """Injected transport implementation. CLI exposes no fixture or payload switches."""
    from .j4c_identity import Client as Identity
    from .j4c_resource_preflight import BucketReader, validate_bucket, table_probe
    if step.startswith('identity-'):
        role = step[len('identity-'):]
        key = bundle.keys[role]
        client = Identity(key,plan['declared_owner_uid'],'usapp-j4c-{}-{}'.format(plan['run_id'],role),pool_factory=factory)
        try:
            return p.State.READY if client.verify() is True else p.State.UNKNOWN
        finally:
            client.close()
    if step == 'bucket':
        reader = BucketReader(plan,bundle.keys['recovery-reader'],factory)
        try:
            return p.State.READY if validate_bucket(reader.get('bucketInfo'),reader.get('versioning'),plan) else p.State.UNKNOWN
        finally:
            reader.close()
    if step == 'table':
        return p.State.READY if table_probe(plan,bundle.keys['recovery-reader'],factory) else p.State.UNKNOWN
    config = Config(plan['region'],plan['instance'],plan['table'],plan['journal_id'],
                    'https://{}.cn-shanghai.ots.aliyuncs.com'.format(plan['instance']))
    writer = bundle.keys['checkpoint-writer']
    if step == 'initialize':
        client = FreshSetup(config,writer.access_key_id,writer.access_key_secret,initial_plan=plan,pool_factory=factory)
        client._sdk.connection.ALLOWED = {'DescribeTable','StartLocalTransaction','AbortTransaction','GetRange','GetRow','PutRow','CommitTransaction'}
        return initialize_client(plan,client)
    if step not in ('intent','aborted','verify'):
        return p.State.INVALID
    read_only = step == 'verify'
    table_key = bundle.keys['recovery-reader'] if read_only else writer
    object_key = bundle.keys['recovery-reader'] if read_only else bundle.keys['object-publisher']
    client = ReadClient(config,table_key.access_key_id,table_key.access_key_secret,pool_factory=factory)
    if read_only:
        client._sdk.connection.ALLOWED = {'DescribeTable','StartLocalTransaction','AbortTransaction','GetRow','GetRange'}
    objects = Objects(ObjectConfig(plan['bucket'],plan['journal_id']),object_key.access_key_id,
                      object_key.access_key_secret,pool_factory=factory)
    # Checkpoint validation reads with its own credential, not the publisher's.
    validator_objects = objects if read_only else Objects(ObjectConfig(plan['bucket'],plan['journal_id']),
                            writer.access_key_id,writer.access_key_secret,pool_factory=factory)
    client.preflight()
    publisher = p.Publisher(plan['journal_id'],objects,SmokeMetadata(client,validator_objects,step,operation(plan)),
                            max_rounds=1,max_batches=2)
    if not read_only:
        return publisher.publish(operation(plan),step)
    snapshot,frames,_,__ = publisher._load()
    op = operation(plan)
    if len(frames) != 2 or [f['phase'] for f in frames] != ['intent','aborted'] or \
            any(p.operation_of(frame) != op for frame in frames) or \
            snapshot['operations'] != {op['operation_id']:{'operation':op,'phase':'aborted'}}:
        return p.State.CONFLICT
    tx = client.start()
    try:
        _,row,token = client.get(tx,'bootstrap','current')
        if token or row is None or Metadata(client,None)._row(row,'bootstrap','current')[1] != receipt(plan):
            return p.State.CONFLICT
    finally:
        client.finish(tx)
    return p.State.ABORTED


class Session:
    def __init__(self,plan,bundle,directory,ack=False):
        from .j4c_preflight import report
        if ack is not True or bundle.manifest_sha256 != report(plan)['manifest_sha256'] or Path(directory).exists():
            raise ValueError('new_explicit_trial_directory_required')
        self.trial = Trial(plan,directory)
        self.plan,self.bundle,self.index = self.trial.plan,bundle,0
        with self.trial.ledger.transaction() as (db,_,__):
            db.execute('CREATE TABLE object_keys (key TEXT PRIMARY KEY)')
        self.path = Path(directory)/'trial-evidence.json'
        self.evidence = dict(version=1,manifest_sha256=self.trial.manifest,journal_id=plan['journal_id'],
             synthetic_operation=operation(plan),steps=[],object_keys=[],serving_allowed=False,
             completed=False,cleanup_performed=False)
        with self.path.open('x',encoding='utf-8') as stream:
            json.dump(self.evidence,stream,sort_keys=True)

    def save(self):
        # Evidence remains exportable after quota/window exhaustion. This read-only
        # connection cannot reopen, refund or reset the write budget.
        with closing(sqlite3.connect(self.trial.ledger.path.as_uri()+'?mode=ro',uri=True,timeout=1)) as db:
            db.row_factory=sqlite3.Row
            row=db.execute('SELECT * FROM budget WHERE id=1').fetchone()
            if row['manifest'] != self.trial.manifest or row['nonce'] != self.trial.nonce or row['pid'] != self.trial.pid:
                raise ValueError('evidence_owner_mismatch')
            self.evidence['object_keys']=[row[0] for row in db.execute('SELECT key FROM object_keys ORDER BY key')]
            self.evidence['quota']={key:row[key] for key in ('requests','puts','upload','wire','receive')}
            self.evidence['quota']['operations']=db.execute('SELECT count(*) FROM operations').fetchone()[0]
        temporary = self.path.with_suffix('.pending')
        with temporary.open('x',encoding='utf-8') as stream:
            json.dump(self.evidence,stream,sort_keys=True,indent=2)
        temporary.replace(self.path)

    def step(self,step,*,fixture=None):
        if self.index >= len(STEPS) or step != STEPS[self.index]:
            raise ValueError('trial_step_order')
        if step in ('intent','aborted'):
            self.trial.admit(operation(self.plan),step)
        ref=(str(self.trial.ledger.path),self.trial.nonce,self.trial.manifest)
        result=run(execute,(self.plan,self.bundle,step,ref,True,fixture),timeout=5.0)
        state=result.value if isinstance(result.value,p.State) else p.State.UNKNOWN
        confirmed=state is EXPECTED[self.index] and not result.timed_out and result.worker_reaped
        self.evidence['steps'].append(dict(step=step,state=state.value,timed_out=result.timed_out,
                                          worker_reaped=result.worker_reaped))
        self.index = self.index+1 if confirmed else len(STEPS)
        self.evidence['completed']=confirmed and self.index==len(STEPS)
        self.save()  # Failure to retain evidence stops the caller; never repeat an unknown step.
        return confirmed


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package',required=True,type=Path)
    parser.add_argument('--manifest-sha',required=True)
    parser.add_argument('--credentials',type=Path)
    parser.add_argument('--sdk-site',type=Path)
    parser.add_argument('--state-directory',type=Path)
    parser.add_argument('--run',action='store_true')
    parser.add_argument('--ack-synthetic-cloud-writes',action='store_true')
    parser.add_argument('--ack-fresh-empty-journal',action='store_true')
    args=parser.parse_args(argv)
    try:
        plan,output=dry_run(args.package,args.manifest_sha)
        output.update(proposed_scope='fresh_initialize_one_synthetic_intent_then_abort_and_verify',
                      synthetic_only=True,planned_steps=list(STEPS),cloud_writes_allowed=False)
        if not args.run:
            print(json.dumps(output,sort_keys=True))
            return 0
        if not args.ack_synthetic_cloud_writes or not args.ack_fresh_empty_journal or \
                args.credentials is None or args.sdk_site is None or args.state_directory is None:
            raise ValueError()
        site=args.sdk_site.resolve()
        if not (site/'tablestore-6.4.8.dist-info'/'METADATA').is_file():
            raise ValueError()
        bundle=load(args.credentials,plan)
        session=Session(plan,bundle,args.state_directory,ack=True)
        old=list(sys.path)
        try:
            sys.path.insert(0,str(site))
            for step in STEPS:
                print('J4C_TRIAL_STEP_'+step.replace('-','_').upper(),flush=True)
                if not session.step(step):
                    print('J4C_TRIAL_UNCONFIRMED_STOP; local_evidence_retained; no_automatic_retry')
                    return 2
        finally:
            sys.path[:]=old
        print('J4C_SYNTHETIC_TRIAL_PASSED; serving_allowed=false; cleanup_performed=false')
        return 0
    except Exception:
        print('J4C_TRIAL_REFUSED_OR_UNKNOWN; no_automatic_retry')
        return 2


if __name__=='__main__':
    raise SystemExit(main())
