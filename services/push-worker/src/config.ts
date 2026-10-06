/**
 * 推送辅助进程的配置。
 *
 * 设计原则：
 * - 零运行时依赖，因此不引入 dotenv。生产由 Docker Compose 的 env_file 注入，
 *   本地由负责人手动导出环境变量。
 * - 配置在进程启动时一次性读取并校验，缺项立刻退出，不带着半个配置跑起来。
 * - 阶段 0 不接入真实推送服务，VAPID 密钥允许为空；一旦 PUSH_WORKER_ENABLED=true，
 *   三个 VAPID 变量必须齐全，否则启动失败（避免上线后才发现推送发不出去）。
 */

export interface PushWorkerConfig {
  readonly enabled: boolean;
  readonly host: string;
  readonly port: number;
  readonly pollIntervalMs: number;
  readonly vapidPublicKey: string;
  readonly vapidPrivateKey: string;
  readonly vapidSubject: string;
  readonly appEnv: string;
  readonly appVersion: string;
  readonly instanceId: string;
}

export class ConfigError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ConfigError';
  }
}

function readString(name: string, fallback = ''): string {
  const raw = process.env[name];
  return raw === undefined ? fallback : raw.trim();
}

function readBool(name: string, fallback: boolean): boolean {
  const raw = readString(name);
  if (raw === '') return fallback;
  const lowered = raw.toLowerCase();
  if (lowered === 'true' || lowered === '1' || lowered === 'yes') return true;
  if (lowered === 'false' || lowered === '0' || lowered === 'no') return false;
  throw new ConfigError(`环境变量 ${name} 必须是布尔值（true/false），实际收到 "${raw}"`);
}

function readInt(name: string, fallback: number, min: number, max: number): number {
  const raw = readString(name);
  if (raw === '') return fallback;
  const value = Number(raw);
  if (!Number.isInteger(value)) {
    throw new ConfigError(`环境变量 ${name} 必须是整数，实际收到 "${raw}"`);
  }
  if (value < min || value > max) {
    throw new ConfigError(`环境变量 ${name} 必须在 ${min}..${max} 之间，实际收到 ${value}`);
  }
  return value;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): PushWorkerConfig {
  const enabled = readBool('PUSH_WORKER_ENABLED', false);
  const host = readString('PUSH_WORKER_HOST', '127.0.0.1');
  const port = readInt('PUSH_WORKER_PORT', 8090, 1, 65535);
  const pollIntervalMs = readInt('PUSH_WORKER_POLL_INTERVAL_MS', 15000, 1000, 3_600_000);

  // 这是一个安全边界，不是风格偏好：进程名里带 "worker"，一旦绑到 0.0.0.0
  // 就会从辅助进程变成公网可达面。阶段 0 只允许回环。
  if (host !== '127.0.0.1' && host !== '::1' && host !== 'localhost') {
    throw new ConfigError(
      `PUSH_WORKER_HOST 必须绑定回环地址，实际收到 "${host}"。` +
        '该进程只应被本机 API 或反向代理访问。',
    );
  }

  const vapidPublicKey = readString('VAPID_PUBLIC_KEY');
  const vapidPrivateKey = readString('VAPID_PRIVATE_KEY');
  const vapidSubject = readString('VAPID_SUBJECT');

  if (enabled) {
    const missing: string[] = [];
    if (vapidPublicKey === '') missing.push('VAPID_PUBLIC_KEY');
    if (vapidPrivateKey === '') missing.push('VAPID_PRIVATE_KEY');
    if (vapidSubject === '') missing.push('VAPID_SUBJECT');
    if (missing.length > 0) {
      throw new ConfigError(
        `PUSH_WORKER_ENABLED=true 时以下变量必须提供：${missing.join(', ')}`,
      );
    }
    if (!vapidSubject.startsWith('mailto:') && !vapidSubject.startsWith('https://')) {
      throw new ConfigError('VAPID_SUBJECT 必须是 mailto: 或 https:// 开头的联系方式');
    }
  }

  const appEnv = readString('APP_ENV', 'development') || 'development';
  const appVersion = readString('APP_VERSION', '0.0.0') || '0.0.0';
  const instanceId = readString('SCHEDULER_INSTANCE_ID') || `push-worker-${process.pid}`;

  return Object.freeze({
    enabled,
    host,
    port,
    pollIntervalMs,
    vapidPublicKey,
    vapidPrivateKey,
    vapidSubject,
    appEnv,
    appVersion,
    instanceId,
  });
}
