/**
 * 推送辅助进程的责任边界。
 *
 * 这个文件是本进程存在的理由里最容易被侵蚀的部分：一个"顺手也能做"的
 * Node 进程很容易被加上定时结算、资产扣减、跨实例锁——那样阶段 0 的
 * 「Node 进程不接管业务调度或资产操作」这条硬约束就废了。
 *
 * 所以边界不是写在文档里，而是写成运行时断言：
 * 任何被本进程执行的操作都必须显式登记在 ALLOWED_OPERATIONS 里，
 * 否则抛出 PolicyViolation。测试覆盖了所有禁止项。
 */

export const ALLOWED_OPERATIONS = [
  /** 自检：返回进程健康与配置摘要 */
  'push.health',
  /** 投递一条已经由 C++ 主进程决定好的推送消息（阶段 1+ 接入） */
  'push.deliver',
] as const;

export type AllowedOperation = (typeof ALLOWED_OPERATIONS)[number];

export interface ForbiddenResponsibility {
  /** 前缀式操作名；任何以该前缀开头的操作都被拒绝 */
  readonly pattern: string;
  /** 为什么禁止；这段理由会在违规时原文抛给调用者 */
  readonly reason: string;
}

/**
 * 被明确禁止由本进程承担的责任。
 *
 * 采用前缀匹配而不是枚举，是为了覆盖 "task.settle.*" 这类不断长出新子类的
 * 操作族——枚举总是会漏。
 */
export const FORBIDDEN_RESPONSIBILITIES: readonly ForbiddenResponsibility[] = Object.freeze([
  {
    pattern: 'task.',
    reason: '任务状态流转与结算属于 C++ 主进程的业务职责，Node 侧只消费已决定好的投递结果。',
  },
  {
    pattern: 'cycle.',
    reason: '周期结算（含个人连续完成计数）必须在主进程内、在调度锁事务的保护下完成。',
  },
  {
    pattern: 'asset.',
    reason: '资产操作（小红花、阳光值）必须与业务数据同事务，Node 进程没有该事务边界。',
  },
  {
    pattern: 'flower.',
    reason: '小红花扣减/兑换属于资产操作，禁止在本进程执行。',
  },
  {
    pattern: 'couple.',
    reason: '情侣空间数据是私人业务数据，本进程不读写、不缓存、不持久化。',
  },
  {
    pattern: 'scheduler.',
    reason: '跨实例调度锁（pg_try_advisory_xact_lock）由 C++ 主进程持有；本进程不获取任何数据库级锁。',
  },
  {
    pattern: 'advisory-lock.',
    reason: '同上：本进程绝不调用 pg_advisory_lock / pg_try_advisory_xact_lock 系列函数。',
  },
  {
    pattern: 'db.write.',
    reason: '本进程在阶段 0 不持有数据库凭证，也不执行任何写操作。',
  },
  {
    pattern: 'admin.',
    reason: '管理面认证与操作只存在于 C++ 管理监听端口，本进程不提供管理功能。',
  },
]);

export class PolicyViolation extends Error {
  readonly operation: string;
  readonly reason: string;

  constructor(operation: string, reason: string) {
    super(`操作 "${operation}" 被推送辅助进程的责任边界拒绝：${reason}`);
    this.name = 'PolicyViolation';
    this.operation = operation;
    this.reason = reason;
  }
}

function isAllowed(operation: string): boolean {
  return (ALLOWED_OPERATIONS as readonly string[]).includes(operation);
}

function findForbidden(operation: string): ForbiddenResponsibility | undefined {
  return FORBIDDEN_RESPONSIBILITIES.find(
    (entry) => operation === entry.pattern || operation.startsWith(entry.pattern),
  );
}

/**
 * 在执行任何操作前调用。
 *
 * 检查顺序有意为之：先查禁止项，再查允许清单。
 * 这样即使有人手滑把一个业务操作塞进 ALLOWED_OPERATIONS，
 * 禁止项仍然会先拦住它（"默认拒绝"优先于"显式允许"的误配）。
 */
export function assertAllowedOperation(operation: string): void {
  const forbidden = findForbidden(operation);
  if (forbidden !== undefined) {
    throw new PolicyViolation(operation, forbidden.reason);
  }
  if (!isAllowed(operation)) {
    throw new PolicyViolation(
      operation,
      '该操作未登记在 ALLOWED_OPERATIONS 中。新增能力必须同时更新允许清单与责任边界测试。',
    );
  }
}

/** 便于在启动日志与 /health 中自证边界。 */
export function describeResponsibilityBoundary(): {
  allowed: readonly string[];
  forbidden: readonly string[];
} {
  return {
    allowed: ALLOWED_OPERATIONS,
    forbidden: FORBIDDEN_RESPONSIBILITIES.map((entry) => entry.pattern),
  };
}
