/**
 * 阶段 1 M1：昵称修改。
 *
 * 1. `updateMe()` 必须走共享 client：PATCH、X-CSRF-Token、same-origin 凭据、
 *    JSON 请求体只有 display_name；离线与缺令牌时一个字节都不发。
 *    fetch 全程被 stubGlobal 替换，不触碰真实网络。
 * 2. `checkDisplayName()` 与服务端 ProfileService 规则一致：按 Unicode 码点计数、
 *    只裁剪首尾空格分隔符、控制字符与换行一律拒绝。
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { updateMe } from '@/api/auth'
import { OfflineMutationBlockedError, setCsrfToken } from '@/api/client'
import { checkDisplayName, codePointLength } from '@/api/display-name'
import type { MeResponse } from '@/api/types'

const ME: MeResponse = {
  user: {
    id: 'user-1',
    email: 'a@example.com',
    display_name: '新昵称',
    created_at: '2025-01-01T00:00:00Z',
  },
  space: null,
}

function okResponse(body: unknown): Response {
  const text = JSON.stringify(body)
  return {
    ok: true,
    status: 200,
    json: async () => JSON.parse(text) as unknown,
    text: async () => text,
  } as unknown as Response
}

function stubOnline(value: boolean): void {
  Object.defineProperty(window.navigator, 'onLine', {
    configurable: true,
    enumerable: true,
    get: () => value,
  })
}

let fetchMock: ReturnType<typeof vi.fn>

beforeEach(() => {
  fetchMock = vi.fn()
  vi.stubGlobal('fetch', fetchMock)
  setCsrfToken(null)
})

afterEach(() => {
  vi.unstubAllGlobals()
  Reflect.deleteProperty(window.navigator, 'onLine')
  setCsrfToken(null)
})

describe('updateMe（共享 client）', () => {
  it('发送 PATCH /api/v1/me，带 X-CSRF-Token 与 same-origin，请求体只有 display_name', async () => {
    setCsrfToken('token-me')
    fetchMock.mockResolvedValue(okResponse(ME))

    const result = await updateMe({ display_name: '新昵称' })

    expect(result).toEqual(ME)
    expect(fetchMock).toHaveBeenCalledTimes(1)
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/v1/me')
    expect(init.method).toBe('PATCH')
    expect(init.credentials).toBe('same-origin')
    const headers = init.headers as Record<string, string>
    expect(headers['X-CSRF-Token']).toBe('token-me')
    expect(headers['Content-Type']).toBe('application/json')
    expect(JSON.parse(init.body as string)).toEqual({ display_name: '新昵称' })
  })

  it('离线时抛 OfflineMutationBlockedError，不发 fetch、不排队', async () => {
    stubOnline(false)
    setCsrfToken('token-me')

    await expect(updateMe({ display_name: '新昵称' })).rejects.toBeInstanceOf(
      OfflineMutationBlockedError,
    )
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('缺少 CSRF 令牌时不发 fetch', async () => {
    await expect(updateMe({ display_name: '新昵称' })).rejects.toMatchObject({
      code: 'CSRF_TOKEN_INVALID',
    })
    expect(fetchMock).not.toHaveBeenCalled()
  })
})

describe('checkDisplayName（与服务端规则一致）', () => {
  const emoji = '\u{1F600}'

  it('按 Unicode 码点计数：1 与 20 通过，21 拒绝（ASCII / 中文 / emoji）', () => {
    expect(checkDisplayName('A')).toEqual({ ok: true, value: 'A' })
    expect(checkDisplayName('a'.repeat(20))).toEqual({ ok: true, value: 'a'.repeat(20) })
    expect(checkDisplayName('a'.repeat(21))).toEqual({ ok: false, problem: 'too_long' })
    expect(checkDisplayName('名'.repeat(20))).toEqual({ ok: true, value: '名'.repeat(20) })
    expect(checkDisplayName('名'.repeat(21))).toEqual({ ok: false, problem: 'too_long' })

    // 20 个 emoji 是 40 个 UTF-16 码元，但仍是 20 个码点。
    expect(emoji.repeat(20).length).toBe(40)
    expect(codePointLength(emoji.repeat(20))).toBe(20)
    expect(checkDisplayName(emoji.repeat(20))).toEqual({ ok: true, value: emoji.repeat(20) })
    expect(checkDisplayName(emoji.repeat(21))).toEqual({ ok: false, problem: 'too_long' })
  })

  it('只裁剪首尾空格分隔符，内部空格保留', () => {
    expect(checkDisplayName('  Alice  ')).toEqual({ ok: true, value: 'Alice' })
    expect(checkDisplayName('　名字　')).toEqual({ ok: true, value: '名字' })
    expect(checkDisplayName(' Bob ')).toEqual({ ok: true, value: 'Bob' })
    expect(checkDisplayName('Al ice')).toEqual({ ok: true, value: 'Al ice' })
    expect(checkDisplayName(`  ${'a'.repeat(20)}  `)).toEqual({ ok: true, value: 'a'.repeat(20) })
  })

  it('空输入与纯空白拒绝', () => {
    expect(checkDisplayName('')).toEqual({ ok: false, problem: 'empty' })
    expect(checkDisplayName('   ')).toEqual({ ok: false, problem: 'empty' })
    expect(checkDisplayName('　  ')).toEqual({ ok: false, problem: 'empty' })
  })

  it('控制字符与换行在任何位置都拒绝（首尾也不裁剪）', () => {
    for (const raw of [
      'Alice\n',
      '\nAlice',
      'A\r\nB',
      'A\tB',
      'A\u0000B',
      'A\u007fB',
      'A\u0085B',
      'A B',
      'A B',
    ]) {
      expect(checkDisplayName(raw)).toEqual({ ok: false, problem: 'control_character' })
    }
  })

  it('孤立代理项（无法编码为合法 UTF-8）拒绝', () => {
    expect(checkDisplayName('A\ud800B')).toEqual({ ok: false, problem: 'invalid' })
    expect(checkDisplayName('\udc00')).toEqual({ ok: false, problem: 'invalid' })
  })
})
