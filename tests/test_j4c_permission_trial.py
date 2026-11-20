"""Negative-result classification and conditional wire requests, synthetic only."""
import datetime
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tools import j4c_permission_trial as n
from tools.j4c_credentials import parse
from tools.j4c_call_deadline import Result
from tools.j4c_resource_package import write_package
from tools.j4c_preflight import report
from test_j4c_initialize import plan
from test_j4c_credentials import raw


def keys():
    return ['journals/'+plan()['journal_id']+'/batches/'+c*64+'.json' for c in ('a','b')]


def bundle():
    return parse(raw(datetime.datetime.now(datetime.timezone.utc)),plan())


class ClassificationTests(unittest.TestCase):
    def test_guard_failure_is_not_accepted_as_readonly_failure(self):
        value=dict(completed=False,test_kind='negative_permissions',manifest_sha256=report(plan())['manifest_sha256'],
                   journal_id=plan()['journal_id'],object_keys=[],quota=dict(operations=0,puts=0,upload=0,requests=51,wire=7554,receive=18708),
                   steps=[dict(step=step,state='ready',timed_out=False,worker_reaped=True) for step in n.STEPS[:7]])
        value['steps'].append(dict(step='guard-create',state='conflict',timed_out=False,worker_reaped=True))
        with tempfile.TemporaryDirectory(prefix='usapp-guard-failure-') as directory:
            path=Path(directory)/'guard.json';path.write_text(json.dumps(value))
            self.assertEqual(n.guard_failure(path,plan()),dict(requests=51,wire=7554,receive=18708))
            with self.assertRaises(ValueError):n.readonly_failure(path,plan())
            for name in ('unreaped','later','object-write'):
                changed=json.loads(json.dumps(value))
                if name=='unreaped':changed['steps'][-1]['worker_reaped']=False
                if name=='later':changed['steps'][-1]['step']='reader-table-denied'
                if name=='object-write':changed['quota']['puts']=1
                path.write_text(json.dumps(changed))
                with self.assertRaises(ValueError):n.guard_failure(path,plan())

    def test_only_exact_pre_write_timeout_can_carry_forward_counters(self):
        value=dict(completed=False,test_kind='negative_permissions',manifest_sha256=report(plan())['manifest_sha256'],
                   journal_id=plan()['journal_id'],object_keys=[],quota=dict(operations=0,puts=0,upload=0,requests=30,wire=4671,receive=10775),
                   steps=[dict(step=step,state='ready',timed_out=False,worker_reaped=True) for step in n.live.STEPS[:5]])
        value['steps'].append(dict(step='verify-before',state='unknown',timed_out=True,worker_reaped=True))
        with tempfile.TemporaryDirectory(prefix='usapp-pre-write-evidence-') as directory:
            path=Path(directory)/'failure.json';path.write_text(json.dumps(value))
            self.assertEqual(n.readonly_failure(path,plan()),dict(requests=30,wire=4671,receive=10775))
            for change in ('write','unreaped','later'):
                changed=json.loads(json.dumps(value))
                if change=='write':changed['quota']['puts']=1
                if change=='unreaped':changed['steps'][-1]['worker_reaped']=False
                if change=='later':changed['steps'][-1]['step']='guard-create'
                path.write_text(json.dumps(changed))
                with self.assertRaises(ValueError):n.readonly_failure(path,plan())

    def test_explicit_permission_codes_only(self):
        self.assertTrue(n.table_denial(403,'OTSNoPermissionAccess','private-server-text'))
        for message in ('The user has no privilege to access the instance.',
                        'The user does not have permission to access the instance.'):
            self.assertTrue(n.table_denial(403,'OTSAuthFailed',message))
        for status,code,message in ((403,'OTSAuthFailed','Signature mismatch.'),
                (403,'OTSAuthFailed','The AccessKeyID is disabled.'),
                (403,'OTSAuthFailed','Request denied by instance ACL policies.'),
                (403,'OTSQuotaExhausted','quota'),(409,'OTSConditionCheckFail','condition'),
                (500,'OTSNoPermissionAccess','untrusted'),(200,'OTSNoPermissionAccess','untrusted')):
            self.assertFalse(n.table_denial(status,code,message))

    def test_prior_evidence_is_bounded_and_not_authority(self):
        with tempfile.TemporaryDirectory(prefix='usapp-negative-evidence-') as directory:
            path=Path(directory)/'synthetic.json'
            value=dict(completed=True,serving_allowed=False,manifest_sha256=report(plan())['manifest_sha256'],
                       journal_id=plan()['journal_id'],synthetic_operation=n.live.operation(plan()),object_keys=keys())
            path.write_text(json.dumps(value))
            self.assertEqual(n.prior_evidence(path,plan()),keys())
            for name,bad in (('completed',False),('manifest_sha256','f'*64),('object_keys',['unscoped'])):
                changed=dict(value);changed[name]=bad;path.write_text(json.dumps(changed))
                with self.assertRaises(ValueError):n.prior_evidence(path,plan())


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='usapp-negative-driver-')
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        write_package(plan(),self.root/'review')
        self.args=['--package',str(self.root/'review'),'--manifest-sha',report(plan())['manifest_sha256']]

    def test_dry_run_does_not_read_any_credentials_or_prior_evidence(self):
        with patch.object(n,'load',side_effect=AssertionError()),patch.object(n,'prior_evidence',side_effect=AssertionError()),patch.object(n,'Session',side_effect=AssertionError()),redirect_stdout(io.StringIO()) as output:
            self.assertEqual(n.main(self.args+['--credentials','not-opened','--prior-evidence','not-opened']),0)
        self.assertFalse(json.loads(output.getvalue())['cloud_writes_allowed'])

    def test_run_requires_explicit_probe_ack_and_all_paths(self):
        for flags in (['--run'],['--run','--ack-conditional-permission-probes']):
            with patch.object(n,'load',side_effect=AssertionError()),patch.object(n,'prior_evidence',side_effect=AssertionError()),redirect_stdout(io.StringIO()):
                self.assertEqual(n.main(self.args+flags),2)

    def test_session_requires_fresh_directory_and_stops_on_unknown(self):
        session=n.Session(plan(),bundle(),self.root/'new',keys(),True)
        with patch.object(n,'run',return_value=Result()) as worker:
            self.assertFalse(session.step(n.STEPS[0]))
            with self.assertRaises(ValueError):session.step(n.STEPS[1])
            self.assertEqual(worker.call_count,1)
        with self.assertRaises(ValueError):n.Session(plan(),bundle(),self.root/'new',keys(),True)

    def test_success_steps_use_five_seconds_and_retain_guard_key(self):
        session=n.Session(plan(),bundle(),self.root/'new',keys(),True)
        with patch.object(n,'run',return_value=Result(value=n.p.State.READY)) as worker:
            for step in n.STEPS:self.assertTrue(session.step(step))
        self.assertTrue(all(call.kwargs['timeout']==5.0 for call in worker.call_args_list))
        value=json.loads(session.path.read_text())
        self.assertEqual(value['guard_row_key'][1:], [['row_type','permission_probe'],['row_id','write-guard-v1']])
        self.assertTrue(value['completed'])
        self.assertFalse(value['serving_allowed'])

    def test_explicit_new_attempt_carries_old_consumption_without_touching_old_directory(self):
        site=self.root/'sdk';(site/'tablestore-6.4.8.dist-info').mkdir(parents=True)
        (site/'tablestore-6.4.8.dist-info'/'METADATA').write_text('synthetic')
        directory=self.root/'new-attempt'
        args=self.args+['--run','--ack-conditional-permission-probes','--credentials','synthetic',
            '--sdk-site',str(site),'--prior-evidence','synthetic-prior','--previous-readonly-failure','synthetic-failure',
            '--state-directory',str(directory)]
        old=dict(requests=30,wire=4671,receive=10775)
        with patch.object(n,'load',return_value=bundle()),patch.object(n,'prior_evidence',return_value=keys()), \
                patch.object(n,'readonly_failure',return_value=old),patch.object(n,'run',return_value=Result()),redirect_stdout(io.StringIO()):
            self.assertEqual(n.main(args),2)
        evidence=json.loads((directory/'trial-evidence.json').read_text())
        self.assertEqual(evidence['previous_readonly_consumption'],old)
        for key,value in old.items():self.assertEqual(evidence['quota'][key],value)


