import { describe, expect, it } from 'vitest';

import { createAdminApi } from '../../src/api/admin';
import {
  AdminApiError,
  AdminHttpClient,
  buildRequestUrl,
  isApiErrorEnvelope,
  type FetchLike,
} from '../../src/api/client';

/**
 * 管理 HTTP 客户端单元测试。
 *
 * 本文件刻意**不依赖** undici / jsdom 的 fetch 实现：
 * 用最小手写 stub 代替 `Response`，测试只验证本客户端自己的契约行为
 * （CSRF 头、缺失令牌、错误信封），不验证网络栈。
 */

interface FakeResponseInit {
  status?: number;
  /** 会被 `json()` 返回的对象。 */
  body?: unknown;
  /** 会被 `text()` 返回的原始文本；不给时由 `body` 序列化得到。 */
  raw?: string;
}

function makeResponse(init: FakeResponseInit = {}): Response {
  const status = init.status ?? 200;
  const raw = init.raw ?? (init.body === undefined ? '' : JSON.stringify(init.body));
  return {
    ok: status >= 200 && status < 300,
    status,
    json: async () => {
      if (raw === '') throw new SyntaxError('Unexpected end of JSON input');
      // 故意模拟真实行为：body 不是 JSON 时 json() 抛错。
      return JSON.parse(raw) as unknown;
    },
    text: async () => raw,
  } as unknown as Response;
}

interface RecordedRequest {
  url: string;
  method: string | undefined;
  headers: Record<string, string>;
  credentials: RequestCredentials | undefined;
  body: string | undefined;
}

/** 记录所有出站请求的假 fetch。 */
function createRecordingFetch(response: Response | (() => Response)) {
  const requests: RecordedRequest[] = [];
  const fetchImpl: FetchLike = async (input, init) => {
    requests.push({
      url: input,
      method: init?.method,
      headers: (init?.headers ?? {}) as Record<string, string>,
      credentials: init?.credentials,
      body: typeof init?.body === 'string' ? init.body : undefined,
    });
    return typeof response === 'function' ? response() : response;
  };
  return { fetchImpl, requests };
}

async function captureError(promise: Promise<unknown>): Promise<AdminApiError> {
  try {
    await promise;
  } catch (error) {
    if (error instanceof AdminApiError) return error;
    throw error;
  }
  throw new Error('预期抛出 AdminApiError，但请求成功了');
}

describe('buildRequestUrl', () => {
  it('拼接相对基址与绝对路径', () => {
    expect(buildRequestUrl('/api/admin/v1', '/users')).toBe('/api/admin/v1/users');
    expect(buildRequestUrl('/api/admin/v1/', 'users')).toBe('/api/admin/v1/users');
  });

  it('丢弃 null / undefined / 空字符串参数，保留 0 与 false', () => {
    const url = buildRequestUrl('/api/admin/v1', '/users', {
      limit: 50,
      cursor: null,
      query: undefined,
      empty: '',
      zero: 0,
      flag: false,
    });
    expect(url).toBe('/api/admin/v1/users?limit=50&zero=0&flag=false');
  });

  it('无有效参数时不产出问号', () => {
    expect(buildRequestUrl('/api/admin/v1', '/users', { cursor: null })).toBe(
      '/api/admin/v1/users',
    );
  });
});

describe('isApiErrorEnvelope', () => {
  it('识别契约信封', () => {
    expect(isApiErrorEnvelope({ error: { code: 'X', message: 'y' } })).toBe(true);
  });

  it('拒绝缺字段与非对象', () => {
    expect(isApiErrorEnvelope(null)).toBe(false);
    expect(isApiErrorEnvelope({})).toBe(false);
    expect(isApiErrorEnvelope({ error: { code: 'X' } })).toBe(false);
    expect(isApiErrorEnvelope({ error: 'boom' })).toBe(false);
  });
});

