#pragma once

#include <batteries/async/task.hpp>

#include <chrono>

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

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
