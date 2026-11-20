"""Opt-in negative permission probes on synthetic resources only. Dry-run default.

Creates one guarded synthetic row; denied probes must never touch the head.
No deletion, initialization, replay, retry, privilege changes or service wiring.
"""
import argparse
import datetime
import json
from pathlib import Path
import re
import sys

from . import deletion_journal as j1
from . import j4c_live_trial as live
from . import deletion_journal_publication as p
from .j4c_credentials import load
from .j4c_identity_preflight import dry_run
from .j4c_call_deadline import run
from .j4c_preflight import report, validate
from .j4c_tablestore_sdk import Client,Config
from .j4c_trial import Ledger
from .j4c_oss_objects import Objects,Config as ObjectConfig


STEPS=live.STEPS[:5]+('verify-before','bootstrap-before','guard-create','guard-read','publisher-table-denied',
                     'reader-table-denied','reader-object-denied','verify-after','bootstrap-after','guard-read-after')
GUARD_TYPE,GUARD_ID='permission_probe','write-guard-v1'


def guard_payload(plan):
    return j1.canonical(dict(version=1,synthetic_only=True,manifest_sha256=report(plan)['manifest_sha256'])).decode('ascii')


def prior_evidence(path,plan):
    with Path(path).open('rb') as stream:
        raw=stream.read(65537)
    if len(raw)>65536:
        raise ValueError('prior_evidence_limit')
    value=j1._decode(raw)
    prefix='journals/'+plan['journal_id']+'/batches/'
    if not isinstance(value,dict) or value.get('completed') is not True or value.get('serving_allowed') is not False or \
            value.get('manifest_sha256')!=report(plan)['manifest_sha256'] or value.get('journal_id')!=plan['journal_id'] or \
            value.get('synthetic_operation')!=live.operation(plan) or not isinstance(value.get('object_keys'),list) or \
            len(value['object_keys'])!=2 or len(set(value['object_keys']))!=2 or \
            any(not isinstance(key,str) or not key.startswith(prefix) or
                re.fullmatch(r'[0-9a-f]{64}\.json',key[len(prefix):]) is None for key in value['object_keys']):
        raise ValueError('prior_evidence_refused')
    return sorted(value['object_keys'])


def table_denial(status,code,message):
    # OTSAuthFailed ALSO covers bad keys, signatures, time and network ACL.
    if status!=403:
        return False
    return code=='OTSNoPermissionAccess' or (code=='OTSAuthFailed' and message in {
        'The user has no privilege to access the instance.',
        'The user does not have permission to access the instance.'})


def readonly_failure(path,plan):
    """Validate this specific old pre-write failure; never a write-resume receipt."""
    with Path(path).open('rb') as stream:
        raw=stream.read(65537)
    if len(raw)>65536:raise ValueError('failure_evidence_limit')
    value=j1._decode(raw)
    expected=[dict(step=step,state='ready',timed_out=False,worker_reaped=True) for step in live.STEPS[:5]]
    expected.append(dict(step='verify-before',state='unknown',timed_out=True,worker_reaped=True))
    quota=value.get('quota',{})
    if value.get('completed') is not False or value.get('test_kind')!='negative_permissions' or \
            value.get('manifest_sha256')!=report(plan)['manifest_sha256'] or value.get('journal_id')!=plan['journal_id'] or \
            value.get('steps')!=expected or value.get('object_keys')!=[] or \
            any(quota.get(key)!=0 for key in ('operations','puts','upload')) or \
            any(type(quota.get(key)) is not int or not 0<=quota[key]<=limit for key,limit in
                (('requests',5000),('wire',8*1024**2),('receive',64*1024**2))):
        raise ValueError('not_a_proven_pre_write_failure')
    return {key:quota[key] for key in ('requests','wire','receive')}


