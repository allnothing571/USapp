<script setup lang="ts">
/**
 * 手动输入邀请码加入空间。
 *
 * 走 `POST /api/v1/invites/{code}/accept`，成功后把空间写回会话快照再进首页。
 * 这里不做任何乐观假设：只有服务端 200 才算加入成功。
 */
import { computed, ref } from 'vue'
import { useRouter } from 'vue-router'

import { describeError, errorCodeOf } from '@/api/error-messages'
import { useSessionStore } from '@/stores/session'
import { useSpaceStore } from '@/stores/space'

const session = useSessionStore()
const spaceStore = useSpaceStore()
const router = useRouter()

const code = ref('')
const busy = ref(false)
const errorText = ref('')

const normalized = computed(() => code.value.trim().toUpperCase())
const canSubmit = computed(() => !busy.value && normalized.value.length >= 4)

function friendlyError(error: unknown): string {
  switch (errorCodeOf(error)) {
    case 'INVITE_INVALID_OR_EXPIRED':
      return '邀请码无效或已过期，请让对方重新生成一个。'
    case 'INVITE_REVOKED':
      return '这个邀请码已被撤销。'
    case 'SPACE_FULL':
      return '对方的空间已经满员了（上限 2 人）。'
    case 'ALREADY_IN_SPACE':
      return '你已经在一个空间里了。'
    default:
      return describeError(error)
  }
}

async function join(): Promise<void> {
  if (!canSubmit.value) return
  busy.value = true
  errorText.value = ''
  try {
    const space = await spaceStore.accept(normalized.value)
    session.setSpace(space)
    await router.replace({ name: 'home' })
  } catch (error) {
    errorText.value = friendlyError(error)
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <section class="page">
    <header>
      <h1 class="page__title">加入对方的空间</h1>
      <p class="page__subtitle">把对方发来的邀请码填进来。</p>
    </header>

    <div class="card">
      <form class="stack" @submit.prevent="join()">
        <div class="field">
          <label class="field__label" for="join-code">邀请码</label>
          <input
            id="join-code"
            v-model="code"
            class="field__input field__input--code"
            type="text"
            name="invite-code"
            autocomplete="off"
            autocapitalize="characters"
            spellcheck="false"
            placeholder="邀请码"
            :disabled="busy"
          />
        </div>

        <p v-if="errorText" class="hint hint--error" role="alert">{{ errorText }}</p>

        <button class="btn btn--block" type="submit" :disabled="!canSubmit">
          {{ busy ? '加入中…' : '加入空间' }}
        </button>
      </form>
    </div>

    <div class="card card--soft">
      <p class="card__hint">
        如果你收到的是一个链接，直接点开链接即可自动加入，不用手抄邀请码。
      </p>
    </div>
  </section>
</template>
