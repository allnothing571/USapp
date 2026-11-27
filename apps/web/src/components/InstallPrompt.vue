<script setup lang="ts">
/**
 * 安装引导。
 *
 * 三种状态：
 *   1. 已经以 standalone 运行（已安装）→ 什么都不显示。
 *   2. Chromium 系：捕获 `beforeinstallprompt`，显示"安装到桌面"按钮，点击后调 prompt()。
 *   3. iOS Safari（没有 beforeinstallprompt）：显示"分享 → 添加到主屏幕"的手动指引。
 *
 * 用户点击"以后再说"后本次会话不再打扰（只是组件内的内存标记，不写任何存储）。
 */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'

/** Chromium 的安装提示事件，DOM lib 里没有，这里就地声明最小结构。 */
interface BeforeInstallPromptEvent extends Event {
  readonly platforms: readonly string[]
  prompt: () => Promise<void>
  readonly userChoice: Promise<{ outcome: 'accepted' | 'dismissed'; platform: string }>
}

const deferredPrompt = ref<BeforeInstallPromptEvent | null>(null)
const isStandalone = ref(false)
const isIos = ref(false)
const dismissed = ref(false)
const busy = ref(false)

const showInstallButton = computed(
  () => !dismissed.value && !isStandalone.value && deferredPrompt.value !== null,
)
const showIosHint = computed(
  () =>
    !dismissed.value &&
    !isStandalone.value &&
    deferredPrompt.value === null &&
    isIos.value,
)
const visible = computed(() => showInstallButton.value || showIosHint.value)

function detectStandalone(): boolean {
  if (typeof window === 'undefined') return false
  const media = typeof window.matchMedia === 'function' ? window.matchMedia('(display-mode: standalone)') : null
  const nav = window.navigator as Navigator & { standalone?: boolean }
  return media?.matches === true || nav.standalone === true
}

function detectIos(): boolean {
  if (typeof navigator === 'undefined') return false
  const ua = navigator.userAgent
  const iOSDevice = /iPad|iPhone|iPod/.test(ua)
  // iPadOS 13+ 默认伪装成 macOS，靠触摸点数补判。
  const iPadOs = navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1
  return iOSDevice || iPadOs
}

function onBeforeInstallPrompt(event: Event): void {
  // 拦下浏览器自带的迷你提示条，改由我们自己的按钮在合适的时机触发。
  event.preventDefault()
  deferredPrompt.value = event as BeforeInstallPromptEvent
}

function onInstalled(): void {
  deferredPrompt.value = null
  isStandalone.value = true
}

async function install(): Promise<void> {
  const prompt = deferredPrompt.value
  if (!prompt || busy.value) return
  busy.value = true
  try {
    await prompt.prompt()
    await prompt.userChoice
  } catch {
    // 用户取消或浏览器拒绝：静默收场，不假装安装成功。
  } finally {
    // prompt() 只能消费一次，无论结果如何都释放掉。
    deferredPrompt.value = null
    busy.value = false
    dismissed.value = true
  }
}

function dismiss(): void {
  dismissed.value = true
}

onMounted(() => {
  isStandalone.value = detectStandalone()
  isIos.value = detectIos()
  window.addEventListener('beforeinstallprompt', onBeforeInstallPrompt)
  window.addEventListener('appinstalled', onInstalled)
})

onBeforeUnmount(() => {
  window.removeEventListener('beforeinstallprompt', onBeforeInstallPrompt)
  window.removeEventListener('appinstalled', onInstalled)
})

// 供父组件/测试使用：暴露当前是否可安装（Chromium 路径）。
defineExpose({ installable: showInstallButton })
</script>

<template>
  <aside v-if="visible" class="install-prompt" aria-live="polite">
    <div class="install-prompt__row">
      <span v-if="showInstallButton" class="install-prompt__text">
        把「今与久」装到桌面，打开更快
      </span>
      <span v-else class="install-prompt__text">
        想装到桌面？点底部分享，再选「添加到主屏幕」
      </span>

      <button
        v-if="showInstallButton"
        type="button"
        class="btn btn--soft btn--small"
        :disabled="busy"
        @click="install()"
      >
        安装
      </button>
      <button type="button" class="btn btn--ghost btn--small" @click="dismiss()">以后再说</button>
    </div>
  </aside>
</template>
