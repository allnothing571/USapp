<script setup lang="ts">
import {computed,ref,watch} from 'vue';
import {RouterLink} from 'vue-router';
import {assetLabels,type Asset,type WalletSummary} from '@/api/wallet';
import {useWalletAdjustmentStore} from '@/stores/wallet-adjustment';
const props=defineProps<{userId:string|null;summary:WalletSummary|null;loading:boolean}>();
const emit=defineEmits<{confirmed:[]}>();
const adjustment=useWalletAdjustmentStore(),asset=ref<Asset>('flower'),delta=ref(''),reason=ref('');
const available=computed(()=>!props.loading&&props.summary?.user.id===props.userId&&props.summary?.user.status==='active'&&props.summary.space!==null);
const preview=computed(()=>adjustment.capture?String(BigInt(adjustment.capture.balance)+BigInt(adjustment.capture.command.delta)):'');
watch(()=>adjustment.stage,(value)=>{if(value==='confirmed')emit('confirmed');});
function prepare(){if(available.value&&props.summary)adjustment.prepare(props.summary,asset.value,delta.value,reason.value);}
</script>
<template>
 <section class="panel" data-test="wallet-adjustment"><div class="panel-head"><h2 class="panel-title">资产差额纠错</h2></div><div class="panel-body">
 <p>仅用于有依据的纠错。每次总差额最多 ±9999，不能令余额为负，不修改旧流水；小红花大额纠错会原子拆分为多条有审计的流水。</p>
 <p>请求及未确认状态只保存在内存。刷新、退出登录会丢失重试资料，无法撤回已发送的请求；结果未确认时请保留此页面。</p>
 <form v-if="!adjustment.locked" @submit.prevent="prepare">
  <label>资产 <select v-model="asset" data-test="adjust-asset" :disabled="!available"><option value="flower">小红花</option><option value="sunshine">阳光值</option></select></label>
  <label>增减差额 <input v-model="delta" data-test="adjust-delta" type="text" inputmode="text" placeholder="例如 5 或 -5" :disabled="!available"/></label>
  <label>纠错原因 <textarea v-model="reason" data-test="adjust-reason" maxlength="2000" placeholder="5–500 字，填写可核查的原因" :disabled="!available"/></label>
  <button class="btn" type="submit" data-test="prepare-adjustment" :disabled="!available">准备纠错并核对</button>
 </form>
 <p v-if="!available&&!adjustment.locked">请先读取有效用户的当前空间和余额。</p>
 <div v-if="adjustment.capture" data-test="adjustment-capture"><p class="mono">目标用户：{{adjustment.capture.userId}}</p>
  <RouterLink v-if="adjustment.capture.userId!==userId" :to="{name:'user-wallet',params:{userId:adjustment.capture.userId}}">返回原纠错用户</RouterLink>
  <p>{{assetLabels[adjustment.capture.command.asset]}} · 差额 {{adjustment.capture.command.delta}} · 已核对余额 {{adjustment.capture.balance}} → 预期 {{preview}}</p>
  <p>核对时流水序号 {{adjustment.capture.command.expected_seq}}</p><p>原因：{{adjustment.capture.command.reason}}</p>
  <p v-if="adjustment.stage==='prepared'">确认后立即尝试记账，请再次核对上述信息。</p>
  <button v-if="adjustment.stage==='prepared'" class="btn" data-test="confirm-adjustment" @click="adjustment.confirm()">确认执行这笔纠错</button>
  <button v-if="adjustment.stage==='prepared'" class="btn" @click="adjustment.dismiss()">取消未发送请求</button>
  <p v-if="adjustment.stage==='sending'">正在提交，请勿重复操作…</p>
  <button v-if="adjustment.stage==='unknown'" class="btn" data-test="retry-adjustment" @click="adjustment.retry()">使用原标识重试</button>
 </div>
 <p v-if="adjustment.message" role="status">{{adjustment.message}}</p>
 <div v-if="adjustment.receipt"><p class="mono">纠错记录：{{adjustment.receipt.id}}</p><p>原纠错后的余额 {{adjustment.receipt.balance_after}}；共 {{adjustment.receipt.operation_count}} 条记账操作。此数值是原回执，不覆盖当前余额。</p></div>
 <button v-if="adjustment.stage==='confirmed'||adjustment.stage==='rejected'" class="btn" data-test="dismiss-adjustment" @click="adjustment.dismiss()">已知晓，关闭本次记录</button>
 </div></section>
</template>
<style scoped>label{display:block;margin-block:.75rem}input,select,textarea{display:block;max-width:100%;width:100%}.panel{overflow-wrap:anywhere}</style>
