/**
 * api/tasks.ts 经共享 client 发请求（M3-B）：发布带 Idempotency-Key 与 CSRF 令牌，
 * 读取不带 CSRF；离线时发布一个字节都不发出，恢复在线后也不会自动重放。
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { OfflineMutationBlockedError, setCsrfToken } from '@/api/client'
import { completeTask, createTask, getTask, listTasks, newIdempotencyKey, progressTask } from '@/api/tasks'

const TASK = {
  id: '0f8fad5b-d9cb-469f-a165-70867728950e',
  title: 'T',
  description: '',
  creator_id: 'a',
  assignee_id: 'b',
  reward_flowers: 1,
  reward_sunshine: 0,
  deadline: null,
  status: 'open',
  completed_at: null,
  created_at: '2026-10-09T00:00:00.000000Z',
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

function ok(body: unknown, status = 200) {
  return { ok: true, status, text: async () => JSON.stringify(body) }
}

describe('task API uses the shared client', () => {
  it('daily progress sends the displayed date, and dated details remain GET requests', async () => {
    setCsrfToken('daily-csrf')
    fetchMock.mockResolvedValue(ok({}))
    await progressTask(TASK.id, 'daily-key-0000000001', '2026-10-16')
    const init = fetchMock.mock.calls[0][1] as RequestInit
    expect(init.body).toBe(JSON.stringify({ period_date: '2026-10-16' }))
    expect((init.headers as Record<string, string>)['X-CSRF-Token']).toBe('daily-csrf')
    await getTask(TASK.id, '2026-10-15')
    expect(fetchMock.mock.calls[1][0]).toBe(`/api/v1/tasks/${TASK.id}?period_date=2026-10-15`)
    expect(fetchMock.mock.calls[1][1].method).toBe('GET')
  })

  it('publish sends POST with JSON body, Idempotency-Key, CSRF token and same-origin cookies', async () => {
    setCsrfToken('csrf-1')
    fetchMock.mockResolvedValue(ok({ task: TASK }, 201))
    const body = { title: 'T', description: '', reward_flowers: 1, reward_sunshine: 0 }
    expect(await createTask(body, 'key-0000000000000001')).toEqual({ task: TASK })
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/v1/tasks')
    expect(init.method).toBe('POST')
    expect(init.credentials).toBe('same-origin')
    expect(init.body).toBe(JSON.stringify(body))
    const headers = init.headers as Record<string, string>
    expect(headers['Idempotency-Key']).toBe('key-0000000000000001')
    expect(headers['X-CSRF-Token']).toBe('csrf-1')
    expect(headers['Content-Type']).toBe('application/json')
  })

  it('publish without a CSRF token is not sent', async () => {
    await expect(createTask({ title: 'T', reward_flowers: 1, reward_sunshine: 0 }, 'k'.repeat(16)))
      .rejects.toMatchObject({ code: 'CSRF_TOKEN_INVALID' })
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('offline publish is blocked before fetch and is not replayed when the browser comes back online', async () => {
    setCsrfToken('csrf-1')
    Object.defineProperty(window.navigator, 'onLine', { configurable: true, get: () => false })
    await expect(createTask({ title: 'T', reward_flowers: 1, reward_sunshine: 0 }, 'k'.repeat(16)))
      .rejects.toBeInstanceOf(OfflineMutationBlockedError)
    expect(fetchMock).not.toHaveBeenCalled()
    Object.defineProperty(window.navigator, 'onLine', { configurable: true, get: () => true })
    window.dispatchEvent(new Event('online'))
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('list and detail are GETs without CSRF header or body; the cursor is passed back verbatim', async () => {
    setCsrfToken('unused-for-get')
    fetchMock.mockResolvedValue(ok({ tasks: [TASK], next_cursor: 'abc_-123' }))
    await listTasks('open')
    await listTasks('history', 'abc_-123', 20)
    const [firstUrl, firstInit] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(firstUrl).toBe('/api/v1/tasks?view=open')
    expect(firstInit.method).toBe('GET')
    expect(firstInit.body).toBeUndefined()
    expect(firstInit.headers).not.toHaveProperty('X-CSRF-Token')
    expect(firstInit.headers).not.toHaveProperty('Idempotency-Key')
    const [secondUrl] = fetchMock.mock.calls[1] as [string, RequestInit]
    expect(secondUrl).toBe('/api/v1/tasks?view=history&cursor=abc_-123&limit=20')

    fetchMock.mockResolvedValue(ok({ task: TASK }))
    await getTask('0f8fad5b-d9cb-469f-a165-70867728950e')
    const [detailUrl, detailInit] = fetchMock.mock.calls[2] as [string, RequestInit]
    expect(detailUrl).toBe('/api/v1/tasks/0f8fad5b-d9cb-469f-a165-70867728950e')
    expect(detailInit.headers).not.toHaveProperty('X-CSRF-Token')
    await getTask('../wallet')
    expect((fetchMock.mock.calls[3] as [string])[0]).toBe('/api/v1/tasks/..%2Fwallet')
  })

  it('preserves server error codes such as RESULT_UNKNOWN and IDEMPOTENCY_KEY_REUSED', async () => {
    setCsrfToken('csrf-1')
    fetchMock.mockResolvedValueOnce({ ok: false, status: 503,
      json: async () => ({ error: { code: 'RESULT_UNKNOWN', message: 'retry' } }) })
    await expect(createTask({ title: 'T', reward_flowers: 1, reward_sunshine: 0 }, 'k'.repeat(16)))
      .rejects.toMatchObject({ status: 503, code: 'RESULT_UNKNOWN' })
    fetchMock.mockResolvedValueOnce({ ok: false, status: 409,
      json: async () => ({ error: { code: 'IDEMPOTENCY_KEY_REUSED', message: 'conflict' } }) })
    await expect(createTask({ title: 'T', reward_flowers: 1, reward_sunshine: 0 }, 'k'.repeat(16)))
      .rejects.toMatchObject({ status: 409, code: 'IDEMPOTENCY_KEY_REUSED' })
  })

  it('completion sends POST to the completion path with Idempotency-Key, CSRF, cookies and no body', async () => {
    setCsrfToken('csrf-2')
    const settlement = { operation_id: '1f8fad5b-d9cb-469f-a165-70867728950e', flower: 1, sunshine: 0 }
    fetchMock.mockResolvedValue(ok({ task: { ...TASK, status: 'completed' }, settlement }))
    const response = await completeTask(TASK.id, 'key-0000000000000009')
    expect(response.settlement).toEqual(settlement)
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe(`/api/v1/tasks/${TASK.id}/completion`)
    expect(init.method).toBe('POST')
    expect(init.credentials).toBe('same-origin')
    expect(init.body).toBeUndefined()
    const headers = init.headers as Record<string, string>
    expect(headers['Idempotency-Key']).toBe('key-0000000000000009')
    expect(headers['X-CSRF-Token']).toBe('csrf-2')
    expect(headers).not.toHaveProperty('Content-Type')
  })

  it('generates distinct UUID-shaped keys accepted by the server rule', () => {
    const a = newIdempotencyKey()
    const b = newIdempotencyKey()
    expect(a).toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/)
    expect(a).toMatch(/^[A-Za-z0-9_-]{16,128}$/)
    expect(a).not.toBe(b)
  })

  it('progress uses the shared guarded POST client with an empty body', async () => {
    setCsrfToken('csrf-progress')
    fetchMock.mockResolvedValue(ok({ task_id: TASK.id, progress_count: 1, target_count: 2,
      event_id: 'event', recorded_at: '2026-10-14T00:00:00.000000Z', completed: false, ignored: false, settlement: null }))
    await progressTask(TASK.id, 'key-progress-0001')
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe(`/api/v1/tasks/${TASK.id}/progress`)
    expect(init.method).toBe('POST')
    expect(init.body).toBeUndefined()
    expect(init.credentials).toBe('same-origin')
    expect(init.headers).toMatchObject({ 'Idempotency-Key': 'key-progress-0001', 'X-CSRF-Token': 'csrf-progress' })
  })
})
