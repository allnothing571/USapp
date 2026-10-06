# 进程内调度与 advisory lock（阶段 0）

> 阶段 0 **只实现并测试锁基础，不做任何任务结算**。
> 本文档中的所有命令都是**给项目负责人手动执行**的；本次写作
> **没有启动过数据库、没有运行过 `psql`、没有编译或运行过 `usapp_api`**，
> 因此第 7 节的两连接竞争实验**尚未执行**，其观察到的 `t`/`f` 结果是**预期**，不是实测记录。

---

## 1. 权威来源

| 文件 | 角色 |
| --- | --- |
| `services/api/src/scheduler/LockKey.h` / `LockKey.cc` | 锁键的派生与校验（跨实例一致性的唯一实现） |
| `services/api/src/scheduler/Scheduler.h` / `Scheduler.cc` | 进程内调度器：tick 顺序、串行执行、失败记录 |
| `database/migrations/0004_scheduler.sql` | 证据表 `scheduler_ticks` 与占位行 |
| `.env.example`（第 75–81 行） | `SCHEDULER_ENABLED`、`SCHEDULER_TICK_INTERVAL_MS`、`SCHEDULER_INSTANCE_ID`、`SCHEDULER_KEY_NAMESPACE` |
| `services/api/src/core/Crypto.h` | `crypto::fnv1a64` 的声明 |

调度运行在 **C++ 主进程内**。**不引入 Redis，不引入额外调度服务，也不由 Node 进程接管业务调度。**

---

## 2. 一次 tick 的锁契约（顺序本身是需求的一部分）

`Scheduler.h` 第 6–22 行把顺序写死，并明确"不要重排"：

1. `TxScope::begin()` —— 从连接池取**一条**连接，开事务。
2. `SELECT pg_try_advisory_xact_lock(<固定的 64 位键>)`
   - `false` → 立刻 `rollback` 并记 `Skipped`。**不写任何行。**
3. `true` → 调用 `SchedulerJob::runLocked()`；该任务的**全部**数据库操作必须使用传进来的这个
   `Transaction`（**同一连接、同一事务**）。
4. 在**同一事务内** `INSERT scheduler_ticks(..., outcome='completed')`。
5. `commit` —— 提交即释放锁（事务级 advisory lock 的生命周期就是事务）。
6. **提交之后**才执行 job 登记的 `afterCommit` 动作（发信 / 推送 / HTTP）。

> **网络操作绝不允许出现在第 2～5 步之间**，否则锁会被网络超时拖住。
> `SchedulerJob::Result::afterCommit` 的注释也明确：绝不允许捕获或使用 `Transaction` / `TxScope`。

失败路径：`runLocked` 报错 → `rollback`（锁在此释放）→ 用**一个独立的语句**写
`outcome='failed'` 行。失败行不属于持锁事务，因此不破坏"锁与 DB 操作同事务"这一可验证性质。

其它已实现的约束（`Scheduler.h`）：

- `configure(bool enabled, int64_t tickIntervalMs, std::string instanceId)` 幂等；
  未 `configure()` 就 `start()` 会抛异常。
- `registerJob()`：`jobKey` 重复、为空或形状非法都抛异常——**宁可启动失败**，
  也不要带着一个跨实例不一致的锁键静默运行。
- `tickOnce()` 按注册顺序**串行**执行任务：任意时刻最多持有一把锁。
- 前一个 tick 未结束时，自动循环不会启动新的 tick（`tickInFlight()`）——调度不重入。
- `jobKey()` 的注释警告：改它等于换锁；滚动升级期间新旧实例会各持一把锁并同时执行，
  要改就一次性全量重启。

---

## 3. 锁键派生

`LockKey::make(keyNamespace, jobKey)`（`LockKey.cc` 第 70–84 行）：

```
fullKey = keyNamespace + ":" + jobKey          // 散列的实际输入
hashed  = crypto::fnv1a64(fullKey)             // 64 位 FNV-1a，确定性
value   = static_cast<int64_t>(hashed & 0x7FFFFFFFFFFFFFFFULL)   // 掩掉符号位，恒为正
if (value == 0) value = 1                      // 0 与 scheduler_ticks.lock_key <> 0 冲突
```

因此：**任何实例、任何时刻、任何语言/时区/locale 下，同一个 `(namespace, jobKey)` 都算出同一个正 64 位整数。**
键值不含时间、随机数、实例名、进程 id、内存地址（`LockKey.h` 第 8–14 行明确列出）。

`LockKey::forJob(jobKey)` 走正常路径：取 `AppConfig::instance().schedulerKeyNamespace`；
`LockKey::make(namespace, jobKey)` 供测试显式指定命名空间（同一 `jobKey`、不同命名空间必须得到不同键）。

