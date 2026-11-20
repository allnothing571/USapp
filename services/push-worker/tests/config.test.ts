/**
 * 配置校验测试。
 *
 * 重点不是"能读环境变量"，而是几个会静默变危险的情形：
 * 绑定到非回环地址、启用推送却缺 VAPID 密钥。
 *
 */

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { assertNoEnvProxy, ConfigError, loadConfig } from '../src/config.js';

// RFC 8292 Section 2.4's example public key, and a test-only private value (not a real pair's use).
const TEST_PUBLIC = 'BA1Hxzyi1RUM1b5wjxsn7nGxAszw2u61m164i3MrAIxHF6YK5h4SDYic-dRuU_RCPCfA5aq9ojSwk5Y2EmClBPs';
const TEST_PRIVATE = 'A'.repeat(43);
const TEST_TOKEN = 't'.repeat(32);

function env(overrides: Record<string, string>): NodeJS.ProcessEnv {
  return { ...overrides } as NodeJS.ProcessEnv;
}

describe('推送辅助进程配置', () => {
  it('默认禁用且绑定回环', () => {
    const config = loadConfig(env({}));
    assert.equal(config.enabled, false);
    assert.equal(config.host, '127.0.0.1');
    assert.equal(config.port, 8090);
  });

  it('拒绝绑定到非回环地址', () => {
    assert.throws(() => loadConfig(env({ PUSH_WORKER_HOST: '0.0.0.0' })), ConfigError);
    assert.throws(() => loadConfig(env({ PUSH_WORKER_HOST: '10.0.0.5' })), ConfigError);
  });

  it('拒绝非整数或越界端口', () => {
    assert.throws(() => loadConfig(env({ PUSH_WORKER_PORT: 'abc' })), ConfigError);
    assert.throws(() => loadConfig(env({ PUSH_WORKER_PORT: '70000' })), ConfigError);
  });

  it('启用推送时必须提供完整 VAPID 配置与令牌', () => {
    assert.throws(() => loadConfig(env({ PUSH_WORKER_ENABLED: 'true' })), ConfigError);
    assert.throws(
      () => loadConfig(env({ PUSH_WORKER_ENABLED: 'true', PUSH_WORKER_TOKEN: TEST_TOKEN, VAPID_PUBLIC_KEY: TEST_PUBLIC, VAPID_PRIVATE_KEY: TEST_PRIVATE })),
      ConfigError,
    );
  });

  const enabled = { PUSH_WORKER_ENABLED: 'true', PUSH_WORKER_TOKEN: TEST_TOKEN, VAPID_PUBLIC_KEY: TEST_PUBLIC, VAPID_PRIVATE_KEY: TEST_PRIVATE, VAPID_SUBJECT: 'mailto:ops@example.com' };

  it('VAPID_SUBJECT 必须是 mailto: 或 https://', () => {
    assert.throws(() => loadConfig(env({ ...enabled, VAPID_SUBJECT: 'someone@example.com' })), ConfigError);
  });

  it('配置齐备时启用成功', () => {
    assert.equal(loadConfig(env(enabled)).enabled, true);
  });

  it('拒绝不在 P-256 曲线上的 VAPID 公钥', () => {
    assert.throws(() => loadConfig(env({ ...enabled, VAPID_PUBLIC_KEY: 'BA' + 'A'.repeat(85) })), ConfigError);
    assert.throws(() => loadConfig(env({ ...enabled, VAPID_PUBLIC_KEY: 'pub' })), ConfigError);
  });

  it('短令牌不能启用', () => {
    assert.throws(() => loadConfig(env({ ...enabled, PUSH_WORKER_TOKEN: 'short' })), ConfigError);
  });

  it('私网监听必须显式开启，且启用时必须有令牌', () => {
    assert.throws(() => loadConfig(env({ ...enabled, PUSH_WORKER_HOST: '0.0.0.0' })), ConfigError);
    assert.equal(loadConfig(env({ ...enabled, PUSH_WORKER_HOST: '0.0.0.0', PUSH_WORKER_PRIVATE_NETWORK: 'true' })).host, '0.0.0.0');
    assert.throws(
      () => loadConfig(env({ ...enabled, PUSH_WORKER_HOST: '0.0.0.0', PUSH_WORKER_PRIVATE_NETWORK: 'true', PUSH_WORKER_TOKEN: '' })),
      ConfigError,
    );
    // Disabled with the private flag, as Compose sets it: no token needed, and it listens on nothing.
    assert.equal(loadConfig(env({ PUSH_WORKER_HOST: '0.0.0.0', PUSH_WORKER_PRIVATE_NETWORK: 'true' })).enabled, false);
  });

  it('使用传入的环境而不是 process.env', () => {
    process.env['PUSH_WORKER_PORT'] = '9999';
    try {
      assert.equal(loadConfig(env({})).port, 8090);
    } finally {
      delete process.env['PUSH_WORKER_PORT'];
    }
  });

  it('拒绝 Node 环境代理配置', () => {
    assert.throws(() => assertNoEnvProxy(env({ NODE_USE_ENV_PROXY: '1' }), []), ConfigError);
    assert.throws(() => assertNoEnvProxy(env({ NODE_OPTIONS: '--max-old-space-size=64 --use-env-proxy' }), []), ConfigError);
    assert.throws(() => assertNoEnvProxy(env({}), ['--use-env-proxy']), ConfigError);
    assert.doesNotThrow(() => assertNoEnvProxy(env({ NODE_USE_ENV_PROXY: '0' }), []));
    assert.doesNotThrow(() => assertNoEnvProxy(env({}), []));
  });

  it('配置对象是冻结的', () => {
    const config = loadConfig(env({}));
    assert.ok(Object.isFrozen(config));
  });
});
