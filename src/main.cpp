#include "types.hpp"

#include <batteries/assert.hpp>
#include <batteries/async/io_result.hpp>
#include <batteries/async/queue.hpp>
#include <batteries/async/task.hpp>
#include <batteries/buffer.hpp>
#include <batteries/env.hpp>
#include <batteries/small_vec.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

struct TcpFrame {
    little_u32 size;
    little_u32 src_ip;
    little_u32 dst_ip;
    little_u16 src_port;
    little_u16 dst_port;
};

struct Memory {
    using Block = std::aligned_storage_t<64, 8>;

    static std::shared_ptr<Memory> alloc(usize n) noexcept
    {
        const usize blocks = (n + 63) / 64;
        auto memory = std::make_shared<Memory>();
        memory->storage.reset(new Block[blocks]);
        memory->size = blocks * 64;
        return memory;
    }

    std::unique_ptr<Block[]> storage;
    usize size;

    batt::MutableBuffer as_buffer() const noexcept
    {
        return batt::MutableBuffer{this->storage.get(), this->size};
    }
};

template <typename T>
struct BufferImpl {
    T buffer;
    std::shared_ptr<Memory> memory;

    usize size() const noexcept
    {
        return this->buffer.size();
    }

    bool empty() const noexcept
    {
        return this->size() == 0;
    }

    operator T() const noexcept
    {
        return this->buffer;
    }

    BufferImpl take(usize n) noexcept
    {
        BATT_CHECK_LE(n, this->size());

        BufferImpl result;
        result.buffer = T{this->buffer.data(), n};
        result.memory = this->memory;

        this->buffer += n;

        return result;
    }
};

using CBuffer = BufferImpl<batt::ConstBuffer>;
using MBuffer = BufferImpl<batt::MutableBuffer>;

template <typename T>
struct BuffersImpl {
    batt::SmallVec<BufferImpl<T>, 2> buffers;

    usize size() const noexcept
    {
        usize n = 0;
        for (const auto& b : this->buffers) {
            n += b.size();
        }
        return n;
    }

    void grow(usize n) noexcept
    {
        auto memory = Memory::alloc(n);
        BufferImpl<T> b;
        b.buffer = memory->as_buffer();
        b.memory = std::move(memory);
        this->buffers.emplace_back(std::move(b));
    }

    BuffersImpl take(usize n) noexcept
    {
        const usize orig_n = n;

        BATT_CHECK_LE(n, this->size());

        BuffersImpl result;

        while (n > 0) {
            const usize m = std::min(n, this->buffers.front().size());
            result.buffers.emplace_back(this->buffers.front().take(m));
            n -= m;
            BATT_CHECK_LT(n, orig_n);
            if (this->buffers.front().empty()) {
                this->buffers.erase(this->buffers.begin());
            } else {
                BATT_CHECK_EQ(n, 0);
            }
        }

        BATT_CHECK_EQ(result.size(), orig_n);

        return result;
    }

    BuffersImpl<batt::ConstBuffer> freeze() const noexcept
    {
        BuffersImpl<batt::ConstBuffer> result;

        for (const auto& b : this->buffers) {
            CBuffer cb;
            cb.buffer = b.buffer;
            cb.memory = b.memory;
            result.buffers.emplace_back(std::move(cb));
        }

        return result;
    }

    std::string collect_str() const noexcept
    {
        std::ostringstream oss;
        for (const auto& b : this->buffers) {
            oss.write((const char*)b.buffer.data(), b.buffer.size());
        }
        return std::move(oss).str();
    }
};

using CBuffers = BuffersImpl<batt::ConstBuffer>;
using MBuffers = BuffersImpl<batt::MutableBuffer>;

struct UdpMessage {
    boost::asio::ip::udp::endpoint src;
    boost::asio::ip::udp::endpoint dst;
    CBuffers data;
};

struct CacheClock {
    using Time = std::chrono::steady_clock::time_point;

    Time now_time = std::chrono::steady_clock::now();

    void run()
    {
        for (;;) {
            this->now_time = std::chrono::steady_clock::now();
            batt::Task::sleep(boost::posix_time::seconds(1));
        }
    }
};

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
// Task functions

void udp_input(boost::asio::io_context& io);
void tcp_input(boost::asio::io_context& io);

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
// Main

int main()
{
    const bool front_mode = batt::getenv_as<bool>("FRONT").value_or(true);

    boost::asio::io_context io;

    if (front_mode) {
        batt::Task udp_front{io.get_executor(), [&] {
                                 udp_input(io);
                             }};

        io.run();
    } else {
        batt::Task tcp_front{io.get_executor(), [&] {
                                 tcp_input(io);
                             }};

        io.run();
    }

    return 0;
}

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
// Impls

void udp_receiver(boost::asio::ip::udp::socket& udp_sock, batt::Queue<UdpMessage>& queue);
void tcp_receiver(boost::asio::ip::tcp::socket& tcp_sock, batt::Queue<UdpMessage>& queue);

void tcp_connect(bool& connected, boost::asio::ip::tcp::socket& tcp_sock,
                 const boost::asio::ip::tcp::endpoint& ep);

