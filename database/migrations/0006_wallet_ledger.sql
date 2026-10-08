-- =====================================================================
-- 0006_wallet_ledger.sql
-- 阶段 1 M2-A：钱包账户、钱包操作与只追加流水账（docs/phase1-plan.md 第 9 节）。
--
-- 硬约束：
--   * wallet_accounts 只是“加锁锚点 + 成员绑定”，不保存任何余额或序号，
--     因此不存在可被应用角色伪造的可变账本状态。
--   * 余额 = 该账户 seq 最大的一条流水的 balance_after；没有流水即为 0。
--   * 链式校验只依据上一条不可变流水：持有账户行锁后读取上一条流水，
--     无流水时起始状态为 seq = 0、balance = 0；要求 seq = 上一条 + 1、
--     balance_after = 上一条 balance_after + amount。
--   * 每条 INSERT 语句只能写入一行流水；多资产操作必须在同一事务内分多条语句写入。
--   * 三张表只追加：UPDATE / DELETE / TRUNCATE 一律由语句级触发器拒绝
--     （对表所有者与维护角色同样生效）；应用角色另外没有这些权限。
--   * 应用角色仅在 wallet_accounts.lock_token 上有列级 UPDATE 权限，
--     唯一用途是让 SELECT ... FOR UPDATE 能够加行锁；任何真正的 UPDATE 都被触发器拒绝。
--   * 已批准的上限（D2）：余额最多 1,000,000 小红花 / 1,000,000,000 阳光值；
--     单条流水绝对值最多 999 小红花 / 100,000 阳光值；超限一律拒绝，绝不截断。
--   * 本迁移不写入任何钱包数据。
--
-- 自定义 SQLSTATE（触发器抛出，测试据此断言，不依赖本地化的错误文本）：
--   UW001 只追加表上的 UPDATE / DELETE / TRUNCATE
--   UW002 写流水时账户不存在
--   UW003 流水 seq 与上一条不连续
--   UW004 流水 balance_after 与上一条 balance_after + amount 不符
--   UW005 一条 INSERT 语句写入了多行流水
--   UW006 提交（或 SET CONSTRAINTS ... IMMEDIATE）时钱包操作没有任何流水
-- =====================================================================

BEGIN;

-- ---------------------------------------------------------------- 角色前置检查
-- 本迁移按固定角色名授权；角色缺失或是超级用户时直接失败，而不是静默授权给错误的身份。
-- 注意：这只能证明角色存在，不能证明运行时真的以 usapp_api 连接；
-- 运行时身份由 services/api/src/db/WalletPrivilegePolicy 的有效权限矩阵检查。
DO $wallet_roles$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'usapp_api') THEN
        RAISE EXCEPTION '0006_wallet_ledger: 角色 usapp_api 不存在';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'usapp_push') THEN
        RAISE EXCEPTION '0006_wallet_ledger: 角色 usapp_push 不存在';
    END IF;
    IF EXISTS (SELECT 1 FROM pg_roles
                WHERE rolname IN ('usapp_api', 'usapp_push') AND rolsuper) THEN
        RAISE EXCEPTION '0006_wallet_ledger: usapp_api / usapp_push 不得是超级用户';
    END IF;
END
$wallet_roles$;

-- ------------------------------------------------- couple_members 复合唯一键
-- user_id 本身已唯一，(couple_id, user_id) 必然唯一；新增该约束只是为了让
-- 钱包表用复合外键在数据库层强制“用户属于该空间”。不修改 0003。
ALTER TABLE couple_members
    ADD CONSTRAINT couple_members_couple_user_unique UNIQUE (couple_id, user_id);

-- ---------------------------------------------------------- wallet_accounts
CREATE TABLE wallet_accounts (
    user_id    uuid        NOT NULL,
    asset_type text        NOT NULL,
    couple_id  uuid        NOT NULL,
    lock_token smallint    NOT NULL DEFAULT 0,
    created_at timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT wallet_accounts_pkey PRIMARY KEY (user_id, asset_type),
    CONSTRAINT wallet_accounts_user_fk
        FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE RESTRICT,
    CONSTRAINT wallet_accounts_member_fk
        FOREIGN KEY (couple_id, user_id) REFERENCES couple_members (couple_id, user_id)
        ON DELETE RESTRICT,
    CONSTRAINT wallet_accounts_asset_type_valid CHECK (asset_type IN ('flower', 'sunshine')),
    CONSTRAINT wallet_accounts_lock_token_zero CHECK (lock_token = 0)
);

