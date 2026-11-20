"""C11a only: discard one SDK-confirmed commit reply, then read-only recovery.

Dry-run default; no network faults, concurrency, resume, cleanup or initialization.
Explicit cloud execution needs separate operator permission and scenario digest.
"""
import argparse
from contextlib import closing, nullcontext
import datetime
import hashlib
import json
from pathlib import Path
import sqlite3
import sys
import uuid

from . import deletion_journal as j1
from . import deletion_journal_publication as p
from . import j4c_live_trial as live
from . import j4c_permission_trial as permission
from .j4c_call_deadline import run
from .j4c_credentials import load
from .j4c_identity_preflight import dry_run
from .j4c_preflight import report, validate
from .j4c_trial import Ledger,MAX_REQUESTS,MAX_PUTS,MAX_UPLOAD,MAX_WIRE
from .j4c_tablestore_sdk import Client,ClientError
from .j4c_metadata import Metadata
from .j4c_oss_objects import Objects,Config as ObjectConfig


STEPS=live.STEPS[:5]+('baseline','bootstrap-before','guard-before','loss-intent',
                     'recover-intent','replay-readonly','abort-intent','verify-final','bootstrap-after','guard-after')
WRITES={'PutRow','UpdateRow','CommitTransaction','PutObject'}
SCHEMA={'GetCallerIdentity','GetBucketInfo','GetBucketVersioning','GetObject','DescribeTable','StartLocalTransaction',
        'AbortTransaction','GetRow','GetRange'}|WRITES


def operation(plan):
    ns=uuid.UUID(plan['journal_id'])
    def identifier(label):return str(uuid.uuid5(ns,'j4c-c11a-reply-loss-v1:'+label))
    return dict(operation_id=identifier('operation'),action='delete',
                request_fingerprint=hashlib.sha256(b'C11a synthetic only, no business effect').hexdigest(),
                subjects=[dict(target_kind='media',target_id=identifier('nonexistent-media'),
                               cycle_id=identifier('cycle'),intent_id=identifier('intent'))])


def expected(plan,phase):
    head=p.zero_head(plan['journal_id'])
    builder=p.Publisher(plan['journal_id'],None,None)
    frames=[]
    sequence=[(live.operation(plan),'intent'),(live.operation(plan),'aborted')]
    if phase in ('intent','aborted'):sequence.append((operation(plan),'intent'))
    elif phase!='baseline':raise ValueError('scenario_phase')
    if phase=='aborted':sequence.append((operation(plan),'aborted'))
    for op,part in sequence:
        frame,head=builder._candidate(op,part,head);frames.append(frame)
    operations,cycles=p.derive_indexes(frames)
    return dict(checkpoint=head,keys=[p.object_key(plan['journal_id'],f['hash']) for f in frames],
                operations=operations,cycles=cycles)


def scenario(plan):
    value=dict(version=1,scenario='c11a-sdk-success-reply-discard',manifest_sha256=report(plan)['manifest_sha256'],
               operation=operation(plan),baseline=expected(plan,'baseline')['checkpoint'],
               final=expected(plan,'aborted')['checkpoint'],max_new_batches=2,concurrency=False,
               initialize=False,cleanup=False,serving_allowed=False)
    return dict(value,scenario_sha256=hashlib.sha256(j1.canonical(value)).hexdigest())


class ScenarioMetadata(Metadata):
    def __init__(self,client,objects,plan,phase):
        super().__init__(client,objects)
        self.required=expected(plan,phase)

    def read(self,journal_id):
        value=super().read(journal_id)
        if value!=self.required:raise ValueError('unexpected_scenario_history')
        return value


