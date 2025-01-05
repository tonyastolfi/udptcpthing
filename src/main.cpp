#include "buffers.hpp"
#include "clock.hpp"
#include "help.hpp"
#include "mapping_table.hpp"
#include "message.hpp"
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

#include <glog/logging.h>

#include <chrono>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
// Task functions

void udp2tcp(boost::asio::io_context& io);
void tcp2udp(boost::asio::io_context& io);

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
// Main

int main()
{
    if (batt::getenv_as<bool>("HELP").value_or(false)) {
        show_help();
        return 0;
    }

    auto mode = batt::getenv_as<std::string>("MODE");

    if (!mode) {
        show_help();
        return 1;
    }

    boost::asio::io_context io;

    if (*mode == "udp2tcp") {
        batt::Task udp2tcp_task{io.get_executor(), [&] {
                                    udp2tcp(io);
                                }};

        io.run();
    } else {
        BATT_CHECK_EQ(*mode, "tcp2udp");

        batt::Task tcp2udp_task{io.get_executor(), [&] {
                                    tcp2udp(io);
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
void udp2tcp(boost::asio::io_context& io)
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

    auto queue_to_tcp_fn = [&] {
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
    };

    batt::Task queue_to_udp_task{
        io.get_executor(),
        [&] {
            udp_sender(from_tcp_queue, udp_srv_sock);
        },
    };

    for (;;) {
        batt::Task queue_to_tcp_task{
            io.get_executor(),
            queue_to_tcp_fn,
        };
        queue_to_tcp_task.join();

        LOG(INFO) << "TCP connection task exited";

        batt::ErrorCode ec;
        tcp_cli_sock.close(ec);

        LOG(INFO) << "TCP connection closed; re-connecting...";
    }

    udp_to_queue_task.join();
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void tcp2udp(boost::asio::io_context& io)
{
    const u16 udp_port = batt::getenv_as<u16>("UDP_PORT").value_or(19132);
    const u16 tcp_port = batt::getenv_as<u16>("TCP_PORT").value_or(7777);

    const usize max_clients = batt::getenv_as<usize>("MAX_CLIENTS").value_or(16);

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
            LOG(INFO) << "Listening on " << tcp_ep;

            for (;;) {
                auto tcp_sock =
                    batt::Task::await<batt::IOResult<boost::asio::ip::tcp::socket>>([&](auto&& handler) {
                        tcp_acceptor.async_accept(BATT_FORWARD(handler));
                    });

                BATT_CHECK_OK(tcp_sock);

                LOG(INFO) << "Accepted connection from " << tcp_sock->remote_endpoint() << std::endl;

                auto shared_tcp_sock = std::make_shared<boost::asio::ip::tcp::socket>(std::move(*tcp_sock));

                batt::Task::spawn(io.get_executor(), [&input_queue, shared_tcp_sock]() {
                    tcp_receiver(*shared_tcp_sock, input_queue);
                });

                auto shared_output_queue = std::make_shared<batt::Queue<UdpMessage>>();

                batt::Task::spawn(  //
                    io.get_executor(), [&input_queue, shared_output_queue, &io, udp_ep, &clock, max_clients] {
                        MappingTable mapper{*shared_output_queue, io, udp_ep, max_clients, clock};

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

                            batt::Status send_status = mapper.lookup(*message)->send(*message);
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
void tcp_connect(bool& connected, boost::asio::ip::tcp::socket& tcp_sock,
                 const boost::asio::ip::tcp::endpoint& ep)
{
    while (!connected) {
        LOG(INFO) << "Connecting to " << ep << std::endl;
        batt::ErrorCode ec = batt::Task::await<batt::ErrorCode>([&](auto&& handler) {
            tcp_sock.async_connect(ep, handler);
        });
        if (ec) {
            LOG(ERROR) << "Failed to connect; retrying after delay..." << std::endl;
            batt::Task::sleep(boost::posix_time::seconds(1));
            LOG(INFO) << "Retrying..." << std::endl;
            continue;
        } else {
            LOG(INFO) << "Connected!" << std::endl;
            connected = true;
            break;
        }
    }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void udp_receiver(boost::asio::ip::udp::socket& udp_srv_sock, batt::Queue<UdpMessage>& queue)
{
    MBuffers buffers;
    for (;;) {
        if (buffers.size() < kMaxMessageSize) {
            buffers.grow(kMaxMessageSize);
            BATT_CHECK_GE(buffers.size(), kMaxMessageSize);
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
        VLOG(2) << "Waiting for UDP reply message...";

        batt::StatusOr<UdpMessage> message = queue.await_next();
        if (!message.ok()) {
            break;
        }

        VLOG(2) << "UDP reply ready to send; sending...";

        batt::IOResult<usize> n_sent = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            udp_sock.async_send_to(message->data.buffers, message->dst, BATT_FORWARD(handler));
        });
        if (!n_sent.ok()) {
            break;
        }

        VLOG(2) << "UDP reply sent (" << message->data.size() << " bytes)";
    }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void tcp_sender(batt::Queue<UdpMessage>& queue, boost::asio::ip::tcp::socket& tcp_sock)
{
    bool alive = true;

    batt::Task keep_alive_task{
        tcp_sock.get_executor(),
        [&] {
            std::default_random_engine rng{std::random_device{}()};
            std::uniform_int_distribution<i64> pick_delay_ms{1500, 4500};
            while (alive) {
                MessageFrame frame;
                std::memset(&frame, 0, sizeof(frame));
                frame.size = kPingMessageSize;

                batt::IOResult<usize> io_result =
                    batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
                        boost::asio::async_write(tcp_sock, batt::ConstBuffer{&frame, sizeof(frame)},
                                                 BATT_FORWARD(handler));
                    });

                if (!io_result.ok()) {
                    LOG(WARNING) << "Failed to send ping: " << io_result;
                    batt::ErrorCode ec;
                    tcp_sock.close(ec);
                    alive = false;
                    break;
                }

                const i64 delay_ms = pick_delay_ms(rng);
                const batt::ErrorCode ec = batt::Task::sleep(boost::posix_time::milliseconds(delay_ms));
                if (ec) {
                    BATT_CHECK(!alive);
                    break;
                }
            }
        },
    };

    auto on_scope_exit = batt::finally([&] {
        alive = false;
        keep_alive_task.wake();
        keep_alive_task.join();
    });

    for (;;) {
        VLOG(2) << "Waiting for message";

        batt::StatusOr<UdpMessage> message = queue.await_next();
        if (!message.ok()) {
            return;
        }

        VLOG(2) << "Message to send: " << *message;

        MessageFrame frame;
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

        VLOG(2) << "Sent header";

        io_result = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            boost::asio::async_write(tcp_sock, message->data.buffers, BATT_FORWARD(handler));
        });
        if (!io_result.ok()) {
            return;
        }

        VLOG(2) << "Sent data (" << *io_result << " bytes)";
    }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
void tcp_receiver(boost::asio::ip::tcp::socket& tcp_sock, batt::Queue<UdpMessage>& queue)
{
    MBuffers buffers;

    usize ping_count = 0;

    for (;;) {
        MessageFrame frame;

        VLOG(2) << "Receiving header";

        batt::IOResult<usize> io_result = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            boost::asio::async_read(tcp_sock, batt::MutableBuffer{&frame, sizeof(frame)},
                                    BATT_FORWARD(handler));
        });

        if (!io_result.ok()) {
            return;
        }

        BATT_CHECK_EQ(*io_result, sizeof(frame));

        // Special value used to indicate this is a "ping" keep-alive message.  Ignore.
        //
        if (frame.size == kPingMessageSize) {
            ping_count += 1;
            LOG_EVERY_N(INFO, 10) << "Received keep-alive ping;" << BATT_INSPECT(ping_count);
            continue;
        }

        BATT_CHECK_LE(frame.size, kMaxMessageSize);
        if (buffers.size() < kMaxMessageSize) {
            buffers.grow(kMaxMessageSize);
            BATT_CHECK_GE(buffers.size(), kMaxMessageSize);
        }

        UdpMessage message;

        MBuffers data_buffer = buffers.take(frame.size);

        message.data = data_buffer.freeze();

        message.src = boost::asio::ip::udp::endpoint{boost::asio::ip::address_v4{frame.src_ip.value()},
                                                     frame.src_port.value()};
        message.dst = boost::asio::ip::udp::endpoint{boost::asio::ip::address_v4{frame.dst_ip.value()},
                                                     frame.dst_port.value()};

        VLOG(2) << "Receiving data;" << BATT_INSPECT(frame.size);

        io_result = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            boost::asio::async_read(tcp_sock, data_buffer.buffers, BATT_FORWARD(handler));
        });

        VLOG(2) << BATT_INSPECT(io_result);

        if (!io_result.ok()) {
            return;
        }

        VLOG(2) << "Message received;" << message;

        if (!queue.push(std::move(message)).ok()) {
            return;
        }
    }
}
