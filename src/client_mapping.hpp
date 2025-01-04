#pragma once

#include "clock.hpp"
#include "message.hpp"

#include <batteries/assert.hpp>
#include <batteries/async/io_result.hpp>
#include <batteries/async/queue.hpp>
#include <batteries/async/task.hpp>
#include <batteries/status.hpp>
#include <batteries/utility.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

class ClientMapping
{
   public:
    batt::Queue<UdpMessage>& reply_queue;
    boost::asio::ip::udp::socket udp_sock;
    boost::asio::ip::udp::endpoint mapped_client;
    boost::asio::ip::udp::endpoint target;
    CacheClock& cache_clock;
    CacheClock::Time last_active = this->cache_clock.now_time;
    batt::Task reply_task;
    bool failed = false;

    //+++++++++++-+-+--+----- --- -- -  -  -   -

    explicit ClientMapping(batt::Queue<UdpMessage>& reply_queue, boost::asio::io_context& io,
                           const boost::asio::ip::udp::endpoint& mapped_client,
                           const boost::asio::ip::udp::endpoint& target, CacheClock& cache_clock) noexcept
        : reply_queue{reply_queue}
        , udp_sock{io, boost::asio::ip::udp::endpoint{boost::asio::ip::udp::v4(), 0}}
        , mapped_client{mapped_client}
        , target{target}
        , cache_clock{cache_clock}
        , reply_task{io.get_executor(), [this] {
                         this->run();
                     }}
    {
    }

    void halt() noexcept
    {
        batt::ErrorCode ec;
        this->udp_sock.close(ec);
        (void)ec;
    }

    void join() noexcept
    {
        this->reply_task.join();
    }

    void run() noexcept
    {
        MBuffers buffers;
        boost::asio::ip::udp::endpoint sender;

        for (;;) {
            if (buffers.size() < kMaxMessageSize) {
                buffers.grow(kMaxMessageSize);
                BATT_CHECK_GE(buffers.size(), kMaxMessageSize);
            }

            batt::IOResult<usize> n_read = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
                this->udp_sock.async_receive_from(buffers.buffers, sender, BATT_FORWARD(handler));
            });

            if (!n_read.ok()) {
                this->failed = true;
                return;
            }
            this->last_active = this->cache_clock.now_time;

            UdpMessage message;
            message.src = sender;
            message.dst = this->mapped_client;
            message.data = buffers.take(*n_read).freeze();

            BATT_CHECK_OK(this->reply_queue.push(std::move(message)));

            this->last_active = this->cache_clock.now_time;
        }
    }

    batt::Status send(const UdpMessage& message) noexcept
    {
        BATT_CHECK_EQ(message.src, this->mapped_client);

        this->last_active = this->cache_clock.now_time;

        batt::IOResult<usize> n_sent = batt::Task::await<batt::IOResult<usize>>([&](auto&& handler) {
            this->udp_sock.async_send_to(message.data.buffers, this->target, BATT_FORWARD(handler));
        });

        BATT_REQUIRE_OK(n_sent);
        BATT_CHECK_EQ(*n_sent, message.data.size());

        this->last_active = this->cache_clock.now_time;

        return batt::OkStatus();
    }
};
