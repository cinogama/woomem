#pragma once

#include <atomic>
#include <cstddef>

namespace woomem
{
    struct UnitHead;

    template<size_t Capacity>
    class alignas(64) MpscGrayQueue
    {
        static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
        static constexpr size_t MASK = Capacity - 1;

        struct Slot
        {
            std::atomic<size_t> sequence;
            UnitHead*           item;
        };

        Slot m_slots[Capacity];

        alignas(64) std::atomic<size_t> m_enqueue_pos{0};
        alignas(64) std::atomic<size_t> m_dequeue_pos{0};

    public:
        MpscGrayQueue()
        {
            for (size_t i = 0; i < Capacity; ++i)
                m_slots[i].sequence.store(i, std::memory_order_relaxed);
        }

        MpscGrayQueue(const MpscGrayQueue&) = delete;
        MpscGrayQueue& operator=(const MpscGrayQueue&) = delete;
        MpscGrayQueue(MpscGrayQueue&&) = delete;
        MpscGrayQueue& operator=(MpscGrayQueue&&) = delete;

        bool try_enqueue(UnitHead* item)
        {
            size_t pos = m_enqueue_pos.load(std::memory_order_relaxed);
            for (;;)
            {
                Slot& slot = m_slots[pos & MASK];
                if (slot.sequence.load(std::memory_order_acquire) < pos)
                    return false;

                if (m_enqueue_pos.compare_exchange_weak(pos, pos + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    slot.item = item;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
        }

#if 0
        void enqueue(UnitHead* item)
        {
            const size_t pos = m_enqueue_pos.fetch_add(1, std::memory_order_relaxed);
            Slot& slot = m_slots[pos & MASK];

            while (slot.sequence.load(std::memory_order_acquire) != pos)
                ;

            slot.item = item;
            slot.sequence.store(pos + 1, std::memory_order_release);
        }
#endif

        UnitHead* dequeue()
        {
            const size_t pos = m_dequeue_pos.load(std::memory_order_relaxed);
            Slot& slot = m_slots[pos & MASK];

            if (slot.sequence.load(std::memory_order_acquire) != pos + 1)
                return nullptr;

            UnitHead* const item = slot.item;
            slot.sequence.store(pos + Capacity, std::memory_order_release);
            m_dequeue_pos.store(pos + 1, std::memory_order_relaxed);
            return item;
        }

        size_t drain(UnitHead** output, size_t max_count)
        {
            size_t count = 0;
            for (; count < max_count; ++count)
            {
                const size_t pos = m_dequeue_pos.load(std::memory_order_relaxed);
                Slot& slot = m_slots[pos & MASK];

                if (slot.sequence.load(std::memory_order_acquire) != pos + 1)
                    break;

                output[count] = slot.item;
                slot.sequence.store(pos + Capacity, std::memory_order_release);
                m_dequeue_pos.store(pos + 1, std::memory_order_relaxed);
            }
            return count;
        }

        bool empty() const
        {
            const size_t pos = m_dequeue_pos.load(std::memory_order_relaxed);
            const Slot& slot = m_slots[pos & MASK];
            return slot.sequence.load(std::memory_order_acquire) != pos + 1;
        }
    };
}
