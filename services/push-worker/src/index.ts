/**
 * Worker entry. Disabled (the default) logs one line and exits 0, listening on nothing. Enabled, it
 * listens per config.ts with the production transport: system DNS, public addresses only, port 443.
 * This process has no database access, no durable state and makes no business decisions (policy.ts).
 */

import { assertNoEnvProxy, ConfigError, loadConfig, type PushWorkerConfig } from './config.js';
import { createLogger } from './logger.js';
import { describeResponsibilityBoundary } from './policy.js';
import { createWorkerServer } from './server.js';
import { createProviderTransport } from './transport.js';

function main(): void {
  let config: PushWorkerConfig;
  try {
    assertNoEnvProxy(process.env, process.execArgv);
    config = loadConfig();
  } catch (error) {
    const reason = error instanceof ConfigError ? error.message : 'configuration could not be read';
    process.stderr.write(`${JSON.stringify({ level: 'error', component: 'push-worker', message: 'invalid configuration', reason })}\n`);
    process.exitCode = 78;  // EX_CONFIG
    return;
  }

  const logger = createLogger('push-worker', config.appVersion);
  if (!config.enabled) {
    logger.info('push worker disabled; not listening', {});
    return;
  }

  const server = createWorkerServer({
    token: config.token,
    version: config.appVersion,
    deps: {
      transport: createProviderTransport(),
      vapid: { subject: config.vapidSubject, publicKey: config.vapidPublicKey, privateKey: config.vapidPrivateKey },
      now: Date.now,
    },
  }, logger);

  server.on('error', (error: NodeJS.ErrnoException) => {
    logger.error('listen failed', { code: error.code ?? 'unknown' });
    process.exitCode = 1;
  });
  server.listen(config.port, config.host, () => {
    logger.info('push worker listening', { host: config.host, port: config.port, privateNetwork: config.privateNetwork });
    logger.info('responsibility boundary', { ...describeResponsibilityBoundary() });
  });

  const shutdown = (signal: string): void => {
    logger.info('shutting down', { signal });
    server.close();
  };
  process.on('SIGTERM', () => shutdown('SIGTERM'));
  process.on('SIGINT', () => shutdown('SIGINT'));
}

main();