describe('AdminHttpClient — CSRF 与请求形状', () => {
  it('通用写请求携带 X-CSRF-Token、X-Requested-By、JSON 内容类型与 same-origin 凭据', async () => {
    const { fetchImpl, requests } = createRecordingFetch(makeResponse({ body: { ok: true } }));
    const client = new AdminHttpClient({ fetchImpl });
    client.setCsrfToken('csrf-abc123');

    await client.request('/audit-logs', {
      method: 'POST',
      body: { note: 'test' },
    });

    expect(requests).toHaveLength(1);
    const sent = requests[0]!;
    expect(sent.url).toBe('/api/admin/v1/audit-logs');
    expect(sent.method).toBe('POST');
    expect(sent.headers['X-CSRF-Token']).toBe('csrf-abc123');
    expect(sent.headers['X-Requested-By']).toBe('usapp-admin');
    expect(sent.headers['Content-Type']).toBe('application/json');
    expect(sent.headers.Accept).toBe('application/json');
    // 管理会话 Cookie（__Host-usapp-admin）必须随同源请求发送，
    // 且绝不能用 'include'（那会把管理请求暴露给跨源场景）。
    expect(sent.credentials).toBe('same-origin');
    expect(sent.body).toBe(JSON.stringify({ note: 'test' }));
  });

  it('准确的 POST /sessions 登录请求豁免 CSRF 令牌，并自动携带 X-Requested-By', async () => {
    const { fetchImpl, requests } = createRecordingFetch(makeResponse({ body: { ok: true } }));
    const client = new AdminHttpClient({ fetchImpl });

    await client.request('/sessions', {
      method: 'POST',
      body: { username: 'admin', password: 'pw' },
    });

    expect(requests).toHaveLength(1);
    const sent = requests[0]!;
    expect(sent.url).toBe('/api/admin/v1/sessions');
    expect(sent.method).toBe('POST');
    expect(sent.headers['X-Requested-By']).toBe('usapp-admin');
    expect(sent.headers['X-CSRF-Token']).toBeUndefined();
    expect(sent.headers['Content-Type']).toBe('application/json');
    expect(sent.credentials).toBe('same-origin');
    expect(sent.body).toBe(JSON.stringify({ username: 'admin', password: 'pw' }));
  });

  it('DELETE 同样需要 CSRF 令牌并携带 X-Requested-By', async () => {
    const { fetchImpl, requests } = createRecordingFetch(makeResponse({ status: 204 }));
    const client = new AdminHttpClient({ fetchImpl });
    client.setCsrfToken('csrf-xyz');

    await client.request('/sessions', { method: 'DELETE' });

    expect(requests[0]!.headers['X-CSRF-Token']).toBe('csrf-xyz');
    expect(requests[0]!.headers['X-Requested-By']).toBe('usapp-admin');
  });

  it('安全方法（GET）不携带 X-CSRF-Token 与 X-Requested-By', async () => {
    const { fetchImpl, requests } = createRecordingFetch(makeResponse({ body: { items: [] } }));
    const client = new AdminHttpClient({ fetchImpl });

    await client.request('/users', { method: 'GET' });

    expect(requests[0]!.headers['X-CSRF-Token']).toBeUndefined();
    expect(requests[0]!.headers['X-Requested-By']).toBeUndefined();
  });

  it('GET /sessions 安全方法不获得写豁免（不发送 X-Requested-By，亦无需 CSRF）', async () => {
    const { fetchImpl, requests } = createRecordingFetch(
      makeResponse({ body: { authenticated: false } }),
    );
    const client = new AdminHttpClient({ fetchImpl });

    await client.request('/sessions', { method: 'GET' });

    expect(requests).toHaveLength(1);
    expect(requests[0]!.headers['X-Requested-By']).toBeUndefined();
    expect(requests[0]!.headers['X-CSRF-Token']).toBeUndefined();
  });

  it('缺令牌的注销（DELETE /sessions）在发送前被拦截', async () => {
    const { fetchImpl, requests } = createRecordingFetch(makeResponse({ status: 204 }));
    const client = new AdminHttpClient({ fetchImpl });

    const error = await captureError(client.request('/sessions', { method: 'DELETE' }));

    expect(error.code).toBe('CSRF_TOKEN_MISSING');
    // status 0 表示"请求根本没发出去"，与真实的 HTTP 失败区分开。
    expect(error.status).toBe(0);
    expect(requests).toHaveLength(0);
  });

  it('缺令牌的其他写请求在发送前被拦截', async () => {
    const { fetchImpl, requests } = createRecordingFetch(makeResponse({ body: {} }));
    const client = new AdminHttpClient({ fetchImpl });

    const error = await captureError(
      client.request('/audit-logs', { method: 'POST', body: { note: 'test' } }),
    );

    expect(error.code).toBe('CSRF_TOKEN_MISSING');
    expect(error.status).toBe(0);
    expect(requests).toHaveLength(0);
  });

  it('前缀类似路径（如 /sessions/detail）不能获得登录豁免，缺令牌被拦截', async () => {
    const { fetchImpl, requests } = createRecordingFetch(makeResponse({ body: {} }));
    const client = new AdminHttpClient({ fetchImpl });

    const error = await captureError(
      client.request('/sessions/detail', { method: 'POST', body: {} }),
    );

    expect(error.code).toBe('CSRF_TOKEN_MISSING');
    expect(error.status).toBe(0);
    expect(requests).toHaveLength(0);
  });

  it('空字符串令牌视同没有令牌（非登录写请求被拦截）', async () => {
    const { fetchImpl, requests } = createRecordingFetch(makeResponse({ body: {} }));
    const client = new AdminHttpClient({ fetchImpl });
    client.setCsrfToken('');

    const error = await captureError(client.request('/sessions', { method: 'DELETE' }));

    expect(error.code).toBe('CSRF_TOKEN_MISSING');
    expect(requests).toHaveLength(0);
  });

  it('单次请求可覆盖令牌，且不改变客户端持有的令牌', async () => {
    const { fetchImpl, requests } = createRecordingFetch(makeResponse({ status: 204 }));
    const client = new AdminHttpClient({ fetchImpl });
    client.setCsrfToken('held');

    await client.request('/sessions', { method: 'DELETE', csrfToken: 'override' });

    expect(requests[0]!.headers['X-CSRF-Token']).toBe('override');
    expect(requests[0]!.headers['X-Requested-By']).toBe('usapp-admin');
    expect(client.getCsrfToken()).toBe('held');
  });

  it('204 响应返回 undefined 而不是解析失败', async () => {
    const { fetchImpl } = createRecordingFetch(makeResponse({ status: 204 }));
    const client = new AdminHttpClient({ fetchImpl });
    client.setCsrfToken('t');

    await expect(client.request('/sessions', { method: 'DELETE' })).resolves.toBeUndefined();
  });
});

