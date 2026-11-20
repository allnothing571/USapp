"""New live-stage implementation, synthetic keys and owned loopback ONLY.

These cases start at the write stages; identity/bucket gates are separately tested.
They do not prove real provider permissions or fresh-lineage authority.
"""
import datetime
import json
import os
from pathlib import Path
import tempfile
import unittest

from tools import j4c_live_trial as t
from tools.j4c_credentials import Bundle,Key
from tools.j4c_preflight import report
from tools.j4c_resource_package import POLICY_ROLES
from test_initialize_worker import InitializationFixture
from test_preflight import plan


@unittest.skipUnless(os.environ.get('USAPP_J4C_LOCAL_TLS_DIR'),'explicit synthetic TLS fixture required')
class LiveStageTests(InitializationFixture,unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.directory=tempfile.TemporaryDirectory(prefix='usapp-live-stage-synthetic-')
        self.addCleanup(self.directory.cleanup)
        key=Key('offline-access-id','offline-synthetic-secret')
        bundle=Bundle(report(plan())['manifest_sha256'],datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(hours=1),
                      {role:key for role in POLICY_ROLES})
        self.session=t.Session(plan(),bundle,Path(self.directory.name)/'new-state',True)
        self.session.index=5  # Test fixture only: do not pretend to execute identity/resource gates.

    def test_new_write_stages_initialize_intent_abort_and_reader_verify(self):
        for step in ('initialize','intent','aborted','verify'):
            self.assertTrue(self.session.step(step,fixture=self.fixture),step)
        self.assertEqual(len(self.objects),2)
        evidence=json.loads(self.session.path.read_text())
        self.assertTrue(evidence['completed'])
        self.assertEqual(len(evidence['object_keys']),2)
        self.assertEqual(evidence['quota']['operations'],1)
        self.assertEqual(evidence['quota']['puts'],2)
        self.assertEqual(evidence['quota']['requests'],len(self.requests))
        before=dict(self.engine.rows)
        # Fixed final verification did not add a third batch or mutate its state.
        self.assertEqual(json.loads(before['head','current'])['batch_seq'],2)
        self.assertFalse(self.engine.active)

    def test_lost_initialization_commit_stops_without_second_attempt(self):
        self.mode='lost_commit'
        self.assertFalse(self.session.step('initialize',fixture=self.fixture))
        self.assertIn(('head','current'),self.engine.rows)
        self.assertIn(('bootstrap','current'),self.engine.rows)
        self.assertEqual(self.requests[-1],'CommitTransaction')
        with self.assertRaises(ValueError):
            self.session.step('intent',fixture=self.fixture)
        self.assertFalse(self.objects)
        self.assertFalse(json.loads(self.session.path.read_text())['completed'])
