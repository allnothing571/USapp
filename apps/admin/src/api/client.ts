/**
 * 管理后台 HTTP 客户端 —— **完全独立于 apps/web**。
 *
 * ── 与情侣端客户端的关键差异（不要合并这两个文件） ──
 *
 * 1. 会话 Cookie 不同。
 *    情侣端使用 `__Host-usapp-session`；管理后台使用 `__Host-usapp-admin`。
 *    两者是不同的 Cookie、不同的服务端会话表、不同的认证主体。
 *    管理账号与"情侣账号体系"没有任何关系，管理员不是"某个用户"。
 *
 *    两种 Cookie 都是 `HttpOnly`，因此**前端永远读不到、也不应该读**会话标识。
 *    前端只持有 CSRF 令牌（一个可公开、不可用于冒充的关联令牌）。
 *
 * 2. 管理入口只绑定回环地址。
 *    生产入口是受信反向代理 + 仅回环监听（契约："/api/admin/v1/** 仅回环 / 受信代理可达"）。
 *    本前端假定"能打开这个页面 = 网络位置已经被服务端校验过"，
 *    这**不是**授权判断，只是一个前提说明。
 *
 * 3. 前端不做任何"权限判断"。
 *    服务端才是唯一权威。本文件中没有任何 `isAdmin` / `hasPermission` 之类的逻辑，
 *    也没有隐藏按钮充当"安全"的做法。前端隐藏一个按钮只是 UI 取舍；
 *    服务端对每一个 `/api/admin/v1/**` 请求独立校验来源、会话与 CSRF。
 *    任何"前端判断过了所以可以放心"的推理都是错的。
 *
 * 4. 状态修改请求（POST/PUT/PATCH/DELETE）自动携带 `X-Requested-By: usapp-admin`。
 *    除 `POST /sessions`（管理员登录端点）采用精确 Origin 与自定义头防护而豁免 CSRF 令牌外，
 *    所有其他管理写请求（包括 DELETE /sessions）必须携带有效 `X-CSRF-Token`。
 *    令牌由 `GET /session`（已登录状态）或 `POST /sessions`（登录成功响应）下发；
 *    非登录写请求在缺少令牌时会在**发出请求之前**直接抛错（status=0），避免无效网络往返。
 */

/** 服务端错误信封：契约 `components/schemas/ErrorEnvelope`。 */
export interface ApiErrorPayload {
  code: string;
  message: string;
  details?: Record<string, unknown>;
}

export interface ApiErrorEnvelope {
  error: ApiErrorPayload;
}

/** 判定任意响应体是否为契约错误信封。 */
export function isApiErrorEnvelope(value: unknown): value is ApiErrorEnvelope {
  if (typeof value !== 'object' || value === null) return false;
  const error = (value as { error?: unknown }).error;
  if (typeof error !== 'object' || error === null) return false;
  const { code, message } = error as { code?: unknown; message?: unknown };
  return typeof code === 'string' && typeof message === 'string';
}

/**
 * 所有管理 API 失败的统一异常。
 *
 * `status === 0` 表示"请求根本没有发出"（例如缺少 CSRF 令牌），
 * 用来与真实的 HTTP 失败区分开。
 */
export class AdminApiError extends Error {
  readonly status: number;
  readonly code: string;
  readonly details: Record<string, unknown> | undefined;

  constructor(
    status: number,
    code: string,
    message: string,
    details?: Record<string, unknown>,
  ) {
    super(message);
    this.name = 'AdminApiError';
    this.status = status;
    this.code = code;
    this.details = details;
  }
}

export type HttpMethod = 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE';

/** 需要 CSRF 令牌与写标记的方法。GET/HEAD 是安全方法。 */
const WRITE_METHODS: ReadonlySet<HttpMethod> = new Set<HttpMethod>([
  'POST',
  'PUT',
  'PATCH',
  'DELETE',
]);

/** 管理面写请求标记头（与 services/api 的 AdminFilter 约定一致）。 */
export const ADMIN_REQUESTED_BY_HEADER = 'X-Requested-By';
export const ADMIN_REQUESTED_BY_VALUE = 'usapp-admin';

export type QueryValue = string | number | boolean | null | undefined;

export interface AdminRequestOptions {
  method?: HttpMethod;
  /** 查询参数；`undefined` / `null` / 空字符串一律不入队。 */
  query?: Record<string, QueryValue>;
  /** JSON 请求体。 */
  body?: unknown;
  /** 覆盖本次请求使用的 CSRF 令牌；默认使用客户端当前持有的令牌。 */
  csrfToken?: string | null;
  signal?: AbortSignal;
  headers?: Record<string, string>;
}

export type FetchLike = (input: string, init?: RequestInit) => Promise<Response>;

export interface AdminHttpClientOptions {
  /**
   * API 基址。默认 `/api/admin/v1`（相对路径 —— 与 Web 同源部署，
   * 不走 CORS，也不接受绝对地址，避免把管理会话送去第三方域名）。
   */
  baseUrl?: string;
  fetchImpl?: FetchLike;
}

/** 默认基址；构建期可用 `VITE_ADMIN_API_BASE` 覆盖（仅限同源路径）。 */
const DEFAULT_BASE_URL = '/api/admin/v1';

/**
 * 把相对路径与查询参数拼成请求 URL。
 * 保留相对形式（`/api/...`）—— 同源请求不需要、也不应该拼绝对 origin。
 */
