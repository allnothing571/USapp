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
import { useNotificationsStore } from './notifications'
import { useRemindersStore } from './reminders'
import { useAnniversariesStore } from './anniversaries'
import { useAlbumsStore } from './albums'
import { useSharedPlansStore } from './shared-plans'
import { useMemoryDeletionsStore } from './memory-deletions'
import { useMediaDeletionsStore } from './media-deletions'
import { useMemoriesStore } from './memories'
import { useUnattachedStore } from './unattached'
import { useMediaCapabilityStore } from './media-capability'
import { useMemoryUploadsStore } from './memory-uploads'
import { useHomeStore } from './home'
import { useTaskReactionsStore } from './task-reactions'
import { useMessagesStore } from './messages'
import { useNotificationChoicesStore } from './notification-choices'
import { usePushStore } from './push'
import { clearMarker } from '@/pwa/push-marker'
import { useSpaceStore } from './space'
import { StaleSessionError } from './stale'
import { useRewardsStore } from './rewards'
import { useTasksStore } from './tasks'
import { useWalletStore } from './wallet'
import { useAvatarStore } from './avatar'
import { useAchievementsStore } from './achievements'
import { useExportStore } from './export'
import { useRoomDraftStore } from './room-draft'

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
   * 身份代数（M10 修复）：每次身份变化（匿名、换账号、注销、引导失败）递增。
   * 异步写入在发出前与落地前都核对发起时的代数与用户；不符则丢弃结果（StaleSessionError），
   * 旧响应不能恢复旧用户、不能在新账号下提交旧草稿，旧的注销完成也不能清空新会话。
   */
  let identityGeneration = 0

  interface IdentityToken {
    generation: number
    userId: string | null
  }

  function identityToken(): IdentityToken {
    return { generation: identityGeneration, userId: user.value?.id ?? null }
  }

  function isCurrentIdentity(token: IdentityToken): boolean {
    return token.generation === identityGeneration && token.userId === (user.value?.id ?? null)
  }

  /**
   * Identity changed: invalidate in-flight work and clear every per-identity store, invitations included.
   * P2-5a: the persistent device marker is cleared too, except on a first load (no identity known
   * in memory yet), where the push store reconciles it against the account's generations instead.
   */
  function resetIdentityScoped(clearDeviceMarker = true): void {
    identityGeneration += 1
    useSpaceStore().reset()
    useWalletStore().reset()
    useAvatarStore().reset()
    useAchievementsStore().reset()
    useExportStore().reset()
    useTasksStore().reset()
    useRewardsStore().reset()
    useHomeStore().reset()
    useRoomDraftStore().reset()
    useMessagesStore().reset()
    useTaskReactionsStore().reset()
    useAnniversariesStore().reset()
    useMemoriesStore().reset()
    useAlbumsStore().reset()
    useMediaDeletionsStore().reset()
    useSharedPlansStore().reset()
    useMemoryDeletionsStore().reset()
    useMemoryUploadsStore().reset()
    useUnattachedStore().reset()
    useMediaCapabilityStore().reset()
    useNotificationChoicesStore().reset()
    useNotificationsStore().reset()
    useRemindersStore().reset()
    usePushStore().reset(clearDeviceMarker)
  }

  /**
   * 把服务端 SessionBootstrap 落到 store。
   * 同时负责：CSRF 令牌轮换落库、space store 的 hydrate/reset。
   */
  function applyBootstrap(payload: SessionBootstrap): void {
    if (!payload.authenticated || user.value?.id !== payload.user?.id) {
      resetIdentityScoped(!(payload.authenticated && user.value === null))
    } else if (space.value?.id !== payload.space?.id) {
      useRoomDraftStore().reset()
    }
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
    // 其它业务 store 的清理入口统一登记在 resetIdentityScoped()。
    resetIdentityScoped()
  }

  /**
   * 在途引导与发起它时的身份。身份在等待期间被别处改变（注销、登录了另一个账号）后，
   * 这次引导就过期了：它的成功与失败都不再写入任何状态（不应用快照、不清空、不改 bootstrapped），
   * 后来的调用也不再复用它，而是发起新的引导；过期引导的收尾也不会清掉更新的在途引导。
   */
  let inFlight: { promise: Promise<void>; token: IdentityToken } | null = null
  let bootstrapSeq = 0

  async function runBootstrap(seq: number, token: IdentityToken): Promise<void> {
    const stillCurrent = () => seq === bootstrapSeq && isCurrentIdentity(token)
    let payload: SessionBootstrap
    try {
      payload = await authApi.bootstrapSession()
    } catch (error) {
      if (!stillCurrent()) return
      // 引导失败时**不臆造**已登录状态，也保留不任何业务数据：fail closed。
      console.warn('[usapp/session] 会话引导失败，已按未认证处理。', error)
      resetBusinessState()
      bootstrapped.value = true
      return
    }
    if (!stillCurrent()) return
    applyBootstrap(payload)
    bootstrapped.value = true
  }

  /**
   * `GET /api/v1/session`。并发调用会复用同一个（仍属于当前身份的）在途请求；
   * 已经引导过则默认直接返回，`force: true` 可强制重新引导。
   */
  function bootstrap(force = false): Promise<void> {
    if (inFlight && isCurrentIdentity(inFlight.token)) return inFlight.promise
    if (bootstrapped.value && !force && !inFlight) return Promise.resolve()
    const token = identityToken()
    const promise: Promise<void> = runBootstrap(++bootstrapSeq, token).finally(() => {
      if (inFlight?.promise === promise) inFlight = null
    })
    inFlight = { promise, token }
    return promise
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
    const token = identityToken()
    let payload
    try {
      payload = await authApi.getMe()
    } catch (error) {
      if (!isCurrentIdentity(token)) throw new StaleSessionError()
      throw error
    }
    if (!isCurrentIdentity(token)) throw new StaleSessionError()
    if (user.value?.id !== payload.user.id) {
      resetIdentityScoped()
    }
    user.value = payload.user
    space.value = payload.space ?? null
    useSpaceStore().hydrate(payload.space ?? null)
  }

  /**
   * `PATCH /api/v1/me` —— 修改自己的昵称。
   * 只有服务端确认成功后才用返回的快照更新 user 与成员槽位；
   * 任何失败（校验、离线、网络、CSRF）都原样抛出，本地状态保持不变。
   * 身份在等待 CSRF 令牌或响应期间变化时：不发出旧草稿、不应用旧响应，抛 StaleSessionError。
   */
  async function updateDisplayName(displayName: string): Promise<void> {
    const token = identityToken()
    if (!token.userId) throw new StaleSessionError()
    let payload
    try {
      await ensureCsrfToken()
      if (!isCurrentIdentity(token)) throw new StaleSessionError()
      payload = await authApi.updateMe({ display_name: displayName })
    } catch (error) {
      if (!isCurrentIdentity(token)) throw error instanceof StaleSessionError ? error : new StaleSessionError()
      throw error
    }
    if (!isCurrentIdentity(token)) throw new StaleSessionError()
    if (user.value?.id !== payload.user.id) {
      resetIdentityScoped()
    }
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
   *
   * 返回值表示本次注销是否清空了发起时的会话；等待期间身份已经变化（例如已经登录了新账号）时，
   * 旧的注销完成不清空新会话，返回 false，调用方也不应跳转到登录页。
   */
  async function logout(): Promise<boolean> {
    const token = identityToken()
    let serverError: unknown = null
    let cleared = false
    // P2-5a: the device marker is cleared at logout initiation, before the server call.
    void clearMarker()
    try {
      await authApi.deleteSession()
    } catch (error) {
      serverError = error
    } finally {
      if (isCurrentIdentity(token)) {
        resetBusinessState()
        cleared = true
      }
    }

    if (serverError) {
      console.warn(
        '[usapp/session] 注销请求未成功送达服务端，本地业务态已清空；' +
          '服务端会话可能仍然有效。',
        serverError,
      )
    }
    return cleared
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
