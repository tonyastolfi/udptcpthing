#pragma once

#include "buffers.hpp"
#include "types.hpp"

#include <boost/asio/ip/udp.hpp>

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

constexpr usize kMaxMessageSize = 1500;

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
