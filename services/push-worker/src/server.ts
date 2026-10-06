/**
 * 推送辅助进程的 HTTP 表面。
 *
 * 只有三个路由，全部绑定回环地址：
 *   GET  /health    —— 存活与配置摘要（用于容器 healthcheck 与人工排查）
 *   GET  /boundary  —— 自证责任边界，让运维一眼看到本进程"不该做什么"
 *   POST /deliver   —— 接收一条已由主进程决定好的推送消息（阶段 0 只演练）
 *
 * 明确没有的：没有管理路由、没有任务/资产路由、没有任何数据库访问。
 * 所有响应都带 Cache-Control: no-store，因为这里面没有任何可共享的内容。
 */

import { createServer, type IncomingMessage, type Server, type ServerResponse } from 'node:http';

import type { PushWorkerConfig } from './config.js';
import type { Logger } from './logger.js';
import { assertAllowedOperation, describeResponsibilityBoundary } from './policy.js';
import { DryRunDispatcher, InMemoryPushQueue, type PushMessage } from './queue.js';

/** 请求体上限。一个通知的正文不该有几十 KB；超过就是有人在拿这个端口当上传通道。 */
const MAX_BODY_BYTES = 16 * 1024;

function sendJson(res: ServerResponse, status: number, payload: unknown): void {
  const body = JSON.stringify(payload);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    // 与业务 API 的契约一致：任何响应都不允许被中间层缓存。
    'Cache-Control': 'no-store, no-cache, must-revalidate, private',
    'X-Content-Type-Options': 'nosniff',
  });
  res.end(body);
}

function readBody(req: IncomingMessage): Promise<string> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    let total = 0;
    req.on('data', (chunk: Buffer) => {
      total += chunk.length;
      if (total > MAX_BODY_BYTES) {
        reject(new Error(`请求体超过 ${MAX_BODY_BYTES} 字节上限`));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', (error: Error) => reject(error));
  });
}

function parsePushMessage(raw: string): PushMessage {
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    throw new Error('请求体不是合法 JSON');
  }
  if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
    throw new Error('请求体必须是 JSON 对象');
  }
  const obj = parsed as Record<string, unknown>;
  const required = ['idempotencyKey', 'recipientRef', 'category', 'title', 'body'] as const;
  const missing = required.filter(
    (key) => typeof obj[key] !== 'string' || (obj[key] as string).trim() === '',
  );
  if (missing.length > 0) {
    throw new Error(`缺少必填字段：${missing.join(', ')}`);
  }
  const enqueuedAt = typeof obj['enqueuedAt'] === 'number' ? obj['enqueuedAt'] : Date.now();
  return {
    idempotencyKey: obj['idempotencyKey'] as string,
    recipientRef: obj['recipientRef'] as string,
    category: obj['category'] as string,
    title: obj['title'] as string,
    body: obj['body'] as string,
    enqueuedAt,
  };
}

export interface PushWorkerRuntime {
  readonly server: Server;
  readonly queue: InMemoryPushQueue;
  /** 处理一次队列排空；由 index.ts 的定时器或手动触发调用 */
  tick(): number;
}

export function createPushWorkerServer(
  config: PushWorkerConfig,
  logger: Logger,
): PushWorkerRuntime {
  const queue = new InMemoryPushQueue();
  const dispatcher = new DryRunDispatcher(logger);

  const tick = (): number => {
    const messages = queue.drain(50);
    if (messages.length === 0) return 0;
    const outcomes = dispatcher.dispatch(messages);
    logger.info('完成了推送队列的演练排空', {
      drained: messages.length,
      delivered: outcomes.filter((item) => item.delivered).length,
    });
    return messages.length;
  };

  const handler = async (req: IncomingMessage, res: ServerResponse): Promise<void> => {
    const method = req.method ?? 'GET';
    const url = new URL(req.url ?? '/', `http://${config.host}:${config.port}`);
    const path = url.pathname;

    try {
      if (method === 'GET' && path === '/health') {
        sendJson(res, 200, {
          status: 'ok',
          component: 'push-worker',
          version: config.appVersion,
          env: config.appEnv,
          instanceId: config.instanceId,
          enabled: config.enabled,
          queueSize: queue.size(),
          // 真实推送是否可用，比"进程活着"更重要，因此直接暴露。
          pushTransport: config.enabled ? 'configured' : 'disabled',
        });
        return;
      }

      if (method === 'GET' && path === '/boundary') {
        sendJson(res, 200, describeResponsibilityBoundary());
        return;
      }

      if (method === 'POST' && path === '/deliver') {
        // 责任边界在这里强制执行；越界的操作会在入队前就被拒绝。
        assertAllowedOperation('push.deliver');
        const raw = await readBody(req);
        const message = parsePushMessage(raw);
        queue.enqueue(message);
        const drained = tick();
        sendJson(res, 202, {
          accepted: true,
          drained,
          // 诚实报告：阶段 0 没有任何消息被真正投递。
          delivered: false,
          reason: '阶段 0 为骨架，未接入真实推送服务（VAPID 与订阅存储留待阶段 1+）',
        });
        return;
      }

      sendJson(res, 404, {
        error: { code: 'NOT_FOUND', message: '该路由不存在于推送辅助进程' },
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      const isClientError = error instanceof RangeError || /请求体|必填字段|JSON/.test(message);
      logger.warn('推送辅助进程请求失败', { path, method, reason: message });
      sendJson(res, isClientError ? 400 : 500, {
        error: {
          code: isClientError ? 'VALIDATION_ERROR' : 'INTERNAL_ERROR',
          message,
        },
      });
    }
  };

  const server = createServer((req, res) => {
    void handler(req, res).catch((error: unknown) => {
      // handler 内部已有 try/catch；这里是最后一道网，避免未处理拒绝直接杀死进程。
      logger.error('推送辅助进程出现未捕获的处理异常', {
        reason: error instanceof Error ? error.message : String(error),
      });
      if (!res.headersSent) {
        sendJson(res, 500, { error: { code: 'INTERNAL_ERROR', message: '内部错误' } });
      } else {
        res.end();
      }
    });
  });

  // 辅助进程永远不该接受慢连接积压；超时后直接断开。
  server.headersTimeout = 10_000;
  server.requestTimeout = 15_000;
  server.keepAliveTimeout = 5_000;

  return { server, queue, tick };
}
