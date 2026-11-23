<script setup lang="ts">
/**
 * 登录页：邮箱 → 验证码两步。
 *
 * 服务端对 `POST /api/v1/auth/email-codes` 恒定返回 202，用于防账号枚举；
 * 所以这里**不**根据响应判断邮箱是否在白名单，白名单拦截只会在真正登录时
 * 以 403 REGISTRATION_NOT_ALLOWED 的形式出现，必须给出明确中文提示。
 */
import { computed, onBeforeUnmount, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import { describeError, errorCodeOf } from '@/api/error-messages'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const route = useRoute()
const router = useRouter()

type Step = 'email' | 'code'

const step = ref<Step>('email')
const email = ref('')
const code = ref('')
const busy = ref(false)
const errorText = ref('')
const noticeText = ref('')

/** 重发倒计时，来源是服务端返回的 resend_after_seconds，不自己猜。 */
const resendSeconds = ref(0)
let timer: ReturnType<typeof setInterval> | null = null

const emailLooksValid = computed(() => /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email.value.trim()))
const codeLooksValid = computed(() => /^\d{6}$/.test(code.value.trim()))
const canSendCode = computed(() => !busy.value && resendSeconds.value === 0 && emailLooksValid.value)
const canSubmitCode = computed(() => !busy.value && codeLooksValid.value)

function stopTimer(): void {
  if (timer !== null) {
    clearInterval(timer)
    timer = null
  }
}

function startCountdown(seconds: number): void {
  stopTimer()
  resendSeconds.value = Math.max(0, Math.floor(seconds))
  if (resendSeconds.value === 0) return
  timer = setInterval(() => {
    resendSeconds.value -= 1
    if (resendSeconds.value <= 0) {
      resendSeconds.value = 0
      stopTimer()
    }
  }, 1000)
}

onBeforeUnmount(stopTimer)

function friendlyError(error: unknown): string {
  // 白名单是最容易让用户困惑的错误，单独给一句能自解释的话。
  if (errorCodeOf(error) === 'REGISTRATION_NOT_ALLOWED') {
    return '这个邮箱还没有开通使用资格，请联系管理员加入白名单后再试。'
  }
  return describeError(error)
}

async function sendCode(): Promise<void> {
  if (!canSendCode.value) return
  busy.value = true
  errorText.value = ''
  noticeText.value = ''
  try {
    const accepted = await session.requestEmailCode(email.value.trim())
    step.value = 'code'
    startCountdown(accepted.resend_after_seconds)
    noticeText.value = '验证码已发送，请查看邮箱（含垃圾邮件文件夹）。'
  } catch (error) {
    errorText.value = friendlyError(error)
  } finally {
    busy.value = false
  }
}

async function submitCode(): Promise<void> {
  if (!canSubmitCode.value) return
  busy.value = true
  errorText.value = ''
  try {
    await session.login(email.value.trim(), code.value.trim())
    const redirect = typeof route.query.redirect === 'string' ? route.query.redirect : ''
    await router.replace(redirect || '/home')
  } catch (error) {
    errorText.value = friendlyError(error)
  } finally {
    busy.value = false
  }
}

function backToEmail(): void {
  step.value = 'email'
  code.value = ''
  errorText.value = ''
  noticeText.value = ''
}
</script>

<template>
  <section class="page login-page">
    <header class="login-welcome">
      <img src="/ui-art/login.jpg" alt="" class="page-illustration">
      <h1 class="page__title">我们的小空间</h1>
      <p class="page__subtitle">用邮箱验证码登录你们的小空间。</p>
    </header>

    <div class="card login-form">
      <template v-if="step === 'email'">
        <h2 class="card__title">第一步 · 邮箱</h2>
        <form class="stack" @submit.prevent="sendCode()">
          <div class="field">
            <label class="field__label" for="login-email">邮箱</label>
            <input
              id="login-email"
              v-model="email"
              class="field__input"
              type="email"
              name="email"
              autocomplete="email"
              inputmode="email"
              placeholder="you@example.com"
              :disabled="busy"
            />
          </div>

          <p v-if="errorText" class="hint hint--error" role="alert">{{ errorText }}</p>

          <button class="btn btn--block" type="submit" :disabled="!canSendCode">
            {{ busy ? '发送中…' : '发送验证码' }}
          </button>
          <p class="hint">登录即表示你同意仅将本应用用于两个人之间的事务记录。</p>
        </form>
      </template>

      <template v-else>
        <h2 class="card__title">第二步 · 验证码</h2>
        <p class="page__subtitle">已发送到 {{ email.trim() }}</p>
        <form class="stack" @submit.prevent="submitCode()">
          <div class="field">
            <label class="field__label" for="login-code">6 位验证码</label>
            <input
              id="login-code"
              v-model="code"
              class="field__input field__input--code"
              type="text"
              name="one-time-code"
              autocomplete="one-time-code"
              inputmode="numeric"
              maxlength="6"
              placeholder="······"
              :disabled="busy"
            />
          </div>

          <p v-if="noticeText" class="hint hint--ok" role="status">{{ noticeText }}</p>
          <p v-if="errorText" class="hint hint--error" role="alert">{{ errorText }}</p>

          <button class="btn btn--block" type="submit" :disabled="!canSubmitCode">
            {{ busy ? '登录中…' : '登录' }}
          </button>

          <div class="row">
            <button
              class="btn btn--ghost btn--small"
              type="button"
              :disabled="!canSendCode"
              @click="sendCode()"
            >
              {{ resendSeconds > 0 ? `重新发送（${resendSeconds}s）` : '重新发送' }}
            </button>
            <span class="spacer" />
            <button class="btn btn--ghost btn--small" type="button" :disabled="busy" @click="backToEmail()">
              换个邮箱
            </button>
          </div>
        </form>
      </template>
    </div>
  </section>
</template>
