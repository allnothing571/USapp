/**
 * Thrown instead of applying a result that belongs to an earlier identity (logout, account switch
 * or user change happened while the request was in flight). Callers ignore it: the old result is
 * discarded and the current session stays untouched.
 */
export class StaleSessionError extends Error {
  constructor() {
    super('会话已变化，旧请求的结果已丢弃。')
    this.name = 'StaleSessionError'
  }
}