class DiscardCommitReply(Client):
    def __init__(self,*args,ledger,op_id,**kwargs):
        super().__init__(*args,**kwargs)
        self.ledger,self.op_id=ledger,op_id

    def finish(self,transaction_id,commit=False):
        value=super().finish(transaction_id,commit)
        if commit is True:
            # SDK has returned after parsing/verifying success, not merely sent bytes.
            with self.ledger.transaction() as (db,_,__):
                changed=db.execute('UPDATE reply_loss_marker SET observed=1 WHERE id=1 AND observed=0 AND operation_id=?',
                                   (self.op_id,)).rowcount
                if changed!=1:raise ValueError('reply_loss_marker_unconfirmed')
            raise ClientError('synthetic_reply_discarded_after_sdk_success')
        return value


def api_name(plan,method,url):
    table='https://{}.cn-shanghai.ots.aliyuncs.com/'.format(plan['instance'])
    bucket='https://{}.oss-cn-shanghai.aliyuncs.com/'.format(plan['bucket'])
    if method=='POST' and url=='https://sts.cn-shanghai.aliyuncs.com/':return 'GetCallerIdentity'
    if method=='POST' and isinstance(url,str) and url.startswith(table):
        name=url[len(table):]
        if name in SCHEMA-{'GetCallerIdentity','GetBucketInfo','GetBucketVersioning','GetObject','PutObject'}:return name
    if method=='GET' and url==bucket+'?bucketInfo':return 'GetBucketInfo'
    if method=='GET' and url==bucket+'?versioning':return 'GetBucketVersioning'
    if isinstance(url,str) and url.startswith(bucket+'journals/'+plan['journal_id']+'/batches/'):
        if method=='GET':return 'GetObject'
        if method=='PUT':return 'PutObject'
    raise ValueError('audit_api_scope')


class StageLedger(Ledger):
    def __init__(self,*args,stage):
        super().__init__(*args)
        self.stage,self.pending_api=stage,None

    def request(self,method,body):
        if self.pending_api not in SCHEMA or method not in ('POST','GET','PUT') or \
                (body is not None and not isinstance(body,bytes)):
            raise ValueError('stage_request_scope')
        size=len(body) if body is not None else 0
        put=int(method=='PUT')
        blocked=self.pending_api in WRITES and self.stage not in ('loss-intent','abort-intent')
        # API audit and network precharge commit TOGETHER, before any send.
        # Avoid an extra fsync per call, never remove or defer accounting.
        with self.transaction() as (db,row,_):
            db.execute('INSERT INTO reply_loss_api VALUES (?,?,1) ON CONFLICT(stage,api) DO UPDATE SET requests=requests+1',
                       (self.stage,self.pending_api))
            if not blocked:
                if row['requests']+1>MAX_REQUESTS or row['puts']+put>MAX_PUTS or \
                        row['upload']+size*put>MAX_UPLOAD or row['wire']+size>MAX_WIRE:
                    raise ValueError('stage_budget_exhausted')
                db.execute('UPDATE budget SET requests=requests+1,puts=puts+?,upload=upload+?,wire=wire+? WHERE id=1',
                           (put,size*put,size))
        if blocked:raise ValueError('read_only_stage_write_refused')


def audited_factory(plan,ledger,stage,factory):
    audited=StageLedger(ledger.path,ledger.nonce,ledger.manifest,stage=stage)
    bounded,close=live.scoped_factory(plan,audited,factory)
    def build(**options):
        pool=bounded(**options)
        class Audit:
            def urlopen(self,method,url,**kwargs):
                audited.pending_api=api_name(plan,method,url)
                try:return pool.urlopen(method,url,**kwargs)
                finally:audited.pending_api=None
        return Audit()
    return build,close


