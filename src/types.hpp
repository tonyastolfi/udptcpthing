#pragma once

#include <batteries/int_types.hpp>

#include <boost/endian/arithmetic.hpp>

using namespace batt::int_types;

using big_u8 = boost::endian::big_uint8_t;
using big_u16 = boost::endian::big_uint16_t;
using big_u24 = boost::endian::big_uint24_t;
using big_u32 = boost::endian::big_uint32_t;
using big_u64 = boost::endian::big_uint64_t;

using big_i8 = boost::endian::big_int8_t;
using big_i16 = boost::endian::big_int16_t;
using big_i24 = boost::endian::big_int24_t;
using big_i32 = boost::endian::big_int32_t;
using big_i64 = boost::endian::big_int64_t;

using little_u8 = boost::endian::little_uint8_t;
using little_u16 = boost::endian::little_uint16_t;
using little_u24 = boost::endian::little_uint24_t;
using little_u32 = boost::endian::little_uint32_t;
using little_u64 = boost::endian::little_uint64_t;

using little_i8 = boost::endian::little_int8_t;
using little_i16 = boost::endian::little_int16_t;
using little_i24 = boost::endian::little_int24_t;
using little_i32 = boost::endian::little_int32_t;
using little_i64 = boost::endian::little_int64_t;
