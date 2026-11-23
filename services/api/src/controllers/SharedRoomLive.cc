#include <drogon/WebSocketController.h>
#include <trantor/net/EventLoop.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "SharedRoomLive.h"
#include "config/AppConfig.h"
#include "db/Db.h"
#include "http/AuthFilter.h"
#include "http/HttpUtil.h"

namespace usapp::controllers {
namespace {
using Socket = drogon::WebSocketConnectionPtr;
using WeakSocket = std::weak_ptr<drogon::WebSocketConnection>;

struct Member {
    Socket socket;
    std::string user;
    std::string space;
    bool visible;
    std::optional<bool> lastPresence;
};

class RoomConnections {
  public:
    bool add(Member member) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!member.socket->connected() || members_.size() >= 128) return false;
        std::size_t userCount = 0;
        for (const auto &entry : members_) {
            if (entry.second.user == member.user) ++userCount;
        }
        if (userCount >= 4) return false;
        const auto socket = member.socket.get();
        const auto space = member.space;
        members_.emplace(socket, std::move(member));
        broadcastPresence(space);
        return true;
    }

    void remove(const Socket &socket) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = members_.find(socket.get());
        if (it == members_.end()) return;
        const auto space = it->second.space;
        members_.erase(it);
        broadcastPresence(space);
    }

    void changed(const std::string &space) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : members_) {
            if (entry.second.space == space) entry.second.socket->send("room-changed");
        }
    }

  private:
    void broadcastPresence(const std::string &space) {
        for (auto &entry : members_) {
            auto &member = entry.second;
            if (member.space != space) continue;
            bool online = false;
            for (const auto &other : members_) {
                const auto &peer = other.second;
                if (peer.space == space && peer.user != member.user && peer.visible && peer.socket->connected()) {
                    online = true;
                    break;
                }
            }
            if (member.lastPresence == online) continue;
            member.lastPresence = online;
            member.socket->send(online ? "peer-online" : "peer-not-visible");
        }
    }

    std::mutex mutex_;
    std::unordered_map<const drogon::WebSocketConnection *, Member> members_;
};

RoomConnections &connections() {
    static RoomConnections value;
    return value;
}

constexpr auto sessionScope =
    "SELECT m.couple_id::text AS space FROM couple_members m "
    "JOIN sessions s ON s.user_id=m.user_id "
    "WHERE m.user_id=$1::uuid AND s.id=$2::uuid AND s.is_authenticated "
    "AND s.revoked_at IS NULL AND s.expires_at>statement_timestamp()";

void close(const Socket &socket) {
    connections().remove(socket);
    socket->shutdown();
}

void checkLiveness(WeakSocket socket, std::string user, std::string session, std::string space) {
    drogon::app().getLoop()->runAfter(20.0, [socket, user, session, space]() {
        const auto live = socket.lock();
        if (!live || !live->connected()) return;
        auto pending = std::make_shared<std::atomic<bool>>(true);
        drogon::app().getLoop()->runAfter(5.0, [socket, pending]() {
            if (!pending->exchange(false)) return;
            if (auto conn = socket.lock()) close(conn);
        });
        db::Pool::instance().client()->execSqlAsync(sessionScope,
            [socket, user, session, space, pending](const drogon::orm::Result &rows) {
                if (!pending->exchange(false)) return;
                const auto conn = socket.lock();
                if (!conn || !conn->connected()) return;
                if (rows.size() != 1 || rows[0]["space"].as<std::string>() != space) { close(conn); return; }
                checkLiveness(socket, user, session, space);
            },
            [socket, pending](const drogon::orm::DrogonDbException &) {
                if (!pending->exchange(false)) return;
                if (auto conn = socket.lock()) close(conn);
            }, user, session);
    });
}
} // namespace

void notifySharedRoomChange(const std::string &user) {
    db::Pool::instance().client()->execSqlAsync(
        "SELECT couple_id::text AS space FROM couple_members WHERE user_id=$1::uuid",
        [](const drogon::orm::Result &rows) {
            if (rows.size() == 1) connections().changed(rows[0]["space"].as<std::string>());
        }, [](const drogon::orm::DrogonDbException &) {}, user);
}

class SharedRoomLive : public drogon::WebSocketController<SharedRoomLive> {
  public:
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/api/v1/shared-room/live", AuthFilter::classTypeName());
    WS_PATH_LIST_END

    void handleNewConnection(const drogon::HttpRequestPtr &req, const Socket &socket) override {
        if (!authctx::isAuthenticated(req) ||
            !http::originMatches(http::requestOrigin(req), AppConfig::instance().publicOrigin)) {
            socket->shutdown();
            return;
        }
        const auto user = authctx::userId(req);
        const auto session = authctx::sessionId(req);
        const bool visible = req->getParameter("presence") == "1";
        auto pending = std::make_shared<std::atomic<bool>>(true);
        const WeakSocket weakSocket = socket;
        drogon::app().getLoop()->runAfter(5.0, [weakSocket, pending]() {
            if (!pending->exchange(false)) return;
            if (auto conn = weakSocket.lock()) conn->shutdown();
        });
        db::Pool::instance().client()->execSqlAsync(sessionScope,
            [socket, user, session, visible, pending](const drogon::orm::Result &rows) {
                if (!pending->exchange(false)) return;
                if (rows.size() != 1) { socket->shutdown(); return; }
                const auto space = rows[0]["space"].as<std::string>();
                if (!connections().add({socket, user, space, visible, std::nullopt})) { socket->shutdown(); return; }
                socket->send("ready");
                checkLiveness(socket, user, session, space);
            }, [socket, pending](const drogon::orm::DrogonDbException &) {
                if (pending->exchange(false)) socket->shutdown();
            }, user, session);
    }

    void handleNewMessage(const Socket &socket, std::string &&, const drogon::WebSocketMessageType &type) override {
        if (type == drogon::WebSocketMessageType::Text || type == drogon::WebSocketMessageType::Binary) close(socket);
    }

    void handleConnectionClosed(const Socket &socket) override { connections().remove(socket); }
};
} // namespace usapp::controllers
