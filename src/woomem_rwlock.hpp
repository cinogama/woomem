#pragma once

#include <atomic>
#include <cstdint>

namespace woomem
{
    class ReadWriteSpinlock
    {
    public:
        ReadWriteSpinlock() = default;

        ReadWriteSpinlock(const ReadWriteSpinlock&) = delete;
        ReadWriteSpinlock& operator=(const ReadWriteSpinlock&) = delete;

        void lock_read()
        {
            uint32_t state = state_.load(std::memory_order_relaxed);
            for (;;)
            {
                while (state & WRITE_LOCKED)
                {
                    state = state_.load(std::memory_order_acquire);
                }
                uint32_t desired = (state & ~WRITE_LOCKED) + READER_ONE;
                if (state_.compare_exchange_weak(
                    state, desired,
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
                {
                    return;
                }
            }
        }

        void unlock_read()
        {
            state_.fetch_sub(READER_ONE, std::memory_order_release);
        }

        void lock_write()
        {
            uint32_t state = state_.load(std::memory_order_relaxed);
            for (;;)
            {
                while (state & (WRITE_LOCKED | READERS_MASK))
                {
                    state = state_.load(std::memory_order_acquire);
                }
                uint32_t desired = state | WRITE_LOCKED;
                if (state_.compare_exchange_weak(
                    state, desired,
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
                {
                    return;
                }
            }
        }

        void unlock_write()
        {
            state_.fetch_and(~WRITE_LOCKED, std::memory_order_release);
        }

        class ReadGuard
        {
        public:
            explicit ReadGuard(ReadWriteSpinlock& rw) : rw_(&rw)
            {
                rw_->lock_read();
            }
            ~ReadGuard() { rw_->unlock_read(); }
            ReadGuard(const ReadGuard&) = delete;
            ReadGuard& operator=(const ReadGuard&) = delete;
        private:
            ReadWriteSpinlock* rw_;
        };

        class WriteGuard
        {
        public:
            explicit WriteGuard(ReadWriteSpinlock& rw) : rw_(&rw)
            {
                rw_->lock_write();
            }
            ~WriteGuard() { rw_->unlock_write(); }
            WriteGuard(const WriteGuard&) = delete;
            WriteGuard& operator=(const WriteGuard&) = delete;
        private:
            ReadWriteSpinlock* rw_;
        };

    private:
        static constexpr uint32_t WRITE_LOCKED  = 1u << 31;
        static constexpr uint32_t READERS_MASK  = ~WRITE_LOCKED;
        static constexpr uint32_t READER_ONE    = 1u;

        std::atomic<uint32_t> state_{0};
    };
}
