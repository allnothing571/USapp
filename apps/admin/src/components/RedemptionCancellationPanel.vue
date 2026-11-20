<script setup lang="ts">
import {ref,watch} from 'vue';
import {RouterLink} from 'vue-router';
import {cancellable} from '@/api/redemption-cancellation';
import type {RedemptionDetail} from '@/api/redemptions';
import {useRedemptionCancellationStore} from '@/stores/redemption-cancellation';
const props=defineProps<{detail:RedemptionDetail|null;loading:boolean}>();
const emit=defineEmits<{confirmed:[]}>();
const cancellation=useRedemptionCancellationStore(),reason=ref('');
watch(()=>cancellation.stage,s=>{if(s==='confirmed')emit('confirmed');});
</script>
<template>
 <section class="panel" data-test="redemption-cancellation"><div class="panel-head"><h2 class="panel-title">异常兑换取消</h2></div><div class="panel-body">
 <p>仅取消尚未履约的兑换。已扣款的兑换按原价退回原兑换者的小红花；未扣款申请不会退款。不修改旧记录，不强制履约。</p>
 <p>请求只保存在内存。结果未确认时请保留页面；刷新或退出会丢失重试资料，无法撤回已发送请求。</p>
 <form v-if="!cancellation.locked&&detail&&!loading&&cancellable(detail.redemption)" @submit.prevent="cancellation.prepare(detail,reason)">
  <label>取消原因 <textarea v-model="reason" data-test="cancel-reason" maxlength="2000" placeholder="5–500 字，填写可核查的原因"/></label>
  <button class="btn" data-test="prepare-cancellation" type="submit">准备取消并核对</button>
 </form>
 <p v-else-if="!cancellation.locked">请读取尚未履约的兑换详情。</p>
 <div v-if="cancellation.capture"><p class="mono">原兑换 {{cancellation.capture.redemptionId}}</p>
  <RouterLink :to="{name:'couple-redemptions',params:{coupleId:cancellation.capture.coupleId,redemptionId:cancellation.capture.redemptionId}}">查看原兑换</RouterLink>
  <p>兑换者 {{cancellation.capture.redeemerId}} · 退款 {{cancellation.capture.command.expected_status==='pending_fulfillment'?cancellation.capture.price:'0'}} 小红花</p>
  <p>原因：{{cancellation.capture.command.reason}}</p>
  <button v-if="cancellation.stage==='prepared'" class="btn" data-test="confirm-cancellation" @click="cancellation.confirm()">确认取消这笔兑换</button>
  <button v-if="cancellation.stage==='prepared'" class="btn" @click="cancellation.dismiss()">取消未发送请求</button>
  <p v-if="cancellation.stage==='sending'">正在提交，请勿重复操作…</p>
  <button v-if="cancellation.stage==='unknown'" class="btn" data-test="retry-cancellation" @click="cancellation.retry()">使用原标识重试</button>
 </div>
 <p v-if="cancellation.message" role="status">{{cancellation.message}}</p>
 <p v-if="cancellation.receipt">原取消记录 {{cancellation.receipt.id}} · 原退款 {{cancellation.receipt.refunded_flowers}} 小红花<span v-if="cancellation.receipt.balance_after!==null"> · 退款当时余额 {{cancellation.receipt.balance_after}}（不是当前余额）</span></p>
 <button v-if="cancellation.stage==='confirmed'||cancellation.stage==='rejected'" class="btn" data-test="dismiss-cancellation" @click="cancellation.dismiss()">已知晓，关闭本次记录</button>
 </div></section>
</template>
<style scoped>label,textarea{display:block}textarea{width:100%;max-width:100%}.panel{overflow-wrap:anywhere}</style>
