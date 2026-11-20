"""Terminal-only direct Metadata.commit; owned TLS and synthetic fixtures."""
import datetime
import json
import os
from pathlib import Path
import tempfile
import unittest

from tools import j4c_terminal_finalize as f
from tools.j4c_credentials import Bundle,Key
from tools.j4c_resource_package import POLICY_ROLES
from test_worker_https import WorkerServerFixture
from test_j4c_metadata import Client as FakeClient,Objects as FakeObjects
from test_preflight import plan


@unittest.skipUnless(os.environ.get('USAPP_J4C_LOCAL_TLS_DIR'),'explicit synthetic fixture required')
class TerminalWorkerTests(WorkerServerFixture,unittest.TestCase):
    def setUp(self):
        super().setUp();self.server.RequestHandlerClass.disable_nagle_algorithm=True
        fake=FakeClient();objects=FakeObjects(fake)
        pub=f.p.Publisher(plan()['journal_id'],objects,f.r.Metadata(fake,objects))
        for op,phase in ((f.r.live.operation(plan()),'intent'),(f.r.live.operation(plan()),'aborted'),(f.r.operation(plan()),'intent')):
            self.assertIn(pub.publish(op,phase),(f.p.State.READY,f.p.State.ABORTED))
        terminal=self._testMethodName.startswith('test_existing_')
        if terminal:self.assertIs(pub.publish(f.r.operation(plan()),'aborted'),f.p.State.ABORTED)
        self.engine.rows=dict(fake.rows)
        self.engine.rows['bootstrap','current']=f.r.j1.canonical(f.r.live.receipt(plan())).decode('ascii')
        self.engine.rows[f.r.permission.GUARD_TYPE,f.r.permission.GUARD_ID]=f.r.permission.guard_payload(plan())
        original=self.pool.reply_hook
        def committed_head_read(api,body,message):
            if api=='GetRow':
                from tablestore.protobuf import table_store_pb2 as pb
                request=pb.GetRowRequest();request.ParseFromString(body)
                if not request.transaction_id:
                    pk,_=self.engine.decode(request.primary_key)
                    kind,row_id=pk[1][1],pk[2][1]
                    value=self.engine.rows.get((kind,row_id))
                    message.row=self.engine.encode(kind,row_id,value) if value is not None else b''
                    return
            original(api,body,message)
        self.pool.reply_hook=committed_head_read
        self.objects.update({'/objects/'+key:data for key,data in objects.data.items()})
        path,data,_=f.candidate(plan());self.objects['/objects/'+path]=data  # Original timed-out run uploaded this.
        key=Key('offline-access-id','offline-synthetic-secret')
        b=Bundle(f.r.report(plan())['manifest_sha256'],datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(hours=1),
                 {role:key for role in POLICY_ROLES})
        self.temp=tempfile.TemporaryDirectory(prefix='usapp-terminal-worker-');self.addCleanup(self.temp.cleanup)
        self.session=f.Session(plan(),b,Path(self.temp.name)/'new',dict(requests=0,puts=0,upload=0,wire=0,receive=0),True)

    def test_direct_commit_reuses_object_then_verifies_final(self):
        self.session.queue=['abort-intent','verify-final']
        self.assertTrue(self.session.step('abort-intent',fixture=self.fixture))
        self.assertTrue(self.session.step('verify-final',fixture=self.fixture))
        self.assertEqual(json.loads(self.engine.rows['head','current'])['batch_seq'],4)
        self.assertEqual(self.session.trial.stats()['puts'],0)
        self.assertNotIn('OBJECT_PUT',self.requests)
        self.assertEqual(len(self.objects),4)

    def test_existing_terminal_queues_no_commit_and_chain_reads_succeed(self):
        self.session.position=5
        self.assertTrue(self.session.step('head',fixture=self.fixture))
        self.assertNotIn('abort-intent',self.session.queue)
        self.assertTrue(self.session.step('chain-aborted',fixture=self.fixture))
        self.assertNotIn('CommitTransaction',self.requests)
        self.assertNotIn('PutRow',self.requests)
        self.assertNotIn('OBJECT_PUT',self.requests)

    def test_lost_direct_commit_reply_stops_after_possibly_applied_commit(self):
        self.mode='lost_commit';self.session.queue=['abort-intent','verify-final']
        self.assertFalse(self.session.step('abort-intent',fixture=self.fixture))
        self.assertEqual(self.requests[-1],'CommitTransaction')
        self.assertEqual(json.loads(self.engine.rows['head','current'])['batch_seq'],4)
        with self.assertRaises(ValueError):self.session.step('verify-final',fixture=self.fixture)
        self.assertFalse(json.loads(self.session.path.read_text())['terminal_fact_confirmed'])
