// =====================================================================
// services/api/src/db/Db.h
//
// 数据库访问层。
//
// 关键保证（对应阶段 0 的调度要求）：
//   * TxScope 在 begin() 时从连接池取出【一条】连接并绑定，
//     生命周期内所有语句都走同一个 drogon::orm::Transaction。
//   * 事务级 advisory lock（pg_try_advisory_xact_lock）与同一事务内的
//     写入因此共享同一连接；提交或回滚即释放锁。
//   * 网络操作（发信、推送等）绝不允许出现在持锁事务内：
//     调度任务把这类动作登记为“提交后动作”，见 scheduler/Scheduler.h。
// =====================================================================
#pragma once

// Transaction 由 <drogon/orm/DbClient.h> 定义（Drogon v1.9.11 没有独立的
// drogon/orm/Transaction.h，该版本起 Transaction 直接随 DbClient 一起提供），
// 所以下面第一个 include 已经覆盖，不要再包含一个不存在的头。
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Result.h>
#include <drogon/orm/Row.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace usapp::db {

using ClientPtr = std::shared_ptr<drogon::orm::DbClient>;
using TxPtr = std::shared_ptr<drogon::orm::Transaction>;

// 进程内连接池单例（避免依赖 Drogon 的命名 client 注册表）。
class Pool {
  public:
    static Pool &instance();

    // 创建 DbClient。必须在 drogon::app().run() 之前调用。
    void init(const std::string &connectionString, uint32_t size);

    const ClientPtr &client() const;
    bool ready() const { return client_ != nullptr; }

  private:
    Pool() = default;
    ClientPtr client_;
};

// ---------------------------------------------------------------------
// TxScope：一个显式生命周期的事务。
//
// 【Drogon v1.9.11 的真实接口——本类所有结束方法都据此实现】
//   class Transaction : public DbClient {
//     virtual void rollback() = 0;                       // 无参数、无回调
//     virtual void setCommitCallback(const std::function<void(bool)> &) = 0;
//     // virtual void commit() = 0;                      // 官方注释掉了，不存在
//   };
//
// 因此：
//   * 没有 Transaction::commit(...)；
//   * 没有 Transaction::rollback(callback)。
// 提交的唯一路径是"装上提交回调 + 释放事务引用"：引用归零后 ~TransactionImpl
// 析构时若既未提交也未回滚，就【自动提交】，并在真正的 commit 语句执行完后
// 调用 setCommitCallback 注册的回调（true=成功，false=失败）。
//
// 使用约定（务必遵守，否则会泄漏连接）：
//   * 任何分支都必须最终调用 commit() / rollback() / fail() 之一。
//   * commit()：只有真正提交成功（回调收到 true）才调用 onDone，失败转 onError_。
//   * fail()：请求回滚 + 立即走 onError_，永远不会提交。
//   * rollback()：只是【请求】回滚，不代表数据库已完成回滚，见下方说明。
// ---------------------------------------------------------------------
class TxScope : public std::enable_shared_from_this<TxScope> {
  public:
    using ErrorHandler = std::function<void(const std::string &)>;

    // 开始事务。onReady 在事务可用时被调用（可能同步也可能异步）。
    static void begin(std::function<void(std::shared_ptr<TxScope>)> onReady,
                      ErrorHandler onError);

    drogon::orm::Transaction &tx() {
        if (settled_ || !tx_) {
            throw std::logic_error("事务作用域已经结束");
        }
        return *tx_;
    }

    // 提交。语义：**只有真正提交成功（回调收到 true）时**才调用 onDone；
    // 提交失败时转 onError_。重复调用是安全的空操作（不会重复回调）。
    void commit(std::function<void()> onDone);

    // 请求回滚。**这不是"回滚已完成"**。
    // Drogon 的 Transaction::rollback() 不接受回调、也没有完成通知：
    // 它只是把一条 "rollback" 命令排进连接的命令队列，而队列中的命令要等
    // 连接下一次IO 空闲时才真正下发。
    // 因此 onDone 的准确含义是"回滚已【请求】"，仅可用于串行化后续步骤，
    // 绝不可用于宣称"数据库已回滚、advisory lock 已释放"。
    // lock 实际释放时间 = 该 rollback 命令在服务器端执行完成的时刻。
    // 重复调用是安全的空操作。
    void rollback(std::function<void()> onDone);

    // 主动失败：请求回滚 + 立即走 onError_（不等回滚完成）。
    // 永远不会提交。重复调用是安全的空操作。
    void fail(const std::string &message);

    bool settled() const { return settled_; }

    // 兜底：析构时若既没 commit 也没 rollback，主动请求回滚。
    // 这一步至关重要——若放任最后一个引用释放，~TransactionImpl 会
    // 【自动提交】这个未完成的事务，把只写了一半的 tick 记成 completed。
    // 重复请求回滚本身是幂等的（Drogon 内部有 isCommitedOrRolledback_ 标记）。
    ~TxScope();

  private:
    TxScope(TxPtr tx, ErrorHandler onError) : tx_(std::move(tx)), onError_(std::move(onError)) {}

    void settle() { settled_ = true; }

    TxPtr tx_;
    ErrorHandler onError_;
    bool settled_ = false;
};

// 把 PostgreSQL 异常翻译成可读但**不含敏感信息**的短串（写日志用）。
// 不把 SQL 或参数拼进返回串。
std::string describePgError(const std::string &context, const std::exception &e);

}  // namespace usapp::db
