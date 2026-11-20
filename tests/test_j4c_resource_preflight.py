"""Synthetic resource responses only; every socket and DNS call forbidden."""
import datetime
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tools import j4c_resource_preflight as r
from tools.j4c_credentials import Key, parse
from tools.j4c_call_deadline import Result
from tools.deletion_journal_publication import Receipt
from tools.j4c_preflight import report
from tools.j4c_resource_package import write_package
from test_j4c_initialize import plan
from test_j4c_credentials import raw
from test_j4c_identity import Response


def info():
    return b'''<BucketInfo><Bucket><Name>usapp-j4c-log-1234abcd</Name>
<Location>oss-cn-shanghai</Location><ExtranetEndpoint>oss-cn-shanghai.aliyuncs.com</ExtranetEndpoint>
<Owner><ID>1234567890123456</ID></Owner><StorageClass>Standard</StorageClass>
<DataRedundancyType>LRS</DataRedundancyType><AccessControlList><Grant>private</Grant></AccessControlList>
<BlockPublicAccess>true</BlockPublicAccess><ServerSideEncryptionRule><SSEAlgorithm>AES256</SSEAlgorithm>
</ServerSideEncryptionRule></Bucket></BucketInfo>'''


VERSION = b'<VersioningConfiguration xmlns="http://doc.oss-cn-hangzhou.aliyuncs.com"/>'


class SchemaTests(unittest.TestCase):
    def test_exact_bucket_and_never_enabled_are_required(self):
        self.assertTrue(r.validate_bucket(info(), VERSION, plan()))
        for old, new in ((b'private', b'public-read'), (b'true', b'false'), (b'AES256', b'KMS'),
                         (b'LRS', b'ZRS'), (b'Standard', b'Archive'), (b'1234567890123456', b'9999999999999999'),
                         (b'oss-cn-shanghai', b'oss-cn-hangzhou'), (b'log-1234abcd', b'log-1234abce')):
            with self.subTest(old=old), self.assertRaises(r.PreflightError):
                r.validate_bucket(info().replace(old, new), VERSION, plan())

    def test_enabled_suspended_empty_or_ambiguous_status_rejected(self):
        for status in (b'Enabled', b'Suspended', b''):
            with self.assertRaises(r.PreflightError):
                r.validate_bucket(info(), b'<VersioningConfiguration><Status>'+status+b'</Status></VersioningConfiguration>', plan())
        with self.assertRaises(r.PreflightError):
            r.validate_bucket(info().replace(b'</Bucket>', b'<Versioning>Suspended</Versioning></Bucket>'), VERSION, plan())

    def test_duplicate_missing_malformed_dtd_namespace_and_size_refused(self):
        for payload in (info().replace(b'</Owner>', b'<ID>1234567890123456</ID></Owner>'),
                        info().replace(b'<BlockPublicAccess>true</BlockPublicAccess>', b''), b'not-xml',
                        b'<!DOCTYPE x [<!ENTITY a "secret">]>'+info(), b'x'*65537,
                        info().replace(b'<BucketInfo>', b'<BucketInfo xmlns="urn:wrong">')):
            with self.assertRaises(r.PreflightError):
                r.validate_bucket(payload, VERSION, plan())

    def test_diagnostics_identify_fields_without_exposing_values(self):
        for old,new,label in ((b'AES256',b'private-secret-value','ENCRYPTION'),
                              (b'true',b'false','PUBLIC_BLOCK'),
                              (b'1234567890123456',b'9999999999999999','OWNER')):
            with self.assertRaises(r.PreflightError) as caught:
                r.validate_bucket(info().replace(old,new),VERSION,plan())
            self.assertEqual(caught.exception.diagnostic,label)
            self.assertNotIn(new.decode(),str(caught.exception))
        with redirect_stdout(io.StringIO()) as output:
            r.emit(True,'arbitrary-private-secret')
            r.emit(True,'ENCRYPTION')
            r.emit(False,'OWNER')
        self.assertEqual(output.getvalue(),'J4C_RESOURCE_DIAG_ENCRYPTION\n')


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='usapp-resource-synthetic-')
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        write_package(plan(), root/'review')
        self.args = ['--package', str(root/'review'), '--manifest-sha', report(plan())['manifest_sha256'], '--phase', 'table']
        self.site = root/'sdk'
        (self.site/'tablestore-6.4.8.dist-info').mkdir(parents=True)
        (self.site/'tablestore-6.4.8.dist-info'/'METADATA').write_text('synthetic')

    def test_dry_run_never_reads_private_or_sdk_path(self):
        with patch.object(r, 'load', side_effect=AssertionError()), patch.object(r, 'run', side_effect=AssertionError()), redirect_stdout(io.StringIO()) as output:
            self.assertEqual(r.main(self.args+['--credentials', 'not-opened', '--sdk-site', 'not-opened']), 0)
        value = json.loads(output.getvalue())
        for field in ('cloud_calls_allowed', 'table_configuration_verified', 'transaction_probe_verified', 'serving_allowed'):
            self.assertIs(value[field], False)

    def test_missing_ack_or_paths_never_loads(self):
        for args in (['--run'], ['--run', '--ack-read-only-cloud-access', '--credentials', 'unused', '--sdk-site', str(self.site)]):
            with patch.object(r, 'load', side_effect=AssertionError()), redirect_stdout(io.StringIO()):
                self.assertEqual(r.main(self.args+args), 2)

    def test_invalid_manifest_before_credential_read(self):
        args = list(self.args)
        args[3] = 'f'*64
        with patch.object(r, 'load', side_effect=AssertionError()), redirect_stdout(io.StringIO()):
            self.assertEqual(r.main(args+['--run']), 2)

    def test_confirmed_worker_reports_only_requested_stage(self):
        args = self.args+['--run', '--ack-read-only-cloud-access', '--ack-start-abort-probe', '--credentials', 'synthetic', '--sdk-site', str(self.site)]
        with patch.object(r, 'load', return_value=object()), patch.object(r, 'run', return_value=Result(value=Receipt.CONFIRMED)), redirect_stdout(io.StringIO()) as output:
            self.assertEqual(r.main(args), 0)
        value = json.loads(output.getvalue())
        self.assertTrue(value['table_configuration_verified'])
        self.assertTrue(value['transaction_probe_verified'])
        self.assertFalse(value['bucket_configuration_verified'])
        self.assertFalse(value['table_ownership_verified'])
        self.assertFalse(value['serving_allowed'])

    def test_timeout_unreaped_unknown_never_confirms(self):
        args = self.args+['--run', '--ack-read-only-cloud-access', '--ack-start-abort-probe', '--credentials', 'synthetic', '--sdk-site', str(self.site)]
        for result in (Result(), Result(value=Receipt.CONFIRMED, worker_reaped=False), Result(value=Receipt.CONFIRMED, timed_out=True)):
            with patch.object(r, 'load', return_value=object()), patch.object(r, 'run', return_value=result), redirect_stdout(io.StringIO()) as output:
                self.assertEqual(r.main(args), 2)
            self.assertEqual(output.getvalue(), 'J4C_RESOURCE_PREFLIGHT_UNKNOWN_OR_REFUSED\n')


