import { defineStore } from 'pinia';
import { computed, ref } from 'vue';

import { adminApi, type AdminIdentity } from '@/api/admin';
import { AdminApiError } from '@/api/client';

function describeError(error: unknown): { code: string; message: string } {
  if (error instanceof AdminApiError) {
    return { code: error.code, message: error.message };
  }
  if (error instanceof Error) {
    return { code: 'UNKNOWN_ERROR', message: error.message };
  }
  return { code: 'UNKNOWN_ERROR', message: String(error) };
}

/**
 * 管理会话状态。
 *
 * 硬约束：
 * - **纯内存**，无 `localStorage` / `sessionStorage` / IndexedDB / Cookie 写入。
 *   刷新页面 = 状态归零 = 重新走 `GET /api/admin/v1/session` 引导。
 *   把管理会话标志或 CSRF 令牌持久化到 Web Storage 只会扩大 XSS 的影响面，
 *   而管理界面本来就不需要"记住我"。
 * - 这里的状态**不是**授权依据，只是渲染依据。
 *   服务端对每个请求独立校验 `__Host-usapp-admin` 会话与 CSRF。
 */
export const useAdminSessionStore = defineStore('admin-session', () => {
  // ── 状态 ──
  const authenticated = ref(false);
  /** Invalidate operation state even when the same admin signs in again. */
  const epoch = ref(0);
  const admin = ref<AdminIdentity | null>(null);
  /** 仅用于展示"我已经引导过了"，不作为认证凭据使用。 */
  const bootstrapped = ref(false);
  const bootstrapping = ref(false);
  const submitting = ref(false);
  const errorCode = ref<string | null>(null);
  const errorMessage = ref<string | null>(null);

  // ── 派生 ──
  const adminLabel = computed(() => admin.value?.username ?? null);
  const hasError = computed(() => errorMessage.value !== null);

  function clearError(): void {
    errorCode.value = null;
    errorMessage.value = null;
  }

  function fail(error: unknown): void {
    const { code, message } = describeError(error);
    errorCode.value = code;
    errorMessage.value = message;
  }

  /**
   * 只清身份，保留错误信息与 bootstrapped 标记。
   * 用于"需要立刻表现为已注销，但还想把失败原因显示出来"的场景。
   */
  function clearIdentity(): void {
    epoch.value++;
    authenticated.value = false;
    admin.value = null;
  }

  /**
   * 清空全部内存态（身份 + 错误）。
   * logout 与"服务端说我们没登录"都必须走这里：
   * 任何残留的 `authenticated=true` 都会让 UI 显示一个不存在的会话。
   */
  function clear(): void {
    clearIdentity();
    clearError();
  }

  function applyBootstrap(payload: {
    authenticated: boolean;
    admin?: AdminIdentity;
  }): void {
    epoch.value++;
    authenticated.value = payload.authenticated;
    admin.value = payload.authenticated ? (payload.admin ?? null) : null;
    bootstrapped.value = true;
  }

  /**
   * 会话引导。同时取回 CSRF 令牌（令牌由 api 层写入 HTTP 客户端）。
   * 路由守卫在首次导航时调用一次；失败不阻塞路由 ——
   * 失败意味着"服务端不可达"，此时应该显示错误并停在登录页，而不是白屏。
   */
  async function bootstrap(): Promise<void> {
    if (bootstrapping.value) return;
    bootstrapping.value = true;
    clearError();
    try {
      const payload = await adminApi.bootstrapSession();
      applyBootstrap(payload);
    } catch (error) {
      // 引导失败 = 无法确认任何身份，一律按未认证处理。
      clear();
      bootstrapped.value = true;
      fail(error);
    } finally {
      bootstrapping.value = false;
    }
  }

  async function login(username: string, password: string): Promise<boolean> {
    submitting.value = true;
    clearError();
    try {
      const payload = await adminApi.login(username, password);
      applyBootstrap(payload);
      return payload.authenticated;
    } catch (error) {
      // 登录失败时不能继承上一次会话的任何渲染状态。
      clear();
      fail(error);
      return false;
    } finally {
      submitting.value = false;
    }
  }

  /**
   * 注销。
   *
   * 刻意在请求**之前**就清空内存身份，并在**任何情况下**都不恢复它，
   * 即使服务端调用失败：
   * 用户点了注销，本地就必须立即表现为已注销。
   * 服务端会话的撤销由服务端负责；如果它没撤销成功，
   * 那是审计日志层面的事，不该让前端继续展示一个"看起来还在登录"的界面。
   * 调用失败的原因仍然会记录到 errorCode/errorMessage 供页面提示。
   */
  async function logout(): Promise<void> {
    submitting.value = true;
    clearError();
    clearIdentity();
    try {
      await adminApi.logout();
    } catch (error) {
      fail(error);
    } finally {
      submitting.value = false;
    }
  }

  return {
    epoch,
    // 状态
    authenticated,
    admin,
    bootstrapped,
    bootstrapping,
    submitting,
    errorCode,
    errorMessage,
    // 派生
    adminLabel,
    hasError,
    // 动作
    bootstrap,
    login,
    logout,
    clear,
    clearError,
  };
});
