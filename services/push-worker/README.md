# services/push-worker —— 推送辅助进程（阶段 0：仅骨架）

Node.js 24 / TypeScript 的推送辅助进程。**阶段 0 不接入真实推送服务**，只交付：

- 配置读取与校验（`src/config.ts`）
- 责任边界断言（`src/policy.ts`）
- 内存投递队列接口与占位实现（`src/queue.ts`）
- 回环地址上的三个 HTTP 路由（`src/server.ts`）
- 进程入口与优雅退出（`src/index.ts`）

## 为什么需要一个独立进程

推送投递会长时间等待外部服务。把它放进 C++ 主进程会污染请求线程池，也无法独立重启。
但它**只能**做投递，绝不能长成第二个业务后端——理由见下。

## 责任边界（硬约束）

本进程**只**允许两个操作（`src/policy.ts` 的 `ALLOWED_OPERATIONS`）：

| 操作 | 用途 |
| --- | --- |
| `push.health` | 健康与配置摘要 |
| `push.deliver` | 投递一条**已由主进程决定好**的消息 |

以下责任**一律禁止**，并且是运行时断言而非文档约定：

| 禁止前缀 | 原因 |
| --- | --- |
| `task.` | 任务状态流转与结算属于 C++ 主进程 |
| `cycle.` | 周期结算（含个人连续完成计数）必须在调度锁事务内完成 |
| `asset.` | 资产操作必须与业务数据同事务，本进程没有该事务边界 |
| `flower.` | 小红花扣减/兑换属于资产操作 |
| `couple.` | 情侣业务数据不读写、不缓存、不持久化 |
| `scheduler.` | 跨实例调度锁由 C++ 主进程持有 |
| `advisory-lock.` | 本进程绝不调用 `pg_advisory_lock` / `pg_try_advisory_xact_lock` |
| `db.write.` | 阶段 0 不持有数据库凭证，不执行任何写操作 |
| `admin.` | 管理面只存在于 C++ 管理监听端口 |

**本进程不连接 PostgreSQL，不引入 Redis，不引入任何消息中间件。**

## HTTP 路由

全部绑定 `PUSH_WORKER_HOST`（默认 `127.0.0.1`）。绑定非回环地址会在启动时被拒绝。

- `GET /health` —— 存活、版本、队列长度、`pushTransport`
- `GET /boundary` —— 返回允许/禁止清单，便于运维自查
- `POST /deliver` —— 接收一条消息（`idempotencyKey`、`recipientRef`、`category`、`title`、`body` 必填），
  入队并演练排空，返回 `delivered: false` 与原因。请求体上限 16 KB。

所有响应带 `Cache-Control: no-store, no-cache, must-revalidate, private`。

`PUSH_WORKER_ENABLED=false`（默认）时进程打印说明后以 0 退出，**不监听端口**。

## 目录

```
services/push-worker/
├── package.json
├── tsconfig.json
├── src/
│   ├── config.ts     环境变量读取与校验
│   ├── logger.ts     单行 JSON 结构化日志（敏感字段打码）
│   ├── policy.ts     责任边界（允许清单 + 禁止族断言）
│   ├── queue.ts      投递队列接口、内存实现、演练投递器
│   ├── server.ts     回环 HTTP 服务
│   └── index.ts      入口与信号处理
└── tests/
    ├── config.test.ts
    ├── policy.test.ts
    └── queue.test.ts
```

## 环境变量

与仓库根 `.env.example` 保持一致（改名必须同时改 `.env.example`、`deploy/docker-compose.yml` 与本文件）：

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `PUSH_WORKER_ENABLED` | `false` | 阶段 0 默认关闭 |
| `PUSH_WORKER_HOST` | `127.0.0.1` | 只允许回环 |
| `PUSH_WORKER_PORT` | `8090` | 1..65535 |
| `PUSH_WORKER_POLL_INTERVAL_MS` | `15000` | 1000..3600000 |
| `VAPID_PUBLIC_KEY` / `VAPID_PRIVATE_KEY` / `VAPID_SUBJECT` | 空 | `ENABLED=true` 时三者必填，`SUBJECT` 须为 `mailto:` 或 `https://` |
| `APP_ENV` / `APP_VERSION` | `development` / `0.0.0` | 日志与健康检查使用 |
| `SCHEDULER_INSTANCE_ID` | 空 | 仅作为实例标识；本进程**不**参与调度 |
| `LOG_LEVEL` | `info` | `debug` / `info` / `warn` / `error` |

## 阶段 1 的替换点

`src/queue.ts` 是唯一需要改动的文件。方向：C++ 主进程在**调度锁事务之外**把已成事实的
业务事件写入 outbox 表，本进程只读取 `dispatched_at IS NULL` 的行并投递，成功后回写。
跨实例去重由 outbox 行承担，**不由本进程的锁承担**。

## 静态检查

`npm run typecheck`——本进程**不引入 ESLint/Prettier**，阶段 0 不增加工具链。

## 状态

| 项目 | 状态 |
| --- | --- |
| 配置读取与校验 | 已实现 |
| 责任边界断言 | 已实现 |
| 回环 HTTP 服务与健康检查 | 已实现 |
| 内存队列 | 已实现（占位） |
| 真实 Web Push（VAPID 签名、订阅存储、重试） | **未实现**，阶段 1+ |
| outbox 持久化队列 | **未实现**，阶段 1+ |
| 投递结果回写 | **未实现**，阶段 1+ |

## 尚未执行的检查

- `npm install`（本目录**没有 lockfile**，由负责人手动生成）
- `npm run typecheck`
- `npm run build`
- `npm test`（3 个 spec 一次都没跑过）
- 进程启动、`/health` 探测、容器构建
