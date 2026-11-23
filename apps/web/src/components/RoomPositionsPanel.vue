<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { ApiError } from '@/api/client'
import { describeError } from '@/api/error-messages'
import { changeRoomPosition, getRoomPositions, oppositeSide, sideLabel, type PositionAction, type RoomPositions } from '@/api/shared-room'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const emit = defineEmits<{ positions: [value: RoomPositions | null] }>()
const positions = ref<RoomPositions | null>(null)
const reading = ref(false)
const sending = ref(false)
const confirming = ref(false)
const error = ref('')
const notice = ref('')
const unresolved = ref<{ id: string; action: PositionAction } | null>(null)
let read: AbortController | null = null
let generation = 0
let poll: ReturnType<typeof setInterval> | undefined

async function refresh(): Promise<void> {
  if (!session.space || sending.value || read) return
  const controller = new AbortController()
  read = controller
  reading.value = true
  try {
    const next = await getRoomPositions(controller.signal)
    if (read !== controller) return
    if (next.pending || !next.paired || (positions.value && positions.value.side !== next.side)) closePreview()
    positions.value = next
    emit('positions', next)
  } catch (cause) {
    if (read === controller && !positions.value) error.value = describeError(cause)
  } finally {
    if (read === controller) { read = null; reading.value = false }
  }
}

function closePreview(): void {
  confirming.value = false
}
function submitRequest(): void { void send('request', crypto.randomUUID()) }

async function send(action: PositionAction, id: string): Promise<void> {
  if (sending.value) return
  const owner = generation
  read?.abort(); read = null; reading.value = false
  sending.value = true
  error.value = ''; notice.value = ''
  try {
    await changeRoomPosition(id, action)
    if (owner !== generation) return
    unresolved.value = null
    notice.value = { request: '换位申请已提交', accept: '已交换站位', decline: '已拒绝换位', cancel: '已撤回申请' }[action]
  } catch (cause) {
    if (owner !== generation) return
    const rejected = cause instanceof ApiError && cause.status >= 400 && cause.status < 500
    unresolved.value = rejected ? null : { id, action }
    error.value = rejected ? (cause.status === 409 ? '申请状态已变化，请查看最新站位。' : describeError(cause))
      : '操作结果待确认，请重试同一次操作。'
  } finally {
    if (owner === generation) {
      closePreview()
      sending.value = false
      positions.value = null
      void refresh()
    }
  }
}

watch([() => session.user?.id, () => session.space?.id], () => {
  generation++
  read?.abort(); read = null; reading.value = false; sending.value = false
  unresolved.value = null; positions.value = null; error.value = ''; notice.value = ''
  closePreview()
  emit('positions', null)
  void refresh()
}, { immediate: true })
function refreshWhenVisible(): void { if (!document.hidden) void refresh() }
onMounted(() => {
  window.addEventListener('focus', refreshWhenVisible)
  poll = setInterval(refreshWhenVisible, 15_000)
})
onBeforeUnmount(() => {
  generation++
  read?.abort(); read = null
  window.removeEventListener('focus', refreshWhenVisible)
  clearInterval(poll)
})
</script>

<template>
  <section class="card stack" aria-labelledby="room-position-title" data-test="room-positions">
    <h2 class="card__title" id="room-position-title">小房间站位</h2>
    <p v-if="positions">我在{{ sideLabel(positions.side) }}</p>
    <p v-if="notice" class="hint hint--ok" role="status">{{ notice }}</p>
    <p v-if="error" class="hint hint--error" role="alert">{{ error }}</p>
    <button v-if="unresolved" type="button" class="btn btn--soft" :disabled="sending"
      @click="send(unresolved.action, unresolved.id)">重试确认换位结果</button>
    <template v-else-if="positions">
      <p v-if="!positions.paired" class="card__hint">对方加入后可以申请换位。</p>
      <template v-else-if="positions.pending?.incoming">
        <p>对方想与你换位。</p>
        <div class="row">
          <button type="button" class="btn" :disabled="sending" @click="send('accept', positions.pending!.id)">同意换位</button>
          <button type="button" class="btn btn--soft" :disabled="sending" @click="send('decline', positions.pending!.id)">拒绝</button>
        </div>
      </template>
      <template v-else-if="positions.pending">
        <p>等待对方确认</p>
        <button type="button" class="btn btn--soft" :disabled="sending" @click="send('cancel', positions.pending!.id)">撤回申请</button>
      </template>
      <template v-else-if="confirming">
        <p>换位后你将在{{ sideLabel(oppositeSide(positions.side)) }}。</p>
        <slot name="preview" />
        <div class="row">
          <button type="button" class="btn" :disabled="sending" @click="submitRequest()">发出申请</button>
          <button type="button" class="btn btn--soft" :disabled="sending" @click="closePreview()">取消预览</button>
        </div>
      </template>
      <button v-else type="button" class="btn btn--soft" @click="confirming = true">申请换位</button>
    </template>
    <p v-else-if="reading || sending" class="card__hint">正在确认站位…</p>
    <button v-else type="button" class="btn btn--soft" @click="refresh()">重新读取站位</button>
  </section>
</template>
