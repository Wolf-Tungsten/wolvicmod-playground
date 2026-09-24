cmake_minimum_required(VERSION 3.20)
project(wolvicmod_examples CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # editor IntelliSense (clangd / cpptools)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../wolvicmod ${CMAKE_BINARY_DIR}/wolvicmod)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

enable_testing()
foreach(ex adder counter sort_pipeline data_mem)
    add_executable(${ex} ${ex}.cpp)
    target_link_libraries(${ex} PRIVATE wolvicmod)
    add_test(NAME ${ex} COMMAND ${ex})
endforeach()
