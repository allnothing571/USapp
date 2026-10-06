/**
 * CSRF 令牌的内存持有者。
 *
 * 设计原因（避免循环依赖）：
 * - session store 是唯一的**写入方**（bootstrap / login / logout 时更新）。
 * - HTTP 层（api/client.ts）只做**读取**。
 * - 如果 client.ts 直接 import store，就会形成 store → client → store 的循环，
 *   所以把令牌放进这个零依赖模块，由 store 调用 setCsrfToken() 注入。
 *
 * 硬约束：令牌**只存在于内存**。任何持久化（localStorage / sessionStorage /
 * document.cookie / IndexedDB）都是违规的，因为它会绕过服务端的会话轮换。
 */

let csrfToken: string | null = null

/** 写入当前 CSRF 令牌；传 null 表示清空（注销、引导失败、切换账号）。 */
export function setCsrfToken(token: string | null): void {
  csrfToken = token && token.length > 0 ? token : null
}

/** 读取当前 CSRF 令牌；未引导或已清空时返回 null。 */
export function getCsrfToken(): string | null {
  return csrfToken
}