def run_stage(plan,bundle,stage,factory,ledger):
    if stage in live.STEPS[:5]:return live.run_stage(plan,bundle,stage,factory)
    if stage in ('bootstrap-before','bootstrap-after'):return permission.run_stage(plan,bundle,stage,[],factory)
    if stage in ('guard-before','guard-after'):return permission.run_stage(plan,bundle,'guard-read',[],factory)
    phase={'baseline':'baseline','loss-intent':'baseline','recover-intent':'intent',
           'replay-readonly':'intent','abort-intent':'intent','verify-final':'aborted'}[stage]
    readonly=stage not in ('loss-intent','abort-intent')
    key=bundle.keys['recovery-reader' if readonly else 'checkpoint-writer']
    config=permission.table_config(plan)
    if stage=='loss-intent':
        client=DiscardCommitReply(config,key.access_key_id,key.access_key_secret,pool_factory=factory,
                                  ledger=ledger,op_id=operation(plan)['operation_id'])
    else:client=Client(config,key.access_key_id,key.access_key_secret,pool_factory=factory)
    if readonly:client._sdk.connection.ALLOWED={'StartLocalTransaction','AbortTransaction','GetRow','GetRange'}
    object_key=bundle.keys['recovery-reader' if readonly else 'object-publisher']
    objects=Objects(ObjectConfig(plan['bucket'],plan['journal_id']),object_key.access_key_id,
                    object_key.access_key_secret,pool_factory=factory)
    verifier=objects if readonly else Objects(ObjectConfig(plan['bucket'],plan['journal_id']),
                    key.access_key_id,key.access_key_secret,pool_factory=factory)
    publisher=p.Publisher(plan['journal_id'],objects,ScenarioMetadata(client,verifier,plan,phase),max_rounds=1,max_batches=4)
    if stage in ('loss-intent','replay-readonly'):return publisher.publish(operation(plan),'intent')
    if stage=='abort-intent':return publisher.publish(operation(plan),'aborted')
    publisher._load()  # Independent full objects, chain, indexes and exact snapshot.
    return p.State.READY


def execute(plan,bundle,stage,ref,ack=False,fixture=None,terminal_finalize=False):
    close=lambda:None;pools=None
    try:
        from .j4c_resource_package import POLICY_ROLES
        plan=validate(plan)
        if type(terminal_finalize) is not bool:return p.State.INVALID
        allowed,runner=STEPS,run_stage
        if terminal_finalize:
            from .j4c_terminal_finalize import STEPS as FINAL_STEPS,run_stage as finalize_stage
            allowed,runner=FINAL_STEPS,finalize_stage
        if ack is not True or stage not in allowed or bundle.manifest_sha256!=report(plan)['manifest_sha256'] or \
                set(bundle.keys)!=set(POLICY_ROLES) or datetime.datetime.now(datetime.timezone.utc)>=bundle.expires_at:
            return p.State.INVALID
        ledger=Ledger(*ref)
        if fixture is None:
            from urllib3 import PoolManager
            factory=PoolManager;context=nullcontext()
        else:
            from .j4c_local_transport import Fixture,Pools,restricted
            if type(fixture) is not Fixture:return p.State.INVALID
            pools=Pools(fixture)
            def factory(**options):
                routes={}
                class Route:
                    def urlopen(self,method,url,**kwargs):
                        if url.startswith('https://sts.cn-shanghai.aliyuncs.com/'):
                            endpoint,route='https://sts.cn-shanghai.aliyuncs.com','identity'
                        elif '.ots.aliyuncs.com/' in url:
                            endpoint,route=permission.table_config(plan).endpoint,'tables'
                        else:endpoint,route=ObjectConfig(plan['bucket'],plan['journal_id']).endpoint,'objects'
                        if endpoint not in routes:routes[endpoint]=pools.factory(endpoint,route)(**options)
                        return routes[endpoint].urlopen(method,url,**kwargs)
                return Route()
            context=restricted(fixture)
        bounded,close=audited_factory(plan,ledger,stage,factory)
        with context:return runner(plan,bundle,stage,bounded,ledger)
    except Exception:return p.State.UNKNOWN
    finally:
        try:close()
        finally:
            if pools:pools.close()


