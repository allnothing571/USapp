/**
 * api/client.ts 的单元测试。
 *
 * 只覆盖**行为契约**，不触碰真实网络：fetch 全程被 stubGlobal 替换。
 * 重点验证两条"必须在 fetch 之前就失败"的守卫：
 *   1. 离线时任何写请求都不得发出；
 *   2. 没有 CSRF 令牌时任何写请求都不得发出。
 *
 * 注意：这里断言的是"没有调用 fetch"，也就是请求一个字节都没出去，
 * 而不是"服务端拒绝了"。
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import {
  ApiError,
  OfflineMutationBlockedError,
  getCsrfToken,
  request,
  setCsrfToken,
} from '@/api/client'

/** 最小可用的 Response 替身，避免依赖运行时是否提供全局 Response。 */
interface FakeResponseInit {
  status?: number
  body?: unknown
  rawBody?: string
}

function fakeResponse(init: FakeResponseInit = {}): Response {
  const status = init.status ?? 200
  const text = init.rawBody ?? (init.body === undefined ? '' : JSON.stringify(init.body))
  return {
    ok: status >= 200 && status < 300,
    status,
    json: async () => JSON.parse(text) as unknown,
    text: async () => text,
  } as unknown as Response
}

/** 直接改写 navigator.onLine 的读取结果；测试结束后由 afterEach 撤销。 */
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

describe('离线守卫', () => {
  it('离线时 POST 抛 OfflineMutationBlockedError，并且不发 fetch', async () => {
    stubOnline(false)
    setCsrfToken('token-1')

    await expect(request('/api/v1/spaces', { method: 'POST' })).rejects.toBeInstanceOf(
      OfflineMutationBlockedError,
    )
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('离线时 DELETE 同样被拦下且不发 fetch', async () => {
    stubOnline(false)
    setCsrfToken('token-1')

    await expect(
      request('/api/v1/auth/sessions', { method: 'DELETE' }),
    ).rejects.toMatchObject({ code: 'OFFLINE_MUTATION_BLOCKED', status: 0 })
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('离线时 GET 仍然放行（允许只读浏览）', async () => {
    stubOnline(false)
    fetchMock.mockResolvedValue(fakeResponse({ body: { authenticated: false } }))

    const result = await request('/api/v1/session')

    expect(result).toEqual({ authenticated: false })
    expect(fetchMock).toHaveBeenCalledTimes(1)
  })
})

describe('CSRF 守卫', () => {
  it('缺少 CSRF 令牌时抛 CSRF_TOKEN_INVALID，并且不发 fetch', async () => {
    expect(getCsrfToken()).toBeNull()

    await expect(request('/api/v1/spaces', { method: 'POST' })).rejects.toMatchObject({
      name: 'ApiError',
      code: 'CSRF_TOKEN_INVALID',
      status: 403,
    })
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('有令牌时把 X-CSRF-Token 放进请求头，并保持 same-origin 凭据策略', async () => {
    setCsrfToken('token-abc')
    fetchMock.mockResolvedValue(fakeResponse({ status: 204 }))

    const result = await request('/api/v1/spaces', { method: 'POST' })

    expect(result).toBeUndefined()
    expect(fetchMock).toHaveBeenCalledTimes(1)

    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/v1/spaces')
    expect(init.method).toBe('POST')
    expect(init.credentials).toBe('same-origin')
    expect((init.headers as Record<string, string>)['X-CSRF-Token']).toBe('token-abc')
  })

  it('GET 不带 X-CSRF-Token', async () => {
    setCsrfToken('token-abc')
    fetchMock.mockResolvedValue(fakeResponse({ body: { ok: true } }))

    await request('/api/v1/me')

    const [, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    const headers = init.headers as Record<string, string>
    expect(headers['X-CSRF-Token']).toBeUndefined()
    expect(init.credentials).toBe('same-origin')
  })
})

describe('错误信封解析', () => {
  it('把 {error:{code,message,details}} 解析成 ApiError', async () => {
    setCsrfToken('token-abc')
    fetchMock.mockResolvedValue(
      fakeResponse({
        status: 403,
        body: {
          error: {
            code: 'REGISTRATION_NOT_ALLOWED',
            message: '该邮箱不在注册白名单内。',
            details: { email: 'a@b.com' },
          },
        },
      }),
    )

    const error = await request('/api/v1/auth/sessions', {
      method: 'POST',
      body: { email: 'a@b.com', code: '123456' },
    }).catch((caught: unknown) => caught)

    expect(error).toBeInstanceOf(ApiError)
    const apiError = error as ApiError
    expect(apiError.code).toBe('REGISTRATION_NOT_ALLOWED')
    expect(apiError.message).toBe('该邮箱不在注册白名单内。')
    expect(apiError.status).toBe(403)
    expect(apiError.details).toEqual({ email: 'a@b.com' })
  })

  it('响应体不是 JSON 时退化为按状态码生成的兜底错误', async () => {
    fetchMock.mockResolvedValue({
      ok: false,
      status: 502,
      json: async () => {
        throw new SyntaxError('not json')
      },
      text: async () => '<html>bad gateway</html>',
    } as unknown as Response)

    await expect(request('/api/v1/session')).rejects.toMatchObject({
      code: 'INTERNAL_ERROR',
      status: 502,
    })
  })
})
