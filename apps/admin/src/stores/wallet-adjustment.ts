import {computed,ref,watch} from 'vue';
import {defineStore} from 'pinia';
import {useAdminSessionStore} from './admin-session';
import {AdminApiError} from '@/api/client';
import {adjustmentApi,parseAdjustmentReceipt,seq,validDelta,type AdjustmentCapture,type AdjustmentReceipt} from '@/api/wallet-adjustment';
import type {Asset,WalletSummary} from '@/api/wallet';
export const useWalletAdjustmentStore=defineStore('wallet-adjustment',()=>{
 const session=useAdminSessionStore();
 const capture=ref<AdjustmentCapture|null>(null),receipt=ref<AdjustmentReceipt|null>(null),message=ref('');
 const stage=ref<'idle'|'prepared'|'sending'|'unknown'|'rejected'|'confirmed'>('idle');
 let generation=0,wasUnknown=false;
 const owner=()=>session.authenticated?`${session.epoch}:${session.admin?.id??''}`:'';
 function clear(){generation++;capture.value=null;receipt.value=null;message.value='';stage.value='idle';wasUnknown=false;}
 watch(owner,clear,{flush:'sync'});
 function prepare(summary:WalletSummary,asset:Asset,delta:string,reason:string):boolean {
  if(stage.value!=='idle'||!owner()||!session.admin||summary.user.status!=='active'||!summary.space)return false;
  const balance=summary.balances.find(b=>b.asset===asset);
  if(!balance||!seq(balance.balance)||!seq(balance.latest_seq)||!validDelta(delta)||[...reason].length<5||[...reason].length>500||reason!==reason.trim()||/[\x00-\x1f\x7f-\x9f]/.test(reason)){message.value='请填写非零整数差额和 5–500 字的完整原因。';return false;}
  const after=BigInt(balance.balance)+BigInt(delta),maximum=asset==='flower'?1000000n:1000000000n;
  if(after<0n||after>maximum){message.value='纠错后余额超出允许范围。';return false;}
  capture.value=Object.freeze({adminId:session.admin.id,userId:summary.user.id,coupleId:summary.space.couple_id,balance:balance.balance,key:`admin-adjust-${crypto.randomUUID()}`,command:Object.freeze({asset,delta,expected_seq:balance.latest_seq,reason})});
  stage.value='prepared';message.value='请再次核对用户、资产、差额、原因和余额，再确认执行。';return true;
 }
 async function send(){
  if(!capture.value||!owner()||(stage.value!=='prepared'&&stage.value!=='unknown'))return;
  const c=capture.value,o=owner(),g=generation;stage.value='sending';message.value='';
  try{const answer=await adjustmentApi.send(c);if(o!==owner()||g!==generation)return;const r=parseAdjustmentReceipt(answer,c);if(!r)throw new Error('unconfirmed receipt');receipt.value=r;stage.value='confirmed';message.value='已确认原纠错记录；显示的是当时结果，请另行刷新当前余额。';}
  catch(e){if(o!==owner()||g!==generation)return;
   const definite=e instanceof AdminApiError&&((e.status>=400&&e.status<500)||(e.status===0&&e.code==='CSRF_TOKEN_MISSING'));
   if(definite&&!wasUnknown){stage.value='rejected';message.value=e instanceof AdminApiError?e.message:'纠错被拒绝。';}
   else{wasUnknown=true;stage.value='unknown';message.value='结果未确认。保留原请求，只能用同一标识和内容重试；不要重复发起另一笔。';}
  }
 }
 function confirm(){if(stage.value==='prepared')return send();}
 function retry(){if(stage.value==='unknown')return send();}
 function dismiss(){if(stage.value==='prepared'||stage.value==='confirmed'||stage.value==='rejected')clear();}
 const locked=computed(()=>stage.value!=='idle');
 // Intentionally not cleared on route unmount: one unresolved command spans admin pages. Session changes do clear private data.
 return {capture,receipt,message,stage,locked,prepare,confirm,retry,dismiss};
});
