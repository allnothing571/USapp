<script setup lang="ts">
/**
 * P2-5a Me-page device reminders card (docs/phase2-web-push.md Section 9).
 *
 * Reads the server gate and the caller's devices on mount; it never asks for notification
 * permission on load (only the "开启" button does, and only when the gate is open). In P2-5a the
 * gate is always closed outside tests: no service worker advertises push:v1 and there is no
 * transport. Lists show no endpoint or key. Unknown results keep their original request and are
 * only resent by the explicit retry button.
 *
 * The device list is the server's COMPLETE set of slot-occupying generations (including old-key
 * ones that still occupy a slot), so every device the user may need to revoke is shown; the
 * history below it is bounded and informational. The empty state is shown only after a
 * successful list.
 */
import { computed, onBeforeUnmount, onMounted } from 'vue'

import type { PushGeneration } from '@/api/push'
import { usePushStore } from '@/stores/push'

const push = usePushStore()

const PROVIDERS: Record<PushGeneration['provider'], string> = {
  fcm: 'Chrome / Android', mozilla: 'Firefox', apple: 'Safari / iOS', windows: 'Windows',
}
const STATES: Record<PushGeneration['state'], string> = {
  active: '已开启', inactive: '未生效（旧密钥，仍占用名额）', revoked: '已关闭',
}
const HISTORY: Record<PushGeneration['state'], string> = {
  active: '已开启', inactive: '已退出登录，不占用名额', revoked: '已关闭',
}

const gateText = computed(() => {
  switch (push.gate) {
    case 'unsupported': return '此浏览器不支持设备提醒。'
    case 'closed': return '设备提醒尚未开放，开放后可以在这里开启。'
    case 'denied': return '浏览器已禁止通知。如需开启，请先在浏览器设置中允许通知。'
    case 'offline': return '当前离线，联网后才能开启或关闭设备提醒。'
    case 'error': return push.loadError
    case 'loading': return '正在读取设备提醒设置…'
    default: return ''
  }
})


function label(g: PushGeneration): string {
  return `${PROVIDERS[g.provider]}${g.user_agent_family ? ` · ${g.user_agent_family}` : ''}`
}

function onOnline(): void { push.setOnline(true) }
function onOffline(): void { push.setOnline(false) }

onMounted(() => {
  push.setOnline(typeof navigator === 'undefined' ? true : navigator.onLine !== false)
  window.addEventListener('online', onOnline)
  window.addEventListener('offline', onOffline)
  void push.load()
})
onBeforeUnmount(() => {
  window.removeEventListener('online', onOnline)
  window.removeEventListener('offline', onOffline)
})
</script>

<template>
  <div class="card" data-test="push-card">
    <h2 class="card__title">设备提醒</h2>
    <p v-if="gateText" class="card__hint" data-test="push-gate">{{ gateText }}</p>
    <p v-if="push.notice" class="hint" role="status" data-test="push-notice">{{ push.notice }}</p>

    <div v-if="push.registerAction" class="stack" data-test="push-register-action">
      <p class="hint" :class="{ 'hint--error': push.registerAction.status !== 'submitting' }">
        {{ push.registerAction.status === 'submitting' ? '正在开启…' : push.registerAction.error }}
      </p>
      <button
        v-if="push.registerAction.status !== 'submitting'"
        class="btn btn--small"
        type="button"
        data-test="push-retry"
        @click="push.retryRegister()"
      >重试</button>
    </div>
    <button
      v-else
      class="btn btn--small btn--soft"
      type="button"
      data-test="push-enable"
      :disabled="push.gate !== 'available'"
      @click="push.enable()"
    >在此设备开启提醒</button>

    <template v-if="push.listed">
      <p class="card__hint" data-test="push-count">已占用 {{ push.occupiedSlots }} / {{ push.deviceLimit }} 个设备名额</p>
      <ul v-if="push.devices.length" class="list">
        <li v-for="g in push.devices" :key="g.id" class="list__item" data-test="push-device">
          <div class="list__main">
            <div class="list__title">{{ label(g) }}{{ g.current_session ? '（当前登录）' : '' }}</div>
            <div class="list__meta">{{ STATES[g.state] }}</div>
            <div v-if="push.revokeActions[g.id] && push.revokeActions[g.id].status !== 'submitting'" class="hint hint--error">
              {{ push.revokeActions[g.id].error }}
            </div>
          </div>
          <button
            class="btn btn--small btn--ghost"
            type="button"
            data-test="push-revoke"
            :disabled="push.revokeActions[g.id]?.status === 'submitting' || push.gate === 'offline'"
            @click="push.revoke(g.id)"
          >{{ push.revokeActions[g.id] && push.revokeActions[g.id].status !== 'submitting' ? '重试关闭' : '关闭' }}</button>
        </li>
      </ul>
      <p v-else class="card__hint" data-test="push-empty">当前没有设备占用名额。</p>
      <template v-if="push.history.length">
        <h3 class="card__hint">最近记录（最多 20 条）</h3>
        <ul class="list" data-test="push-history">
          <li v-for="g in push.history" :key="g.id" class="list__item list__item--muted">
            <div class="list__main">
              <div class="list__title">{{ label(g) }}</div>
              <div class="list__meta">{{ g.revoked_reason === 'gone' ? '推送服务报告此订阅已失效' : HISTORY[g.state] }}</div>
            </div>
          </li>
        </ul>
      </template>
    </template>
  </div>
</template>
