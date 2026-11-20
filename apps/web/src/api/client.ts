/**
 * 同源 HTTP 客户端（阶段 0）。
 *
 * 与 contracts/openapi.yaml 对齐的硬约束：
 * 1. 同源部署：API_BASE 默认空字符串，所有请求都发往当前 origin。
 *    模块加载时用 `window.location.origin` 与 `VITE_PUBLIC_ORIGIN` 比对，
 *    不一致**只打印警告**，不做任何静默降级（不改成跨域请求、不改写 Cookie 策略）。
 * 2. 会话 Cookie 名固定 `__Host-usapp-session`，HttpOnly。前端**不读不写**它，
 *    只依赖浏览器在 `credentials: 'same-origin'` 下自动携带。
 * 3. 所有状态修改请求（POST/PUT/PATCH/DELETE）必须带 `X-CSRF-Token`；
 *    取不到令牌时**先抛错、不发请求**（fail closed）。
 * 4. 离线（`navigator.onLine === false`）时任何非 GET 请求直接抛
 *    OfflineMutationBlockedError，绝不发出业务写请求。
 * 5. 业务响应一律 `Cache-Control: no-store`。前端**不得**把业务数据写入
 *    localStorage / sessionStorage / cookie —— 本模块也不提供任何此类工具函数。
 */
import { getCsrfToken, setCsrfToken } from './csrf'
import type { ErrorEnvelope, ServerErrorCode } from './types'

// setCsrfToken / getCsrfToken 从这里再导出，方便调用方只依赖 client.ts 一个入口。
export { getCsrfToken, setCsrfToken }

/** 同源部署时为空字符串。 */
export const API_BASE: string = import.meta.env.VITE_API_BASE ?? ''

/** 期望的对外 origin（去掉尾部斜杠），未配置时为空串表示跳过自检。 */
const PUBLIC_ORIGIN: string = (import.meta.env.VITE_PUBLIC_ORIGIN ?? '').replace(/\/+$/, '')

/**
 * 同源自检。只告警，不降级：
 * 会话 Cookie（__Host- 前缀要求 Secure + Path=/ + 无 Domain）与 CSRF/Origin 校验
 * 都建立在"页面与 API 同源"之上，一旦不同源，正确的做法是修部署入口，
 * 而不是让前端偷偷绕过。
 */
function warnIfNotSameOrigin(): void {
  if (!PUBLIC_ORIGIN) return
  if (typeof window === 'undefined' || !window.location) return

  const currentOrigin = window.location.origin
  if (currentOrigin !== PUBLIC_ORIGIN) {
    console.warn(
      `[usapp/api] 同源检查未通过：当前页面 origin=${currentOrigin}，` +
        `VITE_PUBLIC_ORIGIN=${PUBLIC_ORIGIN}。` +
        '会话 Cookie 与 CSRF/Origin 校验都要求同源部署；' +
        '这里只告警、不做静默降级，请负责人确认对外入口。',
    )
  }
}

warnIfNotSameOrigin()

/** 前端合成的错误码（**不会**由服务端返回），与契约枚举严格区分开。 */
export type ClientErrorCode = 'NETWORK_ERROR' | 'OFFLINE_MUTATION_BLOCKED' | 'DOWNLOAD_STREAM_UNSUPPORTED'

/** 契约枚举 ∪ 前端合成码。 */
export type ErrorCode = ServerErrorCode | ClientErrorCode

/** 后端 `{error:{code,message,details}}` 信封，或前端合成错误。 */
export class ApiError extends Error {
  readonly code: ErrorCode | string
  readonly status: number
  readonly details?: Record<string, unknown>

  constructor(
    code: ErrorCode | string,
    message: string,
    status: number,
    details?: Record<string, unknown>,
  ) {
    super(message)
    this.name = 'ApiError'
    this.code = code
    this.status = status
    this.details = details
  }
}

/**
 * 离线时尝试发起业务写请求。
 * 单独成类而不是复用 ApiError，是为了让调用方能可靠区分
 * "被本地离线守卫拦下" 与 "服务端拒绝"。
 */
export class OfflineMutationBlockedError extends Error {
  readonly code: ClientErrorCode = 'OFFLINE_MUTATION_BLOCKED'
  readonly status = 0

  constructor(message = '当前离线，业务操作不会提交。') {
    super(message)
    this.name = 'OfflineMutationBlockedError'
  }
}

export type HttpMethod = 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE' | 'HEAD' | 'OPTIONS'

/** 契约：状态修改请求必须带 CSRF 令牌的四种方法。 */
const MUTATING_METHODS: ReadonlySet<string> = new Set(['POST', 'PUT', 'PATCH', 'DELETE'])

export function isMutatingMethod(method: string): boolean {
  return MUTATING_METHODS.has(method.toUpperCase())
}

/** 浏览器报告当前离线。非浏览器环境（单测/Node）视为在线。 */
export function isOffline(): boolean {
  return typeof navigator !== 'undefined' && navigator.onLine === false
}