def prerequisites(positive,negative,plan):
    keys=permission.prior_evidence(positive,plan)
    def read(path):
        with Path(path).open('rb') as stream:raw=stream.read(65537)
        if len(raw)>65536:raise ValueError('evidence_limit')
        return j1._decode(raw)
    pos,neg=read(positive),read(negative)
    required=[dict(step=step,state='ready',timed_out=False,worker_reaped=True) for step in permission.STEPS]
    if neg.get('completed') is not True or neg.get('cleanup_performed') is not False or \
            neg.get('serving_allowed') is not False or neg.get('test_kind')!='negative_permissions' or \
            neg.get('manifest_sha256')!=report(plan)['manifest_sha256'] or neg.get('journal_id')!=plan['journal_id'] or \
            neg.get('steps')!=required or sorted(neg.get('prior_object_keys',[]))!=keys or \
            neg.get('guard_row_key')!=[['journal_id',plan['journal_id']],['row_type',permission.GUARD_TYPE],['row_id',permission.GUARD_ID]]:
        raise ValueError('prior_negative_evidence_refused')
    sums={}
    for name,limit in (('requests',5000),('puts',20),('upload',2*1024**2),('wire',8*1024**2),('receive',64*1024**2)):
        values=[item.get('quota',{}).get(name) for item in (pos,neg)]
        if any(type(v) is not int or v<0 for v in values) or sum(values)>=limit:raise ValueError('prior_budget_refused')
        sums[name]=sum(values)
    if keys!=sorted(expected(plan,'baseline')['keys']):raise ValueError('unexpected_baseline_keys')
    return sums


