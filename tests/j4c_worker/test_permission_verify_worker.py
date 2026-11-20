"""One complete chain read per stage, actual SDK and owned TLS only."""
import datetime
import os
from pathlib import Path
import tempfile
import unittest

from tools import j4c_permission_trial as n
from tools.j4c_call_deadline import run
from tools.j4c_credentials import Bundle,Key
from tools.j4c_resource_package import POLICY_ROLES
from test_worker_https import WorkerServerFixture
from test_j4c_metadata import Client as FakeClient,Objects as FakeObjects
from test_permission_worker import synthetic_stage
from test_preflight import plan


@unittest.skipUnless(os.environ.get('USAPP_J4C_LOCAL_TLS_DIR'),'explicit synthetic fixture required')
class PermissionVerifyWorkerTests(WorkerServerFixture,unittest.TestCase):
    def test_split_chain_and_bootstrap_do_not_duplicate_full_chain(self):
        fake=FakeClient();objects=FakeObjects(fake)
        pub=n.p.Publisher(plan()['journal_id'],objects,n.live.Metadata(fake,objects))
        self.assertIs(pub.publish(n.live.operation(plan()),'intent'),n.p.State.READY)
        self.assertIs(pub.publish(n.live.operation(plan()),'aborted'),n.p.State.ABORTED)
        self.engine.rows=dict(fake.rows)
        self.engine.rows['bootstrap','current']=n.j1.canonical(n.live.receipt(plan())).decode('ascii')
        self.objects.update({'/objects/'+key:data for key,data in objects.data.items()})
        original=dict(self.engine.rows)
        key=Key('offline-access-id','offline-synthetic-secret')
        bundle=Bundle(n.report(plan())['manifest_sha256'],datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(hours=1),
                      {role:key for role in POLICY_ROLES})
        with tempfile.TemporaryDirectory(prefix='usapp-split-verify-') as directory:
            session=n.Session(plan(),bundle,Path(directory)/'new',sorted(objects.data),True)
            ledger=session.trial.ledger
            ref=(str(ledger.path),session.trial.nonce,session.trial.manifest)
            for step in ('verify-before','bootstrap-before'):
                result=run(synthetic_stage,(plan(),bundle,step,sorted(objects.data),ref,self.fixture),timeout=5.0)
                self.assertIs(result.value,n.p.State.READY,step)
                self.assertFalse(result.timed_out)
                self.assertTrue(result.worker_reaped)
            self.assertEqual(self.requests.count('OBJECT_GET'),2)
            self.assertEqual(self.requests.count('StartLocalTransaction'),2)
            self.assertEqual(self.requests.count('AbortTransaction'),2)
            self.assertNotIn('CommitTransaction',self.requests)
            self.assertNotIn('PutRow',self.requests)
            self.assertEqual(self.engine.rows,original)
