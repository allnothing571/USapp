"""C11a real SDK/spawn/owned TLS stages, synthetic keys only.

Prerequisite identity/resource stages are tested separately, not faked as passed here.
"""
import datetime
import json
import os
from pathlib import Path
import tempfile
import time
import unittest

from tools import j4c_reply_loss_trial as r
from tools.j4c_credentials import Bundle,Key
from tools.j4c_resource_package import POLICY_ROLES
from test_worker_https import WorkerServerFixture
from test_j4c_metadata import Client as FakeClient,Objects as FakeObjects
from test_preflight import plan


@unittest.skipUnless(os.environ.get('USAPP_J4C_LOCAL_TLS_DIR'),'explicit synthetic fixture required')
class ReplyLossWorkerTests(WorkerServerFixture,unittest.TestCase):
    def setUp(self):
        super().setUp()
        # Avoid the synthetic server's artificial delayed-ACK/Nagle round trips.
        # Does not alter worker deadlines or any real-provider transport.
        self.server.RequestHandlerClass.disable_nagle_algorithm=True
        fake=FakeClient();objects=FakeObjects(fake)
        pub=r.p.Publisher(plan()['journal_id'],objects,r.Metadata(fake,objects))
        self.assertIs(pub.publish(r.live.operation(plan()),'intent'),r.p.State.READY)
        self.assertIs(pub.publish(r.live.operation(plan()),'aborted'),r.p.State.ABORTED)
        final_case=self._testMethodName.startswith('test_terminal_')
        if final_case:self.assertIs(pub.publish(r.operation(plan()),'intent'),r.p.State.READY)
        self.engine.rows=dict(fake.rows)
        self.engine.rows['bootstrap','current']=r.j1.canonical(r.live.receipt(plan())).decode('ascii')
        self.engine.rows[r.permission.GUARD_TYPE,r.permission.GUARD_ID]=r.permission.guard_payload(plan())
        self.objects.update({'/objects/'+key:data for key,data in objects.data.items()})
        key=Key('offline-access-id','offline-synthetic-secret')
        bundle=Bundle(r.report(plan())['manifest_sha256'],datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(hours=1),
                      {role:key for role in POLICY_ROLES})
        self.temp=tempfile.TemporaryDirectory(prefix='usapp-c11a-worker-');self.addCleanup(self.temp.cleanup)
        self.session=r.Session(plan(),bundle,Path(self.temp.name)/'new',
                               dict(requests=0,puts=0,upload=0,wire=0,receive=0),True)
        self.session.index=r.STEPS.index('loss-intent')  # Fixture only, not claimed prerequisite execution.
        if final_case:
            self.session.trial.admit(r.operation(plan()),'intent')
            with self.session.trial.ledger.transaction() as (db,_,__):db.execute('UPDATE reply_loss_marker SET observed=1')
            self.session.index=r.STEPS.index('abort-intent')

    def call(self,stage):
        began=time.monotonic()
        result=self.session.step(stage,fixture=self.fixture)
        print('C11A_SYNTHETIC_STAGE {} {:.3f}s confirmed={}'.format(stage,time.monotonic()-began,result),flush=True)
        return result

    def test_sdk_reply_discard_then_read_only_recovery(self):
        self.assertTrue(self.call('loss-intent'))
        evidence=json.loads(self.session.path.read_text())
        self.assertTrue(evidence['sdk_success_observed'])
        self.assertEqual(evidence['steps'][-1]['state'],'unknown')
        self.assertTrue(evidence['steps'][-1]['expected_injected_unknown'])
        self.assertEqual(self.requests[-1],'CommitTransaction')
        self.assertEqual(json.loads(self.engine.rows['head','current'])['batch_seq'],3)
        puts=self.session.trial.stats()['puts']
        for step in ('recover-intent','replay-readonly'):
            self.assertTrue(self.call(step),step)
        self.assertEqual(self.session.trial.stats()['puts'],puts)
        _,counts=self.session.details()
        self.assertFalse(any(counts.get(stage,{}).get(api,0) for stage in ('recover-intent','replay-readonly') for api in r.WRITES))
        self.assertEqual(json.loads(self.engine.rows['head','current'])['batch_seq'],3)
        self.assertEqual(len(self.objects),3)
        self.assertEqual(self.session.trial.stats()['puts'],1)

    def test_terminal_abort_and_final_chain_after_confirmed_recovery(self):
        self.assertTrue(self.call('abort-intent'))
        self.assertTrue(self.call('verify-final'))
        self.assertEqual(json.loads(self.engine.rows['head','current'])['batch_seq'],4)
        self.assertEqual(len(self.objects),4)
        self.assertEqual(self.session.trial.stats()['puts'],1)
        self.assertFalse(self.engine.active)

    def test_wire_loss_before_sdk_success_marker_stops_and_never_auto_recovers(self):
        self.mode='lost_commit'
        self.assertFalse(self.call('loss-intent'))
        evidence=json.loads(self.session.path.read_text())
        self.assertFalse(evidence['sdk_success_observed'])
        self.assertFalse(evidence['steps'][-1]['expected_injected_unknown'])
        self.assertEqual(json.loads(self.engine.rows['head','current'])['batch_seq'],3)
        self.assertEqual(self.requests[-1],'CommitTransaction')
        with self.assertRaises(ValueError):self.session.step('recover-intent',fixture=self.fixture)