class Session(live.Session):
    def __init__(self,plan,bundle,directory,consumed,ack=False):
        super().__init__(plan,bundle,directory,ack)
        self.trial.admit(live.operation(plan),'intent');self.trial.admit(live.operation(plan),'aborted')
        with self.trial.ledger.transaction() as (db,_,__):
            db.execute('CREATE TABLE reply_loss_marker(id INTEGER PRIMARY KEY, operation_id TEXT, observed INTEGER)')
            db.execute('INSERT INTO reply_loss_marker VALUES (1,?,0)',(operation(plan)['operation_id'],))
            db.execute('CREATE TABLE reply_loss_api(stage TEXT,api TEXT,requests INTEGER,PRIMARY KEY(stage,api))')
            db.execute('UPDATE budget SET requests=?,puts=?,upload=?,wire=?,receive=? WHERE id=1',
                       tuple(consumed[name] for name in ('requests','puts','upload','wire','receive')))
        self.evidence.update(test_kind='c11a_post_sdk_reply_discard',scenario=scenario(plan),
             synthetic_operation=operation(plan),previous_known_consumption=consumed,
             previous_counts_not_account_wide=True,api_counts={},sdk_success_observed=False,
             original_two_batch_scripts_no_longer_applicable_after_write=True)
        self.save()

    def details(self,*,reaped=False):
        # A killed child can leave a hot SQLite rollback journal. Only after
        # it is reaped may this parent open its existing ledger read/write so
        # SQLite can roll back UNCOMMITTED local changes. No reset/refund SQL.
        mode='rw' if reaped else 'ro'
        with closing(sqlite3.connect(self.trial.ledger.path.as_uri()+'?mode='+mode,uri=True,timeout=1)) as db:
            if reaped:db.execute('BEGIN IMMEDIATE')
            owner=db.execute('SELECT manifest,nonce,pid FROM budget WHERE id=1').fetchone()
            if owner!=(self.trial.manifest,self.trial.nonce,self.trial.pid):raise ValueError('ledger_owner_mismatch')
            marker=db.execute('SELECT operation_id,observed FROM reply_loss_marker WHERE id=1').fetchone()
            counts={}
            for stage,api,count in db.execute('SELECT stage,api,requests FROM reply_loss_api ORDER BY stage,api'):
                counts.setdefault(stage,{})[api]=count
            if reaped:db.rollback()
            return marker,counts

    def step(self,stage,*,fixture=None):
        if self.index>=len(STEPS) or stage!=STEPS[self.index]:raise ValueError('scenario_step_order')
        if stage in ('loss-intent','abort-intent'):
            self.trial.admit(operation(self.plan),'intent' if stage=='loss-intent' else 'aborted')
        ref=(str(self.trial.ledger.path),self.trial.nonce,self.trial.manifest)
        result=run(execute,(self.plan,self.bundle,stage,ref,True,fixture),timeout=5.0)
        state=result.value if isinstance(result.value,p.State) else p.State.UNKNOWN
        if result.timed_out:print('J4C_C11A_WORKER_TIMEOUT',flush=True)
        if not result.worker_reaped:print('J4C_C11A_WORKER_NOT_REAPED',flush=True)
        marker,counts=self.details(reaped=result.worker_reaped)
        target=p.State.UNKNOWN if stage=='loss-intent' else p.State.ABORTED if stage=='abort-intent' else p.State.READY
        confirmed=state is target and not result.timed_out and result.worker_reaped
        if stage=='loss-intent':confirmed=confirmed and marker==(operation(self.plan)['operation_id'],1)
        if stage not in ('loss-intent','abort-intent'):
            confirmed=confirmed and not any(counts.get(stage,{}).get(api,0) for api in WRITES)
        self.evidence['steps'].append(dict(step=stage,state=state.value,timed_out=result.timed_out,worker_reaped=result.worker_reaped,
                                         expected_injected_unknown=stage=='loss-intent' and confirmed))
        self.evidence['sdk_success_observed']=marker==(operation(self.plan)['operation_id'],1)
        self.evidence['api_counts']=counts
        self.index=self.index+1 if confirmed else len(STEPS)
        self.evidence['completed']=confirmed and self.index==len(STEPS)
        self.save()
        return confirmed


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('package','credentials','sdk-site','state-directory','prior-positive','prior-permissions'):
        parser.add_argument('--'+name,type=Path,required=name=='package')
    parser.add_argument('--manifest-sha',required=True)
    parser.add_argument('--scenario-sha')
    parser.add_argument('--run',action='store_true')
    parser.add_argument('--ack-synthetic-extension-and-reply-discard',action='store_true')
    args=parser.parse_args(argv)
    try:
        plan,output=dry_run(args.package,args.manifest_sha)
        output.update(proposed_scope='one_c11a_intent_then_abort_with_post_sdk_reply_discard',
                      scenario=scenario(plan),cloud_writes_allowed=False)
        if not args.run:print(json.dumps(output,sort_keys=True));return 0
        if not args.ack_synthetic_extension_and_reply_discard or args.scenario_sha!=scenario(plan)['scenario_sha256'] or \
                any(v is None for v in (args.credentials,args.sdk_site,args.state_directory,args.prior_positive,args.prior_permissions)):
            raise ValueError()
        site=args.sdk_site.resolve()
        if not (site/'tablestore-6.4.8.dist-info'/'METADATA').is_file():raise ValueError()
        consumed=prerequisites(args.prior_positive,args.prior_permissions,plan)
        bundle=load(args.credentials,plan)
        session=Session(plan,bundle,args.state_directory,consumed,True)
        old=list(sys.path)
        try:
            sys.path.insert(0,str(site))
            for stage in STEPS:
                print('J4C_C11A_STEP_'+stage.replace('-','_').upper(),flush=True)
                if not session.step(stage):
                    print('J4C_C11A_UNCONFIRMED_STOP; no_automatic_retry');return 2
        finally:sys.path[:]=old
        print('J4C_C11A_PASSED; post_sdk_reply_discard_only; serving_allowed=false');return 0
    except Exception:
        print('J4C_C11A_REFUSED_OR_UNKNOWN; no_automatic_retry');return 2


if __name__=='__main__':raise SystemExit(main())
