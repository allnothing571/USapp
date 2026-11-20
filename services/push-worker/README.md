# services/push-worker —— 无状态推送传输进程（P2-5c）

Node.js 24 / TypeScript。C++ 主进程决定发什么、发给谁、何时重试并保存全部持久事实；本进程只负责把
**一次已决定好的投递**加密并发给推送服务商，然后如实报告结果。默认关闭。

**当前状态：** 传输已实现，并用桩解析器、本地测试对端完成隔离验证。**从未连接真实推送服务商，未订阅真实设备，
未部署。** 证据见 [Web Push 文档第 16 节](../../docs/phase2-web-push.md)。

## 责任边界（硬约束）

本进程**只**允许两个操作（`src/policy.ts` 的 `ALLOWED_OPERATIONS`）：

| 操作 | 用途 |
| --- | --- |
| `push.health` | 健康信息 |
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
| `db.write.` | 不持有数据库凭证，不执行任何写操作 |
| `admin.` | 管理面只存在于 C++ 管理监听端口 |

**本进程不连接 PostgreSQL，不引入 Redis 或消息中间件，没有持久队列。**

## HTTP 路由

- `GET /health`：存活与版本
- `GET /boundary`：责任边界清单
- `POST /v1/deliver`：一次投递

`POST /v1/deliver` 的处理顺序：恒定时间校验 `Authorization: Bearer`（失败 401）→ 全局信号量 4（超出 503）→
请求体上限 4 KB（413）→ 严格校验（400，固定错误体）→ 一个 10 秒绝对截止时间覆盖整个操作。
服务商结果一律为 HTTP 200，并带回 `job` 供 C++ 严格对应。错误体不回显任何输入或异常文本；日志不含端点、
密钥、令牌或载荷。

## 出站安全

`web-push` 只用于 `generateRequestDetails`（载荷加密与 VAPID 请求构造），真正的 HTTPS 请求由
`src/transport.ts` 发出：

- 协议、主机语法与白名单（精确匹配或标签边界）；不跟随重定向。
- 受控 DNS：拒绝非公网地址（含 IPv4 映射 IPv6）；`secureConnect` 后再次检查实际对端，通过后才写入请求。
- 正常证书与主机名校验、SNI；每次请求新建 Agent 并在结束时销毁，因此不会复用套接字。
- 只读状态行与响应头（响应头上限 8 KB），不读响应体；由 `AbortSignal` 执行绝对截止时间。
- 启动时拒绝 `NODE_USE_ENV_PROXY` 与 `--use-env-proxy`。

测试通过 `createProviderTransport` 的参数注入解析器与对端；`src/index.ts` 不传任何参数，也没有可关闭这些检查的
环境变量或运行时开关。

## 环境变量

与仓库根 `.env.example` 保持一致（改名必须同时改 `.env.example`、`deploy/docker-compose.yml` 与本文件）：

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `PUSH_WORKER_ENABLED` | `false` | 关闭时打印说明后以 0 退出，**不监听端口** |
| `PUSH_WORKER_HOST` | `127.0.0.1` | 非回环必须同时设置 `PUSH_WORKER_PRIVATE_NETWORK=true` |
| `PUSH_WORKER_PRIVATE_NETWORK` | `false` | 显式声明容器私有网络模式；compose 不发布端口 |
| `PUSH_WORKER_PORT` | `8090` | 1..65535 |
| `PUSH_WORKER_TOKEN` | 空 | 启用时必填，至少 32 个字符；与 API 的同名变量相同 |
| `VAPID_PUBLIC_KEY` / `VAPID_PRIVATE_KEY` / `VAPID_SUBJECT` | 空 | 启用时必填；公钥须在 P-256 曲线上，`SUBJECT` 须为 `mailto:` 或 `https://` |
| `APP_ENV` / `APP_VERSION` | `development` / `0.0.0` | 日志与健康检查使用 |
| `LOG_LEVEL` | `info` | `debug` / `info` / `warn` / `error` |

配置错误时以 78 退出。

## 第三方依赖

`web-push` 3.6.7（精确锁定），声明的许可为 MPL-2.0，未作修改。见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
与 [licenses/web-push-LICENSE.txt](licenses/web-push-LICENSE.txt)。

## 目录

```
services/push-worker/
├── src/
│   ├── config.ts     环境变量读取与校验
│   ├── logger.ts     单行 JSON 结构化日志（敏感字段打码）
│   ├── policy.ts     责任边界
│   ├── endpoint.ts   端点白名单与公网地址判断
│   ├── transport.ts  受控 HTTPS 请求与结果分类
│   ├── deliver.ts    严格校验、到期判断、加密与 VAPID
│   ├── server.ts     认证 HTTP 服务
│   └── index.ts      生产入口与信号处理
└── tests/            单元测试；smoke-server.ts 只用于隔离网络，不进入生产镜像
```