def guard_failure(path,plan):
    """Write may have applied: this receipt only permits explicit read-first resolution."""
    with Path(path).open('rb') as stream:raw=stream.read(65537)
    if len(raw)>65536:raise ValueError('guard_failure_limit')
    value=j1._decode(raw)
    steps=value.get('steps',[])
    prefix=[dict(step=step,state='ready',timed_out=False,worker_reaped=True) for step in STEPS[:7]]
    quota=value.get('quota',{})
    if value.get('completed') is not False or value.get('test_kind')!='negative_permissions' or \
            value.get('manifest_sha256')!=report(plan)['manifest_sha256'] or value.get('journal_id')!=plan['journal_id'] or \
            len(steps)!=8 or steps[:7]!=prefix or steps[-1].get('step')!='guard-create' or \
            steps[-1].get('state') not in ('conflict','unknown') or steps[-1].get('worker_reaped') is not True or \
            steps[-1].get('timed_out') is not False or value.get('object_keys')!=[] or \
            any(quota.get(key)!=0 for key in ('operations','puts','upload')) or \
            any(type(quota.get(key)) is not int or not 0<=quota[key]<=limit for key,limit in
                (('requests',5000),('wire',8*1024**2),('receive',64*1024**2))):
        raise ValueError('guard_failure_scope_refused')
    return {key:quota[key] for key in ('requests','wire','receive')}


class GuardTables(Client):
    def guard_key(self):
        return [('journal_id',self.config.journal_id),('row_type',GUARD_TYPE),('row_id',GUARD_ID)]

    def write_guard(self,plan):
        row=self._types.Row(self.guard_key(),[('payload',guard_payload(plan))])
        condition=self._types.Condition(self._types.RowExistenceExpectation.EXPECT_NOT_EXIST)
        try:
            self._sdk.put_row(self.config.table,row,condition)
            return p.State.COMMITTED  # Unexpected for a negative; never accept as denied.
        except self._types.OTSServiceError as failure:
            if table_denial(failure.get_http_status(),failure.get_error_code(),failure.get_error_message()):
                print('J4C_PERMISSION_DIAG_TABLE_PERMISSION_DENIED',flush=True)
                return p.State.ABORTED
            label={'OTSConditionCheckFail':'CONDITION_FAILED','OTSParameterInvalid':'PARAMETER_INVALID',
                   'OTSAuthFailed':'AUTH_UNCONFIRMED','OTSRowOperationConflict':'ROW_CONFLICT',
                   'OTSTransactionConflict':'TRANSACTION_CONFLICT','OTSTimeout':'TIMEOUT',
                   'OTSQuotaExhausted':'QUOTA_EXHAUSTED'}.get(failure.get_error_code(),'SERVICE_UNCONFIRMED')
            print('J4C_PERMISSION_DIAG_TABLE_'+label,flush=True)
            return p.State.UNKNOWN
        except Exception:
            print('J4C_PERMISSION_DIAG_TABLE_LOCAL_OR_TRANSPORT_UNCONFIRMED',flush=True)
            return p.State.UNKNOWN

    def read_guard(self,plan):
        return self.inspect_guard(plan) is p.State.READY

    def inspect_guard(self,plan):
        _,row,token=self._sdk.get_row(self.config.table,self.guard_key(),max_version=1)
        if token:return p.State.UNKNOWN
        if row is None:return p.State.ABORTED  # Absent at this read, not proof of original outcome.
        return p.State.READY if live.Metadata(self,None)._row(row,GUARD_TYPE,GUARD_ID)[1]==j1._decode(guard_payload(plan).encode('ascii')) else p.State.CONFLICT


