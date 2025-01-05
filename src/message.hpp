#pragma once

#include "buffers.hpp"
#include "types.hpp"

#include <batteries/assert.hpp>

#include <boost/asio/ip/udp.hpp>

#include <ostream>

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

constexpr usize kMaxMessageSize = 1500;
constexpr usize kPingMessageSize = 65535;

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

struct MessageFrame {
    little_u32 size;
    little_u32 src_ip;
    little_u32 dst_ip;
    little_u16 src_port;
    little_u16 dst_port;
};

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

struct UdpMessage {
    boost::asio::ip::udp::endpoint src;
    boost::asio::ip::udp::endpoint dst;
    CBuffers data;
};

inline std::ostream& operator<<(std::ostream& out, const UdpMessage& message)
{
    return out << BATT_INSPECT(message.data.size()) << BATT_INSPECT(message.src) << BATT_INSPECT(message.dst)
               << BATT_INSPECT_STR(message.data.collect_str());
}
