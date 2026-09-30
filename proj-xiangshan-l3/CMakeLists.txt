cmake_minimum_required(VERSION 3.20)
project(proj_xiangshan_l3 CXX)

# 默认 Release：模型库对仿真速度敏感（-O0 下回放 eval 慢约 9 倍），
# 未显式指定构建类型时不得落到无优化默认。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # editor IntelliSense (clangd / cpptools)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # DPI 静态库链接进 emu（-pie）需要

# wolvicmod 框架（header-only，INTERFACE 库；WOLVICMOD_BUILD_TESTS 作为子目录
# 构建时默认 OFF）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../wolvicmod ${CMAKE_BINARY_DIR}/wolvicmod)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

enable_testing()

# 项目模型库：非模板模块的实现（构造函数体）收进 .cpp；模板模块与 flit 数据
# 结构保持 header-only。WOLVICMOD_AUDIT 与测试套件一致编入读集对账插桩。
file(GLOB_RECURSE ZJ_MODEL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/model/*.cpp)
add_library(zjmodel STATIC ${ZJ_MODEL_SOURCES})
target_include_directories(zjmodel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(zjmodel PRIVATE WOLVICMOD_AUDIT=1)
target_link_libraries(zjmodel PUBLIC wolvicmod)

# DPI-C 集成静态库：emu 链接注入（difftest USER_LDFLAGS），见 dpi/sv/WolvicZjBB.sv。
# -fPIC：emu 以 -pie 链接时静态库必需。
add_library(wolviczj_dpi STATIC ${CMAKE_CURRENT_SOURCE_DIR}/dpi/csrc/wolvic_zj_dpi.cpp)
target_link_libraries(wolviczj_dpi PRIVATE zjmodel)
set_target_properties(wolviczj_dpi PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
    target_link_libraries(${name} PRIVATE wolvicmod zjmodel)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# trace 重放测试：trace 文件缺失时以 77 退出 → 跳过（make replay 一键生成+运行）
if(TEST test_trace_replay)
    set_tests_properties(test_trace_replay PROPERTIES SKIP_RETURN_CODE 77)
endif()
if(TEST test_wolvic_top_replay)
    set_tests_properties(test_wolvic_top_replay PROPERTIES SKIP_RETURN_CODE 77)
endif()
