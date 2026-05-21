#include "woomem.h"

extern int test_chunk_main(void);
extern int test_chunk_parallel_main(void);

int main(void)
{
    int result = test_chunk_main();
    if (result != 0)
        return result;
    return test_chunk_parallel_main();
}
