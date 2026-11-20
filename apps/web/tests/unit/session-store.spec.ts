/**
 * session store 的单元测试。
 *
 * 只测状态机与清理语义，网络层整体 mock 掉（`@/api/auth`、`@/api/space`）。
 * 重点：
 *   - bootstrap 拿到 authenticated 快照后进入 authenticated 并下发 CSRF 令牌；
 *   - bootstrap 失败时 fail closed（不臆造登录态）；
 *   - logout() 之后本地业务态被彻底清空，包括 space store。
 */
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@/api/auth', () => ({
  bootstrapSession: vi.fn(),
  requestEmailCode: vi.fn(),
  createSession: vi.fn(),
  deleteSession: vi.fn(),
  getMe: vi.fn(),
  updateMe: vi.fn(),
}))

vi.mock('@/api/space', () => ({
  createSpace: vi.fn(),
  getCurrentSpace: vi.fn(),
  getSpaceById: vi.fn(),
  listSpaceInvites: vi.fn(),
  createSpaceInvite: vi.fn(),
  revokeSpaceInvite: vi.fn(),
  acceptInvite: vi.fn(),
  inviteUrlFor: vi.fn(),
  membersOf: vi.fn(),
}))

import * as authApi from '@/api/auth'
import { ApiError, OfflineMutationBlockedError } from '@/api/client'
import { getCsrfToken } from '@/api/csrf'
import type { SessionBootstrap, Space, User } from '@/api/types'
import { useSessionStore } from '@/stores/session'
import { useSpaceStore } from '@/stores/space'

const USER: User = {
  id: 'user-1',
  email: 'a@example.com',
  display_name: '小 A',
  created_at: '2025-01-01T00:00:00Z',
}

const SPACE: Space = {
  id: 'space-1',
  name: '我们的小空间',
  timezone: 'Asia/Shanghai',
  member_count: 2,
  created_at: '2025-01-02T00:00:00Z',
  members: [
    { user_id: 'user-1', slot: 1, display_name: '小 A', joined_at: '2025-01-02T00:00:00Z' },
    { user_id: 'user-2', slot: 2, display_name: '小 B', joined_at: '2025-01-03T00:00:00Z' },
  ],
}

function authenticatedPayload(): SessionBootstrap {
  return {
    authenticated: true,
    csrf_token: 'csrf-1',
    user: USER,
    space: SPACE,
  }
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
})

afterEach(() => {
  vi.restoreAllMocks()
})

describe('bootstrap', () => {
  it('服务端返回 authenticated 时进入 authenticated，并写入 user / space / CSRF', async () => {
    vi.mocked(authApi.bootstrapSession).mockResolvedValue(authenticatedPayload())

    const session = useSessionStore()
    const spaceStore = useSpaceStore()

    expect(session.status).toBe('unknown')

    await session.bootstrap()

    expect(session.status).toBe('authenticated')
    expect(session.user).toEqual(USER)
    expect(session.space).toEqual(SPACE)
    expect(session.csrfToken).toBe('csrf-1')
    // CSRF 令牌必须同步到 HTTP 层的内存持有者。
    expect(getCsrfToken()).toBe('csrf-1')
    expect(spaceStore.space).toEqual(SPACE)
    expect(spaceStore.members).toHaveLength(2)
  })

  it('服务端返回未认证时进入 anonymous', async () => {
    vi.mocked(authApi.bootstrapSession).mockResolvedValue({
      authenticated: false,
      csrf_token: 'csrf-anon',
    })

    const session = useSessionStore()
    await session.bootstrap()

    expect(session.status).toBe('anonymous')
    expect(session.user).toBeNull()
    expect(session.space).toBeNull()
    // 匿名态也需要 CSRF 令牌，否则连发验证码都发不出去。
    expect(getCsrfToken()).toBe('csrf-anon')
  })

  it('引导失败时 fail closed：不抛错、不臆造登录态', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => undefined)
    vi.mocked(authApi.bootstrapSession).mockRejectedValue(new Error('network down'))

    const session = useSessionStore()
    await expect(session.bootstrap()).resolves.toBeUndefined()

    expect(session.status).toBe('anonymous')
    expect(session.user).toBeNull()
    expect(session.space).toBeNull()
    expect(session.csrfToken).toBeNull()
    expect(getCsrfToken()).toBeNull()
    expect(warn).toHaveBeenCalled()
  })
})

