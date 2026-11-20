"""Protocol preparation ONLY: real SDK, synthetic in-memory services, no sockets.

Post-SDK reply discard is NOT wire loss. Shared-predecessor tests are ordered
software controls, NOT concurrent transactions or vendor atomicity evidence.
"""
import copy
import os
import unittest

from tools import deletion_journal as j1
from tools import deletion_journal_publication as p
from tools.j4c_tablestore_sdk import Client,ClientError
from tools.j4c_metadata import Metadata
import test_j4c_publication_sdk as fixture
from test_j4c_metadata import operation,JOURNAL
from test_j4c_tablestore_sdk import Pool,config


class _PostSdkReplyLoss(Client):
    """Test-local prototype. No CLI, credentials file, real-pool factory or retries."""
    def __init__(self,*args,**kwargs):
        super().__init__(*args,**kwargs)
        self.injected=0

    def finish(self,transaction_id,commit=False):
        result=super().finish(transaction_id,commit)
        if commit is True and self.injected==0:
            self.injected+=1
            raise ClientError('synthetic_reply_discarded_after_sdk_success')
        return result


@unittest.skipUnless(os.environ.get('USAPP_J4C_OFFLINE_SDK')=='1','isolated SDK site required')
class ProtocolPreparationTests(unittest.TestCase):
    setUp=fixture.PublicationSdkTests.setUp

    def new_client(self,kind=Client):
        pool=Pool()
        pool.reply_hook=self.server.reply
        client=kind(config(),'offline-access-id','offline-synthetic-secret',pool_factory=lambda **_:pool)
        pool.sdk=client._sdk
        return client,pool

    def publisher_for(self,client):
        return p.Publisher(JOURNAL,self.objects,Metadata(client,self.objects),max_rounds=1)

    def test_post_sdk_intent_reply_discard_is_unknown_then_fresh_read_replays_without_put(self):
        client,pool=self.new_client(_PostSdkReplyLoss)
        op=operation()
        self.assertIs(self.publisher_for(client).publish(op),p.State.UNKNOWN)
        self.assertEqual(client.injected,1)
        self.assertTrue(pool.responses[-1].released)  # Response parsed/verified by real SDK.
        self.assertEqual(pool.calls[-1][1].rsplit('/',1)[-1],'CommitTransaction')
        self.assertEqual(j1._decode(self.server.rows['head','current'])['batch_seq'],1)
        puts=sum(call[0]=='PUT' for call in self.object_pool.calls)
        fresh,fresh_pool=self.new_client()
        self.assertIs(self.publisher_for(fresh).publish(op),p.State.READY)
        self.assertFalse(any(call[1].endswith('/'+api) for call in fresh_pool.calls
                             for api in ('PutRow','UpdateRow','CommitTransaction')))
        self.assertEqual(sum(call[0]=='PUT' for call in self.object_pool.calls),puts)
        self.assertEqual(len(self.object_pool.data),1)

    def test_post_sdk_terminal_reply_discard_replays_original_aborted_fact_without_put(self):
        op=operation()
        self.assertIs(self.publisher.publish(op),p.State.READY)
        client,pool=self.new_client(_PostSdkReplyLoss)
        self.assertIs(self.publisher_for(client).publish(op,'aborted'),p.State.UNKNOWN)
        self.assertEqual(client.injected,1)
        self.assertEqual(pool.calls[-1][1].rsplit('/',1)[-1],'CommitTransaction')
        before=copy.deepcopy(self.server.rows)
        puts=sum(call[0]=='PUT' for call in self.object_pool.calls)
        fresh,fresh_pool=self.new_client()
        self.assertIs(self.publisher_for(fresh).publish(op,'aborted'),p.State.ABORTED)
        self.assertFalse(any(call[1].endswith('/'+api) for call in fresh_pool.calls
                             for api in ('PutRow','UpdateRow','CommitTransaction')))
        self.assertEqual(self.server.rows,before)
        self.assertEqual(sum(call[0]=='PUT' for call in self.object_pool.calls),puts)
        self.assertEqual(j1._decode(self.server.rows['head','current'])['batch_seq'],2)

    def test_precommit_failure_never_counts_as_post_sdk_reply_loss(self):
        client,pool=self.new_client(_PostSdkReplyLoss)
        before=copy.deepcopy(self.server.rows)
        self.server.fail_kind='batch'
        self.assertIs(self.publisher_for(client).publish(operation()),p.State.UNKNOWN)
        self.assertEqual(client.injected,0)
        self.assertEqual(self.server.rows,before)
        self.assertEqual(pool.calls[-1][1].rsplit('/',1)[-1],'AbortTransaction')
        self.assertEqual(len(self.object_pool.data),1)  # Unreferenced object remains; no absence claim.

    def shared_predecessor(self,first):
        candidates=[]
        head=p.zero_head(JOURNAL)
        for _ in range(2):
            op=operation()
            frame,following=self.publisher._candidate(op,'intent',head)
            key=p.object_key(JOURNAL,frame['hash'])
            self.assertIs(self.objects.put_once(key,j1.canonical(frame)+b'\n'),p.Receipt.CONFIRMED)
            candidates.append((op,key,following))
        for index in (first,1-first):
            client,_=self.new_client()
            _,key,following=candidates[index]
            result=Metadata(client,self.objects).commit(JOURNAL,head,key,following)
            self.assertIs(result,p.Receipt.CONFIRMED if index==first else p.Receipt.HEAD_CHANGED)
        snapshot=self.metadata.read(JOURNAL)
        self.assertEqual(snapshot['checkpoint']['batch_seq'],1)
        self.assertEqual(list(snapshot['operations']),[candidates[first][0]['operation_id']])
        self.assertEqual(snapshot['keys'],[candidates[first][1]])
        self.assertEqual(len(self.object_pool.data),2)  # Loser's candidate retained, not silently cleaned.

    def test_shared_predecessor_first_candidate_wins_ordered_control(self):
        self.shared_predecessor(0)

    def test_shared_predecessor_second_candidate_wins_ordered_control(self):
        self.shared_predecessor(1)
