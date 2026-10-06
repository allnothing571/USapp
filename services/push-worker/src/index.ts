/**
 * 推送辅助进程入口。
 *
 * 阶段 0 行为：
 * - PUSH_WORKER_ENABLED=false（默认）时，打印一条说明后以 0 退出，不监听端口。
 *   这样即使有人误把容器拉起来，也不会多出一个可达的 HTTP 表面。
 * - PUSH_WORKER_ENABLED=true 时绑定回环地址并启动。
 *
 * 本进程不连接数据库、不获取任何数据库锁、不做业务调度——理由见 src/policy.ts。
 */

import { loadConfig, ConfigError } from './config.js';
import { createLogger } from './logger.js';
import { describeResponsibilityBoundary } from './policy.js';
import { createPushWorkerServer } from './server.js';

function main(): void {
  let config;
  try {
    config = loadConfig();
  } catch (error) {
    const reason = error instanceof ConfigError ? error.message : String(error);
    process.stderr.write(
      `${JSON.stringify({ level: 'error', component: 'push-worker', message: '配置校验失败，进程退出', reason })}\n`,
    );
    process.exitCode = 78; // EX_CONFIG
    return;
  }

  const logger = createLogger('push-worker', config.appVersion);

  if (!config.enabled) {
    logger.info('推送辅助进程已禁用（PUSH_WORKER_ENABLED=false），不启动监听', {
      note: '阶段 0 默认关闭；需要启用时由负责人显式设置该变量',
    });
    return;
  }

  const runtime = createPushWorkerServer(config, logger);

  runtime.server.on('error', (error: NodeJS.ErrnoException) => {
    logger.error('推送辅助进程监听失败', { code: error.code ?? 'unknown', reason: error.message });
    process.exitCode = 1;
  });

  runtime.server.listen(config.port, config.host, () => {
    const boundary = describeResponsibilityBoundary();
    logger.info('推送辅助进程已启动', {
      host: config.host,
      port: config.port,
      pollIntervalMs: config.pollIntervalMs,
      pushTransport: 'dry-run',
    });
    // 启动时把边界写进日志：以后有人想在这个进程里加业务逻辑，先得解释为什么这里写着"禁止"。
    logger.info('责任边界', {
      allowed: boundary.allowed,
      forbiddenPrefixes: boundary.forbidden,
    });
  });

  const pollTimer = setInterval(() => {
    try {
      runtime.tick();
    } catch (error) {
      logger.error('推送队列排空失败', {
        reason: error instanceof Error ? error.message : String(error),
      });
    }
  }, config.pollIntervalMs);
  // 定时器不应阻止进程退出。
  pollTimer.unref();

  const shutdown = (signal: string): void => {
    logger.info('收到退出信号，正在关闭推送辅助进程', { signal });
    clearInterval(pollTimer);
    runtime.server.close((error?: Error) => {
      if (error !== undefined) {
        logger.error('关闭监听失败', { reason: error.message });
        process.exitCode = 1;
      }
    });
  };

  process.on('SIGTERM', () => shutdown('SIGTERM'));
  process.on('SIGINT', () => shutdown('SIGINT'));
}

main();
