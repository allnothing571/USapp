// =====================================================================
// services/api/src/db/Db.cc
// =====================================================================
#include "db/Db.h"

#include <drogon/drogon.h>

#include <stdexcept>
#include <utility>

namespace usapp::db {

Pool &Pool::instance() {
    static Pool pool;
    return pool;
}

void Pool::init(const std::string &connectionString, uint32_t size) {
    if (client_) {
        throw std::runtime_error("数据库连接池已初始化，重复调用 init()");
    }
    if (size == 0) {
        throw std::runtime_error("数据库连接池大小不能为 0");
    }
    client_ = drogon::orm::DbClient::newPgClient(connectionString, size);
}

const ClientPtr &Pool::client() const {
    if (!client_) {
        throw std::runtime_error("数据库连接池尚未初始化");
    }
    return client_;
}

void TxScope::begin(std::function<void(std::shared_ptr<TxScope>)> onReady, ErrorHandler onError) {
    TxPtr tx;
    try {
        tx = Pool::instance().client()->newTransaction();
    } catch (const std::exception &e) {
        onError(std::string("创建事务失败: ") + e.what());
        return;
    }
    if (!tx) {
        onError("创建事务失败: newTransaction 返回空");
        return;
    }
    // 构造函数私有 -> 用 new 包一层 shared_ptr。
    std::shared_ptr<TxScope> scope(new TxScope(std::move(tx), std::move(onError)));
    onReady(scope);
}

void TxScope::beginAsync(std::function<void(std::shared_ptr<TxScope>)> onReady, ErrorHandler onError) {
    ClientPtr client;
    try { client = Pool::instance().client(); }
    catch (const std::exception &e) { onError(std::string("创建事务失败: ") + e.what()); return; }
    beginOnClientAsync(std::move(client), std::move(onReady), std::move(onError));
}

void TxScope::beginOnClientAsync(ClientPtr client, std::function<void(std::shared_ptr<TxScope>)> onReady, ErrorHandler onError) {
    try {
        if (!client) { onError("Recovery database client unavailable"); return; }
        client->newTransactionAsync(
            [onReady = std::move(onReady), onError](const TxPtr &tx) {
                if (!tx) {
                    onError("创建事务失败: 等待连接超时");
                    return;
                }
                std::shared_ptr<TxScope> scope(new TxScope(tx, onError));
                onReady(scope);
            });
    } catch (const std::exception &e) {
        onError(std::string("创建事务失败: ") + e.what());
    }
}

// ---------------------------------------------------------------------
// 提交：Drogon v1.9.11 没有 Transaction::commit()，唯一路径是
//   setCommitCallback(收到 true 表示提交成功) + 释放事务引用。
//
// 业务回调可能间接持有 TxScope。安装回调后必须释放 tx_，断开作用域
// 对事务的持有关系；提交回调自身不得直接持有 Transaction。
// 安装回调可能在复制 std::function 时抛异常，安装成功前不能标记 settled。
// ---------------------------------------------------------------------
void TxScope::commit(std::function<void()> onDone) {
    if (settled_) {
        // 重复结束：静默返回，绝不重复触发业务回调（否则会重复发HTTP 响应）。
        return;
    }
    if (!tx_) {
        settle();
        if (onError_) {
            onError_("提交失败: 事务对象已失效");
        }
        return;
    }

    try {
        tx_->setCommitCallback([onDone = std::move(onDone), onError = onError_](bool ok) {
            if (ok) {
                if (onDone) {
                    onDone();
                }
            } else if (onError) {
                onError("事务提交失败: 提交语句执行失败");
            }
        });
    } catch (...) {
        // 仍未 settled，失败路径会请求回滚，析构也保留回滚兜底。
        fail("安装事务提交回调失败");
        return;
    }

    settle();
    // 释放本对象对事务的引用。这是"请求提交"的真正动作：引用归零后
    // ~TransactionImpl 才会发出 commit。若此时还有其它SqlCmd 在飞行中
    // （Drogon 内部用shared_from_this 维持），提交会排到它们之后执行，
    // 因此"先写完所有 SQL，再 commit"的顺序仍然成立。
    tx_.reset();
}

// ---------------------------------------------------------------------
// 请求回滚（不是"回滚已完成"）。
//
// Transaction::rollback() 无参数、无完成通知，只能"请求"。Drogon 会把一条
// "rollback" 命令排进连接的命令队列，由连接的下一次空闲回调下发。
// 所以 onDone 的语义是"回滚已请求"，调用方绝不能据此断言锁已释放。
// ---------------------------------------------------------------------
void TxScope::rollback(std::function<void()> onDone) {
    if (settled_) {
        // 重复结束：静默返回。刻意不调用 onDone —— 那会让调用方误以为
        // 一次新的回滚被受理，从而重复写失败行/ 重复发响应。
        return;
    }
    if (tx_) {
        tx_->rollback();
    }
    // 回滚请求返回后再标记结束。框架内部持有事务，直到回滚处理结束。
    settle();
    tx_.reset();
    if (onDone) {
        // 语义：回滚已【请求】。锁的释放实际发生在服务器端执行 rollback 之后。
        onDone();
    }
}

void TxScope::fail(const std::string &message) {
    if (settled_) {
        return;
    }
    if (tx_) {
        tx_->rollback();
    }
    settle();
    tx_.reset();
    // 移动回调不分配内存；安装提交回调时的复制异常不能再次在这里发生。
    auto onError = std::move(onError_);
    if (onError) {
        // 立刻报错，不等回滚真正完成：调用方需要在本次响应里告知客户端失败。
        onError(message);
    }
}

TxScope::~TxScope() {
    // 兜底：作用域未经任何结束方法就析构（例如调用方漏调，或回调里抛异常
    // 导致栈展开）。必须显式请求回滚，否则 ~TransactionImpl 会自动 commit，
    // 把只写了一半的 scheduler_ticks 记录当成 completed —— 这是数据正确性问题，
    // 不是风格问题。
    if (!settled_ && tx_) {
        try {
            tx_->rollback();
        } catch (...) {
            // 析构函数绝不抛异常。回滚请求失败也只能记日志。
        }
        settled_ = true;
    }
}

std::string describePgError(const std::string &context, const std::exception &e) {
    // Drogon 的 orm::DrogonDbException 的 what() 包含 PostgreSQL 的
    // SQLSTATE 与消息；不包含绑定参数值，因此可以安全写日志。
    return context + ": " + e.what();
}

}  // namespace usapp::db
