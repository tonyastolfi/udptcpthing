#pragma once

#include "client_mapping.hpp"
#include "message.hpp"
#include "types.hpp"

#include <batteries/assert.hpp>
#include <batteries/async/queue.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

class MappingTable
{
   public:
    explicit MappingTable(batt::Queue<UdpMessage>& reply_queue, boost::asio::io_context& io,
                          const boost::asio::ip::udp::endpoint& target, usize max_ports,
                          CacheClock& clock) noexcept
        : reply_queue_{reply_queue}
        , io_{io}
        , target_{target}
        , max_ports_{max_ports}
        , cache_clock_{clock}
    {
    }

    ClientMapping* lookup(const UdpMessage& message) noexcept
    {
        const u64 client_key = u64{message.src.address().to_v4().to_uint()} | (u64{message.src.port()} << 32);

        ClientMapping* entry = nullptr;

        auto iter = this->mappings_.find(client_key);
        if (iter != this->mappings_.end()) {
            VLOG(2) << "Mapping found in hash table!  Verifying entry...";
            BATT_CHECK_LT(iter->second, this->entries_.size());

            entry = this->entries_[iter->second].get();
            VLOG(2) << BATT_INSPECT(entry->mapped_client) << BATT_INSPECT(message.src);

            if (entry && entry->mapped_client != message.src) {
                VLOG(1) << "Expired mapping found; removing...";
                this->mappings_.erase(iter);
                entry = nullptr;
            }
        }

        if (!entry) {
            VLOG(1) << "Allocating new entry...";
            usize slot_i = this->max_ports_;
            if (this->entries_.size() < this->max_ports_) {
                VLOG(1) << "Growing mapping table: " << this->entries_.size() << " -> "
                        << (this->entries_.size() + 1);
                slot_i = this->entries_.size();
                this->entries_.emplace_back();
            } else {
                VLOG(1) << "Table is full; finding LRU entry to evict...";
                auto lru = std::min_element(this->entries_.begin(), this->entries_.end(),
                                            [](const auto& l, const auto& r) -> bool {
                                                if (!l) {
                                                    return !!r;
                                                }
                                                if (!r) {
                                                    return false;
                                                }
                                                return l->last_active < r->last_active;
                                            });
                BATT_CHECK_NE(lru, this->entries_.end());

                const usize lru_i = std::distance(this->entries_.begin(), lru);
                VLOG(1) << BATT_INSPECT(lru_i) << "; evicting...";

                lru->get()->halt();
                lru->get()->join();
                lru->reset();

                slot_i = lru_i;
            }

            BATT_CHECK_LT(slot_i, this->entries_.size());

            this->entries_[slot_i] = std::make_unique<ClientMapping>(
                this->reply_queue_, this->io_, message.src, this->target_, this->cache_clock_);

            this->mappings_.emplace(client_key, slot_i);

            entry = this->entries_[slot_i].get();

            VLOG(1) << "Created new mapping at slot " << slot_i << ": " << message.src << " <-> "
                    << entry->udp_sock.local_endpoint();
        }

        BATT_CHECK_NOT_NULLPTR(entry);

        return entry;
    }

    void halt() noexcept
    {
        for (auto& entry : this->entries_) {
            if (entry) {
                entry->halt();
            }
        }
    }

    void join() noexcept
    {
        for (auto& entry : this->entries_) {
            if (entry) {
                entry->join();
                entry = nullptr;
            }
        }
    }

    batt::Queue<UdpMessage>& reply_queue_;
    boost::asio::io_context& io_;
    boost::asio::ip::udp::endpoint target_;
    usize max_ports_;
    CacheClock& cache_clock_;
    std::vector<std::unique_ptr<ClientMapping>> entries_;
    std::unordered_map<u64, u64> mappings_;
};
