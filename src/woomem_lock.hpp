#pragma once

#include <atomic>

namespace woomem
{
    class Spinlock
    {
    public:
        Spinlock() = default;

        Spinlock(const Spinlock&) = delete;
        Spinlock& operator=(const Spinlock&) = delete;

        void lock()
        {
            while (flag_.test_and_set(std::memory_order_acquire))
                ;
        }

        void unlock()
        {
            flag_.clear(std::memory_order_release);
        }

        bool try_lock()
        {
            return !flag_.test_and_set(std::memory_order_acquire);
        }

    private:
        std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    };
}
