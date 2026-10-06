/**
 * 队列语义测试。
 *
 * 只编写，不执行。
 */

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { InMemoryPushQueue, DryRunDispatcher, type PushMessage } from '../src/queue.js';

function message(key: string): PushMessage {
  return {
    idempotencyKey: key,
    recipientRef: 'user-1',
    category: 'task.reminder',
    title: '标题',
    body: '正文',
    enqueuedAt: 1_700_000_000_000,
  };
}

describe('推送队列', () => {
  it('按幂等键去重', () => {
    const queue = new InMemoryPushQueue(10);
    queue.enqueue(message('k1'));
    queue.enqueue(message('k1'));
    assert.equal(queue.size(), 1);
  });

  it('容量满时明确拒绝，不静默增长', () => {
    const queue = new InMemoryPushQueue(1);
    queue.enqueue(message('k1'));
    assert.throws(() => queue.enqueue(message('k2')), /队列已满/);
  });

  it('drain 按上限取出并允许同一键重新入队', () => {
    const queue = new InMemoryPushQueue(10);
    queue.enqueue(message('k1'));
    queue.enqueue(message('k2'));
    const drained = queue.drain(1);
    assert.equal(drained.length, 1);
    assert.equal(queue.size(), 1);
    // 出队后允许重试同一幂等键。
    queue.enqueue(message('k1'));
    assert.equal(queue.size(), 2);
  });

  it('非法容量被拒绝', () => {
    assert.throws(() => new InMemoryPushQueue(0), RangeError);
    assert.throws(() => new InMemoryPushQueue(-1), RangeError);
  });

  it('演练投递器绝不谎报已投递', () => {
    const lines: string[] = [];
    const dispatcher = new DryRunDispatcher({
      info: (msg) => {
        lines.push(msg);
      },
    });
    const outcomes = dispatcher.dispatch([message('k1')]);
    assert.equal(outcomes.length, 1);
    assert.equal(outcomes[0]?.delivered, false);
    assert.match(outcomes[0]?.reason ?? '', /dry-run/);
    assert.equal(lines.length, 1);
  });
});
