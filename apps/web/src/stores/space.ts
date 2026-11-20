/**
 * 空间与成员业务态。
 *
 * 边界：这里只存"当前登录用户所属空间"的数据，全部是**内存态**。
 * 不落 localStorage / sessionStorage / IndexedDB —— 情侣业务数据不允许持久化到客户端。
 * `reset()` 由 session store 在注销 / 切换账号时调用。
 * 每次 reset() 递增代数；reset 之前发出的请求落地时（成功或失败）不再写入任何状态，
 * 改为抛出 StaleSessionError，旧空间与旧邀请不能回填到已清空或新账号的状态里。
 */
import { defineStore } from 'pinia'
import { computed, ref } from 'vue'

import * as spaceApi from '@/api/space'
import { ApiError } from '@/api/client'
import type { CreatedInvite, Invite, Space, SpaceMember } from '@/api/types'
import { StaleSessionError } from './stale'

export const useSpaceStore = defineStore('space', () => {
  const space = ref<Space | null>(null)
  const members = ref<SpaceMember[]>([])
  const invites = ref<Invite[]>([])
  /** 最近一次生成的邀请（含明文邀请码），只在本次会话内存中保留。 */
  const lastCreatedInvite = ref<CreatedInvite | null>(null)
  const loading = ref(false)
  const errorCode = ref<string | null>(null)

  const memberCount = computed(() => members.value.length)
  const hasSpace = computed(() => space.value !== null)
  const timezone = computed(() => space.value?.timezone ?? null)
  /** 空成员槽位：契约固定 2 个槽位（slot 1/2）。 */
  const openSlots = computed<Array<1 | 2>>(() => {
    const taken = new Set(members.value.map((member) => member.slot))
    return ([1, 2] as const).filter((slot) => !taken.has(slot))
  })
  const availableInvites = computed(() =>
    invites.value.filter(
      (invite) =>
        invite.revoked !== true &&
        invite.used_count < invite.max_uses &&
        new Date(invite.expires_at).getTime() > Date.now(),
    ),
  )

  /** 用服务端返回的 Space 覆盖本地状态（bootstrap / 各写接口共用）。 */
  function hydrate(next: Space | null): void {
    space.value = next
    members.value = next?.members ? [...next.members] : []
  }

  let generation = 0

  /** 注销 / 切换账号时清空全部业务态。 */
  function reset(): void {
    generation += 1
    space.value = null
    members.value = []
    invites.value = []
    lastCreatedInvite.value = null
    loading.value = false
    errorCode.value = null
  }

  async function run<T>(task: () => Promise<T>): Promise<T> {
    const started = generation
    loading.value = true
    errorCode.value = null
    try {
      const result = await task()
      if (started !== generation) throw new StaleSessionError()
      return result
    } catch (error) {
      if (started !== generation) throw error instanceof StaleSessionError ? error : new StaleSessionError()
      errorCode.value = error instanceof ApiError ? error.code : 'NETWORK_ERROR'
      throw error
    } finally {
      if (started === generation) loading.value = false
    }
  }

  /** POST /api/v1/spaces */
  async function create(): Promise<Space> {
    const response = await run(() => spaceApi.createSpace())
    hydrate(response.space)
    await refreshInvites()
    return response.space
  }

  /** GET /api/v1/spaces/current */
  async function loadCurrent(): Promise<Space> {
    const response = await run(() => spaceApi.getCurrentSpace())
    hydrate(response.space)
    return response.space
  }

  /** GET /api/v1/spaces/{spaceId} */
  async function loadById(spaceId: string): Promise<Space> {
    const response = await run(() => spaceApi.getSpaceById(spaceId))
    hydrate(response.space)
    return response.space
  }

  /** GET /api/v1/spaces/current/invites */
  async function refreshInvites(): Promise<Invite[]> {
    const response = await run(() => spaceApi.listSpaceInvites())
    invites.value = response.invites
    return response.invites
  }

  /** POST /api/v1/spaces/current/invites —— 明文邀请码只在返回值里出现一次。 */
  async function createInvite(): Promise<CreatedInvite> {
    const response = await run(() => spaceApi.createSpaceInvite())
    lastCreatedInvite.value = response.invite
    invites.value = [...invites.value, response.invite]
    return response.invite
  }

  /** DELETE /api/v1/spaces/current/invites/{inviteId} */
  async function revokeInvite(inviteId: string): Promise<void> {
    await run(() => spaceApi.revokeSpaceInvite(inviteId))
    invites.value = invites.value.filter((invite) => invite.id !== inviteId)
    if (lastCreatedInvite.value?.id === inviteId) {
      lastCreatedInvite.value = null
    }
  }

  /** POST /api/v1/invites/{code}/accept */
  async function accept(code: string): Promise<Space> {
    const response = await run(() => spaceApi.acceptInvite(code))
    hydrate(response.space)
    return response.space
  }

  return {
    // state
    space,
    members,
    invites,
    lastCreatedInvite,
    loading,
    errorCode,
    // getters
    memberCount,
    hasSpace,
    timezone,
    openSlots,
    availableInvites,
    // actions
    hydrate,
    reset,
    create,
    loadCurrent,
    loadById,
    refreshInvites,
    createInvite,
    revokeInvite,
    accept,
  }
})
