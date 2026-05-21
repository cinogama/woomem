#include "woomem.h"

#include "woomem_page.hpp"

namespace woomem
{
    class Chunk
    {
        Chunk(size_t reserved_size);
        ~Chunk();

        Chunk(const Chunk&) = delete;
        Chunk(Chunk&&) = delete;
        Chunk& operator = (const Chunk&) = delete;
        Chunk& operator = (Chunk&&) = delete;

        Page* allocate_page();
        Page* allocate_huge_page(size_t size);

        Page* validate(void* ptr);
    };
}