export function buildRequestUrl(
  baseUrl: string,
  path: string,
  query?: Record<string, QueryValue>,
): string {
  const normalizedBase = baseUrl.replace(/\/+$/, '');
  const normalizedPath = path.startsWith('/') ? path : `/${path}`;
  const url = `${normalizedBase}${normalizedPath}`;

  if (!query) return url;

  const search = new URLSearchParams();
  for (const [key, value] of Object.entries(query)) {
    if (value === undefined || value === null) continue;
    if (typeof value === 'string' && value === '') continue;
    search.append(key, String(value));
  }
  const qs = search.toString();
  return qs ? `${url}?${qs}` : url;
}

export class AdminHttpClient {
  private readonly baseUrl: string;
  private readonly fetchImpl: FetchLike;
  private csrfToken: string | null = null;

  constructor(options: AdminHttpClientOptions = {}) {
    const base = options.baseUrl ?? import.meta.env.VITE_ADMIN_API_BASE ?? DEFAULT_BASE_URL;
    this.baseUrl = base.replace(/\/+$/, '');
    // 延迟解析 globalThis.fetch：构造期不绑定，便于测试注入与运行时替换。
    this.fetchImpl = options.fetchImpl ?? ((input, init) => globalThis.fetch(input, init));
  }

  /** 当前持有的 CSRF 令牌（可读，用于调试面板；不可持久化）。 */
  getCsrfToken(): string | null {
    return this.csrfToken;
  }

  /**
   * 设置 CSRF 令牌。只保存在内存里：
   * 管理后台不做持久化，刷新页面必然重新走 `GET /session` 引导。
   * 这是刻意的 —— 令牌落进 localStorage 就等于给了 XSS 一个可用的写权限。
   */
  setCsrfToken(token: string | null): void {
    this.csrfToken = token && token.length > 0 ? token : null;
  }

  async request<T>(path: string, options: AdminRequestOptions = {}): Promise<T> {
    const method: HttpMethod = options.method ?? 'GET';
    const url = buildRequestUrl(this.baseUrl, path, options.query);

    const headers: Record<string, string> = {
      Accept: 'application/json',
      ...options.headers,
    };

    if (WRITE_METHODS.has(method)) {
      // 所有管理写请求自动携带管理面请求标记，供服务端 AdminFilter 校验
      headers[ADMIN_REQUESTED_BY_HEADER] = ADMIN_REQUESTED_BY_VALUE;

      const normalizedPath = path.startsWith('/') ? path : `/${path}`;
      const isLoginRequest = method === 'POST' && normalizedPath === '/sessions';
      const token = options.csrfToken ?? this.csrfToken;

      if (!isLoginRequest) {
        if (!token) {
          // 无令牌 = 请求注定 403。在本地直接失败，省掉一次往返，
          // 也让"忘了先引导会话"这个 bug 在开发阶段就暴露。
          throw new AdminApiError(
            0,
            'CSRF_TOKEN_MISSING',
            '缺少 CSRF 令牌：写请求前必须先完成会话引导与登录认证。',
          );
        }
        headers['X-CSRF-Token'] = token;
      } else if (token) {
        headers['X-CSRF-Token'] = token;
      }
    }

    let bodyInit: string | undefined;
    if (options.body !== undefined) {
      headers['Content-Type'] = 'application/json';
      bodyInit = JSON.stringify(options.body);
    }

    const response = await this.fetchImpl(url, {
      method,
      headers,
      body: bodyInit,
      // 同源 Cookie（__Host-usapp-admin）随请求发送。
      // 绝不使用 'include'：管理 API 不存在任何跨源调用场景。
      credentials: 'same-origin',
      // 管理 API 响应一律 no-store，这里再显式声明一次客户端的期望。
      cache: 'no-store',
      redirect: 'error',
      signal: options.signal,
    });

    if (!response.ok) {
      throw await this.toApiError(response);
    }

    return await this.parseSuccessBody<T>(response);
  }

  /** 把失败响应翻译成 `AdminApiError`；响应体不是契约信封时给出兜底错误码。 */
  private async toApiError(response: Response): Promise<AdminApiError> {
    let payload: unknown = null;
    try {
      payload = await response.json();
    } catch {
      payload = null;
    }

    if (isApiErrorEnvelope(payload)) {
      return new AdminApiError(
        response.status,
        payload.error.code,
        payload.error.message,
        payload.error.details,
      );
    }

    // 兜底：契约规定错误响应 `{error:{code,message}}`，但代理/网关层的
    // 502/503 可能返回 HTML 或空体。此时用 INTERNAL_ERROR 标记，
    // 并在 message 里保留 HTTP 状态，避免前端显示 "undefined"。
    return new AdminApiError(
      response.status,
      'INTERNAL_ERROR',
      `管理 API 返回了非契约错误响应（HTTP ${response.status}）。`,
    );
  }

  private async parseSuccessBody<T>(response: Response): Promise<T> {
    if (response.status === 204 || response.status === 205) {
      return undefined as T;
    }

    const text = await response.text();
    if (text.length === 0) {
      return undefined as T;
    }

    try {
      return JSON.parse(text) as T;
    } catch {
      throw new AdminApiError(
        response.status,
        'INTERNAL_ERROR',
        '管理 API 返回了无法解析的响应体（期望 JSON）。',
      );
    }
  }
}
