# 交接文档 —— USapp 阶段 0 编译错误返工（请求属性访问）

> **状态：文件已修改，构建及测试尚未验证。**
> 本轮未执行任何命令、构建、测试、服务启动或 Git 操作；未使用子代理；
> 未读取 `.env`；未写记忆文件。所有结论来自源码静态分析 + Drogon v1.9.11
> 官方头文件核对。

---

## 0. 一句话交接

Drogon v1.9.11 的 `Attributes` 是**封装类而非 `unordered_map`**，只能使用它
公开的 `find` / `operator[]` / `insert` 三个接口。两个过滤器的属性读写已按真实
接口改写完毕。**接手方请直接跑构建验证，不要再改这两个文件。**

---

## 1. 本轮修改的文件（2 个，仅这两个）

| 文件 | 改动的函数 | 行号（修改后） |
|---|---|---|
| `services/api/src/http/AuthFilter.cc` | `findAttribute` | 30–48 |
| `services/api/src/http/AuthFilter.cc` | `attachSession` | 108–120 |
| `services/api/src/http/AdminFilter.cc` | `findAttribute` | 30–48 |
| `services/api/src/http/AdminFilter.cc` | `attachSession` | 110–121 |

两个文件的 `findAttribute` **内容完全一致**（分别在各自文件的匿名 namespace 内），
`attachSession` 仅签名与写入的键不同。

---

## 2. 官方接口依据

来源：`https://github.com/drogonframework/drogon/blob/v1.9.11/lib/inc/drogon/Attribute.h`

```cpp
class Attributes {
  public:
    template <typename T> const T &get(const std::string &key) const;  // 未命中返回静态默认值
    std::any &operator[](const std::string &key);   // 注意：缺失键会【插入】默认 std::any
    void insert(const std::string &key, const std::any &obj);
    void insert(const std::string &key, std::any &&obj);
    void erase(const std::string &key);
    bool find(const std::string &key);              // 只返回 bool，不返回迭代器
    void clear();
  private:
    using AttributesMap = std::map<std::string, std::any>;  // 私有，无公开 end()/begin()
    AttributesMap attributesMap_;
};
using AttributesPtr = std::shared_ptr<Attributes>;
```

**四个必须记住的坑：**

1. `find(key)` 返回 **`bool`**，不返回迭代器 —— 上一轮的 `const auto it = attributes->find(key)` 把 bool 当迭代器用，是错的。
2. **没有公开 `end()`** —— 上一轮的 `attributes->end()` 直接编译失败。
3. `operator[](key)` 返回 `std::any&`，**缺失时会插入** —— 读取路径上滥用它会污染请求，把「属性缺失」变成「属性存在但为空」，直接破坏 fail-closed 语义。
4. `AttributesPtr` = `std::shared_ptr<Attributes>`；`req->attributes()` 返回的是这个 shared_ptr 的**引用**，所以必须 `auto &` 接收，`auto *` 绑定不出指针。

---

## 3. 修改后的完整函数（可直接复制）

### 3.1 `AuthFilter.cc` — `findAttribute`（第 30–48 行）

```cpp
// Drogon v1.9.11 的 Attributes 是一个封装类（drogon/Attribute.h），不是
// std::unordered_map，因此只能使用它公开的接口：
//   * bool find(key)            —— 只回答"键是否存在"，不返回迭代器
//   * std::any &operator[](key) —— 返回 std::any 的引用，键缺失时会【插入】默认值
//   * void insert(key, value)   —— 两参数写入
// 该类没有公开 end()，所以不能写 attributes->end()。
//
// req->attributes() 返回的是 AttributesPtr（= std::shared_ptr<Attributes>）的引用，
// 因此这里用 auto & 接收；写成 auto * 无法从引用绑定出指针。
const std::any *findAttribute(const drogon::HttpRequestPtr &req, const char *key) {
    const auto &attributes = req->attributes();
    if (attributes == nullptr) {
        return nullptr;
    }
    // 先查存在性：缺失时直接返回，绝不走到 operator[]，
    // 否则读取一个不存在的键会向请求里插入一个空的 std::any，
    // 让"属性缺失"与"属性存在但为空"变得无法区分。
    if (!attributes->find(key)) {
        return nullptr;
    }
    // 键已存在，operator[] 不会插入任何东西。这里返回的是请求自己持有的
    // 那个 std::any 的地址，生命周期与 req 一致，不是局部副本的地址。
    //
    // 关于 shared_ptr 按值拷贝：拷贝与原对象共享同一个 Attributes，销毁局部
    // 副本本身不会让对象失效（请求仍持有一份），所以按值拷贝也不会必然悬空；
    // 真正决定有效性的是请求的生命周期。用引用只是避免多一次引用计数增减。
    return &(*attributes)[key];
}
```