void udp_sender(batt::Queue<UdpMessage>& queue, boost::asio::ip::udp::socket& udp_sock);
void tcp_sender(batt::Queue<UdpMessage>& queue, boost::asio::ip::tcp::socket& tcp_sock);

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void udp_input(boost::asio::io_context& io)
{
    const u16 udp_port = batt::getenv_as<u16>("UDP_PORT").value_or(19132);
    const u16 tcp_port = batt::getenv_as<u16>("TCP_PORT").value_or(7777);

    boost::asio::ip::udp::endpoint udp_ep{boost::asio::ip::udp::v4(), udp_port};
    boost::asio::ip::tcp::endpoint tcp_ep{boost::asio::ip::address_v4::loopback(), tcp_port};

    boost::asio::ip::udp::socket udp_srv_sock{io, udp_ep};
    boost::asio::ip::tcp::socket tcp_cli_sock{io};

    batt::Queue<UdpMessage> from_udp_queue;
    batt::Queue<UdpMessage> from_tcp_queue;

    batt::Task udp_to_queue_task{
        io.get_executor(),
        [&] {
            udp_receiver(udp_srv_sock, from_udp_queue);
        },
    };

    batt::Task queue_to_tcp_task{
        io.get_executor(),
        [&] {
            bool connected = false;
            tcp_connect(connected, tcp_cli_sock, tcp_ep);
            if (connected) {
                batt::Task::spawn(  //
                    io.get_executor(), [&] {
                        tcp_sender(from_udp_queue, tcp_cli_sock);
                    });

                batt::Task::spawn(  //
                    io.get_executor(), [&] {
                        tcp_receiver(tcp_cli_sock, from_tcp_queue);
                    });
            }
        },
    };

    batt::Task queue_to_udp_task{
        io.get_executor(),
        [&] {
            udp_sender(from_tcp_queue, udp_srv_sock);
        },
    };

    udp_to_queue_task.join();
    queue_to_tcp_task.join();
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void tcp_connect(bool& connected, boost::asio::ip::tcp::socket& tcp_sock,
                 const boost::asio::ip::tcp::endpoint& ep)
{
    while (!connected) {
        std::cerr << "Connecting to " << ep << std::endl;
        batt::ErrorCode ec = batt::Task::await<batt::ErrorCode>([&](auto&& handler) {
            tcp_sock.async_connect(ep, handler);
        });
        if (ec) {
            std::cerr << "Failed to connect; retrying after delay..." << std::endl;
            batt::Task::sleep(boost::posix_time::seconds(1));
            std::cerr << "Retrying..." << std::endl;
            continue;
        } else {
            std::cerr << "Connected!" << std::endl;
            connected = true;
            break;
        }
    }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void tcp_input(boost::asio::io_context& io)
{
    const u16 udp_port = batt::getenv_as<u16>("UDP_PORT").value_or(19132);
    const u16 tcp_port = batt::getenv_as<u16>("TCP_PORT").value_or(7777);

    boost::asio::ip::tcp::endpoint tcp_ep{boost::asio::ip::tcp::v4(), tcp_port};
    boost::asio::ip::tcp::acceptor tcp_acceptor{io, tcp_ep};

    boost::asio::ip::udp::endpoint udp_ep{boost::asio::ip::address_v4::loopback(), udp_port};

    batt::Queue<UdpMessage> input_queue;

    CacheClock clock;

    batt::Task clock_task{io.get_executor(), [&] {
                              clock.run();
                          }};

    batt::Task accept_task{
        io.get_executor(), [&] {
            std::cerr << "Listening on " << tcp_ep << std::endl;

            for (;;) {
                auto tcp_sock =
                    batt::Task::await<batt::IOResult<boost::asio::ip::tcp::socket>>([&](auto&& handler) {
                        tcp_acceptor.async_accept(BATT_FORWARD(handler));
                    });

                BATT_CHECK_OK(tcp_sock);

                std::cerr << "Accepted connection from " << tcp_sock->remote_endpoint() << std::endl;

                auto shared_tcp_sock = std::make_shared<boost::asio::ip::tcp::socket>(std::move(*tcp_sock));

                batt::Task::spawn(io.get_executor(), [&input_queue, shared_tcp_sock]() {
                    tcp_receiver(*shared_tcp_sock, input_queue);
                });

                auto shared_output_queue = std::make_shared<batt::Queue<UdpMessage>>();

                batt::Task::spawn(  //
                    io.get_executor(), [&input_queue, shared_output_queue, &io, udp_ep, &clock] {
                        UdpPortMapper mapper{*shared_output_queue, io, udp_ep, /*max_ports=*/16, clock};
                        auto on_scope_exit = batt::finally([&] {
                            mapper.halt();
                            mapper.join();
                            shared_output_queue->close();
                        });

                        for (;;) {
                            batt::StatusOr<UdpMessage> message = input_queue.await_next();
                            if (!message.ok()) {
                                break;
                            }

                            batt::Status send_status = mapper.send(*message);
                            if (!send_status.ok()) {
                                break;
                            }
                        }
                    });

                batt::Task::spawn(  //
                    io.get_executor(), [shared_output_queue, shared_tcp_sock] {
                        tcp_sender(*shared_output_queue, *shared_tcp_sock);
                    });
            }
        }};

    accept_task.join();
    clock_task.join();
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void udp_receiver(boost::asio::ip::udp::socket& udp_srv_sock, batt::Queue<UdpMessage>& queue)
{
    MBuffers buffers;
    for (;;) {
        if (buffers.size() < 1500) {
            buffers.grow(1500);
            BATT_CHECK_GE(buffers.size(), 1500);
        }

        boost::asio::ip::udp::endpoint sender;

        auto io_result = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            udp_srv_sock.async_receive_from(buffers.buffers, sender, BATT_FORWARD(handler));
        });

        BATT_CHECK_OK(io_result);

        UdpMessage message;
        message.src = sender;
        message.dst = udp_srv_sock.local_endpoint();
        message.data = buffers.take(*io_result).freeze();

        BATT_CHECK_OK(queue.push(std::move(message)));
    }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void udp_sender(batt::Queue<UdpMessage>& queue, boost::asio::ip::udp::socket& udp_sock)
{
    for (;;) {
        std::cerr << "Waiting for UDP reply message..." << std::endl;

        batt::StatusOr<UdpMessage> message = queue.await_next();
        if (!message.ok()) {
            break;
        }

        std::cerr << "UDP reply ready to send; sending..." << std::endl;

        batt::IOResult<usize> n_sent = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            udp_sock.async_send_to(message->data.buffers, message->dst, BATT_FORWARD(handler));
        });
        if (!n_sent.ok()) {
            break;
        }

        std::cerr << "UDP reply sent (" << message->data.size() << " bytes)" << std::endl;
    }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void tcp_sender(batt::Queue<UdpMessage>& queue, boost::asio::ip::tcp::socket& tcp_sock)
{
    for (;;) {
        std::cerr << "Waiting for message" << std::endl;

        batt::StatusOr<UdpMessage> message = queue.await_next();
        if (!message.ok()) {
            return;
        }

        std::cerr << "Message to send: " << BATT_INSPECT(message->data.size()) << BATT_INSPECT(message->src)
                  << BATT_INSPECT(message->dst) << BATT_INSPECT_STR(message->data.collect_str()) << std::endl;

        TcpFrame frame;
        frame.size = message->data.size();
        frame.src_ip = message->src.address().to_v4().to_uint();
        frame.dst_ip = message->dst.address().to_v4().to_uint();
        frame.src_port = message->src.port();
        frame.dst_port = message->dst.port();

        batt::IOResult<usize> io_result = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            boost::asio::async_write(tcp_sock, batt::ConstBuffer{&frame, sizeof(frame)},
                                     BATT_FORWARD(handler));
        });
        if (!io_result.ok()) {
            return;
        }

        std::cerr << "Sent header" << std::endl;

        io_result = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            boost::asio::async_write(tcp_sock, message->data.buffers, BATT_FORWARD(handler));
        });
        if (!io_result.ok()) {
            return;
        }

        std::cerr << "Sent data (" << *io_result << " bytes)" << std::endl;
    }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void tcp_receiver(boost::asio::ip::tcp::socket& tcp_sock, batt::Queue<UdpMessage>& queue)
{
    MBuffers buffers;

    for (;;) {
        TcpFrame frame;

        std::cerr << "Receiving header" << std::endl;

        batt::IOResult<usize> io_result = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            boost::asio::async_read(tcp_sock, batt::MutableBuffer{&frame, sizeof(frame)},
                                    BATT_FORWARD(handler));
        });

        if (!io_result.ok()) {
            return;
        }

        BATT_CHECK_EQ(*io_result, sizeof(frame));

        BATT_CHECK_LE(frame.size, 1500);
        if (buffers.size() < 1500) {
            buffers.grow(1500);
            BATT_CHECK_GE(buffers.size(), 1500);
        }

        UdpMessage message;

        MBuffers data_buffer = buffers.take(frame.size);

        message.data = data_buffer.freeze();

        message.src = boost::asio::ip::udp::endpoint{boost::asio::ip::address_v4{frame.src_ip.value()},
                                                     frame.src_port.value()};
        message.dst = boost::asio::ip::udp::endpoint{boost::asio::ip::address_v4{frame.dst_ip.value()},
                                                     frame.dst_port.value()};

        std::cerr << "Receiving data;" << BATT_INSPECT(frame.size) << std::endl;

        io_result = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            boost::asio::async_read(tcp_sock, data_buffer.buffers, BATT_FORWARD(handler));
        });

        std::cerr << BATT_INSPECT(io_result) << std::endl;

        if (!io_result.ok()) {
            return;
        }

        std::cerr << "Message received;" << BATT_INSPECT(message.data.size()) << BATT_INSPECT(message.src)
                  << BATT_INSPECT(message.dst) << BATT_INSPECT_STR(message.data.collect_str()) << std::endl;

        if (!queue.push(std::move(message)).ok()) {
            return;
        }
    }
}
