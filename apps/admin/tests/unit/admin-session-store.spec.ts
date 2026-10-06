import { createPinia, setActivePinia } from 'pinia';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import type { AdminSessionBootstrap } from '../../src/api/admin';
import { AdminApiError } from '../../src/api/client';

/**
 * 管理会话 store 单元测试。
 *
 * 重点验证两件事：
 * 1. 注销必须清空内存态 —— 无论服务端调用成功还是失败；
 * 2. store **不做任何持久化** —— 不碰 localStorage / sessionStorage / Cookie 写入。
 *    这不是"顺手测一下"，而是本模块的硬约束：把管理会话或 CSRF 令牌写进
 *    Web Storage，等于把 XSS 的破坏力从"当前页面"扩大到"下一次打开页面"。
 */

const mocks = vi.hoisted(() => ({
  bootstrapSession: vi.fn<() => Promise<AdminSessionBootstrap>>(),
  login: vi.fn<(username: string, password: string) => Promise<AdminSessionBootstrap>>(),
  logout: vi.fn<() => Promise<void>>(),
}));

vi.mock('../../src/api/admin', () => ({
  adminApi: {
    health: vi.fn(),
    bootstrapSession: mocks.bootstrapSession,
    login: mocks.login,
    logout: mocks.logout,
    listUsers: vi.fn(),
    listCouples: vi.fn(),
    listAuditLogs: vi.fn(),
  },
}));

// store 必须在 mock 之后导入，否则会绑定到真实的 adminApi 单例。
const { useAdminSessionStore } = await import('../../src/stores/admin-session');

const ADMIN_ID = '00000000-0000-0000-0000-000000000001';

function authenticatedBootstrap(): AdminSessionBootstrap {
  return {
    authenticated: true,
    csrf_token: 'csrf-live',
    admin: { id: ADMIN_ID, username: 'root' },
  };
}

describe('useAdminSessionStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    mocks.bootstrapSession.mockReset();
    mocks.login.mockReset();
    mocks.logout.mockReset();
    localStorage.clear();
    sessionStorage.clear();
  });

  it('初始状态为未认证且未引导', () => {
    const store = useAdminSessionStore();

    expect(store.authenticated).toBe(false);
    expect(store.admin).toBeNull();
    expect(store.bootstrapped).toBe(false);
    expect(store.hasError).toBe(false);
  });

  it('引导成功且已登录时填充身份信息', async () => {
    mocks.bootstrapSession.mockResolvedValue(authenticatedBootstrap());
    const store = useAdminSessionStore();

    await store.bootstrap();

    expect(store.authenticated).toBe(true);
    expect(store.admin).toEqual({ id: ADMIN_ID, username: 'root' });
    expect(store.adminLabel).toBe('root');
    expect(store.bootstrapped).toBe(true);
  });

  it('引导返回未认证时不保留任何管理员身份', async () => {
    mocks.bootstrapSession.mockResolvedValue({
      authenticated: false,
      csrf_token: 'csrf-pre',
      admin: { id: ADMIN_ID, username: 'root' },
    });
    const store = useAdminSessionStore();

    await store.bootstrap();

    expect(store.authenticated).toBe(false);
    // authenticated=false 时即使响应里带了 admin，也一律丢弃：
    // 未认证状态下的任何身份信息都是不可信的。
    expect(store.admin).toBeNull();
  });

  it('引导失败按未认证处理，并暴露错误码', async () => {
    mocks.bootstrapSession.mockRejectedValue(
      new AdminApiError(502, 'INTERNAL_ERROR', '管理 API 不可达'),
    );
    const store = useAdminSessionStore();

    await store.bootstrap();

    expect(store.authenticated).toBe(false);
    expect(store.errorCode).toBe('INTERNAL_ERROR');
    expect(store.errorMessage).toBe('管理 API 不可达');
    // 引导失败也要标记为"已尝试过"，否则路由守卫会在每次导航时重复重试。
    expect(store.bootstrapped).toBe(true);
  });

  it('登录成功填充身份，登录失败清空身份', async () => {
    mocks.login.mockResolvedValueOnce(authenticatedBootstrap());
    const store = useAdminSessionStore();

    await expect(store.login('root', 'pw')).resolves.toBe(true);
    expect(store.authenticated).toBe(true);
    expect(store.adminLabel).toBe('root');

    mocks.login.mockRejectedValueOnce(
      new AdminApiError(403, 'ORIGIN_NOT_ALLOWED', '请求来源不被允许'),
    );
    await expect(store.login('root', 'wrong')).resolves.toBe(false);
    expect(store.authenticated).toBe(false);
    expect(store.admin).toBeNull();
    expect(store.errorCode).toBe('ORIGIN_NOT_ALLOWED');
  });

  it('注销清空内存态（服务端调用成功）', async () => {
    mocks.bootstrapSession.mockResolvedValue(authenticatedBootstrap());
    mocks.logout.mockResolvedValue(undefined);
    const store = useAdminSessionStore();
    await store.bootstrap();
    expect(store.authenticated).toBe(true);

    await store.logout();

    expect(mocks.logout).toHaveBeenCalledTimes(1);
    expect(store.authenticated).toBe(false);
    expect(store.admin).toBeNull();
    expect(store.adminLabel).toBeNull();
    expect(store.hasError).toBe(false);
  });

  it('注销即使服务端调用失败也必须清空内存态', async () => {
    mocks.bootstrapSession.mockResolvedValue(authenticatedBootstrap());
    mocks.logout.mockRejectedValue(
      new AdminApiError(403, 'CSRF_TOKEN_INVALID', 'CSRF 令牌校验失败'),
    );
    const store = useAdminSessionStore();
    await store.bootstrap();

    await store.logout();

    // 用户点了注销，界面就必须立即表现为已注销。
    // 服务端会话是否真的撤销，是服务端与审计日志的责任，不是继续显示"已登录"的理由。
    expect(store.authenticated).toBe(false);
    expect(store.admin).toBeNull();
    expect(store.errorCode).toBe('CSRF_TOKEN_INVALID');
  });

  it('clear() 可独立用于清空内存态', async () => {
    mocks.bootstrapSession.mockResolvedValue(authenticatedBootstrap());
    const store = useAdminSessionStore();
    await store.bootstrap();

    store.clear();

    expect(store.authenticated).toBe(false);
    expect(store.admin).toBeNull();
    expect(store.errorCode).toBeNull();
    expect(store.errorMessage).toBeNull();
  });

  it('不经由动作擅自清空 bootstrapped 标记（守卫依赖它）', async () => {
    mocks.bootstrapSession.mockResolvedValue(authenticatedBootstrap());
    const store = useAdminSessionStore();
    await store.bootstrap();

    await store.logout();

    // 注销只清身份，不清"已引导"：CSRF 令牌仍是登录页需要的。
    expect(store.bootstrapped).toBe(true);
  });

  it('不写入任何 Web Storage（无持久化硬约束）', async () => {
    const setItemSpy = vi.spyOn(Storage.prototype, 'setItem');
    mocks.bootstrapSession.mockResolvedValue(authenticatedBootstrap());
    mocks.login.mockResolvedValue(authenticatedBootstrap());
    mocks.logout.mockResolvedValue(undefined);

    const store = useAdminSessionStore();
    await store.bootstrap();
    await store.login('root', 'pw');
    await store.logout();
    store.clear();

    expect(setItemSpy).not.toHaveBeenCalled();
    expect(localStorage.length).toBe(0);
    expect(sessionStorage.length).toBe(0);

    setItemSpy.mockRestore();
  });
});