COMMENT ON TABLE wallet_accounts IS
    '钱包账户：每个 (用户, 资产) 一行，只作加锁锚点与成员绑定，不保存余额或序号。'
    '余额来自 wallet_ledger_entries 中该账户 seq 最大的一条流水。';
COMMENT ON COLUMN wallet_accounts.lock_token IS
    '恒为 0。应用角色只在本列拥有列级 UPDATE 权限，用于 SELECT ... FOR UPDATE 加行锁；'
    '任何 UPDATE 语句都会被只追加触发器拒绝。';

-- -------------------------------------------------------- wallet_operations
CREATE TABLE wallet_operations (
    id                  uuid        NOT NULL DEFAULT gen_random_uuid(),
    couple_id           uuid        NOT NULL,
    actor_user_id       uuid        NOT NULL,
    operation_type      text        NOT NULL,
    source_type         text        NOT NULL,
    source_id           text        NOT NULL,
    idempotency_key     text,
    request_fingerprint text,
    created_at          timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT wallet_operations_pkey PRIMARY KEY (id),
    CONSTRAINT wallet_operations_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT wallet_operations_couple_fk
        FOREIGN KEY (couple_id) REFERENCES couples (id) ON DELETE RESTRICT,
    CONSTRAINT wallet_operations_actor_member_fk
        FOREIGN KEY (couple_id, actor_user_id) REFERENCES couple_members (couple_id, user_id)
        ON DELETE RESTRICT,
    CONSTRAINT wallet_operations_type_valid
        CHECK (operation_type IN ('TASK_REWARD', 'REWARD_REDEMPTION', 'REDEMPTION_REFUND')),
    CONSTRAINT wallet_operations_source_type_shape
        CHECK (length(btrim(source_type)) BETWEEN 1 AND 64),
    CONSTRAINT wallet_operations_source_id_shape
        CHECK (length(btrim(source_id)) BETWEEN 1 AND 128),
    CONSTRAINT wallet_operations_idempotency_key_shape
        CHECK (idempotency_key IS NULL OR length(idempotency_key) BETWEEN 16 AND 128),
    CONSTRAINT wallet_operations_fingerprint_pairing
        CHECK ((idempotency_key IS NULL) = (request_fingerprint IS NULL)),
    CONSTRAINT wallet_operations_fingerprint_shape
        CHECK (request_fingerprint IS NULL OR request_fingerprint ~ '^[0-9a-f]{64}$'),
    CONSTRAINT wallet_operations_source_unique UNIQUE (operation_type, source_type, source_id)
);

-- 幂等作用域：(发起成员, 操作类型, 幂等键)。actor_user_id 为 NOT NULL，唯一性保证真实有效；
-- 阶段 1 没有系统发起者（docs/phase1-plan.md 9.7）。
CREATE UNIQUE INDEX wallet_operations_idempotency_unique
    ON wallet_operations (actor_user_id, operation_type, idempotency_key)
    WHERE idempotency_key IS NOT NULL;

COMMENT ON TABLE wallet_operations IS
    '钱包操作：一次用户可见动作一行（例如结算一个任务周期），可对应每种资产各一条流水。'
    '只追加；纠错只能通过新的补偿操作（阶段 5）。';

