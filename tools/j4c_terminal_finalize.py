"""C11a timeout reconciliation only. Never upload, initialize or inject a fault.

Default dry-run. After explicit reads, either verify existing terminal facts or
attempt ONE conditional metadata commit reusing the exact uploaded candidate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

from . import j4c_reply_loss_trial as r
from . import deletion_journal_publication as p
from .j4c_credentials import load
from .j4c_identity_preflight import dry_run
from .j4c_call_deadline import run
from .j4c_tablestore_sdk import Client
from .j4c_oss_objects import Objects,Config as ObjectConfig


STEPS=r.live.STEPS[:5]+('head','chain-intent','chain-aborted','candidate','bootstrap-before',
                       'guard-before','abort-intent','verify-final','bootstrap-after','guard-after')


def candidate(plan):
    frame,head=p.Publisher(plan['journal_id'],None,None)._candidate(
        r.operation(plan),'aborted',r.expected(plan,'intent')['checkpoint'])
    if head!=r.expected(plan,'aborted')['checkpoint']:raise ValueError('fixed_candidate_mismatch')
    return p.object_key(plan['journal_id'],frame['hash']),r.j1.canonical(frame)+b'\n',head


def scenario(plan):
    value=dict(version=1,scenario='c11a-terminal-read-or-single-metadata-commit',
               parent_scenario_sha=r.scenario(plan)['scenario_sha256'],
               manifest_sha256=r.report(plan)['manifest_sha256'],
               intent=r.expected(plan,'intent')['checkpoint'],terminal=r.expected(plan,'aborted')['checkpoint'],
               candidate_key=candidate(plan)[0],object_upload=False,initialize=False,cleanup=False,
               fault_injection=False,serving_allowed=False)
    return dict(value,scenario_sha256=hashlib.sha256(r.j1.canonical(value)).hexdigest())


def failed_evidence(path,plan):
    with Path(path).open('rb') as stream:raw=stream.read(65537)
    if len(raw)>65536:raise ValueError('failed_evidence_limit')
    value=r.j1._decode(raw)
    prefix=[]
    for stage in r.STEPS[:r.STEPS.index('abort-intent')]:
        loss=stage=='loss-intent'
        prefix.append(dict(step=stage,state='unknown' if loss else 'ready',expected_injected_unknown=loss,
                           timed_out=False,worker_reaped=True))
    steps=value.get('steps',[])
    if value.get('completed') is not False or value.get('cleanup_performed') is not False or \
            value.get('serving_allowed') is not False or value.get('test_kind')!='c11a_post_sdk_reply_discard' or \
            value.get('manifest_sha256')!=r.report(plan)['manifest_sha256'] or value.get('journal_id')!=plan['journal_id'] or \
            value.get('scenario')!=r.scenario(plan) or value.get('synthetic_operation')!=r.operation(plan) or \
            value.get('sdk_success_observed') is not True or len(steps)!=len(prefix)+1 or steps[:-1]!=prefix or \
            steps[-1]!=dict(step='abort-intent',state='unknown',expected_injected_unknown=False,timed_out=True,worker_reaped=True):
        raise ValueError('failed_c11a_scope_refused')
    if type(value.get('quota',{}).get('operations')) is not int or value['quota']['operations']!=2:
        raise ValueError('unexpected_operation_count')
    if any(value.get('api_counts',{}).get(stage,{}).get(api,0) for stage in ('recover-intent','replay-readonly') for api in r.WRITES):
        raise ValueError('original_recovery_writes_refused')
    output={}
    for key,limit in (('requests',5000),('puts',20),('upload',2*1024**2),('wire',8*1024**2),('receive',64*1024**2)):
        amount=value.get('quota',{}).get(key)
        if type(amount) is not int or not 0<=amount<limit:raise ValueError('old_budget_refused')
        output[key]=amount
    if candidate(plan)[0] not in value.get('object_keys',[]):raise ValueError('candidate_not_declared')
    return output


class ReadObjects(Objects):
    def _request(self,method,key,body=None):
        if method!='GET' or body is not None:raise ValueError('object_writes_forbidden')
        return super()._request(method,key)


def run_stage(plan,bundle,stage,factory,ledger):
    if stage in r.live.STEPS[:5]:return r.live.run_stage(plan,bundle,stage,factory)
    if stage in ('bootstrap-before','bootstrap-after'):return r.permission.run_stage(plan,bundle,stage,[],factory)
    if stage in ('guard-before','guard-after'):return r.permission.run_stage(plan,bundle,'guard-read',[],factory)
    key=bundle.keys['checkpoint-writer' if stage=='abort-intent' else 'recovery-reader']
    client=r.live.ReadClient(r.permission.table_config(plan),key.access_key_id,key.access_key_secret,pool_factory=factory)
    if stage!='abort-intent':client._sdk.connection.ALLOWED={'GetRow','GetRange','StartLocalTransaction','AbortTransaction'}
    objects=ReadObjects(ObjectConfig(plan['bucket'],plan['journal_id']),key.access_key_id,key.access_key_secret,pool_factory=factory)
    if stage=='head':
        _,row,token=client._sdk.get_row(client.config.table,client._key('head','current'),max_version=1)
        if token or row is None:return p.State.UNKNOWN
        value=r.Metadata(client,None)._row(row,'head','current')[1]
        if value==r.expected(plan,'intent')['checkpoint']:return p.State.READY
        if value==r.expected(plan,'aborted')['checkpoint']:return p.State.ABORTED
        return p.State.CONFLICT
    if stage=='candidate':
        path,data,_=candidate(plan)
        return p.State.READY if objects.get(path)==data else p.State.CONFLICT
    phase='intent' if stage in ('chain-intent','abort-intent') else 'aborted'
    metadata=r.ScenarioMetadata(client,objects,plan,phase)
    if stage=='abort-intent':
        # Preserve Metadata.commit's OWN independent full validation and CAS.
        # Remove only Publisher's redundant outer load/upload/readback sequence.
        path,_,head=candidate(plan)
        receipt=metadata.commit(plan['journal_id'],r.expected(plan,'intent')['checkpoint'],path,head)
        return p.State.ABORTED if receipt is p.Receipt.CONFIRMED else p.State.CONFLICT if receipt in (p.Receipt.CONFLICT,p.Receipt.HEAD_CHANGED) else p.State.UNKNOWN
    p.Publisher(plan['journal_id'],objects,metadata,max_rounds=1,max_batches=4)._load()
    return p.State.READY


class Session(r.Session):
    def __init__(self,plan,bundle,directory,consumed,ack=False):
        super().__init__(plan,bundle,directory,consumed,ack)
        self.trial.admit(r.operation(plan),'intent');self.trial.admit(r.operation(plan),'aborted')
        self.queue=list(r.live.STEPS[:5])+['head']
        self.position=0
        self.evidence.update(test_kind='c11a_terminal_finalize',scenario=scenario(plan),steps=[],
            new_fault_injection=False,object_upload_allowed=False,prior_sdk_success_observed=True,
            original_failed_run_unchanged=True,terminal_fact_confirmed=False,branch=None)
        self.evidence.pop('sdk_success_observed',None)
        self.save()

    def step(self,stage,*,fixture=None):
        if self.position>=len(self.queue) or stage!=self.queue[self.position]:raise ValueError('finalize_step_order')
        ref=(str(self.trial.ledger.path),self.trial.nonce,self.trial.manifest)
        result=run(r.execute,(self.plan,self.bundle,stage,ref,True,fixture,True),timeout=5.0)
        state=result.value if isinstance(result.value,p.State) else p.State.UNKNOWN
        _,counts=self.details(reaped=result.worker_reaped)
        confirmed=not result.timed_out and result.worker_reaped
        if stage=='head':confirmed=confirmed and state in (p.State.READY,p.State.ABORTED)
        else:confirmed=confirmed and state is (p.State.ABORTED if stage=='abort-intent' else p.State.READY)
        if counts.get(stage,{}).get('PutObject',0):confirmed=False
        if stage!='abort-intent' and any(counts.get(stage,{}).get(api,0) for api in r.WRITES):confirmed=False
        self.evidence['steps'].append(dict(step=stage,state=state.value,timed_out=result.timed_out,worker_reaped=result.worker_reaped))
        self.evidence['api_counts']=counts
        self.position+=1
        if confirmed and stage=='head':
            already=state is p.State.ABORTED
            self.evidence['branch']='already-terminal-read-only' if already else 'intent-single-conditional-commit'
            self.queue+=['chain-aborted' if already else 'chain-intent','bootstrap-before','guard-before']
            if not already:self.queue+=['candidate','abort-intent']
            self.queue+=['verify-final','bootstrap-after','guard-after']
        self.evidence['completed']=confirmed and self.position==len(self.queue)
        self.evidence['terminal_fact_confirmed']=self.evidence['completed']
        self.save()
        if not confirmed:self.queue=[]
        return confirmed


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('package','credentials','sdk-site','state-directory','failed-evidence'):
        parser.add_argument('--'+name,type=Path,required=name=='package')
    parser.add_argument('--manifest-sha',required=True)
    parser.add_argument('--scenario-sha')
    parser.add_argument('--run',action='store_true')
    parser.add_argument('--ack-read-or-single-terminal-commit',action='store_true')
    args=parser.parse_args(argv)
    try:
        plan,output=dry_run(args.package,args.manifest_sha)
        output.update(scenario=scenario(plan),cloud_writes_allowed=False,
                      proposed_scope='read_or_single_terminal_metadata_commit_no_object_upload')
        if not args.run:print(json.dumps(output,sort_keys=True));return 0
        if not args.ack_read_or_single_terminal_commit or args.scenario_sha!=scenario(plan)['scenario_sha256'] or \
                any(v is None for v in (args.credentials,args.sdk_site,args.state_directory,args.failed_evidence)):
            raise ValueError()
        site=args.sdk_site.resolve()
        if not (site/'tablestore-6.4.8.dist-info'/'METADATA').is_file():raise ValueError()
        consumed=failed_evidence(args.failed_evidence,plan)
        bundle=load(args.credentials,plan)
        session=Session(plan,bundle,args.state_directory,consumed,True)
        old=list(sys.path)
        try:
            sys.path.insert(0,str(site))
            while session.position<len(session.queue):
                stage=session.queue[session.position]
                print('J4C_TERMINAL_STEP_'+stage.replace('-','_').upper(),flush=True)
                if not session.step(stage):print('J4C_TERMINAL_UNCONFIRMED_STOP; no_automatic_retry');return 2
        finally:sys.path[:]=old
        print('J4C_TERMINAL_FACT_CONFIRMED; original_run_unchanged; serving_allowed=false');return 0
    except Exception:print('J4C_TERMINAL_REFUSED_OR_UNKNOWN; no_automatic_retry');return 2


if __name__=='__main__':raise SystemExit(main())
