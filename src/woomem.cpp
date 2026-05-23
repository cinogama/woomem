#include "woomem.h"
#include "woomem_os_mmap.h"

#include "woomem_page.hpp"
#include "woomem_chunk.hpp"

bool woomem_is_gc_in_marking = false;

struct _woomem_GlobalContext
{
    woomem::Chunk m_chunk;

    std::atomic<woomem::PageHead*> m_page_list;

    _woomem_GlobalContext(size_t reserved_chunk_size)
        : m_chunk(reserved_chunk_size)
        , m_page_list{}
    {
    }
    void _add_page_to_list(woomem::PageHead* page)
    {

    }
};
static _woomem_GlobalContext* _s_ctx;




void woomem_init(
    size_t reserved_chunk_size,
    woomem_MarkCallback mark_callback,
    woomem_FreeCallback free_callback)
{

}
void woomem_shutdown(void);

void woomem_trigger_gc(bool async);

void* woomem_validate_addr(void* ptr_may_invalid);
void* woomem_allocate(size_t size, int attrib);
void* woomem_reallocate(void* ptr, size_t size);
void woomem_free(void* ptr);

void woomem_mark_unit_head(void* ptr_head_may_null);
void woomem_mark_fuzzy_unit(void* ptr_may_invalid_or_null);