def run_stage(plan,bundle,step,keys,factory,resolve_guard=False):
    if step in live.STEPS[:5]:
        return live.run_stage(plan,bundle,step,factory)
    if step in ('verify-before','verify-after'):
        # Full independent chain plus exact candidate-key evidence, not local assertions alone.
        # Do not run live.verify and then load the same full chain again: one
        # snapshot validates both its contents and its expected object keys.
        reader=bundle.keys['recovery-reader']
        client=live.ReadClient(table_config(plan),reader.access_key_id,reader.access_key_secret,pool_factory=factory)
        client._sdk.connection.ALLOWED={'StartLocalTransaction','GetRow','GetRange','AbortTransaction'}
        objects=Objects(ObjectConfig(plan['bucket'],plan['journal_id']),reader.access_key_id,reader.access_key_secret,pool_factory=factory)
        snapshot,frames,__,___=p.Publisher(plan['journal_id'],objects,
                           live.SmokeMetadata(client,objects,'verify',live.operation(plan)))._load()
        if sorted(snapshot['keys'])!=keys or len(frames)!=2 or [frame['phase'] for frame in frames]!=['intent','aborted'] or \
                any(p.operation_of(frame)!=live.operation(plan) for frame in frames):
            return p.State.CONFLICT
        return p.State.READY
    if step in ('bootstrap-before','bootstrap-after'):
        reader=bundle.keys['recovery-reader']
        client=live.ReadClient(table_config(plan),reader.access_key_id,reader.access_key_secret,pool_factory=factory,max_reply=65536)
        client._sdk.connection.ALLOWED={'StartLocalTransaction','GetRow','AbortTransaction'}
        tx=client.start()
        try:
            _,row,token=client.get(tx,'bootstrap','current')
            if token or row is None or live.Metadata(client,None)._row(row,'bootstrap','current')[1]!=live.receipt(plan):
                return p.State.CONFLICT
        finally:
            client.finish(tx)
        return p.State.READY
    role='checkpoint-writer' if step=='guard-create' else 'object-publisher' if step=='publisher-table-denied' else 'recovery-reader'
    key=bundle.keys[role]
    if step=='guard-create' and resolve_guard is True:
        reader=bundle.keys['recovery-reader']
        inspection=GuardTables(table_config(plan),reader.access_key_id,reader.access_key_secret,pool_factory=factory,max_reply=65536)
        inspection._sdk.connection.ALLOWED={'GetRow'}
        current=inspection.inspect_guard(plan)
        if current is p.State.READY:
            print('J4C_PERMISSION_DIAG_GUARD_MATCHES_NO_REWRITE; original_creation_outcome_unproven',flush=True)
            return p.State.READY
        if current is not p.State.ABORTED:
            return current
        print('J4C_PERMISSION_DIAG_GUARD_ABSENT_SINGLE_CONDITIONAL_WRITE',flush=True)
    if step=='reader-object-denied':
        objects=Objects(ObjectConfig(plan['bucket'],plan['journal_id']),key.access_key_id,key.access_key_secret,pool_factory=factory)
        data=objects.get(keys[0])  # Existing, validated complete batch; never arbitrary bytes.
        objects._bytes(keys[0],data)
        status,raw=objects._request('PUT',keys[0],data)
        from .j4c_resource_preflight import xml,field
        if status==403 and field(xml(raw,'Error'),'Code')=='AccessDenied':
            return p.State.READY
        return p.State.CONFLICT if status in (200,201,204,409) else p.State.UNKNOWN
    client=GuardTables(table_config(plan),key.access_key_id,key.access_key_secret,pool_factory=factory,max_reply=65536)
    client._sdk.connection.ALLOWED={'GetRow'} if step in ('guard-read','guard-read-after') else {'PutRow'}
    if step in ('guard-read','guard-read-after'):
        return p.State.READY if client.read_guard(plan) else p.State.UNKNOWN
    state=client.write_guard(plan)
    if step=='guard-create':
        return p.State.READY if state is p.State.COMMITTED else p.State.CONFLICT if state is p.State.ABORTED else p.State.UNKNOWN
    return p.State.READY if state is p.State.ABORTED else p.State.CONFLICT if state is p.State.COMMITTED else p.State.UNKNOWN


def table_config(plan):
    return Config(plan['region'],plan['instance'],plan['table'],plan['journal_id'],
                  'https://{}.cn-shanghai.ots.aliyuncs.com'.format(plan['instance']))


def execute(plan,bundle,step,keys,trial_ref,ack=False,resolve_guard=False):
    close=lambda:None
    try:
        from urllib3 import PoolManager
        plan=validate(plan)
        if ack is not True or step not in STEPS or bundle.manifest_sha256!=report(plan)['manifest_sha256'] or \
                datetime.datetime.now(datetime.timezone.utc)>=bundle.expires_at:
            return p.State.INVALID
        factory,close=live.scoped_factory(plan,Ledger(*trial_ref),PoolManager)
        return run_stage(plan,bundle,step,keys,factory,resolve_guard)
    except Exception:
        return p.State.UNKNOWN
    finally:
        close()


