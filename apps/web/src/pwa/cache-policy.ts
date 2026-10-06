/**
 * Service Worker 缓存策略的**唯一**事实来源。
 *
 * 规则只在这里写一份：sw.ts 与 tests/unit/sw-cache-policy.spec.ts 复用同一实现，
 * 避免"测试里一套、运行时另一套"的假安全感。
 *
 * ── 黑名单（永不缓存）─────────────────────────────────────────────
 *   /api/**      契约要求所有 API 响应 `Cache-Control: no-store`；缓存会残留情侣业务数据。
 *   /media/**    阶段 3 引入的私有 OSS 媒体前缀，这里先把边界写死。
 *   /private/**  私有资源前缀。
 *   /uploads/**  上传资源前缀。
 *   另外：任何非 GET 请求一律不可缓存。
 *
 * ── 白名单（静态外壳，StaleWhileRevalidate，缓存名 usapp-shell-v1）──
 *   同源 GET，且 request.destination ∈ {document, script, style, font, image}
 *   且 URL 不落在上面的黑名单前缀里。
 * ──────────────────────────────────────────────────────────────
 */

/** 静态外壳缓存名。改名即等于让旧缓存整体失效。 */
export const SHELL_CACHE_NAME = 'usapp-shell-v1'

/** 永不可缓存的路径前缀。 */
export const SENSITIVE_PREFIXES: readonly string[] = [
  '/api/',
  '/media/',
  '/private/',
  '/uploads/',
]

/** 允许进外壳缓存的 request.destination 取值。 */
export const SHELL_DESTINATIONS: readonly string[] = [
  'document',
  'script',
  'style',
  'font',
  'image',
]

/** destination 缺失时（部分合成请求）退化为按扩展名判断。 */
export const SHELL_EXTENSIONS: readonly string[] = [
  '.js',
  '.mjs',
  '.css',
  '.html',
  '.svg',
  '.png',
  '.jpg',
  '.jpeg',
  '.webp',
  '.ico',
  '.woff',
  '.woff2',
  '.webmanifest',
]

/** 任意输入（绝对 URL / 相对路径）→ pathname。解析失败时手工剥掉 query 与 hash。 */
export function toPathname(input: string | URL, base = 'http://localhost'): string {
  const raw = typeof input === 'string' ? input : input.toString()
  try {
    const looksAbsolute = /^[a-zA-Z][a-zA-Z\d+\-.]*:\/\//.test(raw)
    const url = looksAbsolute ? new URL(raw) : new URL(raw, base)
    return url.pathname
  } catch {
    return raw.split('?')[0]?.split('#')[0] ?? raw
  }
}

/**
 * 是否命中敏感路径（/api、/media、/private、/uploads）。
 * 同时匹配前缀本身（如 `/api`）与其子路径（如 `/api/v1/session`）。
 */
export function isSensitivePath(input: string | URL, base?: string): boolean {
  const path = toPathname(input, base)
  return SENSITIVE_PREFIXES.some((prefix) => {
    const bare = prefix.endsWith('/') ? prefix.slice(0, -1) : prefix
    return path === bare || path.startsWith(prefix)
  })
}

export interface CacheabilityContext {
  /** 是否与 Service Worker 同源；显式 false 时不可缓存。 */
  sameOrigin?: boolean
  /** request.destination，缺失时退化为扩展名判断。 */
  destination?: string
}

/**
 * 该请求能否进静态外壳缓存。
 *
 * @param input       请求 URL（绝对或相对均可）
 * @param method      HTTP 方法，默认 GET
 * @param context     sameOrigin / destination
 */
export function isCacheableRequest(
  input: string | URL,
  method = 'GET',
  context: CacheabilityContext = {},
): boolean {
  if ((method || 'GET').toUpperCase() !== 'GET') return false
  if (context.sameOrigin === false) return false
  if (isSensitivePath(input)) return false

  const destination = context.destination ?? ''
  if (destination.length > 0) {
    return SHELL_DESTINATIONS.includes(destination)
  }

  const path = toPathname(input).toLowerCase()
  return SHELL_EXTENSIONS.some((extension) => path.endsWith(extension))
}
