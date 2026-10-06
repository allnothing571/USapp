/**
 * 配置校验测试。
 *
 * 重点不是"能读环境变量"，而是几个会静默变危险的情形：
 * 绑定到非回环地址、启用推送却缺 VAPID 密钥。
 *
 * 只编写，不执行。
 */

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { ConfigError, loadConfig } from '../src/config.js';

function env(overrides: Record<string, string>): NodeJS.ProcessEnv {
  return { ...overrides } as NodeJS.ProcessEnv;
}

describe('推送辅助进程配置', () => {
  it('默认禁用且绑定回环', () => {
    const config = loadConfig(env({}));
    assert.equal(config.enabled, false);
    assert.equal(config.host, '127.0.0.1');
    assert.equal(config.port, 8090);
    assert.equal(config.pollIntervalMs, 15000);
  });

  it('拒绝绑定到非回环地址', () => {
    assert.throws(() => loadConfig(env({ PUSH_WORKER_HOST: '0.0.0.0' })), ConfigError);
    assert.throws(() => loadConfig(env({ PUSH_WORKER_HOST: '10.0.0.5' })), ConfigError);
  });

  it('拒绝非整数或越界端口', () => {
    assert.throws(() => loadConfig(env({ PUSH_WORKER_PORT: 'abc' })), ConfigError);
    assert.throws(() => loadConfig(env({ PUSH_WORKER_PORT: '70000' })), ConfigError);
  });

  it('启用推送时必须提供完整 VAPID 配置', () => {
    assert.throws(() => loadConfig(env({ PUSH_WORKER_ENABLED: 'true' })), ConfigError);
    assert.throws(
      () =>
        loadConfig(
          env({
            PUSH_WORKER_ENABLED: 'true',
            VAPID_PUBLIC_KEY: 'pub',
            VAPID_PRIVATE_KEY: 'priv',
          }),
        ),
      ConfigError,
    );
  });

  it('VAPID_SUBJECT 必须是 mailto: 或 https://', () => {
    assert.throws(
      () =>
        loadConfig(
          env({
            PUSH_WORKER_ENABLED: 'true',
            VAPID_PUBLIC_KEY: 'pub',
            VAPID_PRIVATE_KEY: 'priv',
            VAPID_SUBJECT: 'someone@example.com',
          }),
        ),
      ConfigError,
    );
  });

  it('配置齐备时启用成功', () => {
    const config = loadConfig(
      env({
        PUSH_WORKER_ENABLED: 'true',
        VAPID_PUBLIC_KEY: 'pub',
        VAPID_PRIVATE_KEY: 'priv',
        VAPID_SUBJECT: 'mailto:ops@example.com',
      }),
    );
    assert.equal(config.enabled, true);
  });

  it('配置对象是冻结的', () => {
    const config = loadConfig(env({}));
    assert.ok(Object.isFrozen(config));
  });
});