> `AdminFilter.cc` 第 30–48 行的 `findAttribute` 与上面**逐字相同**。

### 3.2 `AuthFilter.cc` — `attachSession`（第 107–120 行）

```cpp
// 把会话状态写进请求属性。属性值是 std::any，读取端只按同一组键取值。
void attachSession(const drogon::HttpRequestPtr &req, const SessionRecord &session) {
    // req->attributes() 返回 AttributesPtr 的引用，用auto & 接收。
    // 写入统一走 Attributes::insert(key, value) 这个两参数接口。
    auto &attributes = req->attributes();
    if (attributes == nullptr) {
        return;
    }
    attributes->insert(authctx::kSessionId, session.id);
    attributes->insert(authctx::kAuthenticated, session.authenticated);
    attributes->insert(authctx::kPreauth, !session.authenticated);
    attributes->insert(authctx::kExpiresAt, session.expiresAt);
    if (session.authenticated && !session.userId.empty()) {
        attributes->insert(authctx::kUserId, session.userId);
    }
}
```

### 3.3 `AdminFilter.cc` — `attachSession`（第 110–121 行）

```cpp
void attachSession(const drogon::HttpRequestPtr &req, const AdminSessionRecord &session) {
    // req->attributes() 返回 AttributesPtr 的引用，用 auto & 接收。
    // 写入统一走 Attributes::insert(key, value) 这个两参数接口。
    auto &attributes = req->attributes();
    if (attributes == nullptr) {
        return;
    }
    attributes->insert(adminctx::kSessionId, session.id);
    attributes->insert(adminctx::kUserId, session.adminUserId);
    attributes->insert(adminctx::kUsername, session.username);
    attributes->insert(adminctx::kAuthenticated, true);
}
```

---

## 4. 关键差异对照（本轮 vs 上一轮）

| 位置 | 上一轮（错误） | 本轮（正确） |
|---|---|---|
| `findAttribute` 取容器 | `const auto &attributes` | 不变（`auto &` 是对的，保留） |
| `findAttribute` 查键 | `const auto it = attributes->find(key);` （把 bool 当迭代器） | `if (!attributes->find(key)) { return nullptr; }` |
| `findAttribute` 判缺失 | `if (it == attributes->end())` （`end()` 不存在） | 已由上面的 `find` 直接判掉，删除 |
| `findAttribute` 取值 | `return &it->second;` | `return &(*attributes)[key];` |
| `attachSession` 写入 | `attributes->insert(k, v)` | 不变（本来就是两参数，**已符合真实接口**） |
| 注释中「值拷贝必然悬空」 | 声称值拷贝会悬空 | **已更正**：拷贝共享同一对象，销毁局部副本不会让对象失效 |

---

## 5. 语义保护确认（逐条核对）

| 要求 | 落实情况 |
|---|---|
| 先检查容器与键是否存在 | `attributes == nullptr` + `!attributes->find(key)`，两道检查 |
| 仅键存在时用 `operator[]` 取值并返回地址 | `find` 通过后才执行 `&(*attributes)[key]` |
| 禁止返回局部 `std::any` 副本地址 | 返回的是 `Attributes::attributesMap_` 内既有元素的地址，请求持有 |
| 禁止读取缺失键时意外插入 | 缺失即 `return nullptr`，**永不进入 `operator[]`** |
| `attachSession` 用真实 `insert(k,v)` | 5 处（Auth）/ 4 处（Admin）全部为两参数调用 |
| 保留缺失属性与类型不匹配处理 | `readAttribute` 未改：nullptr → false；`std::bad_any_cast` → false，**fail-closed 保持** |
| 保留认证/会话/来源/防护逻辑 | `doFilter` 的 Origin → Cookie → CSRF 顺序、白名单 404、入口头、预认证标记等**一行未动** |
| 修正错误注释 | 已更正（见第 4 节末行） |
| 不动事务头文件与 Mailer | 本轮**未触碰**这两个文件 |

