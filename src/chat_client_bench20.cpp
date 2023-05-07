#include <asio.hpp>
#include <set>
#include <deque>
#include <iostream>

using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::redirect_error;
using asio::use_awaitable;
using asio::ip::tcp;
namespace this_coro = asio::this_coro;

int max_client_count = 10000;
int ok_count = 0;
int64_t ok_count_total = 0;

awaitable<void> print_proc() {
    auto executor = co_await this_coro::executor;
    for (;;) {
        asio::steady_timer timer(executor);
        timer.expires_after(std::chrono::seconds(1));
        co_await timer.async_wait(use_awaitable);

        ok_count_total += ok_count;
        std::cout << "ok_count:" << ok_count << " ok_count_total:" << ok_count_total << std::endl;
        ok_count = 0;
    }
}

class chat_participant {
   public:
    virtual ~chat_participant() {}
    virtual void deliver(const std::string& msg) = 0;
};

using chat_participant_ptr = std::shared_ptr<chat_participant>;

class chat_room {
   public:
    void join(chat_participant_ptr participant) {
        participants_.insert(participant);
        for (auto& msg : recent_msgs_) {
            participant->deliver(msg);
        }
    }
    void leave(chat_participant_ptr participant) { participants_.erase(participant); }

    void deliver(const std::string& msg) {
        recent_msgs_.push_back(msg);
        while (recent_msgs_.size() > max_recent_msgs) {
            recent_msgs_.pop_front();
        }
        for (auto participant : participants_) {
            participant->deliver(msg);
        }
    }

   private:
    std::set<chat_participant_ptr> participants_;
    enum { max_recent_msgs = 100 };
    std::deque<std::string> recent_msgs_;
};

class chat_session : public chat_participant, public std::enable_shared_from_this<chat_session> {
   public:
    chat_session(tcp::socket socket, chat_room& room)
        : socket_(std::move(socket)), timer_(socket.get_executor()), room_(room) {
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    void start() {
        room_.join(shared_from_this());

        co_spawn(
            socket_.get_executor(), [self = shared_from_this()] { return self->reader(); }, detached);

        co_spawn(
            socket_.get_executor(), [self = shared_from_this()] { return self->writer(); }, detached);
    }

    virtual void deliver(const std::string& msg) {
        write_msgs_.push_back(msg);
        timer_.cancel_one();
    }

   private:
    awaitable<void> reader() {
        try {
            std::string read_msg;
            for (;;) {
                std::size_t n =
                    co_await asio::async_read_until(socket_, asio::dynamic_buffer(read_msg, 1024), "\n", use_awaitable);
                room_.deliver(read_msg);
                read_msg.erase(0, n);
            }
        } catch (const std::exception& e) {
            stop();
        }
    }

    awaitable<void> writer() {
        try {
            while (socket_.is_open()) {
                if (write_msgs_.empty()) {
                    asio::error_code ec;
                    co_await timer_.async_wait(redirect_error(use_awaitable, ec));
                } else {
                    co_await asio::async_write(socket_, asio::buffer(write_msgs_.front()), use_awaitable);
                    write_msgs_.pop_front();
                }
            }
        } catch (const std::exception& e) {
            stop();
        }
    }

    void stop() {
        room_.leave(shared_from_this());
        socket_.close();
        timer_.cancel();
    }

   private:
    tcp::socket socket_;
    asio::steady_timer timer_;
    chat_room& room_;
    std::deque<std::string> write_msgs_;
};


awaitable<void> client_proc(int cidx, auto& ctx, auto host, auto port) {
    auto executor = co_await this_coro::executor;
    // std::cout << "client_proc cidx:" << cidx << std::endl;
    std::unique_ptr<tcp::socket> skt;

    co_spawn(
        socket_.get_executor(), [self = shared_from_this()] { return self->reader(); }, detached);

    co_spawn(
        socket_.get_executor(), [self = shared_from_this()] { return self->writer(); }, detached);
}

int main(int argc, const char** argv) {
    try {
        std::string host = "127.0.0.1";
        std::string port = "8888";
        if (argc >= 4) {
            host = argv[1];
            port = argv[2];
            max_client_count = atoi(argv[3]);
            if (max_client_count < 2) max_client_count = 2;
        }

        asio::io_context io_context(1);

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { io_context.stop(); });

        for (int i = 1; i <= max_client_count; ++i) {
            co_spawn(io_context, client_proc(i, io_context, host, port), detached);
        }

        co_spawn(io_context, print_proc(), detached);

        io_context.run();
    } catch (const std::exception& e) {
        std::cout << "exception:" << e.what() << std::endl;
    }

    return 0;
}