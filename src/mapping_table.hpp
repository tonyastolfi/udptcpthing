#pragma once

class MappingTable
{
   public:
    explicit UdpPortMapper(batt::Queue<UdpMessage>& reply_queue, boost::asio::io_context& io,
                           const boost::asio::ip::udp::endpoint& target, usize max_ports,
                           CacheClock& clock) noexcept
        : reply_queue_{reply_queue}
        , io_{io}
        , target_{target}
        , max_ports_{max_ports}
        , cache_clock_{clock}
    {
    }

    struct Entry {
        batt::Queue<UdpMessage>& reply_queue;
        boost::asio::ip::udp::socket udp_sock;
        boost::asio::ip::udp::endpoint mapped_client;
        boost::asio::ip::udp::endpoint target;
        CacheClock& cache_clock;
        CacheClock::Time last_active = this->cache_clock.now_time;
        batt::Task reply_task;
        bool failed = false;

        explicit Entry(batt::Queue<UdpMessage>& reply_queue, boost::asio::io_context& io,
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
                if (buffers.size() < 1500) {
                    buffers.grow(1500);
                    BATT_CHECK_GE(buffers.size(), 1500);
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

    batt::Status send(const UdpMessage& message) noexcept
    {
        const u64 client_key = u64{message.src.address().to_v4().to_uint()} | (u64{message.src.port()} << 32);

        Entry* entry = nullptr;

        auto iter = this->mappings_.find(client_key);
        if (iter != this->mappings_.end()) {
            std::cerr << "Mapping found in hash table!  Verifying entry..." << std::endl;
            BATT_CHECK_LT(iter->second, this->entries_.size());

            entry = this->entries_[iter->second].get();
            std::cerr << BATT_INSPECT(entry->mapped_client) << BATT_INSPECT(message.src) << std::endl;

            if (entry && entry->mapped_client != message.src) {
                entry = nullptr;
            }
        }

        if (!entry) {
            std::cerr << "Allocating new entry..." << std::endl;
            usize slot_i = this->max_ports_;
            if (this->entries_.size() < this->max_ports_) {
                std::cerr << "Growing mapping table: " << this->entries_.size() << " -> "
                          << (this->entries_.size() + 1) << std::endl;
                slot_i = this->entries_.size();
                this->entries_.emplace_back();
            } else {
                std::cerr << "Table is full; finding LRU entry to evict..." << std::endl;
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
                std::cerr << BATT_INSPECT(lru_i) << "; evicting..." << std::endl;

                lru->get()->halt();
                lru->get()->join();
                lru->reset();

                slot_i = lru_i;
            }

            BATT_CHECK_LT(slot_i, this->entries_.size());

            this->entries_[slot_i] = std::make_unique<Entry>(this->reply_queue_, this->io_, message.src,
                                                             this->target_, this->cache_clock_);

            this->mappings_.emplace(client_key, slot_i);

            entry = this->entries_[slot_i].get();

            std::cerr << "Created new mapping at slot " << slot_i << ": " << message.src << " <-> "
                      << entry->udp_sock.local_endpoint() << std::endl;
        }

        BATT_CHECK_NOT_NULLPTR(entry);

        return entry->send(message);
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
    std::vector<std::unique_ptr<Entry>> entries_;
    std::unordered_map<u64, u64> mappings_;
};
