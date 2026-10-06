<script setup lang="ts">
/**
 * 邀请链接落地页：/invite/:code
 *
 * 这个路由在"已认证但还没有空间"时也保持可达（见 router 的 ALLOWED_WITHOUT_SPACE），
 * 因此到达这里时分两种情况：
 *   - 已认证：挂载后自动接受邀请。
 *   - 未认证：守卫会先带去 /login 并带上 redirect，登录后回到本页再自动接受。
 *
 * 自动接受必须**幂等**：同一次挂载只尝试一次，失败后改为手动重试按钮，
 * 避免用户反复刷新触发重复请求。
 */
import { onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'

import { describeError, errorCodeOf } from '@/api/error-messages'
import { useSessionStore } from '@/stores/session'
import { useSpaceStore } from '@/stores/space'

const props = defineProps<{ code: string }>()

const session = useSessionStore()
const spaceStore = useSpaceStore()
const router = useRouter()

const busy = ref(false)
const errorText = ref('')
const done = ref(false)
let attempted = false

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
  if (busy.value) return
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

onMounted(() => {
  if (attempted) return
  attempted = true
  if (session.status === 'authenticated') {
    void accept()
  }
})
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
        <p class="card__hint">登录后会自动接受这个邀请。</p>
      </template>

      <template v-else-if="done">
        <h2 class="card__title">已加入</h2>
        <p class="card__hint">正在进入首页…</p>
      </template>

      <template v-else>
        <h2 class="card__title">{{ busy ? '正在接受邀请…' : '接受邀请' }}</h2>
        <p class="card__hint">确认后你将成为该空间的第二位成员。</p>
      </template>

      <p v-if="errorText" class="hint hint--error" role="alert">{{ errorText }}</p>

      <button
        v-if="session.status === 'authenticated' && !done"
        class="btn btn--block"
        type="button"
        :disabled="busy"
        @click="accept()"
      >
        {{ busy ? '处理中…' : '接受邀请' }}
      </button>
    </div>
  </section>
</template>
