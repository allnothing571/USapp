<script setup lang="ts">
import { onBeforeUnmount, ref, watch } from 'vue'
import { useRouter } from 'vue-router'

import { describeError, errorCodeOf } from '@/api/error-messages'
import { previewInvite } from '@/api/space'
import { oppositeSide, sideLabel, type RoomSide } from '@/api/shared-room'
import { useSessionStore } from '@/stores/session'
import { useSpaceStore } from '@/stores/space'

const props = defineProps<{ code: string }>()

const session = useSessionStore()
const spaceStore = useSpaceStore()
const router = useRouter()

const busy = ref(false)
const errorText = ref('')
const done = ref(false)
const side = ref<RoomSide | null>(null)
const loading = ref(false)
let pending: AbortController | null = null

function friendlyError(error: unknown): string {
  switch (errorCodeOf(error)) {
    case 'INVITE_INVALID_OR_EXPIRED':
      return '这个邀请链接无效或已过期，请让对方重新生成。'
    case 'INVITE_REVOKED':
      return '这个邀请已被撤销。'
    case 'SPACE_FULL':
      return '对方的空间已经满员了（上限 2 人）。'
    case 'ALREADY_IN_SPACE':
      return '你已经在一个空间里了。'
    default:
      return describeError(error)
  }
}

async function accept(): Promise<void> {
  if (busy.value || !side.value) return
  busy.value = true
  errorText.value = ''
  try {
    const space = await spaceStore.accept(props.code)
    session.setSpace(space)
    done.value = true
    await router.replace({ name: 'home' })
  } catch (error) {
    errorText.value = friendlyError(error)
  } finally {
    busy.value = false
  }
}

async function readInvite(): Promise<void> {
  pending?.abort()
  const read = new AbortController()
  pending = read
  side.value = null
  errorText.value = ''
  loading.value = false
  if (session.status !== 'authenticated') return
  if (session.space) { errorText.value = '你已经在一个空间里了。'; return }
  loading.value = true
  try {
    const result = await previewInvite(props.code, read.signal)
    if (pending === read) side.value = result
  } catch (error) {
    if (pending === read) errorText.value = friendlyError(error)
  } finally {
    if (pending === read) loading.value = false
  }
}
watch([() => props.code, () => session.status, () => session.user?.id], readInvite, { immediate: true })
onBeforeUnmount(() => { pending?.abort(); pending = null })
</script>

<template>
  <section class="page">
    <header>
      <h1 class="page__title">收到一个邀请</h1>
      <p class="page__subtitle">邀请码 {{ code }}</p>
    </header>

    <div class="card">
      <template v-if="session.status !== 'authenticated'">
        <h2 class="card__title">请先登录</h2>
        <p class="card__hint">登录后可以查看站位并接受邀请。</p>
      </template>

      <template v-else-if="done">
        <h2 class="card__title">已加入</h2>
        <p class="card__hint">正在进入首页…</p>
      </template>

      <template v-else>
        <h2 class="card__title">{{ busy ? '正在接受邀请…' : '接受邀请' }}</h2>
        <p v-if="loading" class="card__hint" role="status">正在读取邀请…</p>
        <p v-else-if="side" class="card__hint">加入后你站{{ sideLabel(side) }}，对方站{{ sideLabel(oppositeSide(side)) }}。</p>
      </template>

      <p v-if="errorText" class="hint hint--error" role="alert">{{ errorText }}</p>

      <button
        v-if="session.status === 'authenticated' && !done && side"
        class="btn btn--block"
        type="button"
        :disabled="busy"
        @click="accept()"
      >
        {{ busy ? '处理中…' : '接受邀请' }}
      </button>
      <button v-else-if="errorText && !session.space && !loading" class="btn btn--soft"
        type="button" @click="readInvite()">重新查看邀请</button>
    </div>
  </section>
</template>