export type QueryValue = string | number | boolean | null | undefined

export interface RequestOptions {
  method?: HttpMethod | string
  query?: Record<string, QueryValue>
  body?: unknown
  signal?: AbortSignal
  credentials?: RequestCredentials
  headers?: Record<string, string>
}

/** 拼接 `API_BASE + path + query`。null / undefined / 空串的查询参数会被丢弃。 */
export function buildUrl(path: string, query?: Record<string, QueryValue>): string {
  const base = `${API_BASE}${path}`
  if (!query) return base

  const search = new URLSearchParams()
  for (const [key, value] of Object.entries(query)) {
    if (value === undefined || value === null || value === '') continue
    search.append(key, String(value))
  }

  const qs = search.toString()
  if (!qs) return base
  return base.includes('?') ? `${base}&${qs}` : `${base}?${qs}`
}

/** 把错误响应解析成 ApiError。解析失败时退化为按 HTTP 状态码生成的兜底错误。 */
async function toApiError(response: Response): Promise<ApiError> {
  let code: ErrorCode | string = 'INTERNAL_ERROR'
  let message = `请求失败（HTTP ${response.status}）`
  let details: Record<string, unknown> | undefined

  try {
    const parsed: unknown = await response.json()
    if (parsed && typeof parsed === 'object' && 'error' in parsed) {
      const envelope = parsed as ErrorEnvelope
      const body = envelope.error
      if (body && typeof body === 'object') {
        if (typeof body.code === 'string' && body.code.length > 0) code = body.code
        if (typeof body.message === 'string' && body.message.length > 0) message = body.message
        if (body.details && typeof body.details === 'object') details = body.details
      }
    }
  } catch {
    // 响应体不是 JSON（例如网关返回的 HTML 错误页）：保留上面的兜底文案。
  }

  return new ApiError(code, message, response.status, details)
}

/**
 * 发起一次同源 API 请求。
 *
 * 守卫顺序是刻意的：
 *   离线守卫 → CSRF 守卫 → 真正 fetch
 * 两个守卫都在 fetch 之前抛出，保证"被拦下的请求一个字节都不会发出去"。
 */
export async function request<T>(path: string, options: RequestOptions = {}): Promise<T> {
  const method = (options.method ?? 'GET').toUpperCase()
  const mutating = isMutatingMethod(method)

  if (mutating && isOffline()) {
    throw new OfflineMutationBlockedError()
  }

  const headers: Record<string, string> = {
    Accept: 'application/json',
    ...(options.headers ?? {}),
  }

  if (mutating) {
    const token = getCsrfToken()
    if (!token) {
      throw new ApiError(
        'CSRF_TOKEN_INVALID',
        '缺少 CSRF 令牌，写请求未发出。请先通过 GET /api/v1/session 建立会话。',
        403,
      )
    }
    headers['X-CSRF-Token'] = token
  }

  const hasBody = options.body !== undefined
  if (hasBody) {
    headers['Content-Type'] = 'application/json'
  }

  const init: RequestInit = {
    method,
    headers,
    // 同源 Cookie 会话：仅依赖浏览器自动携带 HttpOnly Cookie，前端不接触它的值。
    credentials: options.credentials ?? 'same-origin',
  }
  if (hasBody) init.body = JSON.stringify(options.body)
  if (options.signal) init.signal = options.signal

  let response: Response
  try {
    response = await fetch(buildUrl(path, options.query), init)
  } catch (error) {
    // 调用方主动取消：原样抛出，不要伪装成网络故障。
    if (error instanceof Error && error.name === 'AbortError') throw error
    throw new ApiError('NETWORK_ERROR', '网络请求失败，未收到服务端响应。', 0)
  }

  if (!response.ok) {
    throw await toApiError(response)
  }

  // 契约里 DELETE 端点返回 204（无响应体）。
  if (response.status === 204) return undefined as T

  const text = await response.text()
  if (text.length === 0) return undefined as T

  try {
    return JSON.parse(text) as T
  } catch {
    throw new ApiError('INTERNAL_ERROR', '响应不是合法 JSON。', response.status)
  }
}

/**
 * E1 export: the most body bytes the application accumulates (the server's exact cap). This bounds what this code reads and keeps;
 * it does not bound the browser's or the transport's own buffering below the stream.
 */
export const MAX_DOWNLOAD_BYTES = 16 * 1024 * 1024

export interface DownloadedFile {
  blob: Blob
  /** The server's attachment name when it is a plain file name; null otherwise. */
  filename: string | null
  bytes: number
}

export interface DownloadOptions {
  method?: 'POST'
  body?: unknown
  signal?: AbortSignal
  maxBytes?: number
}

function attachmentName(disposition: string | null): string | null {
  const match = /filename="([A-Za-z0-9._-]{1,128})"/.exec(disposition ?? '')
  return match ? match[1] : null
}

