#include "asio.hpp"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>
#include <asio/write.hpp>
#include <cstdio>

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

awaitable<void> client_proc(int cidx, auto& ctx, auto host, auto port) {
    auto executor = ctx.get_executor();
    // ilog("client_proc cidx:", cidx);
    std::unique_ptr<tcp::socket> skt;

    for (;;) {
        if (skt != nullptr && skt->is_open()) {
            dlog("skt != nullptr && skt->is_open() cidx:", cidx);

            char request[max_length] = {0};
            std::memset(request, 'a' + ((cidx) % 26), max_length);
            request[max_length - 1] = '\0';
            dlog("pre async_write cidx:", cidx);
            co_await asio::async_write(*skt, asio::buffer(request, max_length), use_awaitable);
            dlog("post async_write cidx:", cidx);

            char reply[max_length];
            dlog("pre async_read cidx:", cidx);
            size_t reply_length = co_await asio::async_read(*skt, asio::buffer(reply, max_length), use_awaitable);
            dlog("post async_read cidx:", cidx);
            if (reply_length != max_length) {
                elog("reply_length != max_length reply_length:", reply_length);
            }
            if (strncmp(reply, request, max_length) != 0) {
                elog("reply not match request reply:", reply, " request:", request);
            } else {
                // dlog("everything is ok cidx:", cidx);
                ++ok_count;
            }
        } else {
            skt = nullptr;
            skt = std::make_unique<tcp::socket>(executor);
            tcp::resolver resolver(executor);
            asio::connect(*skt, resolver.resolve(host, port));
            ilog("open and connecting cidx:", cidx);
        }

        asio::steady_timer timer(executor);
        timer.expires_after(std::chrono::seconds(1));
        co_await timer.async_wait(use_awaitable);
    }
    ilog("client_proc exiting cidx:", cidx);
}

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
            co_spawn(io_context, client_proc(i, io_context, host, port), detached);
        }

        co_spawn(io_context, print_proc(), detached);

        io_context.run();
    } catch (std::exception& e) {
        elog("Exception: ", e.what());
    }

    return 0;
}