### 键的合法性校验（非法即抛 `std::invalid_argument`）

| 对象 | 规则 | 依据 |
| --- | --- | --- |
| `job_key` | 非空；≤ 63 字节；只允许 ASCII 小写 `[a-z0-9._-]`；**首字符**必须是 `[a-z0-9]` | `LockKey.cc` 第 28–45 行 |
| `key_namespace` | 非空；≤ 64 字节；可打印 ASCII（`0x21`–`0x7e`）且**不含空白**；**不含冒号**（冒号是拼接分隔符） | `LockKey.cc` 第 47–63 行 |
| 默认命名空间 | `usapp.scheduler`（与 `.env.example` 的 `SCHEDULER_KEY_NAMESPACE` 一致） | `LockKey.cc` 第 26 行 |

`job_key` 被限制为 ASCII 小写的原因写在 `LockKey.h` 第 13–14 行：
不同实例的 locale、终端编码、Unicode 规范化形式都不得影响结果。

> ⚠️ **改动派生算法或键空间等同于更换锁**：滚动升级期间两个实例会同时执行同一任务，
> 而且不会报任何错，只会重复执行（`LockKey.h` 第 8–10 行）。
> `scheduler_ticks.lock_key` 的列注释同样要求这种改动必须配套迁移。

---

## 4. 为什么用 `pg_try_advisory_xact_lock`，而不是表租约或 Redis

| 方案 | 为什么不选 |
| --- | --- |
| **表租约（`leases` 表 + `locked_until`）** | 需要自己实现心跳、超时判定与过期清理；进程崩溃后租约残留，要么等超时、要么人工清理。租约的"过期"与业务事务**不能原子结合**：要么业务跑完但租约已过期（第二个实例插进来重复执行），要么为保证原子性再去抢行锁，最后又绕回数据库锁。 |
| **Redis 锁** | 会重新引入一个必须部署、必须监控、必须选主/持久化的组件——这正是阶段 0 有意删掉的分布式失败模式（见 `docs/architecture.md` 第 2 节）。而且 Redis 的锁与 PostgreSQL 事务不具备原子性，网络分区下要么误判持锁、要么锁与事务边界撕裂。 |
| **`pg_try_advisory_xact_lock`** | 锁的生命周期**天然等于事务**：`COMMIT` / `ROLLBACK` 自动释放，不需要心跳、不需要清理、不新增组件；`try` 版本**不等待**，正好对应"未获得锁立即跳过"的需求；锁与业务写在同一事务里，因此"持锁期间做的 DB 工作"是可验证的。 |

已知局限（阶段 0 接受）：

- 只在**同一个 PostgreSQL 数据库**内互斥；跨集群/跨库不成立。
- 不做公平排队：竞争失败者直接跳过本次 tick（下一 tick 再试）。
- 键是 64 位 FNV-1a，**不是密码学散列**：理论上存在两个不同 `job_key` 碰撞的可能。
  碰撞的后果是这两个任务互相排斥 → 少跑，而**不会**让同一任务并发执行；
  阶段 0 只注册一个任务，因此可接受。

---

## 5. 证据表 `scheduler_ticks`

`database/migrations/0004_scheduler.sql` 第 16–34 行：

| 列 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `bigserial PRIMARY KEY` | — |
| `job_key` | `text NOT NULL` | 任务标识 |
| `lock_key` | `bigint NOT NULL` | 由命名空间 + job_key 派生的键，便于事后核对 |
| `instance_id` | `text NOT NULL` | 哪个实例执行了这次 tick |
| `outcome` | `text NOT NULL`，`CHECK (outcome IN ('completed','failed'))` | 只有这两种取值 |
| `detail` | `jsonb NOT NULL DEFAULT '{}'` | 补充信息 |
| `started_at` / `finished_at` | `timestamptz NOT NULL DEFAULT now()` | — |

约束与索引：

- `scheduler_ticks_job_key_not_blank`：`length(btrim(job_key)) > 0`
- `scheduler_ticks_instance_id_not_blank`：`length(btrim(instance_id)) > 0`
- `scheduler_ticks_lock_key_nonzero`：`lock_key <> 0`
- `scheduler_ticks_job_started_idx`：`(job_key, started_at DESC)`

迁移还插入了一行**占位记录**：

```sql
INSERT INTO scheduler_ticks (job_key, lock_key, instance_id, outcome, detail)
VALUES ('__bootstrap__', -1, 'migration', 'completed',
        '{"note":"阶段 0 调度基础占位记录，可在首次部署后删除"}'::jsonb);
```

