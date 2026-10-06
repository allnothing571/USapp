/**
 * 管理 API 绑定层 —— 逐条对应 `contracts/openapi.yaml` 的 `/api/admin/v1/**`。
 *
 * 这里**只**封装契约已经定义的管理端点。契约里没有的端点（资产修正、
 * 阳光商店维护、备份触发、数据库/迁移状态……）在本文件里不存在，
 * 阶段 0 也不允许凭空发明。
 */

import { AdminHttpClient, type QueryValue } from './client';

// ─────────────────────────── 契约类型 ───────────────────────────

/** `schemas/Health` */
export interface Health {
  status: 'ok';
  version: string;
  time: string;
}

/** `schemas/AdminSessionBootstrap` 里的 `admin` 对象 */
export interface AdminIdentity {
  id: string;
  username: string;
}

/** `schemas/AdminSessionBootstrap` */
export interface AdminSessionBootstrap {
  authenticated: boolean;
  csrf_token: string;
  admin?: AdminIdentity;
}

/** `schemas/AdminUserList` 的条目 */
export interface AdminUserListItem {
  id: string;
  email: string;
  display_name: string;
  status: string;
  created_at: string;
}

/** `schemas/AdminUserList` */
export interface AdminUserList {
  items: AdminUserListItem[];
  next_cursor: string | null;
}

/** `schemas/AdminCoupleList` 的条目 */
export interface AdminCoupleListItem {
  id: string;
  name: string;
  member_count: number;
  created_at: string;
}

/** `schemas/AdminCoupleList` */
export interface AdminCoupleList {
  items: AdminCoupleListItem[];
  next_cursor: string | null;
}

/** `schemas/AdminAuditLogList` 的条目 */
export interface AdminAuditLogItem {
  id: number;
  actor_type: 'user' | 'admin' | 'system';
  action: string;
  created_at: string;
}

/** `schemas/AdminAuditLogList` */
export interface AdminAuditLogList {
  items: AdminAuditLogItem[];
  next_cursor: string | null;
}

/** 分页入参；`limit` 服务端上限 100、默认 50。 */
export interface AdminListParams {
  limit?: number;
  cursor?: string | null;
  /** 仅 `/users` 支持（契约参数 `Query`）。 */
  query?: string;
}

export interface AdminApi {
  health(): Promise<Health>;
  bootstrapSession(): Promise<AdminSessionBootstrap>;
  login(username: string, password: string): Promise<AdminSessionBootstrap>;
  logout(): Promise<void>;
  listUsers(params?: AdminListParams): Promise<AdminUserList>;
  listCouples(params?: AdminListParams): Promise<AdminCoupleList>;
  listAuditLogs(params?: AdminListParams): Promise<AdminAuditLogList>;
}

// ─────────────────────────── 实现 ───────────────────────────

function toQuery(params: AdminListParams | undefined): Record<string, QueryValue> {
  const query: Record<string, QueryValue> = {};
  if (!params) return query;
  if (params.limit !== undefined) query.limit = params.limit;
  if (params.cursor !== undefined && params.cursor !== null && params.cursor !== '') {
    query.cursor = params.cursor;
  }
  if (params.query !== undefined && params.query !== '') query.query = params.query;
  return query;
}

export function createAdminApi(http: AdminHttpClient): AdminApi {
  /**
   * 引导管理会话。
   * 该端点本身不改服务端状态，因此不需要 CSRF。
   * 未登录时服务端返回空令牌（管理面无预认证会话，登录接口依靠 Origin 与 X-Requested-By 防护）；
   * 已登录时返回当前会话绑定的有效 CSRF 令牌。因此引导后将响应令牌写入客户端（未登录时清空或写入 null）。
   */
  async function bootstrapSession(): Promise<AdminSessionBootstrap> {
    const payload = await http.request<AdminSessionBootstrap>('/session');
    http.setCsrfToken(payload.csrf_token);
    return payload;
  }

  return {
    health() {
      return http.request<Health>('/health');
    },

    bootstrapSession,

    async login(username, password) {
      const payload = await http.request<AdminSessionBootstrap>('/sessions', {
        method: 'POST',
        body: { username, password },
      });
      // 契约要求登录成功后更换会话标识与 CSRF 令牌 —— 必须用响应里的新令牌覆盖旧的。
      http.setCsrfToken(payload.csrf_token);
      return payload;
    },

    async logout() {
      await http.request<void>('/sessions', { method: 'DELETE' });
      // 服务端已撤销会话，本地令牌立即作废，避免复用已失效的令牌。
      http.setCsrfToken(null);
    },

    listUsers(params) {
      return http.request<AdminUserList>('/users', { query: toQuery(params) });
    },

    listCouples(params) {
      return http.request<AdminCoupleList>('/couples', { query: toQuery(params) });
    },

    listAuditLogs(params) {
      return http.request<AdminAuditLogList>('/audit-logs', { query: toQuery(params) });
    },
  };
}

/**
 * 全应用共享的管理 API 实例。
 *
 * 之所以是单例：CSRF 令牌必须跟着会话走，多个客户端实例会导致
 * "登录写进了 A 实例、列表请求读的是 B 实例"这种难查的 403。
 */
export const adminHttp = new AdminHttpClient();
export const adminApi: AdminApi = createAdminApi(adminHttp);
