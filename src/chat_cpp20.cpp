#include <asio.hpp>
#include <set>
#include <deque>
#include <iostream>

#include "chat_msg.h"
#include "simple_log.h"

using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::redirect_error;
using asio::use_awaitable;
using asio::ip::tcp;

class chat_participant {
   public:
    int uid = 0;
    chat_participant() : uid(0) {}
    chat_participant(int uid) : uid(uid) {}
    virtual ~chat_participant() {}
    virtual void sendmsg(int fuid, ChatMsgId msgId, const std::string& msg) = 0;
};

using chat_participant_ptr = std::shared_ptr<chat_participant>;

class chat_room {
   public:
    void join(chat_participant_ptr participant) {
        participants_.insert(participant);
        for (auto& msg : recent_msgs_) {
            participant->sendmsg(msg.fuid, ChatMsgId::ChatAll, msg.msg);
        }
    }
    void leave(chat_participant_ptr participant) { participants_.erase(participant); }

    void broadcast(int fuid, ChatMsgId msgId, const std::string& msg) {
        ChatMsg cmsg;
        cmsg.fuid = fuid;
        cmsg.tuid = UID_ALL;
        cmsg.msg = msg;
        recent_msgs_.emplace_back(cmsg);
        while (recent_msgs_.size() > max_recent_msgs) {
            recent_msgs_.pop_front();
        }
        for (auto participant : participants_) {
            participant->sendmsg(cmsg.fuid, ChatMsgId::ChatAll, cmsg.msg);
        }
    }

   private:
    std::set<chat_participant_ptr> participants_;
    enum { max_recent_msgs = 30 };
    std::deque<ChatMsg> recent_msgs_;
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

    virtual void sendmsg(int fuid, ChatMsgId msgId, const std::string& msg) {
        ChatMsg cmsg;
        cmsg.fuid = fuid;
        cmsg.tuid = uid;
        cmsg.msgId = msgId;
        cmsg.msg = msg;
        // should we do serialize here, or pre send ???
        write_msgs_.emplace_back(cmsg);
        timer_.cancel_one();
    }
    void on_msg(const ChatMsg& msg) {
        ilog("on_msg fuid:", msg.fuid, " tuid:", msg.tuid, " msgid:", msg.msgId, " msg:", msg.msg);
    }

   private:
    awaitable<void> reader() {
        try {
            // std::string read_msg;
            for (;;) {
                std::size_t n = co_await asio::async_read(
                    socket_, asio::buffer(recvBuff + recvBuffPos, MsgBuffSize - recvBuffPos - 1), use_awaitable);
                if (n > 0) {
                    recvBuffPos += n;
                    // process msgs
                    int processedBufPos = 0;
                    while (true) {
                        if (processedBufPos + ChatMsgHead::size() > recvBuffPos) break;
                        auto mhead = (ChatMsgHead*)(recvBuff + processedBufPos);
                        if (processedBufPos + ChatMsgHead::size() + mhead->msgLen > recvBuffPos) break;
                        ChatMsg cmsg;
                        cmsg.fuid = mhead->uid;
                        cmsg.tuid = mhead->tuid;
                        cmsg.msgId = mhead->msgId;
                        cmsg.msg.resize(mhead->msgLen);
                        memcpy(cmsg.msg.data(), recvBuff + processedBufPos + ChatMsgHead::size());
                        this->on_msg(cmsg);
                    }

                    // rewind
                    if (processedBufPos > 0) {
                        memmove(recvBuff, recvBuff + processedBufPos, recvBuffPos - processedBufPos);
                        recvBuffPos -= processedBufPos;
                    }
                }

                // std::size_t n =
                //     co_await asio::async_read_until(socket_, asio::dynamic_buffer(read_msg, 1024), "\n",
                //     use_awaitable);
                // room_.deliver(read_msg);
                // read_msg.erase(0, n);
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
                    auto& msg = write_msgs_.front();
                    auto mhead = (ChatMsgHead*)(sendBuff);
                    mhead->uid = msg.tuid;
                    mhead->tuid = msg.fuid;
                    mhead->msgId = msg.msgId;
                    mhead->msgLen = (int)msg.msg.size();
                    if (ChatMsgHead::size() + msg.msg.size() > MsgBuffSize) {
                        elog("send buff size not enough. msgSize:", ChatMsgHead::size() + msg.msg.size(),
                             " MsgBuffSize:", MsgBuffSize);
                        write_msgs_.pop_front();
                        continue;
                    }
                    memcpy(sendBuff + ChatMsgHead::size(), msg.msg.data(), msg.msg.size());
                    co_await asio::async_write(socket_, asio::buffer(sendBuff), use_awaitable);
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
    std::deque<ChatMsg> write_msgs_;
    constexpr static int MsgBuffSize = 1024;
    char sendBuff[MsgBuffSize] = {0};
    char recvBuff[MsgBuffSize] = {0};
    int recvBuffPos = 0;
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