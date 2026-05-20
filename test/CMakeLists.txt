cmake_minimum_required (VERSION 3.13)

include_directories("../include")

add_executable(woomem_test 
    test_main.cpp)

target_link_libraries(woomem_test 
    PRIVATE woomem
    PRIVATE woomem_options)
