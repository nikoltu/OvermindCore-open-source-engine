#include "sc2/api/server.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "s2clientprotocol/sc2api.pb.h"

#include "sc2/api/bridge.hpp"
#include "sc2/api/ws.hpp"
#include "sc2/app/melee_game.hpp"
#include "sc2/app/platform.hpp"

namespace sc2::api {

struct Server::Impl {
    std::unique_ptr<app::platform::Listener> listener;
    std::unique_ptr<app::platform::Connection> conn;
    std::thread thread;
    std::atomic<bool> running{false};
    int port = 0;

    std::mutex m;
    std::condition_variable cv;
    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> response;
    bool have_request = false;
    bool have_response = false;

    Dispatcher dispatch;

    void run();
    void serve(app::platform::Connection& c);
};

void Server::Impl::serve(app::platform::Connection& c) {
    std::vector<std::uint8_t> buf;
    std::uint8_t chunk[8192];

    std::string head;
    while (running.load() && head.find("\r\n\r\n") == std::string::npos) {
        const int n = c.recv(chunk, int(sizeof chunk));
        if (n <= 0) return;
        head.append(reinterpret_cast<const char*>(chunk), std::size_t(n));
        if (head.size() > (1u << 20)) return;
    }
    const std::size_t head_end = head.find("\r\n\r\n");
    if (head_end == std::string::npos) return;
    std::string reply, err;
    if (!ws::handshake_response(head.substr(0, head_end + 4), reply, err)) {
        const std::string http =
            "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        c.send_all(http.data(), int(http.size()));
        return;
    }
    if (!c.send_all(reply.data(), int(reply.size()))) return;
    const std::string tail = head.substr(head_end + 4);
    buf.assign(tail.begin(), tail.end());

    std::vector<std::uint8_t> message;
    while (running.load()) {
        ws::Frame f;
        std::size_t consumed = 0;
        const ws::Decode d = buf.empty() ? ws::Decode::NeedMore
                                         : ws::decode(buf.data(), buf.size(), f, consumed);
        if (d == ws::Decode::Protocol) {
            std::vector<std::uint8_t> code{std::uint8_t(ws::kProtocolError >> 8),
                                           std::uint8_t(ws::kProtocolError & 0xFFu)};
            const std::vector<std::uint8_t> frame =
                ws::encode_control(std::uint8_t(ws::Op::Close), code);
            c.send_all(frame.data(), int(frame.size()));
            return;
        }
        if (d == ws::Decode::NeedMore) {
            const int n = c.recv(chunk, int(sizeof chunk));
            if (n <= 0) return;
            buf.insert(buf.end(), chunk, chunk + n);
            continue;
        }
        buf.erase(buf.begin(), buf.begin() + std::ptrdiff_t(consumed));

        if (f.op == ws::Op::Close) {
            const std::vector<std::uint8_t> frame =
                ws::encode_control(std::uint8_t(ws::Op::Close), {});
            c.send_all(frame.data(), int(frame.size()));
            return;
        }
        if (f.op == ws::Op::Ping) {
            const std::vector<std::uint8_t> frame =
                ws::encode_control(std::uint8_t(ws::Op::Pong), f.payload);
            if (!c.send_all(frame.data(), int(frame.size()))) return;
            continue;
        }
        if (f.op == ws::Op::Pong) continue;

        message.insert(message.end(), f.payload.begin(), f.payload.end());
        if (!f.fin) continue;

        std::vector<std::uint8_t> answer;
        {
            std::unique_lock<std::mutex> lock(m);
            request = std::move(message);
            have_request = true;
            have_response = false;
            cv.wait(lock, [&] { return have_response || !running.load(); });
            if (!running.load()) return;
            answer = std::move(response);
            have_response = false;
        }
        message.clear();
        const std::vector<std::uint8_t> frame = ws::encode_binary(answer);
        if (!c.send_all(frame.data(), int(frame.size()))) return;
    }
}

void Server::Impl::run() {
    while (running.load()) {
        std::unique_ptr<app::platform::Connection> c = listener->accept();
        if (!c) break;
        {
            std::lock_guard<std::mutex> lock(m);
            conn = std::move(c);
        }
        dispatch.reset();
        serve(*conn);
        std::lock_guard<std::mutex> lock(m);
        if (conn) conn->close();
        conn.reset();
    }
}

Server::Server() : impl_(std::make_unique<Impl>()) {}

Server::~Server() { stop(); }

int Server::port() const { return impl_->port; }

bool Server::start(int port, std::string& error) {
    impl_->listener = app::platform::listen_tcp(port, error);
    if (!impl_->listener) return false;
    impl_->port = port;
    impl_->running.store(true);
    impl_->thread = std::thread([this] { impl_->run(); });
    return true;
}

void Server::stop() {
    if (!impl_) return;
    if (!impl_->running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        if (impl_->conn) impl_->conn->close();
    }
    impl_->cv.notify_all();
    if (impl_->listener) impl_->listener->close();
    if (impl_->thread.joinable()) impl_->thread.join();
}

bool Server::pump(Host& host) {
    std::vector<std::uint8_t> raw;
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        if (!impl_->have_request) return false;
        raw = std::move(impl_->request);
        impl_->have_request = false;
    }

    std::vector<std::uint8_t> out;
    (void)impl_->dispatch.handle(host, raw, out);
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        impl_->response = std::move(out);
        impl_->have_response = true;
    }
    impl_->cv.notify_all();
    return true;
}

}
