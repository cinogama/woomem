cmake_minimum_required (VERSION 3.13)

include_directories("../include")
include_directories("../src")

add_executable(woomem_test 
    test_main.cpp
    test_chunk.cpp
    test_chunk_parallel.cpp)

target_link_libraries(woomem_test 
    PRIVATE woomem
    PRIVATE woomem_options)

add_test (NAME woomem_test COMMAND woomem_test)