> 注意：该行的 `lock_key = -1` 是一个**哨兵值**，它不满足
> `scheduler_ticks_lock_key_nonzero` 吗？——满足（`-1 <> 0`）。它与真实调度键不会冲突，
> 因为 `LockKey` 的值**恒为正**（`LockKey.h` 第 45–47 行）。

### 为什么被跳过的（锁竞争）tick 故意不写任何行

表注释（`0004_scheduler.sql` 第 36–39 行）给出了官方理由：

> 「调度 tick 的持锁事务内记录。只在真正持有 advisory lock 的事务中写入；
> 因锁竞争被跳过的 tick 不产生任何行（只写进程日志），
> 因此本表可以证明"调度数据库操作与锁同事务、同连接"。」

换句话说：

1. **不写行才是证据。** 如果跳过的 tick 也写行，那么"写了行"就同时包含
   "持锁执行"与"没持锁跳过"两种含义，这张表就再也无法证明任何东西。
2. **跳过的 tick 想写行就必须另开事务/另用连接**，那就不再属于持锁事务，
   反而会污染这一性质。
3. **锁竞争是正常且高频的。** 每个实例每个 tick 都会 `try` 一次；
   若每次竞争都落库，表会以"实例数 × tick 频率"的速率无意义膨胀。
4. 跳过的信息保留了，只是留在**进程日志**里（`TickOutcome::Skipped`）。

---

## 6. 已实现 / 仅骨架 / 尚未实现

### 已实现

- `services/api/src/scheduler/LockKey.h` / `LockKey.cc`：键派生、`job_key` 与命名空间校验、
  `defaultKeyNamespace()`、`describe()`。
- `services/api/src/scheduler/Scheduler.h` / `Scheduler.cc`：`configure()` / `registerJob()` /
  `start()` / `stop()` / `tickOnce()` / `recordFailedTick()`、串行 tick、
  `tickInFlight()` 不重入、`outcomeToString()`。
- `database/migrations/0004_scheduler.sql`：`scheduler_ticks` 表、约束、索引、占位行。
- `.env.example` 中的 4 个 `SCHEDULER_*` 变量。

### 仅骨架

- **没有任何具体的 `SchedulerJob` 实现。** `services/api/src/scheduler/` 下只有
  `LockKey.*` 与 `Scheduler.*`；迁移里那个 `__bootstrap__` 占位任务
  **在 C++ 侧没有对应的 job 类**，它只是数据库里的一行记录。
- `SchedulerJob::runLocked()` 目前只有接口（纯虚函数），没有可运行的心跳任务。
- `services/api/src/main.cc` **不存在**，因此"主进程内启动调度线程"这一步尚未接线
  （`Scheduler` 类本身提供了 `start()`/`stop()`）。

### 尚未实现

- **任何任务结算**：没有周期推进、没有结算、没有状态机。
- **提醒（reminders）**：不存在。
- **推送驱动的调度**：不存在；`services/push-worker` 是**被动的辅助进程**，
  不参与业务调度，也不接管任何资产操作。
- `scheduler_ticks.outcome='failed'` 的重试/告警策略：只写行，没有消费者。
- 没有任何自动化测试运行器覆盖调度（`services/api/tests` 目录**不存在**，见
  `tests/README.md` 第 5 节）。

---

## 7. 手动两连接竞争实验（由负责人手动执行）

目的：证明 advisory lock 在真实数据库上确实互斥，并且**回滚即释放**。

前提：不需要启动 `usapp_api`，只需要一个可连接的 `usapp` 数据库和 `psql`。

### 7.1 先确定键值

`pg_try_advisory_xact_lock` 只接受一个 `bigint` 键。真正的键由 `LockKey` 用 FNV-1a 算出，
**本次写作没有运行过该算法**，因此下面给两条路：

- **路线 A（推荐，与实现一致）**：用下面的 `node` 单行命令复算 `usapp.scheduler:__bootstrap__`
  的键（与 `LockKey.cc` 使用同一算法：FNV-1a 64，掩掉符号位，0 映射为 1）。
  该命令**由负责人手动执行**：

  ```powershell
  node -e "let h=0xCBF29CE484222325n;for(const b of Buffer.from('usapp.scheduler:__bootstrap__')){h^=BigInt(b);h=BigInt.asUintN(64,h*0x100000001B3n)}let v=h&0x7FFFFFFFFFFFFFFFn;if(v===0n)v=1n;console.log(v.toString())"
  ```

- **路线 B（最简单，仅验证锁语义）**：直接用一个**固定字面量**（例如 `1`）。
  advisory lock 的键只是一个编号；用任何固定数字，两个会话看到的行为完全一样。
  这条路线验证的是 PostgreSQL 的锁语义，**不是**本项目的键派生结果。

