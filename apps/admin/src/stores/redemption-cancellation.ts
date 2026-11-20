import {computed,ref,watch} from 'vue';
import {defineStore} from 'pinia';
import {useAdminSessionStore} from './admin-session';
import {AdminApiError} from '@/api/client';
import {cancellationApi,cancellable,parseCancellationReceipt,type CancellationCapture,type CancellationReceipt} from '@/api/redemption-cancellation';
import type {RedemptionDetail} from '@/api/redemptions';
export const useRedemptionCancellationStore=defineStore('redemption-cancellation',()=>{
 const session=useAdminSessionStore(),capture=ref<CancellationCapture|null>(null),receipt=ref<CancellationReceipt|null>(null),message=ref('');
 const stage=ref<'idle'|'prepared'|'sending'|'unknown'|'rejected'|'confirmed'>('idle');
 let generation=0,wasUnknown=false;
 const owner=()=>session.authenticated?`${session.epoch}:${session.admin?.id??''}`:'';
 function clear(){generation++;capture.value=null;receipt.value=null;message.value='';stage.value='idle';wasUnknown=false;}
 watch(owner,clear,{flush:'sync'});
 function prepare(detail:RedemptionDetail,reason:string){
  if(stage.value!=='idle'||!owner()||!session.admin||!cancellable(detail.redemption)||!/^[1-9]\d{0,2}$/.test(detail.redemption.price_flowers))return false;
  if([...reason].length<5||[...reason].length>500||reason!==reason.trim()||/[\x00-\x1f\x7f-\x9f]/.test(reason)){message.value='请填写 5–500 字、可核查的取消原因。';return false;}
  const r=detail.redemption;
  capture.value=Object.freeze({adminId:session.admin.id,coupleId:detail.space.id,redemptionId:r.id,redeemerId:r.redeemer_id,price:r.price_flowers,key:`admin-cancel-${crypto.randomUUID()}`,command:Object.freeze({expected_status:r.status as 'pending_confirmation'|'pending_fulfillment',reason})});
  stage.value='prepared';message.value='请核对原兑换、退款数量和原因，再次确认。';return true;
 }
 async function send(){
  if(!capture.value||!owner()||(stage.value!=='prepared'&&stage.value!=='unknown'))return;
  const c=capture.value,o=owner(),g=generation;stage.value='sending';message.value='';
  try{const answer=await cancellationApi.send(c);if(o!==owner()||g!==generation)return;const r=parseCancellationReceipt(answer,c);if(!r)throw new Error('unconfirmed receipt');receipt.value=r;stage.value='confirmed';message.value='已确认原取消记录；请刷新兑换状态，回执中的余额是当时结果。';}
  catch(e){if(o!==owner()||g!==generation)return;const definite=e instanceof AdminApiError&&((e.status>=400&&e.status<500)||(e.status===0&&e.code==='CSRF_TOKEN_MISSING'));
   if(definite&&!wasUnknown){stage.value='rejected';message.value=e.message;}
   else{wasUnknown=true;stage.value='unknown';message.value='结果未确认。保留原请求，只能使用相同标识和内容重试，不要发起另一笔。';}
  }
 }
 function confirm(){if(stage.value==='prepared')return send();}
 function retry(){if(stage.value==='unknown')return send();}
 function dismiss(){if(stage.value==='prepared'||stage.value==='confirmed'||stage.value==='rejected')clear();}
 const locked=computed(()=>stage.value!=='idle');
 // Route changes do not discard an uncertain financial command. Account changes clear private state.
 return {capture,receipt,message,stage,locked,prepare,confirm,retry,dismiss};
});
