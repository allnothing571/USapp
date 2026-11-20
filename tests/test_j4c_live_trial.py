"""Synthetic credentials, local quota and mocked stages. Never cloud/network."""
import datetime
import io
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tools import j4c_live_trial as t
from tools.j4c_call_deadline import Result
from tools.j4c_credentials import parse
from tools.j4c_preflight import report
from tools.j4c_resource_package import write_package
from test_j4c_initialize import plan
from test_j4c_credentials import raw


def bundle():
    return parse(raw(datetime.datetime.now(datetime.timezone.utc)),plan())


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='usapp-live-trial-synthetic-')
        self.addCleanup(self.temp.cleanup)
        root=Path(self.temp.name)
        write_package(plan(),root/'review')
        self.args=['--package',str(root/'review'),'--manifest-sha',report(plan())['manifest_sha256']]
        self.site=root/'sdk'
        (self.site/'tablestore-6.4.8.dist-info').mkdir(parents=True)
        (self.site/'tablestore-6.4.8.dist-info'/'METADATA').write_text('synthetic')

    def test_dry_run_does_not_read_credentials_or_create_ledger(self):
        with patch.object(t,'load',side_effect=AssertionError()),patch.object(t,'Session',side_effect=AssertionError()),redirect_stdout(io.StringIO()) as output:
            self.assertEqual(t.main(self.args+['--credentials','not-opened','--state-directory','not-created']),0)
        value=json.loads(output.getvalue())
        self.assertFalse(value['cloud_writes_allowed'])
        self.assertFalse(value['serving_allowed'])
        self.assertTrue(value['synthetic_only'])

    def test_run_requires_both_write_and_fresh_ack_before_read(self):
        for flags in (['--run'],['--run','--ack-synthetic-cloud-writes'],['--run','--ack-fresh-empty-journal']):
            with patch.object(t,'load',side_effect=AssertionError()),redirect_stdout(io.StringIO()):
                self.assertEqual(t.main(self.args+flags),2)

    def test_bad_manifest_precedes_credential_read(self):
        args=list(self.args);args[3]='f'*64
        with patch.object(t,'load',side_effect=AssertionError()),redirect_stdout(io.StringIO()):
            self.assertEqual(t.main(args+['--run']),2)

    def test_cli_stops_at_first_unconfirmed_stage(self):
        args=self.args+['--run','--ack-synthetic-cloud-writes','--ack-fresh-empty-journal',
                       '--credentials','synthetic','--sdk-site',str(self.site),'--state-directory','not-created']
        with patch.object(t,'load',return_value=bundle()),patch.object(t,'Session') as session,redirect_stdout(io.StringIO()) as output:
            session.return_value.step.side_effect=[True,False]
            self.assertEqual(t.main(args),2)
            self.assertEqual([v.args[0] for v in session.return_value.step.call_args_list],list(t.STEPS[:2]))
        self.assertNotIn('synthetic-secret',output.getvalue())
        self.assertIn('no_automatic_retry',output.getvalue())


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='usapp-session-synthetic-')
        self.addCleanup(self.temp.cleanup)
        self.directory=Path(self.temp.name)/'new-state'

    def test_requires_fresh_directory_and_ack(self):
        with self.assertRaises(ValueError):
            t.Session(plan(),bundle(),self.directory)
        self.assertFalse(self.directory.exists())
        t.Session(plan(),bundle(),self.directory,True)
        with self.assertRaises(ValueError):
            t.Session(plan(),bundle(),self.directory,True)

    def test_exact_order_success_retains_synthetic_operation_and_quotas(self):
        session=t.Session(plan(),bundle(),self.directory,True)
        with patch.object(t,'run',side_effect=[Result(value=v) for v in t.EXPECTED]) as worker:
            with self.assertRaises(ValueError):
                session.step('initialize')
            for step in t.STEPS:
                self.assertTrue(session.step(step))
        self.assertEqual(worker.call_count,len(t.STEPS))
        self.assertTrue(all(call.kwargs['timeout']==5.0 for call in worker.call_args_list))
        evidence=json.loads(session.path.read_text())
        self.assertTrue(evidence['completed'])
        self.assertEqual(evidence['quota']['operations'],1)
        self.assertFalse(evidence['serving_allowed'])
        self.assertNotIn('access_key',session.path.read_text())

    def test_unknown_timeout_or_unreaped_stops_and_never_advances(self):
        for n,result in enumerate((Result(),Result(value=t.p.State.READY,timed_out=True),
                                   Result(value=t.p.State.READY,worker_reaped=False))):
            session=t.Session(plan(),bundle(),Path(self.temp.name)/str(n),True)
            with patch.object(t,'run',return_value=result) as worker:
                self.assertFalse(session.step(t.STEPS[0]))
                with self.assertRaises(ValueError):
                    session.step(t.STEPS[1])
                self.assertEqual(worker.call_count,1)
            self.assertFalse(json.loads(session.path.read_text())['completed'])

    def test_evidence_can_be_saved_after_budget_window_expires(self):
        session=t.Session(plan(),bundle(),self.directory,True)
        with session.trial.ledger.transaction() as (db,_,__):
            db.execute('UPDATE budget SET end_ns=?',(time.monotonic_ns()-1,))
        session.save()
        self.assertEqual(json.loads(session.path.read_text())['quota']['requests'],0)

    def test_fixed_operation_ids_are_deterministic_and_never_user_input(self):
        self.assertEqual(t.operation(plan()),t.operation(plan()))
        self.assertEqual(t.operation(plan())['action'],'delete')
        self.assertNotEqual(t.operation(plan())['subjects'][0]['target_id'],plan()['journal_id'])

    def test_execute_missing_ack_never_opens_transport(self):
        with patch.object(t,'scoped_factory',side_effect=AssertionError()):
            self.assertIs(t.execute(plan(),bundle(),'intent',None,False),t.p.State.INVALID)

    def test_unexpected_journal_history_is_refused_before_extension(self):
        op=t.operation(plan())
        client=type('Client',(),{'config':type('Config',(),{'journal_id':plan()['journal_id']})()})()
        metadata=t.SmokeMetadata(client,None,'intent',op)
        zero=dict(checkpoint=t.p.zero_head(plan()['journal_id']),keys=[],operations={},cycles=[])
        with patch.object(t.Metadata,'read',return_value=zero):
            self.assertEqual(metadata.read(plan()['journal_id']),zero)
        for changed in (dict(zero,operations={'other':{}}),dict(zero,keys=['unknown']),
                        dict(zero,checkpoint=dict(zero['checkpoint'],event_seq=1))):
            with patch.object(t.Metadata,'read',return_value=changed),self.assertRaises(ValueError):
                metadata.read(plan()['journal_id'])


class ScopeTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='usapp-scoped-trial-')
        self.addCleanup(self.temp.cleanup)
        self.session=t.Session(plan(),bundle(),Path(self.temp.name)/'state',True)
        self.calls=[]
        owner=self
        class Pool:
            def urlopen(self,*args,**kwargs):
                owner.calls.append(args)
                return object()
        self.factory,self.close=t.scoped_factory(plan(),self.session.trial.ledger,lambda **_:Pool())
        self.pool=self.factory()

    def test_other_hosts_methods_and_apis_never_network_or_spend(self):
        for method,url in (('DELETE','https://x/'),('POST','https://example.com/'),
                           ('POST','https://uj4c1234abcd.cn-shanghai.ots.aliyuncs.com/DeleteRow'),
                           ('PUT','https://usapp-j4c-log-1234abcd.oss-cn-shanghai.aliyuncs.com/other')):
            with self.assertRaises(ValueError):
                self.pool.urlopen(method,url,body=b'')
        self.assertFalse(self.calls)
        self.assertEqual(self.session.trial.stats()['requests'],0)

    def test_quota_precharged_and_candidate_key_retained(self):
        key='journals/'+plan()['journal_id']+'/batches/'+'a'*64+'.json'
        self.pool.urlopen('PUT','https://usapp-j4c-log-1234abcd.oss-cn-shanghai.aliyuncs.com/'+key,body=b'synthetic')
        self.session.save()
        evidence=json.loads(self.session.path.read_text())
        self.assertEqual(evidence['object_keys'],[key])
        self.assertEqual(evidence['quota']['puts'],1)
        self.assertEqual(evidence['quota']['upload'],9)

    def test_request_exhaustion_never_sends(self):
        from tools.j4c_trial import MAX_REQUESTS
        with self.session.trial.ledger.transaction() as (db,_,__):
            db.execute('UPDATE budget SET requests=?',(MAX_REQUESTS,))
        with self.assertRaises(Exception):
            self.pool.urlopen('GET','https://usapp-j4c-log-1234abcd.oss-cn-shanghai.aliyuncs.com/?bucketInfo')
        self.assertFalse(self.calls)


@unittest.skipUnless(os.environ.get('USAPP_J4C_OFFLINE_SDK')=='1','isolated SDK site required')
class StageTests(unittest.TestCase):
    def setUp(self):
        for name in ('socket.socket.connect','socket.create_connection','socket.getaddrinfo'):
            patcher=patch(name,side_effect=AssertionError('external_network_forbidden'))
            patcher.start();self.addCleanup(patcher.stop)

    def test_identity_uses_the_selected_role_and_closes(self):
        b=bundle()
        with patch('tools.j4c_identity.Client') as identity:
            identity.return_value.verify.return_value=True
            self.assertIs(t.run_stage(plan(),b,'identity-checkpoint-writer',None),t.p.State.READY)
            self.assertEqual(identity.call_args.args[0],b.keys['checkpoint-writer'])
            identity.return_value.close.assert_called_once()

    def test_checkpoint_validator_uses_writer_not_publisher_credential(self):
        b=bundle()
        with patch.object(t,'ReadClient') as client,patch.object(t,'Objects') as objects,patch.object(t.p,'Publisher') as publisher:
            publisher.return_value.publish.return_value=t.p.State.READY
            self.assertIs(t.run_stage(plan(),b,'intent',None),t.p.State.READY)
            self.assertEqual(objects.call_args_list[0].args[1],b.keys['object-publisher'].access_key_id)
            self.assertEqual(objects.call_args_list[1].args[1],b.keys['checkpoint-writer'].access_key_id)
            self.assertEqual(publisher.call_args.kwargs['max_rounds'],1)

    def test_existing_any_row_blocks_fresh_initialization(self):
        with patch('tools.j4c_initialize.SetupClient.any_partition_row',return_value=(None,None,[object()],None)):
            fresh=object.__new__(t.FreshSetup)
            with self.assertRaises(ValueError):
                fresh.any_partition_row('synthetic-tx')
