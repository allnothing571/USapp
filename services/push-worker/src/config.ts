/**
 * Worker configuration: read once at startup, and a missing or dangerous value stops the process.
 *
 * - Disabled (the default) needs nothing and listens on nothing.
 * - Non-loopback listening is allowed only in explicitly configured authenticated private mode:
 *   PUSH_WORKER_PRIVATE_NETWORK=true, and when enabled a token of at least 32 characters. Compose never
 *   publishes the port.
 * - Enabled needs complete VAPID configuration, with the public key on the P-256 curve.
 * - Node's environment proxy support is refused: a proxy would stand between the address checks and
 *   the provider. The worker's per-request agent has no proxyEnv, and refusing also covers anything
 *   that reads these settings globally.
 */

import { ECDH } from 'node:crypto';

export interface PushWorkerConfig {
  readonly enabled: boolean;
  readonly host: string;
  readonly port: number;
  readonly privateNetwork: boolean;
  readonly token: string;
  readonly vapidPublicKey: string;
  readonly vapidPrivateKey: string;
  readonly vapidSubject: string;
  readonly appEnv: string;
  readonly appVersion: string;
}

export class ConfigError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ConfigError';
  }
}

const LOOPBACK: ReadonlySet<string> = new Set(['127.0.0.1', '::1', 'localhost']);
export const MIN_TOKEN_LENGTH = 32;

function isP256Point(base64url: string): boolean {
  try {
    const raw = Buffer.from(base64url, 'base64url');
    if (raw.length !== 65 || raw[0] !== 0x04) return false;
    ECDH.convertKey(raw, 'prime256v1');  // throws for a point that is not on the curve
    return true;
  } catch {
    return false;
  }
}

/** Throws if this process could route through Node's environment proxy. */
export function assertNoEnvProxy(env: NodeJS.ProcessEnv, execArgv: readonly string[]): void {
  const flag = (env['NODE_USE_ENV_PROXY'] ?? '').trim();
  if (flag !== '' && flag !== '0') throw new ConfigError('NODE_USE_ENV_PROXY must not be set for the push worker');
  const options = env['NODE_OPTIONS'] ?? '';
  if (execArgv.includes('--use-env-proxy') || /(^|\s)--use-env-proxy(\s|=|$)/.test(options)) {
    throw new ConfigError('--use-env-proxy must not be used for the push worker');
  }
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): PushWorkerConfig {
  const readString = (name: string, fallback = ''): string => {
    const raw = env[name];
    return raw === undefined ? fallback : raw.trim();
  };
  const readBool = (name: string, fallback: boolean): boolean => {
    const lowered = readString(name).toLowerCase();
    if (lowered === '') return fallback;
    if (lowered === 'true' || lowered === '1' || lowered === 'yes') return true;
    if (lowered === 'false' || lowered === '0' || lowered === 'no') return false;
    throw new ConfigError(`${name} must be true or false`);
  };
  const readPort = (name: string, fallback: number): number => {
    const raw = readString(name);
    if (raw === '') return fallback;
    const value = Number(raw);
    if (!Number.isInteger(value) || value < 1 || value > 65535) throw new ConfigError(`${name} must be a port number`);
    return value;
  };

  const enabled = readBool('PUSH_WORKER_ENABLED', false);
  const privateNetwork = readBool('PUSH_WORKER_PRIVATE_NETWORK', false);
  const host = readString('PUSH_WORKER_HOST', '127.0.0.1');
  const port = readPort('PUSH_WORKER_PORT', 8090);
  if (!LOOPBACK.has(host) && !privateNetwork) {
    throw new ConfigError('PUSH_WORKER_HOST must be loopback unless PUSH_WORKER_PRIVATE_NETWORK=true');
  }

  const token = readString('PUSH_WORKER_TOKEN');
  const vapidPublicKey = readString('VAPID_PUBLIC_KEY');
  const vapidPrivateKey = readString('VAPID_PRIVATE_KEY');
  const vapidSubject = readString('VAPID_SUBJECT');

  if (enabled) {
    if (token.length < MIN_TOKEN_LENGTH) throw new ConfigError(`PUSH_WORKER_TOKEN must have at least ${MIN_TOKEN_LENGTH} characters`);
    const missing = [
      ['VAPID_PUBLIC_KEY', vapidPublicKey], ['VAPID_PRIVATE_KEY', vapidPrivateKey], ['VAPID_SUBJECT', vapidSubject],
    ].filter(([, value]) => value === '').map(([name]) => name);
    if (missing.length > 0) throw new ConfigError(`Required when enabled: ${missing.join(', ')}`);
    if (!vapidSubject.startsWith('mailto:') && !vapidSubject.startsWith('https://')) {
      throw new ConfigError('VAPID_SUBJECT must start with mailto: or https://');
    }
    if (!isP256Point(vapidPublicKey)) throw new ConfigError('VAPID_PUBLIC_KEY must be a base64url P-256 public point');
    if (Buffer.from(vapidPrivateKey, 'base64url').length !== 32) throw new ConfigError('VAPID_PRIVATE_KEY must be 32 base64url bytes');
  }

  return Object.freeze({
    enabled,
    host,
    port,
    privateNetwork,
    token,
    vapidPublicKey,
    vapidPrivateKey,
    vapidSubject,
    appEnv: readString('APP_ENV', 'development') || 'development',
    appVersion: readString('APP_VERSION', '0.0.0') || '0.0.0',
  });
}
