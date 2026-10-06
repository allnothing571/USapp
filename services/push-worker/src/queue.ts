/**
 * 投递队列——阶段 0 只是接口与内存实现的占位。
 *
 * 阶段 0 的取舍：
 * - 不引入 Redis、不引入消息中间件、不引入 pg-boss。
 * - 队列只存在于内存，进程重启即丢。这是可以接受的，因为阶段 0 根本
 *   没有真实推送（PUSH_WORKER_ENABLED 默认 false）。
 * - 之所以现在就把接口定下来，是为了让阶段 1+ 的替换点只有这一个文件。
 *
 * 阶段 1 的替换方向（写在这里以免后人在别处乱加持久化）：
 * C++ 主进程在【调度锁事务之外】把已成事实的业务事件写入一张 outbox 表，
 * 本进程只读取 outbox 中 `dispatched_at IS NULL` 的行并投递，
 * 投递成功后回写 dispatched_at。真正的跨实例去重由 outbox 行承担，
 * 不由本进程的锁承担。
 */

export interface PushMessage {
  /** 业务侧幂等键，用于投递去重；同一 key 重复入队只保留一条 */
  readonly idempotencyKey: string;
  /** 最终接收者标识（阶段 0 不解析、不校验，仅透传） */
  readonly recipientRef: string;
  /** 通知类别，用于用户侧偏好开关的后续接入 */
  readonly category: string;
  /** 已渲染好的展示内容；本进程不做业务语义判断 */
  readonly title: string;
  readonly body: string;
  /** 排队时间（epoch ms），由主进程给出 */
  readonly enqueuedAt: number;
}

export interface DeliveryOutcome {
  readonly idempotencyKey: string;
  readonly delivered: boolean;
  readonly reason: string;
  readonly attemptedAt: number;
}

export interface PushQueue {
  enqueue(message: PushMessage): void;
  size(): number;
  drain(limit: number): readonly PushMessage[];
}

/**
 * 内存队列。带容量上限，满了就拒绝新消息而不是无限增长——
 * 一个静默 OOM 的辅助进程比一个明确报错的辅助进程危险得多。
 */
export class InMemoryPushQueue implements PushQueue {
  readonly #capacity: number;
  readonly #items: PushMessage[] = [];
  readonly #seen = new Set<string>();

  constructor(capacity = 1000) {
    if (!Number.isInteger(capacity) || capacity <= 0) {
      throw new RangeError(`队列容量必须是正整数，实际收到 ${capacity}`);
    }
    this.#capacity = capacity;
  }

  enqueue(message: PushMessage): void {
    if (this.#seen.has(message.idempotencyKey)) {
      return;
    }
    if (this.#items.length >= this.#capacity) {
      throw new Error(`推送队列已满（容量 ${this.#capacity}），拒绝入队`);
    }
    this.#seen.add(message.idempotencyKey);
    this.#items.push(message);
  }

  size(): number {
    return this.#items.length;
  }

  drain(limit: number): readonly PushMessage[] {
    const take = Math.max(0, Math.min(limit, this.#items.length));
    const drained = this.#items.splice(0, take);
    for (const message of drained) {
      // 已出队即允许同一幂等键再次入队（重试语义）。
      this.#seen.delete(message.idempotencyKey);
    }
    return drained;
  }
}

/**
 * 阶段 0 的投递器：只记录，不发送。
 *
 * 它必须返回 delivered=false 而不是假装成功——把没发出去的推送标成已投递，
 * 会让阶段 1 的去重逻辑基于错误事实工作。
 */
export class DryRunDispatcher {
  constructor(private readonly logger: { info(msg: string, f?: Record<string, unknown>): void }) {}

  dispatch(messages: readonly PushMessage[]): readonly DeliveryOutcome[] {
    const now = Date.now();
    return messages.map((message) => {
      this.logger.info('推送投递为演练模式，未实际发送', {
        idempotencyKey: message.idempotencyKey,
        category: message.category,
      });
      return {
        idempotencyKey: message.idempotencyKey,
        delivered: false,
        reason: 'dry-run: 阶段 0 不接入真实推送服务',
        attemptedAt: now,
      };
    });
  }
}
