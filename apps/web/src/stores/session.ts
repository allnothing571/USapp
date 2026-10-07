/**
 * 会话 store。
 *
 * 硬约束：
 * - 全部是**内存态**。不使用 pinia 持久化插件，不写 localStorage / sessionStorage / cookie。
 * - `csrfToken` 是 CSRF 令牌在前端的唯一副本，任何变化都必须同步推给
 *   api/csrf.ts 的内存持有者（HTTP 层从那里读）。
 * - 注销与切换账号必须调用 `resetBusinessState()`，清空 user / space / csrfToken
 *   以及其它 store 的业务数据，并回到 `anonymous`。
 *   新增业务 store 时，请把它的 reset 一并登记进 `resetBusinessState()`。
 */
import { defineStore } from 'pinia'
import { computed, ref } from 'vue'

import * as authApi from '@/api/auth'
import { ApiError } from '@/api/client'
import { setCsrfToken } from '@/api/csrf'
import type { EmailCodeAccepted, SessionBootstrap, Space, User } from '@/api/types'
import { useSpaceStore } from './space'

/** 会话状态机：unknown（未引导）→ anonymous | authenticated。 */
export type SessionStatus = 'unknown' | 'anonymous' | 'authenticated'

export const useSessionStore = defineStore('session', () => {
  const status = ref<SessionStatus>('unknown')
  const user = ref<User | null>(null)
  const space = ref<Space | null>(null)
  const csrfToken = ref<string | null>(null)
  const preAuthExpiresAt = ref<string | null>(null)
  /** 是否已经跑过一次会话引导（无论成功还是退化为匿名）。 */
  const bootstrapped = ref(false)

  const isAuthenticated = computed(() => status.value === 'authenticated')
  const hasSpace = computed(() => space.value !== null)
  const isAnonymous = computed(() => status.value === 'anonymous')

  /** 只更新会话快照，不碰业务 store。 */
  function applySessionSnapshot(): void {
    setCsrfToken(csrfToken.value)
  }

  /**
   * 把服务端 SessionBootstrap 落到 store。
   * 同时负责：CSRF 令牌轮换落库、space store 的 hydrate/reset。
   */
  function applyBootstrap(payload: SessionBootstrap): void {
    csrfToken.value = payload.csrf_token
    setCsrfToken(payload.csrf_token)
    preAuthExpiresAt.value = payload.pre_auth_expires_at ?? null

    const spaceStore = useSpaceStore()

    if (payload.authenticated) {
      status.value = 'authenticated'
      user.value = payload.user ?? null
      space.value = payload.space ?? null
      spaceStore.hydrate(payload.space ?? null)
    } else {
      status.value = 'anonymous'
      user.value = null
      space.value = null
      spaceStore.reset()
    }
  }

  /**
   * 清空所有业务态。注销、切换账号、会话失效都必须走这里。
   * 注意：这里**不**发任何网络请求，纯本地清理。
   */
  function resetBusinessState(): void {
    user.value = null
    space.value = null
    csrfToken.value = null
    preAuthExpiresAt.value = null
    setCsrfToken(null)
    status.value = 'anonymous'
    // 其它业务 store 的清理入口统一登记在这里。
    useSpaceStore().reset()
  }

  let inFlight: Promise<void> | null = null

  async function runBootstrap(): Promise<void> {
    try {
      const payload = await authApi.bootstrapSession()
      applyBootstrap(payload)
    } catch (error) {
      // 引导失败时**不臆造**已登录状态，也保留不任何业务数据：fail closed。
      console.warn('[usapp/session] 会话引导失败，已按未认证处理。', error)
      status.value = 'anonymous'
      user.value = null
      space.value = null
      csrfToken.value = null
      preAuthExpiresAt.value = null
      setCsrfToken(null)
      useSpaceStore().reset()
    } finally {
      bootstrapped.value = true
    }
  }

  /**
   * `GET /api/v1/session`。并发调用会复用同一个在途请求；
   * 已经引导过则默认直接返回，`force: true` 可强制重新引导。
   */
  function bootstrap(force = false): Promise<void> {
    if (inFlight) return inFlight
    if (bootstrapped.value && !force) return Promise.resolve()
    inFlight = runBootstrap().finally(() => {
      inFlight = null
    })
    return inFlight
  }

  /** 写请求前确保手里有 CSRF 令牌；没有就先引导一次。 */
  async function ensureCsrfToken(): Promise<void> {
    if (csrfToken.value) return
    await bootstrap(true)
    if (!csrfToken.value) {
      throw new ApiError(
        'CSRF_TOKEN_INVALID',
        '无法获取安全令牌：会话引导未成功，请检查网络后重试。',
        0,
      )
    }
  }

  /** `POST /api/v1/auth/email-codes` */
  async function requestEmailCode(email: string, signal?: AbortSignal): Promise<EmailCodeAccepted> {
    await ensureCsrfToken()
    return authApi.requestEmailCode(email, signal)
  }

  /**
   * `POST /api/v1/auth/sessions`
   * 成功后服务端会轮换会话标识与 CSRF 令牌，applyBootstrap 会一并覆盖本地旧值。
   */
  async function login(email: string, code: string): Promise<void> {
    await ensureCsrfToken()
    const payload = await authApi.createSession(email, code)
    applyBootstrap(payload)
  }

  /** 用 `GET /api/v1/me` 刷新用户与空间快照。 */
  async function refreshMe(): Promise<void> {
    const payload = await authApi.getMe()
    user.value = payload.user
    space.value = payload.space ?? null
    useSpaceStore().hydrate(payload.space ?? null)
  }

  /**
   * `PATCH /api/v1/me` —— 修改自己的昵称。
   * 只有服务端确认成功后才用返回的快照更新 user 与成员槽位；
   * 任何失败（校验、离线、网络、CSRF）都原样抛出，本地状态保持不变。
   */
  async function updateDisplayName(displayName: string): Promise<void> {
    await ensureCsrfToken()
    const payload = await authApi.updateMe({ display_name: displayName })
    user.value = payload.user
    space.value = payload.space ?? null
    useSpaceStore().hydrate(payload.space ?? null)
  }

  /** 把新空间写回会话快照（创建 / 接受邀请成功后调用）。 */
  function setSpace(next: Space | null): void {
    space.value = next
    useSpaceStore().hydrate(next)
  }

  /**
   * `DELETE /api/v1/auth/sessions`。
   *
   * 无论服务端是否成功撤销，本地业务态一律清空——否则离线或网络故障会把用户
   * 锁在"看起来已登录"的状态里。但服务端撤销失败时**不谎报成功**：
   * 打印警告，且此时服务端会话可能仍然有效，由负责人按需处理。
   */
  async function logout(): Promise<void> {
    let serverError: unknown = null
    try {
      await authApi.deleteSession()
    } catch (error) {
      serverError = error
    } finally {
      resetBusinessState()
    }

    if (serverError) {
      console.warn(
        '[usapp/session] 注销请求未成功送达服务端，本地业务态已清空；' +
          '服务端会话可能仍然有效。',
        serverError,
      )
    }
  }

  return {
    // state
    status,
    user,
    space,
    csrfToken,
    preAuthExpiresAt,
    bootstrapped,
    // getters
    isAuthenticated,
    hasSpace,
    isAnonymous,
    // actions
    applyBootstrap,
    applySessionSnapshot,
    resetBusinessState,
    bootstrap,
    ensureCsrfToken,
    requestEmailCode,
    login,
    refreshMe,
    updateDisplayName,
    setSpace,
    logout,
  }
})