-- ---------------------------------------------------- wallet_ledger_entries
CREATE TABLE wallet_ledger_entries (
    id            uuid        NOT NULL DEFAULT gen_random_uuid(),
    operation_id  uuid        NOT NULL,
    couple_id     uuid        NOT NULL,
    user_id       uuid        NOT NULL,
    asset_type    text        NOT NULL,
    seq           bigint      NOT NULL,
    amount        bigint      NOT NULL,
    balance_after bigint      NOT NULL,
    created_at    timestamptz NOT NULL DEFAULT now(),

    CONSTRAINT wallet_ledger_entries_pkey PRIMARY KEY (id),
    CONSTRAINT wallet_ledger_entries_operation_fk
        FOREIGN KEY (operation_id, couple_id) REFERENCES wallet_operations (id, couple_id)
        ON DELETE RESTRICT,
    CONSTRAINT wallet_ledger_entries_member_fk
        FOREIGN KEY (couple_id, user_id) REFERENCES couple_members (couple_id, user_id)
        ON DELETE RESTRICT,
    CONSTRAINT wallet_ledger_entries_account_fk
        FOREIGN KEY (user_id, asset_type) REFERENCES wallet_accounts (user_id, asset_type)
        ON DELETE RESTRICT,
    CONSTRAINT wallet_ledger_entries_asset_type_valid CHECK (asset_type IN ('flower', 'sunshine')),
    CONSTRAINT wallet_ledger_entries_seq_positive CHECK (seq >= 1),
    CONSTRAINT wallet_ledger_entries_amount_nonzero CHECK (amount <> 0),
    CONSTRAINT wallet_ledger_entries_amount_bound CHECK (
        (asset_type = 'flower'   AND amount BETWEEN -999 AND 999)
        OR (asset_type = 'sunshine' AND amount BETWEEN -100000 AND 100000)
    ),
    CONSTRAINT wallet_ledger_entries_balance_bound CHECK (
        (asset_type = 'flower'   AND balance_after BETWEEN 0 AND 1000000)
        OR (asset_type = 'sunshine' AND balance_after BETWEEN 0 AND 1000000000)
    ),
    CONSTRAINT wallet_ledger_entries_account_seq_unique UNIQUE (user_id, asset_type, seq),
    CONSTRAINT wallet_ledger_entries_operation_account_unique UNIQUE (operation_id, user_id, asset_type)
);

COMMENT ON TABLE wallet_ledger_entries IS
    '只追加流水账。每个账户 seq 从 1 连续递增；balance_after = 上一条 balance_after + amount'
    '（无上一条时为 0 + amount），由 BEFORE INSERT 链式触发器在账户行锁下校验。';

-- ------------------------------------------------------------- 只追加保护
-- 语句级触发器：即使 UPDATE / DELETE 命中 0 行也会拒绝；TRUNCATE 只能用语句级触发器拦截。
-- SELECT ... FOR UPDATE 与 INSERT ... ON CONFLICT DO NOTHING 不会触发 UPDATE 触发器。
CREATE FUNCTION usapp_wallet_reject_mutation()
RETURNS trigger
LANGUAGE plpgsql
AS $wallet_reject$
BEGIN
    RAISE EXCEPTION USING
        ERRCODE = 'UW001',
        MESSAGE = 'USAPP_WALLET_APPEND_ONLY',
        DETAIL  = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$wallet_reject$;

CREATE TRIGGER wallet_accounts_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON wallet_accounts
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_wallet_reject_mutation();
CREATE TRIGGER wallet_operations_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON wallet_operations
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_wallet_reject_mutation();
CREATE TRIGGER wallet_ledger_entries_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON wallet_ledger_entries
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_wallet_reject_mutation();

-- ------------------------------------------------------------- 链式校验
-- 只依据上一条不可变流水，绝不读取任何可变副本。
-- 关键字段为 NULL 时直接放行，交给 NOT NULL 约束报 23502，避免用链式错误掩盖空值错误。
-- READ COMMITTED 下，PL/pgSQL（volatile）内每条语句取新快照：等到账户锁之后，
-- 读取上一条流水的查询能看到另一事务刚提交的流水；同一事务中先前语句插入的流水同样可见。
-- UNIQUE (user_id, asset_type, seq) 是独立兜底。
CREATE FUNCTION usapp_wallet_ledger_chain()
RETURNS trigger
LANGUAGE plpgsql
AS $wallet_chain$
DECLARE
    prev_seq     bigint;
    prev_balance bigint;
