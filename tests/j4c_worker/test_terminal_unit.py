"""C11a terminal-only reconciliation gates. Synthetic files and mocks only."""
import datetime
import io
import json
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tools import j4c_terminal_finalize as f
from tools.j4c_credentials import parse
from tools.j4c_call_deadline import Result
from tools.j4c_resource_package import write_package
from test_j4c_initialize import plan
from test_j4c_credentials import raw


def bundle():return parse(raw(datetime.datetime.now(datetime.timezone.utc)),plan())
ZERO=dict(requests=0,puts=0,upload=0,wire=0,receive=0)


def source():
    stages=[]
    for step in f.r.STEPS[:f.r.STEPS.index('abort-intent')]:
        loss=step=='loss-intent'
        stages.append(dict(step=step,state='unknown' if loss else 'ready',expected_injected_unknown=loss,timed_out=False,worker_reaped=True))
    stages.append(dict(step='abort-intent',state='unknown',expected_injected_unknown=False,timed_out=True,worker_reaped=True))
    return dict(completed=False,cleanup_performed=False,serving_allowed=False,test_kind='c11a_post_sdk_reply_discard',
                manifest_sha256=f.r.report(plan())['manifest_sha256'],journal_id=plan()['journal_id'],
                scenario=f.r.scenario(plan()),synthetic_operation=f.r.operation(plan()),sdk_success_observed=True,
                steps=stages,object_keys=[f.candidate(plan())[0]],api_counts={},
                quota=dict(operations=2,requests=278,puts=5,upload=5731,wire=57944,receive=1169315))


class GateTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='usapp-terminal-synthetic-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)

    def test_exact_candidate_and_different_scenario_digest(self):
        key,data,head=f.candidate(plan())
        self.assertEqual(head,f.r.expected(plan(),'aborted')['checkpoint'])
        self.assertEqual(key,'journals/'+plan()['journal_id']+'/batches/'+head['batch_head']+'.json')
        self.assertEqual(json.loads(data)['phase'],'aborted')
        self.assertFalse(f.scenario(plan())['object_upload'])
        self.assertNotEqual(f.scenario(plan())['scenario_sha256'],f.r.scenario(plan())['scenario_sha256'])

    def test_failed_source_matches_only_reaped_exact_abort_timeout(self):
        path=self.root/'source.json';value=source();path.write_text(json.dumps(value))
        self.assertEqual(f.failed_evidence(path,plan()),dict(requests=278,puts=5,upload=5731,wire=57944,receive=1169315))
        for change in ('not-reaped','not-marker','wrong-stage','recovery-write','no-candidate'):
            bad=json.loads(json.dumps(value))
            if change=='not-reaped':bad['steps'][-1]['worker_reaped']=False
            if change=='not-marker':bad['sdk_success_observed']=False
            if change=='wrong-stage':bad['steps'][-1]['step']='loss-intent'
            if change=='recovery-write':bad['api_counts']={'recover-intent':{'PutRow':1}}
            if change=='no-candidate':bad['object_keys']=[]
            path.write_text(json.dumps(bad))
            with self.assertRaises(ValueError):f.failed_evidence(path,plan())

    def test_dry_run_never_reads_private_paths_or_creates_ledger(self):
        write_package(plan(),self.root/'review')
        with patch.object(f,'load',side_effect=AssertionError()),patch.object(f,'failed_evidence',side_effect=AssertionError()), \
                patch.object(f,'Session',side_effect=AssertionError()),redirect_stdout(io.StringIO()) as output:
            self.assertEqual(f.main(['--package',str(self.root/'review'),'--manifest-sha',f.r.report(plan())['manifest_sha256'],
                                      '--credentials','not-opened','--failed-evidence','not-opened','--state-directory','not-created']),0)
        value=json.loads(output.getvalue())
        self.assertFalse(value['cloud_writes_allowed'])
        self.assertEqual(value['proposed_scope'],'read_or_single_terminal_metadata_commit_no_object_upload')

    def test_matching_terminal_head_queues_only_reads(self):
        session=f.Session(plan(),bundle(),self.root/'new',ZERO,True)
        session.position=5
        with patch.object(f,'run',return_value=Result(value=f.p.State.ABORTED)):
            self.assertTrue(session.step('head'))
        self.assertNotIn('abort-intent',session.queue)
        self.assertNotIn('candidate',session.queue)
        self.assertIn('chain-aborted',session.queue)

    def test_intent_head_queues_one_commit_and_never_injection(self):
        session=f.Session(plan(),bundle(),self.root/'new',ZERO,True)
        session.position=5
        with patch.object(f,'run',return_value=Result(value=f.p.State.READY)):
            self.assertTrue(session.step('head'))
        self.assertEqual(session.queue.count('abort-intent'),1)
        self.assertIn('candidate',session.queue)
        self.assertNotIn('loss-intent',session.queue)

    def test_unknown_head_or_timeout_stops_without_commit(self):
        for i,result in enumerate((Result(),Result(value=f.p.State.READY,timed_out=True),Result(value=f.p.State.ABORTED,worker_reaped=False))):
            session=f.Session(plan(),bundle(),self.root/str(i),ZERO,True);session.position=5
            with patch.object(f,'run',return_value=result):self.assertFalse(session.step('head'))
            self.assertEqual(session.queue,[])

    def test_commit_timeout_never_retries_or_claims_completed(self):
        session=f.Session(plan(),bundle(),self.root/'new',ZERO,True)
        session.queue=['abort-intent']
        with patch.object(f,'run',return_value=Result(timed_out=True)) as worker:
            self.assertFalse(session.step('abort-intent'))
            with self.assertRaises(ValueError):session.step('abort-intent')
            self.assertEqual(worker.call_args.kwargs['timeout'],5.0)
        self.assertFalse(json.loads(session.path.read_text())['terminal_fact_confirmed'])

    def test_metadata_commit_remains_independent_and_called_once(self):
        with patch.object(f.r.live,'ReadClient'),patch.object(f,'ReadObjects'),patch.object(f.r,'ScenarioMetadata') as metadata:
            metadata.return_value.commit.return_value=f.p.Receipt.CONFIRMED
            self.assertIs(f.run_stage(plan(),bundle(),'abort-intent',None,None),f.p.State.ABORTED)
            key,_,head=f.candidate(plan())
            metadata.return_value.commit.assert_called_once_with(plan()['journal_id'],f.r.expected(plan(),'intent')['checkpoint'],key,head)

    def test_missing_or_wrong_candidate_cannot_upload(self):
        with patch.object(f.r.live,'ReadClient'),patch.object(f,'ReadObjects') as objects:
            objects.return_value.get.return_value=b'wrong'
            self.assertIs(f.run_stage(plan(),bundle(),'candidate',None,None),f.p.State.CONFLICT)
            objects.return_value.put_once.assert_not_called()
        with self.assertRaises(ValueError):object.__new__(f.ReadObjects)._request('PUT','any',b'any')
