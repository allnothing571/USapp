import {beforeEach,describe,it,expect,vi} from 'vitest';
import {createPinia,setActivePinia} from 'pinia';
import {mount,flushPromises} from '@vue/test-utils';
import {useAdminSessionStore} from '../../src/stores/admin-session';
import {useWalletAdjustmentStore} from '../../src/stores/wallet-adjustment';
import {parseAdjustmentReceipt,type AdjustmentCapture} from '../../src/api/wallet-adjustment';
import type {WalletSummary} from '../../src/api/wallet';
import {AdminApiError} from '../../src/api/client';
import Panel from '../../src/components/WalletAdjustmentPanel.vue';
const api=vi.hoisted(()=>({send:vi.fn()}));
vi.mock('../../src/api/wallet-adjustment',async original=>({...await original<typeof import('../../src/api/wallet-adjustment')>(),adjustmentApi:api}));
const U='11111111-1111-4111-8111-111111111111',C='22222222-2222-4222-8222-222222222222',A='33333333-3333-4333-8333-333333333333',I='44444444-4444-4444-8444-444444444444',NOW='2026-11-14T00:00:00.000000Z';
const summary=():WalletSummary=>({user:{id:U,status:'active'},space:{couple_id:C,slot:1},balances:[{asset:'flower',balance:'0',latest_seq:'0',account_exists:false},{asset:'sunshine',balance:'0',latest_seq:'0',account_exists:false}],read_at:NOW});
const result=(c:AdjustmentCapture)=>{const d=BigInt(c.command.delta),a=d<0n?-d:d,n=c.command.asset==='flower'?(a+998n)/999n:1n;return {adjustment:{id:I,admin_id:c.adminId,user_id:c.userId,couple_id:c.coupleId,asset:c.command.asset,delta:c.command.delta,expected_seq:c.command.expected_seq,balance_before:c.balance,balance_after:String(BigInt(c.balance)+d),operation_count:Number(n),last_seq:String(BigInt(c.command.expected_seq)+n),reason:c.command.reason,created_at:NOW}};};
const deferred=<T>()=>{let resolve!:(v:T)=>void,reject!:(e:unknown)=>void;const promise=new Promise<T>((r,j)=>{resolve=r;reject=j;});return {promise,resolve,reject};};
function signIn(){const s=useAdminSessionStore();s.$patch({authenticated:true,admin:{id:A,username:'test'},epoch:s.epoch+1});}
function prepared(){const s=useWalletAdjustmentStore();expect(s.prepare(summary(),'flower','1000','Verified test correction')).toBe(true);return s;}
beforeEach(()=>{setActivePinia(createPinia());vi.clearAllMocks();signIn();api.send.mockImplementation(async(c:AdjustmentCapture)=>result(c));});
describe('audited admin wallet command',()=>{
 it('captures exact target, key, expected sequence and review terms; sends only on second confirmation',async()=>{
  const s=prepared(),c=s.capture!;expect(api.send).not.toHaveBeenCalled();expect(c.command.delta).toBe('1000');expect(c.command.expected_seq).toBe('0');expect(c.userId).toBe(U);expect(s.prepare(summary(),'sunshine','1','Another correction')).toBe(false);await s.confirm();expect(api.send).toHaveBeenCalledTimes(1);expect(api.send).toHaveBeenCalledWith(c);expect(s.stage).toBe('confirmed');expect(s.receipt?.operation_count).toBe(2);expect(s.receipt?.balance_after).toBe('1000');
 });
 it('refuses zero, noncanonical or over-limit deltas, unreviewable reasons and unavailable target state',()=>{
  const s=useWalletAdjustmentStore();for(const d of ['0','-0','01','10000','-10000','1.5','+1'])expect(s.prepare(summary(),'flower',d,'Verified test correction')).toBe(false);
  for(const reason of ['short',' text ','bad\nreason','x'.repeat(501)]){const valid=reason==='short';expect(s.prepare(summary(),'flower','1',reason)).toBe(valid);if(valid)s.dismiss();}
  expect(s.prepare({...summary(),space:null},'flower','1','Verified test correction')).toBe(false);expect(s.prepare({...summary(),user:{id:U,status:'disabled'}},'flower','1','Verified test correction')).toBe(false);
 });
 it('uses exact arithmetic and preserves existing balance maxima and nonnegative result',()=>{
  const s=useWalletAdjustmentStore();expect(s.prepare(summary(),'flower','-1','Verified test correction')).toBe(false);const v=summary();v.balances[0].balance='1000000';expect(s.prepare(v,'flower','1','Verified test correction')).toBe(false);v.balances[0].balance='999999';expect(s.prepare(v,'flower','1','Verified test correction')).toBe(true);expect(s.capture?.balance).toBe('999999');
 });
 it('a prepared cancellation sends nothing; a rejected command requires dismissal before a fresh key',async()=>{
  const s=prepared(),old=s.capture!.key;s.dismiss();expect(api.send).not.toHaveBeenCalled();expect(s.stage).toBe('idle');s.prepare(summary(),'flower','1000','Verified test correction');expect(s.capture!.key).not.toBe(old);api.send.mockRejectedValueOnce(new AdminApiError(409,'ADMIN_ADJUSTMENT_CONFLICT','Re-read the account'));await s.confirm();expect(s.stage).toBe('rejected');expect(s.prepare(summary(),'flower','1','Another correction')).toBe(false);s.dismiss();expect(s.prepare(summary(),'flower','1','Another correction')).toBe(true);
 });
 it('sending is single-flight and cannot be dismissed or replaced',async()=>{
  const pending=deferred<unknown>();api.send.mockReturnValueOnce(pending.promise);const s=prepared(),c=s.capture!;const sending=s.confirm();await s.confirm();s.dismiss();expect(s.stage).toBe('sending');expect(api.send).toHaveBeenCalledTimes(1);pending.resolve(result(c));await sending;expect(s.stage).toBe('confirmed');
 });
 it('unknown outcomes preserve the exact command and key; successful same-key retry confirms original facts',async()=>{
  const s=prepared(),c=s.capture!;api.send.mockRejectedValueOnce(new TypeError('connection lost'));await s.confirm();expect(s.stage).toBe('unknown');s.dismiss();expect(s.capture).toEqual(c);expect(s.prepare(summary(),'flower','1','Another correction')).toBe(false);await s.retry();expect(api.send).toHaveBeenNthCalledWith(1,c);expect(api.send).toHaveBeenNthCalledWith(2,c);expect(s.stage).toBe('confirmed');
 });
 for(const status of [400,401,403,404,409])it(`a later ${status} cannot settle an earlier uncertain write`,async()=>{
  const s=prepared();api.send.mockRejectedValueOnce(new AdminApiError(503,'RESULT_UNKNOWN','Unknown'));await s.confirm();const key=s.capture!.key;api.send.mockRejectedValueOnce(new AdminApiError(status,'REFUSED','Refused'));await s.retry();expect(s.stage).toBe('unknown');expect(s.capture!.key).toBe(key);s.dismiss();expect(s.stage).toBe('unknown');
 });
 it('known pre-send missing CSRF is a definite first refusal',async()=>{
  const s=prepared();api.send.mockRejectedValueOnce(new AdminApiError(0,'CSRF_TOKEN_MISSING','Bootstrap first'));await s.confirm();expect(s.stage).toBe('rejected');
 });
 it('malformed success is uncertain; rejects receipt identity, sequence, amount and split-count mismatches',async()=>{
  const s=prepared(),c=s.capture!;const r=result(c);expect(parseAdjustmentReceipt(r,c)).not.toBeNull();for(const patch of [{user_id:A},{admin_id:U},{delta:'999'},{operation_count:1},{last_seq:'1'},{balance_after:'999'},{reason:'Different reason'},{idempotency_key:'private'}])expect(parseAdjustmentReceipt({adjustment:{...r.adjustment,...patch}},c)).toBeNull();api.send.mockResolvedValueOnce({});await s.confirm();expect(s.stage).toBe('unknown');
 });
 it('identity change clears private state and ignores a delayed confirmation',async()=>{
  const p=deferred<unknown>();api.send.mockReturnValueOnce(p.promise);const s=prepared(),c=s.capture!;const old=s.confirm();signIn();expect(s.capture).toBeNull();p.resolve(result(c));await old;expect(s.stage).toBe('idle');expect(s.receipt).toBeNull();
 });
 it('component makes prepare and confirm separate, displays reviewed terms, and emits only after confirmation',async()=>{
  const view=mount(Panel,{props:{userId:U,summary:summary(),loading:false},global:{stubs:{RouterLink:{template:'<a><slot/></a>',props:['to']}}}});await view.get('[data-test="adjust-delta"]').setValue('1000');await view.get('[data-test="adjust-reason"]').setValue('Verified UI correction');await view.get('form').trigger('submit');expect(api.send).not.toHaveBeenCalled();expect(view.text()).toContain('预期 1000');await view.get('[data-test="confirm-adjustment"]').trigger('click');await flushPromises();expect(api.send).toHaveBeenCalledTimes(1);expect(view.emitted('confirmed')).toHaveLength(1);expect(view.text()).toContain('不覆盖当前余额');view.unmount();
 });
 it('route unmount keeps one unresolved command; another target cannot replace the original',async()=>{
  const s=prepared();api.send.mockRejectedValueOnce(new TypeError('lost'));await s.confirm();const c=s.capture!;const view=mount(Panel,{props:{userId:A,summary:null,loading:false},global:{stubs:{RouterLink:{template:'<a><slot/></a>',props:['to']}}}});expect(view.text()).toContain(U);expect(view.text()).toContain('返回原纠错用户');view.unmount();expect(s.capture).toEqual(c);expect(s.stage).toBe('unknown');
 });
});