/**
 * A narrowly scoped write that returns the raw response body instead of parsed JSON (E1 user-data export).
 * Same guards and order as request(): offline refusal, then CSRF, then a same-origin fetch; error envelopes are parsed the same way.
 * request() itself is unchanged. A 429 carries Retry-After in details.retry_after_seconds.
 */
export async function requestDownload(path: string, options: DownloadOptions = {}): Promise<DownloadedFile> {
  const method = options.method ?? 'POST'
  const maxBytes = options.maxBytes ?? MAX_DOWNLOAD_BYTES
  if (isOffline()) throw new OfflineMutationBlockedError()
  const token = getCsrfToken()
  if (!token) {
    throw new ApiError('CSRF_TOKEN_INVALID', '缺少 CSRF 令牌，写请求未发出。请先通过 GET /api/v1/session 建立会话。', 403)
  }
  const headers: Record<string, string> = { Accept: 'application/json', 'X-CSRF-Token': token }
  const init: RequestInit = { method, headers, credentials: 'same-origin' }
  if (options.body !== undefined) {
    headers['Content-Type'] = 'application/json'
    init.body = JSON.stringify(options.body)
  }
  if (options.signal) init.signal = options.signal

  let response: Response
  try {
    response = await fetch(buildUrl(path), init)
  } catch (error) {
    if (error instanceof Error && error.name === 'AbortError') throw error
    throw new ApiError('NETWORK_ERROR', '网络请求失败，未收到服务端响应。', 0)
  }
  if (!response.ok) {
    const failure = await toApiError(response)
    const retry = Number(response.headers?.get('Retry-After'))
    if (response.status === 429 && Number.isFinite(retry) && retry > 0) {
      throw new ApiError(failure.code, failure.message, failure.status, { ...(failure.details ?? {}), retry_after_seconds: retry })
    }
    throw failure
  }
  const tooLarge = () => new ApiError('EXPORT_TOO_LARGE', '文件超过浏览器端的接收上限，已放弃。', response.status)
  // A declared length above the cap: refuse without consuming, and cancel the body. A smaller or missing declaration is not trusted.
  const declaredHeader = response.headers?.get('Content-Length')
  const declared = declaredHeader === null || declaredHeader === undefined || declaredHeader === '' ? NaN : Number(declaredHeader)
  if (Number.isFinite(declared) && declared > maxBytes) {
    await cancelBody(response.body)
    throw tooLarge()
  }
  // Read incrementally and count the actual bytes. No unbounded fallback: without a readable stream the download fails clearly.
  const stream = response.body
  if (!stream || typeof stream.getReader !== 'function') {
    throw new ApiError('DOWNLOAD_STREAM_UNSUPPORTED', '当前浏览器无法按上限分块接收导出文件，已放弃。', response.status)
  }
  const reader = stream.getReader()
  const chunks: Uint8Array[] = []
  let total = 0
  for (;;) {
    if (options.signal?.aborted) {
      await cancelReader(reader)
      throw abortError()
    }
    let step: ReadableStreamReadResult<Uint8Array>
    try {
      step = await reader.read()
    } catch (error) {
      await cancelReader(reader)
      if ((error instanceof Error || error instanceof DOMException) && error.name === 'AbortError') throw error
      throw new ApiError('NETWORK_ERROR', '接收导出文件时中断，没有保存任何文件。', 0)
    }
    if (step.done) break
    const chunk = step.value
    if (!chunk) continue
    // Stop before the chunk that would cross the cap: nothing past the limit is kept, and the body is cancelled.
    if (total + chunk.byteLength > maxBytes) {
      await cancelReader(reader)
      throw tooLarge()
    }
    chunks.push(chunk)
    total += chunk.byteLength
  }
  if (options.signal?.aborted) throw abortError()
  const type = response.headers?.get('Content-Type') ?? 'application/json'
  const blob = new Blob(chunks as BlobPart[], { type })
  return { blob, filename: attachmentName(response.headers?.get('Content-Disposition') ?? null), bytes: total }
}

function abortError(): Error {
  const error = new Error('The download was aborted.')
  error.name = 'AbortError'
  return error
}

async function cancelReader(reader: ReadableStreamDefaultReader<Uint8Array>): Promise<void> {
  try {
    await reader.cancel()
  } catch {
    // Already errored or closed: nothing more will be consumed either way.
  }
}

async function cancelBody(body: ReadableStream<Uint8Array> | null | undefined): Promise<void> {
  if (!body || typeof body.cancel !== 'function') return
  try {
    await body.cancel()
  } catch {
    // A locked or errored body cannot be read by this code afterwards either.
  }
}

/** GET 快捷方式。 */
export function get<T>(path: string, options: Omit<RequestOptions, 'method' | 'body'> = {}): Promise<T> {
  return request<T>(path, { ...options, method: 'GET' })
}

/** 带 CSRF 的写请求快捷方式。 */
export function send<T>(path: string, options: RequestOptions = {}): Promise<T> {
  return request<T>(path, options)
}