@unittest.skipUnless(os.environ.get('USAPP_J4C_OFFLINE_SDK')=='1','isolated dependency site required')
class WireTests(unittest.TestCase):
    def setUp(self):
        for name in ('socket.socket.connect','socket.create_connection','socket.getaddrinfo'):
            p=patch(name,side_effect=AssertionError('external_network_forbidden'))
            p.start();self.addCleanup(p.stop)
        self.calls=[]

    def table_client(self,status,code,message):
        from tablestore.protobuf import table_store_pb2 as pb
        from test_j4c_tablestore_sdk import Response
        owner=self
        class Pool:
            def urlopen(self,method,url,**kwargs):
                owner.calls.append((method,url,kwargs))
                body=pb.Error(code=code,message=message).SerializeToString()
                return Response(body,{},status)
        key=bundle().keys['recovery-reader']
        return n.GuardTables(n.table_config(plan()),key.access_key_id,key.access_key_secret,pool_factory=lambda **_:Pool())

    def test_real_sdk_put_is_only_the_guard_row_with_not_exist_condition(self):
        from tablestore.protobuf import table_store_pb2 as pb
        from tablestore.plainbuffer.plain_buffer_builder import PlainBufferBuilder
        client=self.table_client(403,'OTSNoPermissionAccess','private-message')
        self.assertIs(client.write_guard(plan()),n.p.State.ABORTED)
        self.assertEqual(len(self.calls),1)
        self.assertTrue(self.calls[0][1].endswith('/PutRow'))
        request=pb.PutRowRequest();request.ParseFromString(self.calls[0][2]['body'])
        self.assertEqual(request.table_name,'journal')
        self.assertEqual(request.condition.row_existence,pb.EXPECT_NOT_EXIST)
        self.assertEqual(bytes(request.row), bytes(PlainBufferBuilder.serialize_for_put_row(
            client.guard_key(),[('payload',n.guard_payload(plan()))])))

    def test_bad_signature_condition_or_generic_auth_not_counted_as_denied(self):
        for code,message,status in (('OTSAuthFailed','Signature mismatch.',403),
                ('OTSAuthFailed','Request denied by instance ACL policies.',403),
                ('OTSConditionCheckFail','private',409),('OTSInternalServerError','private',500)):
            self.assertIs(self.table_client(status,code,message).write_guard(plan()),n.p.State.UNKNOWN)
        self.assertEqual(len(self.calls),4)

    def test_guard_creation_does_not_relabel_unknown_as_conflict(self):
        for answer,expected in ((n.p.State.UNKNOWN,n.p.State.UNKNOWN),(n.p.State.ABORTED,n.p.State.CONFLICT),
                                (n.p.State.COMMITTED,n.p.State.READY)):
            with patch.object(n,'GuardTables') as client:
                client.return_value.write_guard.return_value=answer
                self.assertIs(n.run_stage(plan(),bundle(),'guard-create',keys(),None),expected)

    def test_explicit_guard_resolution_reuses_matching_row_without_put(self):
        with patch.object(n,'GuardTables') as client,redirect_stdout(io.StringIO()) as output:
            client.return_value.inspect_guard.return_value=n.p.State.READY
            self.assertIs(n.run_stage(plan(),bundle(),'guard-create',keys(),None,True),n.p.State.READY)
            self.assertEqual(client.call_count,1)
            client.return_value.write_guard.assert_not_called()
            self.assertIn('original_creation_outcome_unproven',output.getvalue())

    def test_absent_guard_resolution_uses_one_conditional_write_after_reader_check(self):
        with patch.object(n,'GuardTables') as client,redirect_stdout(io.StringIO()):
            from unittest.mock import Mock
            reader,writer=Mock(),Mock()
            client.side_effect=[reader,writer]
            reader.inspect_guard.return_value=n.p.State.ABORTED
            writer.write_guard.return_value=n.p.State.COMMITTED
            b=bundle()
            self.assertIs(n.run_stage(plan(),b,'guard-create',keys(),None,True),n.p.State.READY)
            self.assertEqual(client.call_args_list[0].args[1],b.keys['recovery-reader'].access_key_id)
            self.assertEqual(client.call_args_list[1].args[1],b.keys['checkpoint-writer'].access_key_id)
            writer.write_guard.assert_called_once_with(plan())

    def test_mismatch_or_unknown_inspection_never_writes(self):
        for state in (n.p.State.CONFLICT,n.p.State.UNKNOWN):
            with patch.object(n,'GuardTables') as client:
                client.return_value.inspect_guard.return_value=state
                self.assertIs(n.run_stage(plan(),bundle(),'guard-create',keys(),None,True),state)
                client.return_value.write_guard.assert_not_called()

    def test_failure_diagnostics_do_not_include_error_message_or_request_id(self):
        client=self.table_client(400,'OTSParameterInvalid','private-synthetic-message')
        with redirect_stdout(io.StringIO()) as output:
            self.assertIs(client.write_guard(plan()),n.p.State.UNKNOWN)
        self.assertEqual(output.getvalue(),'J4C_PERMISSION_DIAG_TABLE_PARAMETER_INVALID\n')

    def test_positive_guard_read_decodes_real_sdk_wire(self):
        from test_j4c_tablestore_sdk import Pool
        from tablestore.plainbuffer.plain_buffer_builder import PlainBufferBuilder
        pool=Pool()
        client=n.GuardTables(n.table_config(plan()),'offline-access-id','offline-synthetic-secret',pool_factory=lambda **_:pool)
        pool.sdk=client._sdk
        def reply(api,body,message):
            if api=='GetRow':
                message.row=bytes(PlainBufferBuilder.serialize_for_put_row(client.guard_key(),[('payload',n.guard_payload(plan()),1)]))
        pool.reply_hook=reply
        self.assertTrue(client.read_guard(plan()))
        self.assertEqual(len(pool.calls),1)

    def test_object_only_explicit_403_access_denied_passes(self):
        for status,code,expected in ((403,'AccessDenied',n.p.State.READY),(403,'SignatureDoesNotMatch',n.p.State.UNKNOWN),
                (409,'FileAlreadyExists',n.p.State.CONFLICT),(200,'None',n.p.State.CONFLICT),
                (500,'InternalError',n.p.State.UNKNOWN)):
            with patch.object(n,'Objects') as objects:
                objects.return_value.get.return_value=b'synthetic-valid-batch'
                objects.return_value._request.return_value=(status,('<Error><Code>'+code+'</Code></Error>').encode())
                self.assertIs(n.run_stage(plan(),bundle(),'reader-object-denied',keys(),None),expected)
                objects.return_value._request.assert_called_once_with('PUT',keys()[0],b'synthetic-valid-batch')

    def test_allowed_negative_row_write_is_a_conflict_never_a_pass(self):
        for step in ('publisher-table-denied','reader-table-denied'):
            with patch.object(n,'GuardTables') as client:
                client.return_value.write_guard.return_value=n.p.State.COMMITTED
                self.assertIs(n.run_stage(plan(),bundle(),step,keys(),None),n.p.State.CONFLICT)

    def test_stage_selects_correct_denied_principal(self):
        b=bundle()
        for step,role in (('publisher-table-denied','object-publisher'),('reader-table-denied','recovery-reader')):
            with patch.object(n,'GuardTables') as client:
                client.return_value.write_guard.return_value=n.p.State.ABORTED
                self.assertIs(n.run_stage(plan(),b,step,keys(),None),n.p.State.READY)
                self.assertEqual(client.call_args.args[1],b.keys[role].access_key_id)

    def test_chain_verification_loads_once_and_does_not_repeat_live_verify(self):
        op=n.live.operation(plan())
        frames=[dict(op,phase=phase) for phase in ('intent','aborted')]
        snapshot={'keys':keys()}
        for step in ('verify-before','verify-after'):
            with patch.object(n.live,'run_stage',side_effect=AssertionError('duplicate_full_verify_forbidden')) as duplicate, \
                    patch.object(n.live,'ReadClient'),patch.object(n,'Objects'),patch.object(n.p,'Publisher') as publisher:
                publisher.return_value._load.return_value=(snapshot,frames,b'',None)
                self.assertIs(n.run_stage(plan(),bundle(),step,keys(),None),n.p.State.READY)
                publisher.return_value._load.assert_called_once_with()
                duplicate.assert_not_called()

    def test_bootstrap_is_separate_and_always_aborts_without_loading_chain(self):
        for step in ('bootstrap-before','bootstrap-after'):
            with patch.object(n.live,'ReadClient') as client,patch.object(n.live,'Metadata') as metadata, \
                    patch.object(n.p,'Publisher',side_effect=AssertionError('chain_not_needed')):
                client.return_value.start.return_value='synthetic-tx'
                client.return_value.get.return_value=(None,object(),None)
                metadata.return_value._row.return_value=('current',n.live.receipt(plan()),0)
                self.assertIs(n.run_stage(plan(),bundle(),step,keys(),None),n.p.State.READY)
                client.return_value.get.assert_called_once_with('synthetic-tx','bootstrap','current')
                client.return_value.finish.assert_called_once_with('synthetic-tx')

    def test_reduced_chain_path_still_rejects_wrong_keys_or_phases(self):
        op=n.live.operation(plan())
        good=[dict(op,phase=phase) for phase in ('intent','aborted')]
        for snapshot,frames in (({'keys':[keys()[0]]},good),({'keys':keys()},good[:1]),
                                ({'keys':keys()},[dict(op,phase='intent')]*2)):
            with patch.object(n.live,'ReadClient'),patch.object(n,'Objects'),patch.object(n.p,'Publisher') as publisher:
                publisher.return_value._load.return_value=(snapshot,frames,b'',None)
                self.assertIs(n.run_stage(plan(),bundle(),'verify-before',keys(),None),n.p.State.CONFLICT)
