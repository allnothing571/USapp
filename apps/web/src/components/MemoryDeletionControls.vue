<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import type { Memory } from '@/api/types'
import { useMemoryDeletionsStore } from '@/stores/memory-deletions'
import { useSessionStore } from '@/stores/session'

const props = defineProps<{ memoryId: string; memory: Memory | null; busy: boolean }>()
const store = useMemoryDeletionsStore()
const session = useSessionStore()
const action = computed(() => store.actions[props.memoryId])
const creator = computed(() => props.memory?.creator_id === session.user?.id)
const pending = computed(() => props.memory?.pending_delete_request)
const confirming = ref<'delete' | 'approve' | 'request' | null>(null)
watch([() => props.memoryId, () => session.user?.id], () => { confirming.value = null })
function confirm(): void {
  const kind = confirming.value
  confirming.value = null
  if (kind === 'approve' && pending.value) void store.submit({ kind: 'decide', memoryId: props.memoryId,
    requestId: pending.value.id, requesterId: pending.value.requester_id, outcome: 'approve' })
  else if (kind === 'delete' || kind === 'request') void store.submit({ kind, memoryId: props.memoryId })
}
function decide(outcome: 'reject' | 'withdraw'): void {
  if (pending.value) void store.submit({ kind: 'decide', memoryId: props.memoryId, requestId: pending.value.id,
    requesterId: pending.value.requester_id, outcome })
}
</script>

<template>
  <div class="card stack" data-test="memory-deletion-controls">
    <p v-if="store.notice" class="card__hint" role="status">{{ store.notice }}</p>
    <template v-if="action">
      <p class="hint hint--error" role="alert">{{ action.error || '正在提交…' }}</p>
      <p class="card__hint">保留的是原回忆和原删除轮次的请求。</p>
      <button v-if="action.state !== 'sending'" class="btn btn--small" type="button" data-test="memory-deletion-retry" @click="store.retry(memoryId)">重试原请求</button>
    </template>
    <template v-else-if="memory">
      <template v-if="confirming">
        <p v-if="confirming === 'request'" class="card__hint">提交删除申请后，由创建者决定。申请处理前不能修改正文。</p>
        <p v-else class="card__hint" data-test="memory-delete-explanation">
          删除后双方都不能打开这条回忆。创建者自己上传且未单独删除的照片会随之删除，伴侣上传的照片由伴侣保留和管理。
          创建者可在 7 天内撤销这一轮删除及其关联照片的删除；过期后不能撤销，内容可能进入不可恢复的清除流程。
          已签发的链接可能暂时有效，下载或保存的副本不会被收回。原计划自己的文字会保留。
        </p>
        <div class="row">
          <button class="btn btn--small" type="button" :disabled="busy" data-test="memory-delete-confirm" @click="confirm">确认</button>
          <button class="btn btn--small btn--ghost" type="button" @click="confirming = null">取消</button>
        </div>
      </template>
      <template v-else>
        <p v-if="pending" class="card__hint">有一条待处理的删除申请，正文编辑暂时关闭。</p>
        <div v-if="pending" class="row">
          <template v-if="creator">
            <button class="btn btn--small" type="button" :disabled="busy" data-test="memory-delete-approve" @click="confirming = 'approve'">同意删除</button>
            <button class="btn btn--small btn--soft" type="button" :disabled="busy" data-test="memory-delete-reject" @click="decide('reject')">拒绝申请</button>
          </template>
          <button v-else-if="pending.requester_id === session.user?.id" class="btn btn--small btn--soft" type="button" :disabled="busy"
            data-test="memory-delete-withdraw" @click="decide('withdraw')">撤回删除申请</button>
        </div>
        <button v-if="creator" class="btn btn--small btn--ghost" type="button" :disabled="busy" data-test="memory-delete" @click="confirming = 'delete'">删除整条回忆</button>
        <button v-else-if="!pending && !memory.pending_proposal" class="btn btn--small btn--ghost" type="button" :disabled="busy"
          data-test="memory-request-delete" @click="confirming = 'request'">请求删除回忆</button>
      </template>
    </template>
  </div>
</template>
