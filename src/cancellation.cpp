#include <asio.hpp>
#include <asio/as_tuple.hpp>
#include <iostream>
#include <span>

using namespace asio;

int main() {
    io_context ioContext;
    cancellation_signal signal;

    const auto writeUntilCancellation = [&ioContext, &signal]() -> awaitable<void> {
        ip::tcp::acceptor acceptor(ioContext, ip::tcp::endpoint(ip::tcp::v4(), 1234));
        ip::tcp::socket socket(ioContext);
        co_await acceptor.async_accept(socket, use_awaitable);
        uint32_t counter = 0;
        for (;;) {
            std::vector<std::uint32_t> data(1024 * 1024);
            std::ranges::generate(data, [&counter]() { return counter++; });
            const auto [error, bytesTransfered] = co_await async_write(
                socket, buffer(data), bind_cancellation_slot(signal.slot(), as_tuple(use_awaitable)));
            if (error) {
                co_await this_coro::reset_cancellation_state();
                const auto remainingData = std::as_bytes(std::span(data)).subspan(bytesTransfered);
                co_await async_write(socket, buffer(std::data(remainingData), std::size(remainingData)), use_awaitable);
                throw std::system_error(error);
            }
        }
    };

    const auto readCreatingBackPressure = [&ioContext]() -> awaitable<void> {
        ip::tcp::socket socket(ioContext);
        co_await socket.async_connect(ip::tcp::endpoint(ip::address::from_string("127.0.0.1"), 1234), use_awaitable);
        uint32_t i = 0;
        for (;;) {
            std::vector<std::uint32_t> data(1024 * 1024);
            co_await async_read(socket, buffer(data), use_awaitable);
            for (const auto value : data) {
                if (value != i++) {
                    std::cerr << "mismatch at " << value << ", should be " << i - 1 << ".\n";
                    std::exit(EXIT_FAILURE);
                }
            }
            // throttle to produce back pressure on the TCP stream
            co_await steady_timer(co_await this_coro::executor, std::chrono::milliseconds(200))
                .async_wait(use_awaitable);
        }
    };

    const auto cancelWhenBackpressureIsEstablished = [&signal]() -> awaitable<void> {
        // wait for back pressure to establish on the TCP stream
        co_await steady_timer(co_await this_coro::executor, std::chrono::seconds(2)).async_wait(use_awaitable);

        signal.emit(cancellation_type::partial);

        // wait for the program to continue without error
        co_await steady_timer(co_await this_coro::executor, std::chrono::seconds(2)).async_wait(use_awaitable);

        std::cout << "success" << std::endl;
        std::exit(EXIT_SUCCESS);
    };

    co_spawn(ioContext, writeUntilCancellation, detached);
    co_spawn(ioContext, readCreatingBackPressure, detached);
    co_spawn(ioContext, cancelWhenBackpressureIsEstablished, detached);
    ioContext.run();
}