下面用 `<KEY>` 代指你选定的那个整数。

### 7.2 开两个 psql 会话

```powershell
# 由负责人手动执行：会话 1
psql -h 127.0.0.1 -p 5432 -U usapp_api -d usapp
```

```powershell
# 由负责人手动执行：会话 2（另开一个终端）
psql -h 127.0.0.1 -p 5432 -U usapp_api -d usapp
```

> 两个会话必须连接**同一个数据库**：advisory lock 只在同一个数据库内互相排斥。

### 7.3 会话 1 先持锁（不要提交/回滚）

```sql
-- 会话 1
BEGIN;
SELECT pg_try_advisory_xact_lock(<KEY>);
```

预期：返回 `t`（`psql` 把布尔值显示为 `t` / `f`，不是 `true` / `false`）。
此时**会话 1 保持事务打开**，锁被持有。不要执行 `COMMIT` 或 `ROLLBACK`。

### 7.4 会话 2 尝试抢锁 → 必须失败且不阻塞

```sql
-- 会话 2
BEGIN;
SELECT pg_try_advisory_xact_lock(<KEY>);
```

预期：立即返回 `f`。`try` 版本**不等待**——如果这里卡住不动，说明用错了函数
（例如误用了会阻塞的 `pg_advisory_xact_lock`）。

收尾：

```sql
-- 会话 2
ROLLBACK;
```

### 7.5 会话 1 回滚释放锁

```sql
-- 会话 1
ROLLBACK;
```

预期：`ROLLBACK` 立即返回，锁随事务结束自动释放（**不需要**任何 `pg_advisory_unlock` 调用）。

### 7.6 会话 2 立即可以拿到锁

```sql
-- 会话 2
BEGIN;
SELECT pg_try_advisory_xact_lock(<KEY>);
```

预期：返回 `t`。收尾：

```sql
-- 会话 2
ROLLBACK;
```

### 7.7 可选的旁证查询

在会话 1 持锁期间，可以在第三个会话里观察锁：

```sql
SELECT locktype, database, classid, objid, objsubid, pid,
       mode, granted
FROM pg_locks
WHERE locktype = 'advisory';
```

预期：能看到 `mode = ExclusiveLock` 且 `granted = true` 的一行（属于会话 1 的 pid）；
会话 2 的 `try` 返回 `f` 时**不会**留下 `granted = false` 的等待行。

> 本次写作未执行上述任何一条 SQL，因此这是一个**待验证的预期**。
> 特别地，"FNV-1a 复算出的 `<KEY>` 与 `LockKey::make()` 的结果一致"这一点
> 在本次写作中**没有被验证过**（没有编译、没有运行 C++）。

---

## 8. 配置项

来自 `.env.example` 第 75–81 行：

| 变量 | 样例值 | 说明（来自文件内注释） |
| --- | --- | --- |
| `SCHEDULER_ENABLED` | `true` | 是否启动调度循环 |
| `SCHEDULER_TICK_INTERVAL_MS` | `15000` | tick 间隔 |
| `SCHEDULER_INSTANCE_ID` | 空 | 实例标识，用于日志与 `scheduler_ticks` 记录；同一时刻只有一个实例持有锁 |
| `SCHEDULER_KEY_NAMESPACE` | `usapp.scheduler` | 任务 key 的命名空间；**跨实例必须完全一致，改动等同于换锁** |

---

## 9. 尚未执行的检查

1. 未编译过 `services/api`，因此 `LockKey.cc` / `Scheduler.cc` 从未被构建或运行。
2. 未执行第 7 节的两连接竞争实验，`t`/`f` 序列未被观测。
3. 未用 `node` 复算过 `usapp.scheduler:__bootstrap__` 的锁键，
   因此其实际数值**未知**（本文档中不给出任何具体数字）。
4. 未查询过 `scheduler_ticks`（表可能尚未创建，迁移未执行）。
5. 未验证 `LockKey::make()` 与 FNV-1a 单行命令结果一致。
6. 未验证 `validateJobKey()` / `validateKeyNamespace()` 的抛错分支。
7. 未验证 `tickOnce()` 的串行语义与 `tickInFlight()` 的不重入行为。
8. 未验证跳过（锁竞争）的 tick 确实不产生任何行——这是**设计意图**，未做实测。
9. 未验证失败路径（`runLocked` 抛错）确实在回滚后另写一行 `outcome='failed'`。
10. 未验证多实例（两个 `usapp_api` 进程）下的互斥——阶段 0 只提供锁基础，未做双实例部署演练。
