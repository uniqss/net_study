#include "asio.hpp"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>
#include <asio/write.hpp>
#include <cstdio>
#include <unordered_set>
#include <unordered_map>
#include <deque>
#include <memory>

#include "simple_log.h"

using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::use_awaitable;
using asio::ip::tcp;
namespace this_coro = asio::this_coro;

enum { max_length = 1024 };

int max_client_count = 10000;
int ok_count = 0;
int64_t ok_count_total = 0;

void setdebugging() {
    if (max_client_count <= 2) goDebugging = true;
}

awaitable<void> print_proc() {
    auto executor = co_await this_coro::executor;
    int ok_count_0 = 0;
    for (;;) {
        asio::steady_timer timer(executor);
        timer.expires_after(std::chrono::seconds(1));
        co_await timer.async_wait(use_awaitable);

        if (ok_count == 0) {
            ++ok_count_0;
        } else {
            ok_count_0 = 0;
        }
        bool need_print = ok_count_0 <= 3;

        ok_count_total += ok_count;
        if (need_print) {
            ilog("ok_count:", ok_count, " ok_count_total:", ok_count_total);
        }
        ok_count = 0;
    }
}

class client : public std::enable_shared_from_this<client> {
   public:
    client(int cidx, asio::io_context& ctx, const std::string& host, const std::string& port)
        : m_ctx(ctx), m_cidx(cidx), m_skt(ctx), m_host(host), m_port(port) {
        std::memset(m_buffRequest, 'a' + ((cidx) % 26), max_length);
        m_buffRequest[max_length - 1] = '\0';
    }

    awaitable<void> client_proc_connect() {
        auto executor = m_skt.get_executor();
        try {
            for (;;) {
                if (!m_skt.is_open()) {
                    tcp::resolver resolver(executor);
                    asio::connect(m_skt, resolver.resolve(m_host, m_port));
                    ilog("open and connecting cidx:", m_cidx);
                    if (m_skt.is_open()) {
                        co_spawn(
                            m_skt.get_executor(), [self = shared_from_this()] { return self->client_proc_read(); },
                            detached);
                        co_spawn(
                            m_skt.get_executor(), [self = shared_from_this()] { return self->client_proc_write(); },
                            detached);
                    }
                } else {
                }
                asio::steady_timer timer(executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(use_awaitable);
            }
        } catch (const std::exception& e) {
            elog("client_proc_connect error:", e.what());
            m_skt.close();
        }
    }

    awaitable<void> client_proc_read() {
        auto executor = m_skt.get_executor();
        try {
            for (;;) {
                if (m_skt.is_open()) {
                    dlog("pre async_read cidx:", m_cidx);
                    size_t reply_length =
                        co_await asio::async_read(m_skt, asio::buffer(m_buffReply, max_length), use_awaitable);
                    dlog("post async_read cidx:", m_cidx);
                    if (reply_length != max_length) {
                        elog("reply_length != max_length reply_length:", reply_length);
                    }
                    if (strncmp(m_buffReply, m_buffRequest, max_length) != 0) {
                        elog("reply not match request reply:", m_buffReply, " request:", m_buffRequest);
                    } else {
                        // dlog("everything is ok cidx:", m_cidx);
                        ++ok_count;
                    }
                }
                asio::steady_timer timer(executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(use_awaitable);
            }
        } catch (const std::exception& e) {
            elog("client_proc_read error:", e.what());
            m_skt.close();
        }
    }

    awaitable<void> client_proc_write() {
        auto executor = m_skt.get_executor();
        try {
            for (;;) {
                if (m_skt.is_open()) {
                    if (!m_writemsgs.empty()) {
                        std::string msg = m_writemsgs.front();
                        m_writemsgs.pop_front();
                        dlog("pre async_write cidx:", m_cidx);
                        co_await asio::async_write(m_skt, asio::buffer(msg.data(), max_length), use_awaitable);
                        dlog("post async_write cidx:", m_cidx);
                    }
                }
                asio::steady_timer timer(executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(use_awaitable);
            }
        } catch (const std::exception& e) {
            elog("client_proc_write error:", e.what());
            m_skt.close();
        }
    }

    awaitable<void> addmsg_forsend() {
        auto executor = m_skt.get_executor();
        try {
            for (;;) {
                if (m_writemsgs.size() < 2) m_writemsgs.push_back(m_buffRequest);

                asio::steady_timer timer(executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(use_awaitable);
            }
        } catch (const std::exception& e) {
            elog("addmsg_forsend error:", e.what());
        }
    }

    awaitable<void> client_proc() {
        auto executor = m_skt.get_executor();
        try {
            for (;;) {
                if (!m_skt.is_open()) {
                    co_spawn(
                        m_skt.get_executor(), [self = shared_from_this()] { return self->client_proc_connect(); },
                        detached);
                }
                asio::steady_timer timer(executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(use_awaitable);
            }
        } catch (const std::exception& e) {
            elog("client_proc_write error:", e.what());
        }
    }

    void start() {
        co_spawn(
            m_skt.get_executor(), [self = shared_from_this()] { return self->client_proc(); }, detached);
        co_spawn(
            m_skt.get_executor(), [self = shared_from_this()] { return self->addmsg_forsend(); }, detached);

        ilog("client_proc exiting cidx:", m_cidx);
    }

   private:
    asio::io_context& m_ctx;
    int m_cidx;
    tcp::socket m_skt;
    std::string m_host;
    std::string m_port;
    char m_buffRequest[max_length];
    char m_buffReply[max_length];
    std::deque<std::string> m_writemsgs;
};

using clientPtr = std::shared_ptr<client>;
std::unordered_map<int, clientPtr> gClients;

int main(int argc, char* argv[]) {
    try {
        std::string host = "127.0.0.1";
        std::string port = "8888";
        if (argc >= 4) {
            host = argv[1];
            port = argv[2];
            max_client_count = atoi(argv[3]);
            if (max_client_count < 2) max_client_count = 2;
        }

        setdebugging();

        asio::io_context io_context(1);

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { io_context.stop(); });

        for (int i = 1; i <= max_client_count; ++i) {
            gClients[i] = std::make_shared<client>(i, io_context, host, port);
            gClients[i]->start();
        }

        co_spawn(io_context, print_proc(), detached);

        io_context.run();
    } catch (std::exception& e) {
        elog("Exception: ", e.what());
    }

    return 0;
}
