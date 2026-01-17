cmake_minimum_required (VERSION 3.13)

include_directories("../include")

# 功能测试
add_executable(woomem_test 
    "test_main.cpp")

target_link_libraries(woomem_test 
    PRIVATE woomem
    PRIVATE woomem_options)

# 性能测试
add_executable(woomem_bench 
    bench_main.c)

target_link_libraries(woomem_bench 
    PRIVATE woomem
    PRIVATE woomem_options)