class Session(live.Session):
    def __init__(self,plan,bundle,directory,keys,ack=False,resolve_guard=False):
        super().__init__(plan,bundle,directory,ack)
        self.keys,self.resolve_guard=keys,resolve_guard
        self.evidence.update(test_kind='negative_permissions',completed=False,
               prior_object_keys=keys,guard_resolution_mode=resolve_guard,
               guard_creation_outcome_not_proven_by_matching_row=resolve_guard,
               guard_row_key=[['journal_id',plan['journal_id']],['row_type',GUARD_TYPE],['row_id',GUARD_ID]])
        self.save()

    def step(self,step):
        if self.index>=len(STEPS) or step!=STEPS[self.index]:
            raise ValueError('permission_step_order')
        ref=(str(self.trial.ledger.path),self.trial.nonce,self.trial.manifest)
        result=run(execute,(self.plan,self.bundle,step,self.keys,ref,True,self.resolve_guard),timeout=5.0)
        state=result.value if isinstance(result.value,p.State) else p.State.UNKNOWN
        confirmed=state is p.State.READY and not result.timed_out and result.worker_reaped
        self.evidence['steps'].append(dict(step=step,state=state.value,timed_out=result.timed_out,worker_reaped=result.worker_reaped))
        self.index=self.index+1 if confirmed else len(STEPS)
        self.evidence['completed']=confirmed and self.index==len(STEPS)
        self.save()
        return confirmed


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package',required=True,type=Path)
    parser.add_argument('--manifest-sha',required=True)
    parser.add_argument('--credentials',type=Path)
    parser.add_argument('--sdk-site',type=Path)
    parser.add_argument('--state-directory',type=Path)
    parser.add_argument('--prior-evidence',type=Path)
    parser.add_argument('--previous-readonly-failure',type=Path,
                        help='optional exact old pre-write timeout evidence; carries consumed counters, never resumes')
    parser.add_argument('--previous-guard-failure',type=Path)
    parser.add_argument('--ack-read-first-guard-resolution',action='store_true')
    parser.add_argument('--run',action='store_true')
    parser.add_argument('--ack-conditional-permission-probes',action='store_true')
    args=parser.parse_args(argv)
    try:
        plan,output=dry_run(args.package,args.manifest_sha)
        output.update(proposed_scope='one_synthetic_guard_row_then_three_conditional_negative_probes',
                      planned_steps=list(STEPS),cloud_writes_allowed=False)
        if not args.run:
            print(json.dumps(output,sort_keys=True));return 0
        if not args.ack_conditional_permission_probes or any(v is None for v in
             (args.credentials,args.sdk_site,args.state_directory,args.prior_evidence)):
            raise ValueError()
        if (args.previous_guard_failure is not None)!=args.ack_read_first_guard_resolution or \
                (args.previous_guard_failure is not None and args.previous_readonly_failure is not None):
            raise ValueError()
        site=args.sdk_site.resolve()
        if not (site/'tablestore-6.4.8.dist-info'/'METADATA').is_file():
            raise ValueError()
        keys=prior_evidence(args.prior_evidence,plan)
        previous=readonly_failure(args.previous_readonly_failure,plan) if args.previous_readonly_failure else None
        if args.previous_guard_failure is not None:previous=guard_failure(args.previous_guard_failure,plan)
        bundle=load(args.credentials,plan)
        session=Session(plan,bundle,args.state_directory,keys,True,args.ack_read_first_guard_resolution)
        if previous is not None:
            with session.trial.ledger.transaction() as (db,_,__):
                db.execute('UPDATE budget SET requests=?,wire=?,receive=? WHERE id=1',
                           (previous['requests'],previous['wire'],previous['receive']))
            session.evidence['previous_guard_failure_consumption' if args.previous_guard_failure is not None else 'previous_readonly_consumption']=previous
            session.save()
        old=list(sys.path)
        try:
            sys.path.insert(0,str(site))
            for step in STEPS:
                print('J4C_PERMISSION_STEP_'+step.replace('-','_').upper(),flush=True)
                if not session.step(step):
                    print('J4C_PERMISSION_UNCONFIRMED_STOP; no_automatic_retry');return 2
        finally:
            sys.path[:]=old
        print('J4C_PERMISSION_PROBES_PASSED; three_tested_actions_only; serving_allowed=false')
        return 0
    except Exception:
        print('J4C_PERMISSION_REFUSED_OR_UNKNOWN; no_automatic_retry');return 2


if __name__=='__main__':
    raise SystemExit(main())