describe('AdminHttpClient — 错误信封解析', () => {
  it('解析 {error:{code,message,details}} 到 AdminApiError', async () => {
    const { fetchImpl } = createRecordingFetch(
      makeResponse({
        status: 403,
        body: {
          error: {
            code: 'CSRF_TOKEN_INVALID',
            message: 'CSRF 令牌校验失败',
            details: { reason: 'rotated' },
          },
        },
      }),
    );
    const client = new AdminHttpClient({ fetchImpl });
    client.setCsrfToken('t');

    const error = await captureError(client.request('/sessions', { method: 'DELETE' }));

    expect(error).toBeInstanceOf(AdminApiError);
    expect(error.status).toBe(403);
    expect(error.code).toBe('CSRF_TOKEN_INVALID');
    expect(error.message).toBe('CSRF 令牌校验失败');
    expect(error.details).toEqual({ reason: 'rotated' });
  });

  it('401 UNAUTHENTICATED 同样按信封解析', async () => {
    const { fetchImpl } = createRecordingFetch(
      makeResponse({
        status: 401,
        body: { error: { code: 'UNAUTHENTICATED', message: '会话已失效' } },
      }),
    );
    const client = new AdminHttpClient({ fetchImpl });

    const error = await captureError(client.request('/users'));

    expect(error.status).toBe(401);
    expect(error.code).toBe('UNAUTHENTICATED');
    expect(error.details).toBeUndefined();
  });

  it('非契约响应体（网关 HTML / 空体）退回 INTERNAL_ERROR 并保留 HTTP 状态', async () => {
    const { fetchImpl } = createRecordingFetch(
      makeResponse({ status: 502, raw: '<html>502 Bad Gateway</html>' }),
    );
    const client = new AdminHttpClient({ fetchImpl });

    const error = await captureError(client.request('/health'));

    expect(error.code).toBe('INTERNAL_ERROR');
    expect(error.status).toBe(502);
    expect(error.message).toContain('502');
  });

  it('成功响应体不是 JSON 时抛 INTERNAL_ERROR', async () => {
    const { fetchImpl } = createRecordingFetch(makeResponse({ status: 200, raw: 'not json' }));
    const client = new AdminHttpClient({ fetchImpl });

    const error = await captureError(client.request('/health'));

    expect(error.code).toBe('INTERNAL_ERROR');
    expect(error.status).toBe(200);
  });
});

