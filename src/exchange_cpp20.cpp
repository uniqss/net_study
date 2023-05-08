#include <asio.hpp>
#include <set>
#include <deque>
#include <iostream>

#include "simple_log.h"
#include "exchange_msg.h"

using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::redirect_error;
using asio::use_awaitable;
using asio::ip::tcp;

class eclient_interface {
   public:
    virtual ~eclient_interface() = 0;
    virtual int64_t gold() = 0;
    virtual int64_t dimand() = 0;
    virtual bool gold2dimand(int64_t gold) = 0;
    virtual bool dimand2gold(int64_t dimand) = 0;
};

using eclient_interface_ptr = std::shared_ptr<eclient_interface>;

class exchange_server {
   public:
    void join(eclient_interface_ptr participant) { clients_.insert(participant); }
    void leave(eclient_interface_ptr participant) { clients_.erase(participant); }

    std::string exchange(eclient_interface_ptr client, const std::string& exchange) {
        int ret = -1;
        do {
            if (clients_.size() < 2) {  // not enough players
                break;
            }
            if (exchange[0] == 'd') {  // dimand => gold
                int64_t exchange_dimand = std::atoi(exchange.substr(1).c_str());
                if (exchange_dimand <= 0) break;
                int64_t exchange_gold = exchange_dimand * 10;
                if (client->dimand() < exchange_dimand) {
                    break;
                }
                for (auto other : clients_) {
                    if (other->gold() >= exchange_gold) {
                        if (!other->gold2dimand(exchange_gold)) continue;
                        if (!client->dimand2gold(exchange_dimand)) {
                            elog("this should not happen, gold will be not enough. exchange_gold:", exchange_gold);
                            break;
                        }
                        ret = 0;
                        break;
                    }
                }
            } else if (exchange[0] == 'g') {  // gold => dimand
                int64_t exchange_gold = std::atoi(exchange.substr(1).c_str());
                if (exchange_gold <= 0 || ((exchange_gold % 10) != 0)) break;
                int64_t exchange_dimand = exchange_gold / 10;
                if (client->gold() < exchange_gold) {
                    break;
                }
                for (auto other : clients_) {
                    if (other->dimand() >= exchange_dimand) {
                        if (!other->dimand2gold(exchange_dimand)) continue;
                        if (!client->gold2dimand(exchange_gold)) {
                            elog("this should not happen, dimand will be not enough. exchange_dimand:",
                                 exchange_dimand);
                            break;
                        }
                        ret = 0;
                        break;
                    }
                }
            } else {
                elog("exchange type error");
                return;
            }
            ret = 0;
        } while (false);
        std::string ret_str = exchange + "|" = std::to_string(ret);
        return ret_str;
    }

   private:
    std::set<eclient_interface_ptr> clients_;
};

class exchange_client : public eclient_interface, public std::enable_shared_from_this<exchange_client> {
   public:
    exchange_client(tcp::socket socket, exchange_server& room)
        : socket_(std::move(socket)), timer_(socket.get_executor()), server_(room), gold_(10000), dimand_(10000) {
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    void start() {
        server_.join(shared_from_this());

        co_spawn(
            socket_.get_executor(), [self = shared_from_this()] { return self->reader(); }, detached);

        co_spawn(
            socket_.get_executor(), [self = shared_from_this()] { return self->writer(); }, detached);
    }

    virtual void deliver(const std::string& msg) {
        write_msgs_.push_back(msg);
        timer_.cancel_one();
    }

    virtual int64_t gold() { return gold_; }
    virtual int64_t dimand() { return dimand_; }
    virtual bool gold2dimand(int64_t exchange) {
        if ((exchange % 10) != 0) return false;
        if (exchange < 0) return false;
        if (gold_ < exchange) return false;
        gold_ -= exchange;
        dimand_ += exchange / 10;

        this->deliver("sg" + std::to_string(gold_));
        this->deliver("sd" + std::to_string(dimand_));
        return true;
    }
    virtual bool dimand2gold(int64_t exchange) {
        if (exchange < 0) return false;
        if (dimand_ < exchange) return false;
        dimand_ -= exchange;
        gold_ += exchange * 10;

        this->deliver("sg" + std::to_string(gold_));
        this->deliver("sd" + std::to_string(dimand_));
        return true;
    }

   private:
    awaitable<void> reader() {
        try {
            std::string read_msg;
            for (;;) {
                stExchangeMsgHead msgHead;
                std::size_t n = co_await asio::async_read(socket_, asio::buffer(&msgHead, stExchangeMsgHead::size()),
                                                          use_awaitable);

                if (n != stExchangeMsgHead::size()) {
                    elog("read error");
                }

                
                co_await asio::async_read(socket_, asio::buffer())

                std::size_t n =
                    co_await asio::async_read_until(socket_, asio::dynamic_buffer(read_msg, 1024), "\n", use_awaitable);
                server_.exchange(read_msg);
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
        server_.leave(shared_from_this());
        socket_.close();
        timer_.cancel();
    }

   private:
    tcp::socket socket_;
    asio::steady_timer timer_;
    exchange_server& server_;
    std::deque<std::string> write_msgs_;
    int64_t gold_;
    int64_t dimand_;
};

awaitable<void> listener(tcp::acceptor acceptor) {
    exchange_server room;
    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(use_awaitable);
        std::shared_ptr<exchange_client> session = std::make_shared<exchange_client>(std::move(socket), room);
        session->start();
        // std::make_shared<exchange_client>(socket, room)->start();
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