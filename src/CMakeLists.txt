cmake_minimum_required (VERSION 3.13)

include_directories("../include")

file(GLOB_RECURSE woomem_src_cpp *.cpp)
file(GLOB_RECURSE woomem_src_c *.c)
file(GLOB_RECURSE woomem_src_hpp *.hpp)
file(GLOB_RECURSE woomem_src_h *.h)

add_library(woomem 
    ${woomem_src_cpp} 
    ${woomem_src_c}
    ${woomem_src_hpp}
    ${woomem_src_h})

set_target_properties(
    woomem PROPERTIES OUTPUT_NAME libwoomem)
set_target_properties(
    woomem PROPERTIES PREFIX "")

target_link_libraries(woomem 
    PRIVATE woomem_options)