describe('AdminApi — 会话引导与令牌轮换', () => {
  it('未登录引导返回空令牌后，登录能够发出，并携带正确请求标记', async () => {
    const responses: Response[] = [
      makeResponse({
        body: { authenticated: false, csrf_token: '' },
      }),
      makeResponse({
        body: {
          authenticated: true,
          csrf_token: 'login-token-123',
          admin: { id: '00000000-0000-0000-0000-000000000001', username: 'root' },
        },
      }),
    ];
    const { fetchImpl, requests } = createRecordingFetch(() => responses.shift()!);
    const client = new AdminHttpClient({ fetchImpl });
    const api = createAdminApi(client);

    const boot = await api.bootstrapSession();
    expect(boot.authenticated).toBe(false);
    expect(boot.csrf_token).toBe('');
    expect(client.getCsrfToken()).toBeNull();
    expect(requests[0]!.headers['X-CSRF-Token']).toBeUndefined();
    expect(requests[0]!.headers['X-Requested-By']).toBeUndefined();

    const loggedIn = await api.login('root', 'pw');
    expect(loggedIn.authenticated).toBe(true);
    // 登录请求为豁免路由，不携带 X-CSRF-Token，但必须携带 X-Requested-By 标记
    expect(requests[1]!.url).toBe('/api/admin/v1/sessions');
    expect(requests[1]!.method).toBe('POST');
    expect(requests[1]!.headers['X-Requested-By']).toBe('usapp-admin');
    expect(requests[1]!.headers['X-CSRF-Token']).toBeUndefined();
    // 登录成功后保存服务端下发的新令牌
    expect(client.getCsrfToken()).toBe('login-token-123');
  });

  it('登录成功保存新令牌，注销请求携带该令牌与写请求标记', async () => {
    const responses: Response[] = [
      makeResponse({
        body: {
          authenticated: true,
          csrf_token: 'live-token-456',
          admin: { id: '00000000-0000-0000-0000-000000000001', username: 'root' },
        },
      }),
      makeResponse({ status: 204 }),
    ];
    const { fetchImpl, requests } = createRecordingFetch(() => responses.shift()!);
    const client = new AdminHttpClient({ fetchImpl });
    const api = createAdminApi(client);

    await api.login('root', 'pw');
    expect(client.getCsrfToken()).toBe('live-token-456');

    await api.logout();
    expect(requests[1]!.url).toBe('/api/admin/v1/sessions');
    expect(requests[1]!.method).toBe('DELETE');
    expect(requests[1]!.headers['X-CSRF-Token']).toBe('live-token-456');
    expect(requests[1]!.headers['X-Requested-By']).toBe('usapp-admin');
    // 注销后本地令牌清空
    expect(client.getCsrfToken()).toBeNull();
  });

  it('列表端点的查询参数与契约参数名一致', async () => {
    const { fetchImpl, requests } = createRecordingFetch(
      makeResponse({ body: { items: [], next_cursor: null } }),
    );
    const client = new AdminHttpClient({ fetchImpl });
    const api = createAdminApi(client);

    await api.listUsers({ limit: 25, cursor: 'c1', query: 'a@b.c' });
    await api.listCouples({ limit: 10 });
    await api.listAuditLogs({});

    expect(requests[0]!.url).toBe('/api/admin/v1/users?limit=25&cursor=c1&query=a%40b.c');
    expect(requests[1]!.url).toBe('/api/admin/v1/couples?limit=10');
    // 契约未给 /couples、/audit-logs 定义 query 参数，客户端也不应凭空空造。
    expect(requests[2]!.url).toBe('/api/admin/v1/audit-logs');
    for (const request of requests) {
      expect(request.method).toBe('GET');
    }
  });
});
