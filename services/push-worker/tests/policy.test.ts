/**
 * 责任边界测试。
 *
 * 这是本进程最重要的一组测试：它把「Node 进程不接管业务调度或资产操作」
 * 从一句文档变成一条会失败的断言。任何人往这个进程里加业务能力，
 * 都必须先改 ALLOWED_OPERATIONS，而改这里就会让下面的测试亮红。
 *
 * 只编写，不执行（阶段 0 由负责人手动运行）。
 */

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import {
  ALLOWED_OPERATIONS,
  FORBIDDEN_RESPONSIBILITIES,
  PolicyViolation,
  assertAllowedOperation,
  describeResponsibilityBoundary,
} from '../src/policy.js';

describe('推送辅助进程责任边界', () => {
  it('允许已登记的操作', () => {
    for (const operation of ALLOWED_OPERATIONS) {
      assert.doesNotThrow(() => assertAllowedOperation(operation));
    }
  });

  it('拒绝未登记的操作（默认拒绝）', () => {
    assert.throws(
      () => assertAllowedOperation('unregistered.whatever'),
      (error: unknown) => error instanceof PolicyViolation,
    );
  });

  it('拒绝全部禁止族下的操作', () => {
    // 每个禁止前缀都必须真的能拦下一个具体操作名，
    // 否则这条边界只是一行没人会命中的配置。
    for (const entry of FORBIDDEN_RESPONSIBILITIES) {
      const probe = entry.pattern.endsWith('.') ? `${entry.pattern}probe` : entry.pattern;
      assert.throws(
        () => assertAllowedOperation(probe),
        (error: unknown) =>
          error instanceof PolicyViolation && error.reason === entry.reason,
        `前缀 "${entry.pattern}" 未能拦下 "${probe}"`,
      );
    }
  });

  it('拒绝任务结算与资产操作（硬约束的核心）', () => {
    const mustReject = [
      'task.settle',
      'task.start',
      'cycle.settleDaily',
      'asset.debit',
      'flower.charge',
      'flower.redeem.cancel',
      'couple.read',
      'scheduler.acquire',
      'advisory-lock.acquire',
      'db.write.insert',
      'admin.login',
    ];
    for (const operation of mustReject) {
      assert.throws(() => assertAllowedOperation(operation), PolicyViolation, operation);
    }
  });

  it('禁止项优先于允许清单（防止误配穿透）', () => {
    // 构造一个"既在允许清单、又命中禁止前缀"的名字。
    // 当前实现下不可能出现，但断言了检查顺序：即使出现也必须被拒绝。
    const forbidden = FORBIDDEN_RESPONSIBILITIES[0];
    assert.ok(forbidden !== undefined);
    assert.throws(() => assertAllowedOperation(`${forbidden.pattern}health`), PolicyViolation);
  });

  it('对外描述包含允许与禁止两侧', () => {
    const boundary = describeResponsibilityBoundary();
    assert.deepEqual([...boundary.allowed], [...ALLOWED_OPERATIONS]);
    assert.equal(boundary.forbidden.length, FORBIDDEN_RESPONSIBILITIES.length);
    assert.ok(boundary.forbidden.includes('scheduler.'));
  });
});