describe('updateDisplayName（M1）', () => {
  it('成功后用服务端快照更新当前用户与自己的成员槽位，对方槽位不变', async () => {
    vi.mocked(authApi.bootstrapSession).mockResolvedValue(authenticatedPayload())
    const renamedUser: User = { ...USER, display_name: '新昵称' }
    const renamedSpace: Space = {
      ...SPACE,
      members: [
        { user_id: 'user-1', slot: 1, display_name: '新昵称', joined_at: '2025-01-02T00:00:00Z' },
        { user_id: 'user-2', slot: 2, display_name: '小 B', joined_at: '2025-01-03T00:00:00Z' },
      ],
    }
    vi.mocked(authApi.updateMe).mockResolvedValue({ user: renamedUser, space: renamedSpace })

    const session = useSessionStore()
    const spaceStore = useSpaceStore()
    await session.bootstrap()

    await session.updateDisplayName('新昵称')

    expect(authApi.updateMe).toHaveBeenCalledTimes(1)
    expect(authApi.updateMe).toHaveBeenCalledWith({ display_name: '新昵称' })
    expect(session.user?.display_name).toBe('新昵称')
    expect(spaceStore.members.find((m) => m.slot === 1)?.display_name).toBe('新昵称')
    expect(spaceStore.members.find((m) => m.slot === 2)?.display_name).toBe('小 B')
    expect(spaceStore.members).toHaveLength(2)
    // 会话与 CSRF 不受影响。
    expect(session.status).toBe('authenticated')
    expect(getCsrfToken()).toBe('csrf-1')
  })

  it('服务端校验失败时原样抛出，本地 user / space / 成员保持不变', async () => {
    vi.mocked(authApi.bootstrapSession).mockResolvedValue(authenticatedPayload())
    vi.mocked(authApi.updateMe).mockRejectedValue(
      new ApiError('VALIDATION_ERROR', '昵称最多 20 个字符', 400),
    )

    const session = useSessionStore()
    const spaceStore = useSpaceStore()
    await session.bootstrap()

    await expect(session.updateDisplayName('x'.repeat(21))).rejects.toMatchObject({
      code: 'VALIDATION_ERROR',
    })

    expect(session.user).toEqual(USER)
    expect(session.space).toEqual(SPACE)
    expect(spaceStore.members).toEqual(SPACE.members)
    expect(session.status).toBe('authenticated')
  })

  it('离线被拦下时原样抛出，本地状态保持不变', async () => {
    vi.mocked(authApi.bootstrapSession).mockResolvedValue(authenticatedPayload())
    vi.mocked(authApi.updateMe).mockRejectedValue(new OfflineMutationBlockedError())

    const session = useSessionStore()
    const spaceStore = useSpaceStore()
    await session.bootstrap()

    await expect(session.updateDisplayName('新昵称')).rejects.toBeInstanceOf(
      OfflineMutationBlockedError,
    )

    expect(session.user).toEqual(USER)
    expect(spaceStore.members).toEqual(SPACE.members)
  })
})

describe('logout', () => {
  it('清空 user / space / csrfToken 以及 space store 的全部业务态', async () => {
    vi.mocked(authApi.bootstrapSession).mockResolvedValue(authenticatedPayload())
    vi.mocked(authApi.deleteSession).mockResolvedValue(undefined)

    const session = useSessionStore()
    const spaceStore = useSpaceStore()
    await session.bootstrap()

    // 制造一些残留业务数据，确认它们也会被清掉。
    spaceStore.invites = [
      {
        id: 'invite-1',
        expires_at: '2025-02-01T00:00:00Z',
        created_at: '2025-01-05T00:00:00Z',
        max_uses: 1,
        used_count: 0,
      },
    ]
    spaceStore.lastCreatedInvite = {
      id: 'invite-1',
      expires_at: '2025-02-01T00:00:00Z',
      created_at: '2025-01-05T00:00:00Z',
      max_uses: 1,
      used_count: 0,
      code: 'ABC123',
      invite_url: 'https://example.com/invite/ABC123',
    }

    await session.logout()

    expect(authApi.deleteSession).toHaveBeenCalledTimes(1)
    expect(session.status).toBe('anonymous')
    expect(session.user).toBeNull()
    expect(session.space).toBeNull()
    expect(session.csrfToken).toBeNull()
    expect(session.preAuthExpiresAt).toBeNull()
    expect(getCsrfToken()).toBeNull()

    expect(spaceStore.space).toBeNull()
    expect(spaceStore.members).toEqual([])
    expect(spaceStore.invites).toEqual([])
    expect(spaceStore.lastCreatedInvite).toBeNull()
    expect(spaceStore.loading).toBe(false)
    expect(spaceStore.errorCode).toBeNull()
  })

  it('服务端注销失败时本地照样清空，但不谎报成功', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => undefined)
    vi.mocked(authApi.bootstrapSession).mockResolvedValue(authenticatedPayload())
    vi.mocked(authApi.deleteSession).mockRejectedValue(new Error('offline'))

    const session = useSessionStore()
    await session.bootstrap()
    // Never rejects; true = this logout cleared the session it started from (M10 repair).
    await expect(session.logout()).resolves.toBe(true)

    expect(session.status).toBe('anonymous')
    expect(session.user).toBeNull()
    expect(getCsrfToken()).toBeNull()
    expect(warn).toHaveBeenCalled()
  })
})