**一个需要接手方知晓的副作用（预期内、非缺陷）**：`operator[]` 走的是
`attributesMap_[key]`，键已存在时**不会**覆盖，只是返回既有元素的引用。因此
「认证通过后属性被后写覆盖」这一风险不存在。

---

## 6. 已确认 vs 静态推断

**已确认（有官方头文件或既有编译错误为据）**
- `Attributes` 的四个接口签名 —— 已逐字核对 v1.9.11 `Attribute.h`（见第 2 节）
- 上一轮 `auto *` 无法编译、`attributes->end()` 不存在 —— 上一轮遗留的已确认错误
- 事务头文件与 Mailer 返回类型 —— 前两轮已确认并已修好，本轮未动

**静态推断（未经编译器证实）**
- 本轮 4 个函数能否通过编译，取决于 `HttpRequest::attributes()` 在 v1.9.11 中的
 确切声明（应为返回 `AttributesPtr&`，可能同时有 const 与非 const 两个重载）。
  本轮按「返回引用」处理，故读取端用 `const auto &`、写入端用 `auto &`；
  若实际只有非 const 重载，`const auto &` 仍可绑定（非 const 引用可绑到 const 引用），
  两种情况都不会编译失败。
- 修复后可能暴露**下一批**此前被这两个错误遮挡的编译错误，属正常现象。
  已知下一批候选：`AuthService.cc` / `AdminAuthService.cc` 等文件里若存在
  同样的 `find`/`end` 误用（**本轮已全仓 grep 确认 `src/http` 下无残留**，
  但未逐个核对 `src/services/` 全部文件）。

---

## 7. 接手方的下一步

1. **只构建，不改代码**：
   ```
   docker build -f deploy/Dockerfile.api --target builder .
   ```
2. 构建通过后再跑测试阶段（测试此前从未开始）：
   ```
   docker build -f deploy/Dockerfile.api --target tester .
   ```
3. 若报错，按第 6 节「静态推断」处理：优先怀疑**同类`find`/`end` 误用**，
   用同一模式（`find` 判存在 → `operator[]` 取值 → `insert(k,v)` 写入）改写，
   **不要**给 Drogon 打补丁、不要自造头文件。
4. 前序已完成、本轮**不要重做**的修复（详见第 8 节）。

---

## 8. 前序已完成修复（保持不动，仅供上下文）

1. `deploy/Dockerfile.api` Drogon 构建段 —— `git clone` 加 `--recurse-submodules`
   （Trantor 子模块），`-DBUILD_DROGON_SHARED=ON` → `-DBUILD_SHARED_LIBS=ON`。
   *遗留：同文件第 161 行原 TODO 注释仍写 `BUILD_DROGON_SHARED=OFF`，
   开关名已变但注释未同步，下次触碰运行阶段时顺手修正。*
2. `services/api/src/db/Db.h` —— 删除 `#include <drogon/orm/Transaction.h>`
   （`DbClient.h` 已提供 `Transaction`）。
3. `services/api/src/scheduler/Scheduler.h` —— 同上头替换为 `DbClient.h`。
4. `services/api/src/mailer/Mailer.cc` —— 3 处 `Mailer::SendResult` 改为裸
   `SendResult`（该结构体在 `usapp::mailer` 命名空间作用域，不嵌套于 `Mailer`）。

---

## 9. 附：一个容易误判的对照

`AuthService.h:41` 的 `SendResult` **确实**嵌套在 `class AuthService` 内，
所以 `AuthController.cc:102` 的 `AuthService::SendResult` 是**正确**的。

而 `Mailer.h:25` 的 `SendResult` 在**命名空间作用域**，`Mailer::SendResult` 不成立。

两者形态相似但结论相反 —— 若后续有人「顺手统一」成同一种写法，会引入新错误。
