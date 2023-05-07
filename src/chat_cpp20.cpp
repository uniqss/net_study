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

awaitable<void> listener(tcp::acceptor acceptor) {
    chat_room room;
    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(use_awaitable);
        std::shared_ptr<chat_session> session = std::make_shared<chat_session>(std::move(socket), room);
        session->start();
        // std::make_shared<chat_session>(socket, room)->start();
    }
}

int main(int argc, const char** argv) {
    try {
        asio::io_context ctx(1);

        std::string host = "0.0.0.0";
        std::vector<unsigned short> ports;
        if (argc < 2) {
            ports = {8888};
        } else {
            for (int i = 1; i < argc; ++i) {
                unsigned short port = std::atoi(argv[i]);
                ports.push_back(port);
            }
        }

        for (auto port : ports) {
            tcp::endpoint ep(tcp::v4(), port);
            tcp::acceptor acceptor(ctx, ep);
            co_spawn(ctx, listener(std::move(acceptor)), detached);
            // co_spawn(ctx, listener(tcp::acceptor(ctx, {tcp::v4(), port})), detached);
        }

        asio::signal_set signals(ctx, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { ctx.stop(); });

        ctx.run();
    } catch (const std::exception& e) {
        std::cout << "exception:" << e.what() << std::endl;
    }

    return 0;
}