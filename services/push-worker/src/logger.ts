/**
 * 极简结构化日志。零依赖，输出一行一个 JSON，便于被容器日志收集。
 * 绝不打印密钥、令牌、邮箱或推送订阅端点。
 */

type Level = 'debug' | 'info' | 'warn' | 'error';

const SENSITIVE_KEYS = new Set([
  'password',
  'secret',
  'token',
  'vapidprivatekey',
  'vapidpublickey',
  'authorization',
  'cookie',
  'email',
  'endpoint',
  'subscription',
]);

const LEVEL_ORDER: Record<Level, number> = { debug: 10, info: 20, warn: 30, error: 40 };

function currentLevel(): Level {
  const raw = (process.env['LOG_LEVEL'] ?? 'info').toLowerCase();
  if (raw === 'debug' || raw === 'info' || raw === 'warn' || raw === 'error') return raw;
  return 'info';
}

function redact(value: unknown, depth = 0): unknown {
  if (depth > 4) return '[truncated]';
  if (value === null || typeof value !== 'object') return value;
  if (Array.isArray(value)) return value.map((item) => redact(item, depth + 1));
  const out: Record<string, unknown> = {};
  for (const [key, item] of Object.entries(value as Record<string, unknown>)) {
    if (SENSITIVE_KEYS.has(key.toLowerCase())) {
      out[key] = '[redacted]';
      continue;
    }
    out[key] = redact(item, depth + 1);
  }
  return out;
}

export interface Logger {
  debug(message: string, fields?: Record<string, unknown>): void;
  info(message: string, fields?: Record<string, unknown>): void;
  warn(message: string, fields?: Record<string, unknown>): void;
  error(message: string, fields?: Record<string, unknown>): void;
}

export function createLogger(component: string, version: string): Logger {
  const threshold = LEVEL_ORDER[currentLevel()];

  const emit = (level: Level, message: string, fields?: Record<string, unknown>): void => {
    if (LEVEL_ORDER[level] < threshold) return;
    const line = {
      ts: new Date().toISOString(),
      level,
      component,
      version,
      message,
      ...(fields === undefined ? {} : (redact(fields) as Record<string, unknown>)),
    };
    const serialized = JSON.stringify(line);
    if (level === 'error' || level === 'warn') {
      process.stderr.write(`${serialized}\n`);
    } else {
      process.stdout.write(`${serialized}\n`);
    }
  };

  return {
    debug: (message, fields) => emit('debug', message, fields),
    info: (message, fields) => emit('info', message, fields),
    warn: (message, fields) => emit('warn', message, fields),
    error: (message, fields) => emit('error', message, fields),
  };
}
