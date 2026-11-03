cmake_minimum_required(VERSION 3.24)

project(emulator-lift
    VERSION 0.1.0
    DESCRIPTION "Lift AGE.EXE .lst into a guest-state x86 emulator (SDL3 host)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# vcpkg 由 CMakePresets 的 toolchain 注入；这里只做显式检查，缺依赖时给可读的失败信息。
if(NOT DEFINED VCPKG_TARGET_TRIPLET AND NOT DEFINED ENV{VCPKG_ROOT})
    message(WARNING
        "没有检测到 vcpkg toolchain。请用 CMakePresets.json（跨目录复用 ../emulator-cxx/vcpkg），"
        "或用 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake。")
endif()

find_package(SDL3 CONFIG REQUIRED)

# 运行时（guest 状态 / 块表 / 调度）——先放最小接口；生成器与 shim 后续加。
add_library(emulator-lift-runtime STATIC
    src/runtime/x86.cpp)
target_include_directories(emulator-lift-runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(emulator-lift-runtime PUBLIC SDL3::SDL3)
target_compile_features(emulator-lift-runtime PUBLIC cxx_std_20)

add_executable(emulator-lift
    src/main.cpp)
target_link_libraries(emulator-lift PRIVATE emulator-lift-runtime)

enable_testing()
add_test(NAME emulator-lift-smoke COMMAND emulator-lift --smoke)