@unittest.skipUnless(os.environ.get('USAPP_J4C_OFFLINE_SDK') == '1', 'isolated SDK site required')
class TransportTests(unittest.TestCase):
    def setUp(self):
        for name in ('socket.socket.connect', 'socket.create_connection', 'socket.getaddrinfo'):
            p = patch(name, side_effect=AssertionError('external_network_forbidden'))
            p.start()
            self.addCleanup(p.stop)
        self.calls = []
        self.responses = []
        self.status, self.encoding, self.large = 200, False, False
        owner = self
        def factory(**options):
            owner.options = options
            class Pool:
                def urlopen(self, method, url, **kwargs):
                    owner.calls.append((method, url, kwargs))
                    data = info() if 'bucketInfo' in url else VERSION
                    response = Response(b'x'*65537 if owner.large else data, owner.status,
                                        {'content-encoding':'gzip'} if owner.encoding else {})
                    owner.responses.append(response)
                    return response
            return Pool()
        self.reader = r.BucketReader(plan(), Key('synthetic-id','synthetic-secret'), factory,
                                    clock=lambda:'20261107T010000Z')

    def test_fixed_get_only_queries_and_query_is_signed(self):
        with patch.object(r, 'string_to_sign', wraps=r.string_to_sign) as signer:
            self.assertTrue(r.validate_bucket(self.reader.get('bucketInfo'), self.reader.get('versioning'), plan()))
        for (method,url,request), query in zip(self.calls, ('bucketInfo','versioning')):
            self.assertEqual(method,'GET')
            self.assertEqual(url,'https://usapp-j4c-log-1234abcd.oss-cn-shanghai.aliyuncs.com/?'+query)
            self.assertIsNone(request['body'])
            self.assertFalse(request['redirect'])
            self.assertFalse(request['retries'])
            self.assertNotIn('synthetic',url)
        self.assertEqual(signer.call_args_list[0][0][2].split('\n')[2], 'bucketInfo')
        self.assertEqual(signer.call_args_list[1][0][2].split('\n')[2], 'versioning')
        self.assertEqual(self.options['cert_reqs'],'CERT_REQUIRED')

    def test_official_empty_subresource_canonical_form_exact_bytes(self):
        # Full canonical bytes from the documented OSS rule, not the production builder.
        expected = ('GET\n/usapp-j4c-log-1234abcd/\nbucketInfo\n'
                    'x-oss-content-sha256:UNSIGNED-PAYLOAD\n'
                    'x-oss-date:20261107T010000Z\n\n\nUNSIGNED-PAYLOAD')
        with patch.object(r,'string_to_sign',wraps=r.string_to_sign) as signer:
            self.reader.get('bucketInfo')
        self.assertEqual(signer.call_args[0][2].encode('ascii'),expected.encode('ascii'))
        self.assertNotEqual(signer.call_args[0][2],expected.replace('bucketInfo\n','bucketInfo=\n'))

    def test_duplicate_or_other_query_never_sends(self):
        self.reader.get('bucketInfo')
        for query in ('bucketInfo','acl','delete','versioning&bad=1'):
            with self.assertRaises(r.PreflightError):
                self.reader.get(query)
        self.assertEqual(len(self.calls),1)

    def test_errors_are_fixed_no_redirect_retry_or_sensitive_body(self):
        for attr,value in (('status',302),('status',403),('status',500),('encoding',True),('large',True)):
            setattr(self,attr,value)
            self.reader.remaining={'bucketInfo'}
            before=len(self.calls)
            with self.assertRaisesRegex(r.PreflightError,'^bucket_read_unknown_or_refused$'):
                self.reader.get('bucketInfo')
            self.assertEqual(len(self.calls),before+1)
            self.assertTrue(self.responses[-1].closed)
            setattr(self,attr,False if attr!='status' else 200)

    def test_default_bucket_transport_is_blocked(self):
        reader = r.BucketReader(plan(), Key('synthetic-id','synthetic-secret'))
        with self.assertRaises(r.PreflightError):
            reader.get('bucketInfo')
        self.assertFalse(self.calls)

    def test_real_sdk_describe_start_abort_and_no_write(self):
        from test_j4c_tablestore_sdk import Pool
        original = r.Client
        pool = Pool()
        def client(*args, **kwargs):
            kwargs['pool_factory']=lambda **options:pool
            result=original(*args,**kwargs)
            pool.sdk=result._sdk
            return result
        with patch.object(r,'Client',side_effect=client):
            self.assertTrue(r.table_probe(plan(),Key('offline-access-id','offline-synthetic-secret'),None))
        self.assertEqual([call[1].rsplit('/',1)[-1] for call in pool.calls],
                         ['DescribeTable','StartLocalTransaction','AbortTransaction'])

    def test_bad_table_stops_before_transaction(self):
        from test_j4c_tablestore_sdk import Pool
        original=r.Client
        pool=Pool(); pool.bad_table=True
        def client(*args,**kwargs):
            kwargs['pool_factory']=lambda **options:pool
            value=original(*args,**kwargs); pool.sdk=value._sdk
            return value
        with patch.object(r,'Client',side_effect=client), self.assertRaises(Exception):
            r.table_probe(plan(),Key('offline-access-id','offline-synthetic-secret'),None)
        self.assertEqual(len(pool.calls),1)

    def test_lost_abort_is_unknown_and_never_retried(self):
        from test_j4c_tablestore_sdk import Pool
        original=r.Client
        pool=Pool()
        transport_call=pool.urlopen
        def lose_abort(method,url,**kwargs):
            if url.endswith('/AbortTransaction'):
                pool.calls.append((method,url,kwargs))
                raise RuntimeError('private-synthetic-reply-lost')
            return transport_call(method,url,**kwargs)
        pool.urlopen=lose_abort
        def client(*args,**kwargs):
            kwargs['pool_factory']=lambda **options:pool
            value=original(*args,**kwargs); pool.sdk=value._sdk
            return value
        with patch.object(r,'Client',side_effect=client), self.assertRaises(Exception):
            r.table_probe(plan(),Key('offline-access-id','offline-synthetic-secret'),None)
        self.assertEqual([call[1].rsplit('/',1)[-1] for call in pool.calls],
                         ['DescribeTable','StartLocalTransaction','AbortTransaction'])

    def test_table_transport_rejects_commit_write_and_duplicate(self):
        from test_j4c_tablestore_sdk import Pool
        original=r.Client
        pool=Pool(); captured=[]
        def client(*args,**kwargs):
            kwargs['pool_factory']=lambda **options:pool
            value=original(*args,**kwargs); pool.sdk=value._sdk
            captured.append(value)
            return value
        with patch.object(r,'Client',side_effect=client):
            self.assertTrue(r.table_probe(plan(),Key('offline-access-id','offline-synthetic-secret'),None))
        for api in ('/CommitTransaction','/PutRow','/UpdateRow','/DeleteRow','/DescribeTable'):
            with self.assertRaises(r.PreflightError):
                captured[0]._sdk.connection.send_receive(api,{},b'')
        self.assertEqual(len(pool.calls),3)

    def test_bucket_phase_confirms_only_after_identity_and_both_checks(self):
        bundle=parse(raw(datetime.datetime.now(datetime.timezone.utc)),plan())
        with patch('tools.j4c_identity.Client') as identity, patch.object(r,'BucketReader') as reader:
            identity.return_value.verify.return_value=True
            reader.return_value.get.side_effect=[info(),VERSION]
            self.assertIs(r.probe(plan(),bundle,'bucket',True),Receipt.CONFIRMED)
            self.assertEqual([call.args[0] for call in reader.return_value.get.call_args_list], ['bucketInfo','versioning'])
            reader.return_value.close.assert_called_once()

    def test_expired_or_missing_transaction_ack_never_calls_identity(self):
        bundle=parse(raw(datetime.datetime.now(datetime.timezone.utc)),plan())
        with patch('tools.j4c_identity.Client') as client:
            self.assertIs(r.probe(plan(),bundle,'table',True,False),Receipt.UNKNOWN)
            object.__setattr__(bundle,'expires_at',datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc))
            self.assertIs(r.probe(plan(),bundle,'bucket',True),Receipt.UNKNOWN)
            client.assert_not_called()

    def test_identity_refusal_never_reads_resources(self):
        bundle=parse(raw(datetime.datetime.now(datetime.timezone.utc)),plan())
        with patch('tools.j4c_identity.Client') as identity, patch.object(r,'BucketReader') as reader:
            identity.return_value.verify.return_value=False
            self.assertIs(r.probe(plan(),bundle,'bucket',True),Receipt.UNKNOWN)
            reader.assert_not_called()
            identity.return_value.close.assert_called_once()

    def test_diagnostic_failure_reports_stage_and_whitelisted_reason_only(self):
        bundle=parse(raw(datetime.datetime.now(datetime.timezone.utc)),plan())
        with patch('tools.j4c_identity.Client') as identity, patch.object(r,'BucketReader') as reader, redirect_stdout(io.StringIO()) as output:
            identity.return_value.verify.return_value=True
            reader.return_value.get.side_effect=r.PreflightError('private-synthetic-error','ACCESS_DENIED')
            self.assertIs(r.probe(plan(),bundle,'bucket',True,False,True),Receipt.UNKNOWN)
        self.assertEqual(output.getvalue().splitlines(),[
            'J4C_RESOURCE_DIAG_IDENTITY_BEGIN','J4C_RESOURCE_DIAG_IDENTITY_CONFIRMED',
            'J4C_RESOURCE_DIAG_BUCKET_INFO_BEGIN','J4C_RESOURCE_DIAG_ACCESS_DENIED'])
        self.assertEqual(reader.return_value.get.call_count,1)

    def test_http_diagnostic_extracts_only_known_error_code(self):
        owner=self
        class Pool:
            def urlopen(self,*args,**kwargs):
                owner.calls.append(args)
                return Response(b'<Error><Code>SignatureDoesNotMatch</Code><Message>private-secret</Message></Error>',403)
        reader=r.BucketReader(plan(),Key('synthetic-id','synthetic-secret'),lambda **_:Pool())
        with self.assertRaises(r.PreflightError) as caught:
            reader.get('bucketInfo')
        self.assertEqual(caught.exception.diagnostic,'SIGNATURE_REJECTED')
        self.assertNotIn('private-secret',str(caught.exception))
        self.assertEqual(len(self.calls),1)
