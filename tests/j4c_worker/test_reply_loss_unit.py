"""Separate bounded C11a gates/ledger group. All credentials synthetic."""
import datetime
import io
import json
import os
import sqlite3
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tools import j4c_reply_loss_trial as r
from tools.j4c_credentials import parse
from tools.j4c_call_deadline import Result
from tools.j4c_resource_package import write_package
from test_j4c_initialize import plan
from test_j4c_credentials import raw


def bundle():return parse(raw(datetime.datetime.now(datetime.timezone.utc)),plan())
ZERO=dict(requests=0,puts=0,upload=0,wire=0,receive=0)


def leave_uncommitted_local_changes(path):
    db=sqlite3.connect(path,timeout=1,isolation_level=None)
    db.execute('PRAGMA cache_size=1')
    db.execute('BEGIN IMMEDIATE')
    db.execute('UPDATE budget SET requests=4999')
    for i in range(8):db.execute('INSERT INTO reply_loss_api VALUES (?,?,1)',('uncommitted',str(i)+'x'*4096))
    os._exit(0)  # Test-only abrupt exit: deliberately skip rollback/close.


class ScenarioTests(unittest.TestCase):
    def test_exact_scenario_binding_and_nonbusiness_operation(self):
        self.assertEqual(r.scenario(plan()),r.scenario(plan()))
        self.assertNotEqual(r.operation(plan()),r.live.operation(plan()))
        self.assertEqual([r.expected(plan(),phase)['checkpoint']['batch_seq'] for phase in ('baseline','intent','aborted')],[2,3,4])
        self.assertEqual(r.scenario(plan())['max_new_batches'],2)
        self.assertFalse(r.scenario(plan())['concurrency'])
        self.assertFalse(r.scenario(plan())['initialize'])

    def test_metadata_rejects_unexpected_head_keys_or_operations(self):
        class Client:config=r.permission.table_config(plan())
        metadata=r.ScenarioMetadata(Client(),None,plan(),'baseline')
        expected=r.expected(plan(),'baseline')
        with patch.object(r.Metadata,'read',return_value=expected):
            self.assertEqual(metadata.read(plan()['journal_id']),expected)
        for changed in (dict(expected,keys=[]),dict(expected,operations={}),
                        dict(expected,checkpoint=r.p.zero_head(plan()['journal_id']))):
            with patch.object(r.Metadata,'read',return_value=changed),self.assertRaises(ValueError):
                metadata.read(plan()['journal_id'])


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='usapp-c11a-driver-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);write_package(plan(),self.root/'review')
        self.args=['--package',str(self.root/'review'),'--manifest-sha',r.report(plan())['manifest_sha256']]

    def test_dry_run_never_reads_credentials_evidence_or_creates_state(self):
        with patch.object(r,'load',side_effect=AssertionError()),patch.object(r,'prerequisites',side_effect=AssertionError()), \
                patch.object(r,'Session',side_effect=AssertionError()),redirect_stdout(io.StringIO()) as output:
            self.assertEqual(r.main(self.args+['--credentials','not-opened','--prior-positive','not-opened','--state-directory','not-created']),0)
        value=json.loads(output.getvalue());self.assertFalse(value['cloud_writes_allowed'])
        self.assertEqual(value['scenario']['scenario_sha256'],r.scenario(plan())['scenario_sha256'])

    def test_missing_ack_or_wrong_scenario_refuses_before_reads(self):
        for args in (['--run'],['--run','--ack-synthetic-extension-and-reply-discard','--scenario-sha','f'*64]):
            with patch.object(r,'load',side_effect=AssertionError()),patch.object(r,'prerequisites',side_effect=AssertionError()),redirect_stdout(io.StringIO()):
                self.assertEqual(r.main(self.args+args),2)

    def test_prior_evidence_combines_known_counters_once_but_not_account_wide(self):
        p=self.root/'positive.json';n=self.root/'negative.json'
        keys=sorted(r.expected(plan(),'baseline')['keys'])
        positive=dict(completed=True,serving_allowed=False,manifest_sha256=r.report(plan())['manifest_sha256'],
            journal_id=plan()['journal_id'],synthetic_operation=r.live.operation(plan()),object_keys=keys,
            quota=dict(requests=88,puts=2,upload=2292,wire=20883,receive=26181))
        negative=dict(completed=True,cleanup_performed=False,serving_allowed=False,test_kind='negative_permissions',
            manifest_sha256=r.report(plan())['manifest_sha256'],journal_id=plan()['journal_id'],prior_object_keys=keys,
            guard_row_key=[['journal_id',plan()['journal_id']],['row_type',r.permission.GUARD_TYPE],['row_id',r.permission.GUARD_ID]],
            steps=[dict(step=step,state='ready',timed_out=False,worker_reaped=True) for step in r.permission.STEPS],
            quota=dict(requests=91,puts=1,upload=1147,wire=15066,receive=39775))
        p.write_text(json.dumps(positive));n.write_text(json.dumps(negative))
        self.assertEqual(r.prerequisites(p,n,plan()),dict(requests=179,puts=3,upload=3439,wire=35949,receive=65956))
        negative['steps'][-1]['state']='unknown';n.write_text(json.dumps(negative))
        with self.assertRaises(ValueError):r.prerequisites(p,n,plan())


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='usapp-c11a-session-');self.addCleanup(self.temp.cleanup)
        self.session=r.Session(plan(),bundle(),Path(self.temp.name)/'new',ZERO,True)

    def loss_client(self):
        client=object.__new__(r.DiscardCommitReply)
        client.ledger=self.session.trial.ledger;client.op_id=r.operation(plan())['operation_id']
        return client

    def test_marker_written_only_after_parent_sdk_finish_succeeds(self):
        client=self.loss_client()
        with patch.object(r.Client,'finish',return_value=None) as sdk,self.assertRaises(r.ClientError):
            client.finish('synthetic-tx',True)
        sdk.assert_called_once_with('synthetic-tx',True)
        self.assertEqual(self.session.details()[0],(client.op_id,1))

    def test_reaped_child_hot_journal_rolls_back_only_uncommitted_local_changes(self):
        ledger=self.session.trial.ledger
        ledger.request('GET',None)  # Committed charge must survive crash recovery.
        result=r.run(leave_uncommitted_local_changes,(str(ledger.path),),timeout=5.0)
        self.assertTrue(result.worker_reaped)
        self.assertFalse(result.timed_out)
        marker,counts=self.session.details(reaped=True)
        self.assertEqual(marker[1],0)
        self.assertEqual(counts,{})
        self.assertEqual(self.session.trial.stats()['requests'],1)

    def test_failed_or_read_only_finish_never_marks_success(self):
        client=self.loss_client()
        with patch.object(r.Client,'finish',side_effect=TimeoutError('synthetic')),self.assertRaises(TimeoutError):
            client.finish('synthetic-tx',True)
        self.assertEqual(self.session.details()[0][1],0)
        with patch.object(r.Client,'finish',return_value=None):client.finish('synthetic-read',False)
        self.assertEqual(self.session.details()[0][1],0)

    def test_unknown_without_success_marker_stops(self):
        self.session.index=r.STEPS.index('loss-intent')
        with patch.object(r,'run',return_value=Result(value=r.p.State.UNKNOWN)):
            self.assertFalse(self.session.step('loss-intent'))
        with self.assertRaises(ValueError):self.session.step('recover-intent')

    def test_expected_unknown_requires_marker_no_timeout_and_reaped_child(self):
        with self.session.trial.ledger.transaction() as (db,_,__):db.execute('UPDATE reply_loss_marker SET observed=1')
        self.session.index=r.STEPS.index('loss-intent')
        with patch.object(r,'run',return_value=Result(value=r.p.State.UNKNOWN)) as worker:
            self.assertTrue(self.session.step('loss-intent'))
            self.assertEqual(worker.call_args.kwargs['timeout'],5.0)
        self.assertTrue(json.loads(self.session.path.read_text())['steps'][-1]['expected_injected_unknown'])

    def test_timeout_or_unreaped_stops_even_with_marker(self):
        with self.session.trial.ledger.transaction() as (db,_,__):db.execute('UPDATE reply_loss_marker SET observed=1')
        for result in (Result(value=r.p.State.UNKNOWN,timed_out=True),Result(value=r.p.State.UNKNOWN,worker_reaped=False)):
            self.session.index=r.STEPS.index('loss-intent')
            with patch.object(r,'run',return_value=result):self.assertFalse(self.session.step('loss-intent'))

    def test_read_only_audit_records_but_never_sends_forbidden_mutation(self):
        calls=[]
        class Pool:
            def urlopen(self,*args,**kwargs):calls.append(args);return object()
        factory,close=r.audited_factory(plan(),self.session.trial.ledger,'replay-readonly',lambda **_:Pool())
        try:
            with self.assertRaises(ValueError):
                factory().urlopen('POST',r.permission.table_config(plan()).endpoint+'/PutRow',body=b'synthetic')
        finally:close()
        self.assertFalse(calls)
        self.assertEqual(self.session.details()[1]['replay-readonly']['PutRow'],1)
        self.session.index=r.STEPS.index('replay-readonly')
        with patch.object(r,'run',return_value=Result(value=r.p.State.READY)):
            self.assertFalse(self.session.step('replay-readonly'))

    def test_audit_and_budget_are_both_durable_before_a_lost_request_reply(self):
        owner=self
        class Pool:
            def urlopen(self,*args,**kwargs):
                self_stats=owner.session.trial.stats()
                counts=owner.session.details()[1]
                owner.assertEqual(self_stats['requests'],1)
                owner.assertEqual(counts['recover-intent']['GetRow'],1)
                raise TimeoutError('synthetic-reply-loss')
        factory,close=r.audited_factory(plan(),self.session.trial.ledger,'recover-intent',lambda **_:Pool())
        try:
            with self.assertRaises(TimeoutError):
                factory().urlopen('POST',r.permission.table_config(plan()).endpoint+'/GetRow',body=b'synthetic')
        finally:close()
        self.assertEqual(self.session.trial.stats()['requests'],1)

    def test_successful_loss_without_marker_or_normal_committed_answer_cannot_pass_fault_case(self):
        self.session.index=r.STEPS.index('loss-intent')
        with patch.object(r,'run',return_value=Result(value=r.p.State.READY)):
            self.assertFalse(self.session.step('loss-intent'))
