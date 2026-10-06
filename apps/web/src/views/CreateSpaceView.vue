<script setup lang="ts">
/**
 * 创建空间。
 *
 * 流程：POST /api/v1/spaces（**不带请求体**，契约没有声明 requestBody）
 *      → 拿到 space → 立刻 POST /api/v1/spaces/current/invites 生成第一个邀请
 *      → 展示明文邀请码与邀请链接，并提供复制。
 *
 * 明文邀请码只在创建响应里出现这一次，因此必须当场让用户复制走；
 * 刷新页面后就只能重新生成。这里不写任何本地存储。
 */
import { computed, ref } from 'vue'
import { useRouter } from 'vue-router'

import { describeError, errorCodeOf } from '@/api/error-messages'
import { useSessionStore } from '@/stores/session'
import { useSpaceStore } from '@/stores/space'

const session = useSessionStore()
const spaceStore = useSpaceStore()
const router = useRouter()

const busy = ref(false)
const errorText = ref('')
const copied = ref('')
let copiedTimer: ReturnType<typeof setTimeout> | null = null

const createdInvite = computed(() => spaceStore.lastCreatedInvite)
const space = computed(() => spaceStore.space)
const inviteLink = computed(() => createdInvite.value?.invite_url ?? '')

function flashCopied(label: string): void {
  copied.value = label
  if (copiedTimer !== null) clearTimeout(copiedTimer)
  copiedTimer = setTimeout(() => {
    copied.value = ''
    copiedTimer = null
  }, 2000)
}

/**
 * 复制文本。优先用 Clipboard API（需要安全上下文）；
 * 不可用时**不伪造成功**，而是把文案留在页面上让用户手动长按复制。
 */
async function copyText(text: string, label: string): Promise<void> {
  if (!text) return
  try {
    if (navigator.clipboard?.writeText && window.isSecureContext) {
      await navigator.clipboard.writeText(text)
      flashCopied(label)
      return
    }
  } catch {
    // 落到下面的手动提示。
  }
  errorText.value = '当前环境无法自动复制，请长按上方内容手动复制。'
}

async function createSpace(): Promise<void> {
  if (busy.value) return
  busy.value = true
  errorText.value = ''
  try {
    const next = await spaceStore.create()
    session.setSpace(next)
    // 立刻生成第一个邀请，让用户当场就能分享出去。
    await spaceStore.createInvite()
  } catch (error) {
    const code = errorCodeOf(error)
    if (code === 'SPACE_ALREADY_EXISTS' || code === 'ALREADY_IN_SPACE') {
      // 已经有一个空间：不报错，直接同步现状。
      await spaceStore.loadCurrent()
      session.setSpace(spaceStore.space)
      errorText.value = '你已经有一个空间了。'
    } else {
      errorText.value = describeError(error)
    }
  } finally {
    busy.value = false
  }
}

/** 空间已存在但还没有邀请码时，单独生成一个（不会被误当成重新创建空间）。 */
async function generateInvite(): Promise<void> {
  if (busy.value) return
  busy.value = true
  errorText.value = ''
  try {
    await spaceStore.createInvite()
  } catch (error) {
    errorText.value = describeError(error)
  } finally {
    busy.value = false
  }
}

function goHome(): void {
  void router.replace({ name: 'home' })
}
</script>

<template>
  <section class="page">
    <header>
      <h1 class="page__title">创建你们的小空间</h1>
      <p class="page__subtitle">一个空间最多两个人，时区固定为 Asia/Shanghai。</p>
    </header>

    <div v-if="!space" class="card">
      <h2 class="card__title">还没有空间</h2>
      <p class="card__hint">
        创建后会立刻生成一个邀请码，把它发给对方即可加入。空间一旦创建，成员上限为 2 人。
      </p>
      <p v-if="errorText" class="hint hint--error" role="alert">{{ errorText }}</p>
      <button class="btn btn--block" type="button" :disabled="busy" @click="createSpace()">
        {{ busy ? '创建中…' : '创建空间' }}
      </button>
    </div>

    <template v-else>
      <div class="card">
        <h2 class="card__title">空间已创建</h2>
        <dl class="kv">
          <dt class="kv__key">空间名称</dt>
          <dd class="kv__value">{{ space.name }}</dd>
          <dt class="kv__key">时区</dt>
          <dd class="kv__value">{{ space.timezone }}</dd>
          <dt class="kv__key">成员</dt>
          <dd class="kv__value">{{ space.member_count }} / 2</dd>
        </dl>
      </div>

      <div class="card">
        <h2 class="card__title">邀请对方加入</h2>

        <template v-if="createdInvite">
          <div class="code-box">
            <span class="code-box__value">{{ createdInvite.code }}</span>
            <button
              class="btn btn--soft btn--small"
              type="button"
              @click="copyText(createdInvite.code, '邀请码')"
            >
              复制
            </button>
          </div>

          <div class="code-box">
            <span class="code-box__value">{{ inviteLink }}</span>
            <button
              class="btn btn--soft btn--small"
              type="button"
              @click="copyText(inviteLink, '邀请链接')"
            >
              复制
            </button>
          </div>

          <p class="hint">邀请码只在这里显示这一次，离开本页后将无法再次查看。</p>
          <p class="hint">
            有效期至 {{ new Date(createdInvite.expires_at).toLocaleString('zh-CN') }}，可用
            {{ createdInvite.max_uses }} 次。
          </p>
          <p v-if="copied" class="hint hint--ok" role="status">{{ copied }}已复制</p>
        </template>

        <template v-else>
          <p class="card__hint">还没有可用的邀请码。</p>
          <button
            class="btn btn--block btn--soft"
            type="button"
            :disabled="busy"
            @click="generateInvite()"
          >
            生成邀请码
          </button>
        </template>

        <p v-if="errorText" class="hint hint--error" role="alert">{{ errorText }}</p>
      </div>

      <button class="btn btn--block btn--ghost" type="button" @click="goHome()">进入首页</button>
    </template>
  </section>
</template>
