/**
 * Service Worker 注册与"新版本可用"提示。
 *
 * - 使用 `virtual:pwa-register`（vite-plugin-pwa 提供的运行时封装）。
 * - 与 `registerType: 'prompt'` 配套：拿到 needRefresh 后**不自动**刷新，
 *   交给 UI 询问用户；用户确认才 `updateSW(true)`（内部会 postMessage SKIP_WAITING 并 reload）。
 * - 开发模式不注册：vite.config.ts 未开启 devOptions，此时 /sw.js 并不存在。
 */
import { computed, ref } from 'vue'
import type { Ref } from 'vue'
import { registerSW } from 'virtual:pwa-register'

export interface ServiceWorkerController {
  /** 有新版本已进入 waiting，等待用户确认刷新。 */
  needRefresh: Readonly<Ref<boolean>>
  /** 静态外壳已可离线使用。 */
  offlineReady: Readonly<Ref<boolean>>
  /** 用户确认：让 waiting 的 SW 接管并刷新页面。 */
  update: () => Promise<void>
  /** 用户选择"稍后"。 */
  dismiss: () => void
}

export function setupServiceWorker(): ServiceWorkerController {
  const needRefresh = ref(false)
  const offlineReady = ref(false)

  let updateSW: ((reloadPage?: boolean) => Promise<void>) | null = null

  const supported = typeof window !== 'undefined' && 'serviceWorker' in navigator

  if (supported && !import.meta.env.DEV) {
    updateSW = registerSW({
      immediate: true,
      onNeedRefresh() {
        needRefresh.value = true
      },
      onOfflineReady() {
        offlineReady.value = true
      },
      onRegisterError(error: unknown) {
        console.warn('[usapp/pwa] Service Worker 注册失败。', error)
      },
    })
  } else {
    console.info('[usapp/pwa] 当前环境不注册 Service Worker（开发模式或浏览器不支持）。')
  }

  async function update(): Promise<void> {
    if (!updateSW) {
      needRefresh.value = false
      return
    }
    await updateSW(true)
    needRefresh.value = false
  }

  function dismiss(): void {
    needRefresh.value = false
    offlineReady.value = false
  }

  return {
    needRefresh: computed(() => needRefresh.value),
    offlineReady: computed(() => offlineReady.value),
    update,
    dismiss,
  }
}