BEGIN
    IF NEW.user_id IS NULL OR NEW.asset_type IS NULL OR NEW.seq IS NULL
       OR NEW.amount IS NULL OR NEW.balance_after IS NULL THEN
        RETURN NEW;
    END IF;

    PERFORM 1
       FROM wallet_accounts a
      WHERE a.user_id = NEW.user_id AND a.asset_type = NEW.asset_type
        FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING
            ERRCODE = 'UW002',
            MESSAGE = 'USAPP_WALLET_ACCOUNT_MISSING';
    END IF;

    SELECT e.seq, e.balance_after
      INTO prev_seq, prev_balance
      FROM wallet_ledger_entries e
     WHERE e.user_id = NEW.user_id AND e.asset_type = NEW.asset_type
     ORDER BY e.seq DESC
     LIMIT 1;
    IF NOT FOUND THEN
        prev_seq := 0;
        prev_balance := 0;
    END IF;

    IF NEW.seq <> prev_seq + 1 THEN
        RAISE EXCEPTION USING
            ERRCODE = 'UW003',
            MESSAGE = 'USAPP_WALLET_CHAIN_SEQ',
            DETAIL  = 'expected seq ' || (prev_seq + 1) || ', got ' || NEW.seq;
    END IF;
    IF NEW.balance_after <> prev_balance + NEW.amount THEN
        RAISE EXCEPTION USING
            ERRCODE = 'UW004',
            MESSAGE = 'USAPP_WALLET_CHAIN_BALANCE',
            DETAIL  = 'expected balance_after ' || (prev_balance + NEW.amount)
                      || ', got ' || NEW.balance_after;
    END IF;
    RETURN NEW;
END
$wallet_chain$;

CREATE TRIGGER wallet_ledger_entries_chain
    BEFORE INSERT ON wallet_ledger_entries
    FOR EACH ROW EXECUTE FUNCTION usapp_wallet_ledger_chain();

-- ------------------------------------------------------ 每条语句只写一行
-- 不依赖“同一语句内行级 BEFORE 触发器能否看到前面行”的可见性规则。
CREATE FUNCTION usapp_wallet_ledger_single_row()
RETURNS trigger
LANGUAGE plpgsql
AS $wallet_single$
BEGIN
    IF (SELECT count(*) FROM wallet_inserted_rows) > 1 THEN
        RAISE EXCEPTION USING
            ERRCODE = 'UW005',
            MESSAGE = 'USAPP_WALLET_MULTI_ROW_INSERT';
    END IF;
    RETURN NULL;
END
$wallet_single$;

CREATE TRIGGER wallet_ledger_entries_single_row
    AFTER INSERT ON wallet_ledger_entries
    REFERENCING NEW TABLE AS wallet_inserted_rows
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_wallet_ledger_single_row();

-- ---------------------------------------------- 操作必须至少有一条流水（延迟）
CREATE FUNCTION usapp_wallet_operation_has_entries()
RETURNS trigger
LANGUAGE plpgsql
AS $wallet_complete$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM wallet_ledger_entries e WHERE e.operation_id = NEW.id) THEN
        RAISE EXCEPTION USING
            ERRCODE = 'UW006',
            MESSAGE = 'USAPP_WALLET_OPERATION_WITHOUT_ENTRIES';
    END IF;
    RETURN NULL;
END
$wallet_complete$;

CREATE CONSTRAINT TRIGGER wallet_operations_require_entries
    AFTER INSERT ON wallet_operations
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_wallet_operation_has_entries();

-- ------------------------------------------------------------- 权限收窄
-- 默认权限（init-roles / 01-init-roles.sh）会给新表授予 usapp_api 全部 DML，这里显式收回。
-- 表级 REVOKE 同时收回对应的列级权限。init-roles.sql 末尾有同样的可重复收窄段，
-- 防止重新执行 init-roles 时被 ALL TABLES 授权放宽。
REVOKE ALL ON wallet_accounts, wallet_operations, wallet_ledger_entries FROM PUBLIC;
REVOKE ALL ON wallet_accounts, wallet_operations, wallet_ledger_entries FROM usapp_api;
REVOKE ALL ON wallet_accounts, wallet_operations, wallet_ledger_entries FROM usapp_push;
GRANT SELECT, INSERT ON wallet_accounts, wallet_operations, wallet_ledger_entries TO usapp_api;
GRANT UPDATE (lock_token) ON wallet_accounts TO usapp_api;

COMMIT;
