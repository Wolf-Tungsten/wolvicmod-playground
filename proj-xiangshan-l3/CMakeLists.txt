cmake_minimum_required(VERSION 3.20)
project(proj_xiangshan_l3 CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # editor IntelliSense (clangd / cpptools)

# wolvicmod 框架（header-only，INTERFACE 库；WOLVICMOD_BUILD_TESTS 作为子目录
# 构建时默认 OFF）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../wolvicmod ${CMAKE_BINARY_DIR}/wolvicmod)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

enable_testing()

# 项目侧预制菜单测：doctest 从 wolvicmod/third_party 引入；
# 每个 test 源文件一个可执行 + 独立 ctest 条目（ctest -R <模块> 可单跑）。
file(GLOB ZJ_PREFAB_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)
list(REMOVE_ITEM ZJ_PREFAB_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp)

foreach(src ${ZJ_PREFAB_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} tests/test_main.cpp ${src})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../wolvicmod/third_party)
    # 与 wolvicmod/tests 一致：测试套件编入读集对账插桩（运行时开关默认仍关）
    target_compile_definitions(${name} PRIVATE WOLVICMOD_AUDIT=1)
    target_link_libraries(${name} PRIVATE wolvicmod)
    add_test(NAME ${name} COMMAND ${name})
endforeach()
