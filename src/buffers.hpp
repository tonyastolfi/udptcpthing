#pragma once

#include "types.hpp"

#include <batteries/assert.hpp>
#include <batteries/buffer.hpp>
#include <batteries/small_vec.hpp>

#include <algorithm>
#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

constexpr usize kMemoryBlockSize = 64;
constexpr usize kMemoryBlockAlign = 8;

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

struct Memory {
    using Block = std::aligned_storage_t<kMemoryBlockSize, 8>;

    static std::shared_ptr<Memory> alloc(usize n) noexcept
    {
        const usize n_blocks = (n + kMemoryBlockSize - 1) / kMemoryBlockSize;
        auto memory = std::make_shared<Memory>();
        memory->storage.reset(new Block[n_blocks]);
        memory->size = n_blocks * kMemoryBlockSize;
        return memory;
    }

    std::unique_ptr<Block[]> storage;
    usize size;

    batt::MutableBuffer as_buffer() const noexcept
    {
        return batt::MutableBuffer{this->storage.get(), this->size};
    }
};

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

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

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

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
