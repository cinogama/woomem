cmake_minimum_required (VERSION 3.13)

include_directories("../include")

file(GLOB_RECURSE woomem_test_src_cpp *.cpp)
file(GLOB_RECURSE woomem_test_src_c *.c)
file(GLOB_RECURSE woomem_test_src_hpp *.hpp)
file(GLOB_RECURSE woomem_test_src_h *.h)

add_executable(woomem_test 
    ${woomem_test_src_cpp} 
    ${woomem_test_src_c}
    ${woomem_test_src_hpp}
    ${woomem_test_src_h})

target_link_libraries(woomem_test 
    PRIVATE woomem
    PRIVATE woomem_options)
