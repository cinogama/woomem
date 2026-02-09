#include "woomem.h"

namespace woomem_cppimpl::gc
{
    struct GlobalGCMethods
    {
        woomem_UserContext          m_user_ctx;
        woomem_MarkCallbackFunc     m_marker;
        woomem_DestroyCallbackFunc  m_destroyer;        
    };
    extern GlobalGCMethods g_global_gc_methods;
}