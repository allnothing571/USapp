"""Negative probe codec + spawn + TLS using owned synthetic services only."""
import datetime
import os
from pathlib import Path
import ssl
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer

from tools import j4c_permission_trial as n
from tools.j4c_call_deadline import run
from tools.j4c_credentials import parse
from tools.j4c_local_transport import Fixture,Pools,restricted
from tools.j4c_trial import Ledger
from test_j4c_credentials import raw
from test_preflight import plan
from test_j4c_oss_objects import candidate


def synthetic_stage(plan,bundle,step,keys,ref,fixture):
    pools=Pools(fixture)
    table='https://{}.cn-shanghai.ots.aliyuncs.com'.format(plan['instance'])
    obj='https://{}.oss-cn-shanghai.aliyuncs.com'.format(plan['bucket'])
    def factory(**options):
        routes={}
        class Route:
            def urlopen(self,method,url,**kwargs):
                endpoint,kind=(table,'tables') if url.startswith(table+'/') else (obj,'objects')
                if endpoint not in routes:routes[endpoint]=pools.factory(endpoint,kind)(**options)
                return routes[endpoint].urlopen(method,url,**kwargs)
        return Route()
    guarded,close=n.live.scoped_factory(plan,Ledger(*ref),factory)
    try:
        with restricted(fixture):return n.run_stage(plan,bundle,step,keys,guarded)
    finally:
        close();pools.close()


@unittest.skipUnless(os.environ.get('USAPP_J4C_LOCAL_TLS_DIR'),'explicit synthetic fixture required')
class PermissionWorkerTests(unittest.TestCase):
    def test_three_negative_requests_reach_service_and_require_explicit_denial(self):
        self.key,self.data=candidate()
        self.calls=[]
        owner=self
        class Handler(BaseHTTPRequestHandler):
            protocol_version='HTTP/1.1'
            def log_message(self,*_):pass
            def do_POST(self):
                from tablestore.protobuf import table_store_pb2 as pb
                request=pb.PutRowRequest()
                request.ParseFromString(self.rfile.read(int(self.headers['Content-Length'])))
                owner.assertEqual(self.path,'/tables/PutRow')
                owner.assertEqual(request.condition.row_existence,pb.EXPECT_NOT_EXIST)
                owner.assertIn(b'permission_probe',request.row)
                owner.assertNotIn(b'current',request.row)
                owner.calls.append('PutRow')
                body=pb.Error(code='OTSNoPermissionAccess',message='private-synthetic-error').SerializeToString()
                self.send_response(403);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            def do_GET(self):
                owner.assertEqual(self.path,'/objects/'+owner.key)
                owner.calls.append('GET')
                self.send_response(200);self.send_header('Content-Length',str(len(owner.data)));self.end_headers();self.wfile.write(owner.data)
            def do_PUT(self):
                data=self.rfile.read(int(self.headers['Content-Length']))
                owner.assertEqual(data,owner.data)
                owner.assertEqual(self.headers['x-oss-forbid-overwrite'],'true')
                owner.assertEqual(self.path,'/objects/'+owner.key)
                owner.calls.append('PUT')
                body=b'<Error><Code>AccessDenied</Code><Message>private-synthetic-error</Message></Error>'
                self.send_response(403);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        directory=Path(os.environ['USAPP_J4C_LOCAL_TLS_DIR'])
        ctx.load_cert_chain(str(directory/'server.crt'),str(directory/'server.key'))
        server.socket=ctx.wrap_socket(server.socket,server_side=True)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix='usapp-negative-worker-') as temp:
                bundle=parse(raw(datetime.datetime.now(datetime.timezone.utc)),plan())
                session=n.Session(plan(),bundle,Path(temp)/'new',[self.key,'unused-second-key'],True)
                ledger=session.trial.ledger
                ref=(str(ledger.path),session.trial.nonce,session.trial.manifest)
                fixture=Fixture(server.server_address[1],str(directory))
                for step in ('publisher-table-denied','reader-table-denied','reader-object-denied'):
                    result=run(synthetic_stage,(plan(),bundle,step,[self.key],ref,fixture),timeout=5.0)
                    self.assertIs(result.value,n.p.State.READY)
                    self.assertFalse(result.timed_out)
                    self.assertTrue(result.worker_reaped)
                self.assertEqual(session.trial.stats()['requests'],4)
                self.assertEqual(session.trial.stats()['puts'],1)
                self.assertEqual(self.calls,['PutRow','PutRow','GET','PUT'])
        finally:
            server.shutdown();server.server_close();thread.join(timeout=2)
