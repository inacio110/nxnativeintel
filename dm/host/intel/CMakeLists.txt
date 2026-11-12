cmake_minimum_required(VERSION 3.20)

# Build only the AArch64 frontend and x86_64 backend.  The native Apple path
# does not link this target, so an arm64 build never pulls a second CPU engine
# into the app.
project(dm_dynarmic LANGUAGES C CXX ASM)

# CMake 4 removed compatibility with the minimum version used by Dynarmic's
# bundled robin-map.  This variable is ignored by older CMake releases.
if (CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_ENABLE_CPU_FEATURE_DETECTION ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" CACHE PATH "" FORCE)

get_filename_component(NX_NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_subdirectory("${NX_NATIVE_ROOT}/dm/third_party/dynarmic" dynarmic)

# AppleClang 21 (Xcode 26) rejects the bundled fmt 10 compile-time checker
# when it evaluates FMT_STRING through its consteval constructor on Intel.
# Keep the format validation, but use the constexpr-compatible path for this
# compiler so the Intel smoke target can build with the system C++ runtime.
if (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND
    CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "21")
    target_compile_definitions(fmt PUBLIC FMT_CONSTEVAL=constexpr)
endif()

add_library(dm_guest_cpu STATIC
    ../guest_cpu_dynarmic.cpp
    ../guest_cpu_abi_x86_64.S
    ../guest_cpu_guest_abi_x86_64.S
)
target_compile_features(dm_guest_cpu PUBLIC cxx_std_20)
target_include_directories(dm_guest_cpu PUBLIC ..)
target_link_libraries(dm_guest_cpu PUBLIC dynarmic)
set_target_properties(dm_guest_cpu PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_executable(dm_guest_cpu_smoke ../guest_cpu_smoke.cpp)
target_include_directories(dm_guest_cpu_smoke PRIVATE ..)
target_link_libraries(dm_guest_cpu_smoke PRIVATE dm_guest_cpu)
target_compile_options(dm_guest_cpu_smoke PRIVATE -UNDEBUG)
set_target_properties(dm_guest_cpu_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# The Makefile links these archives explicitly because the host itself is still
# built by its established Objective-C/C compile line.  Keep the list in one
# generated file so a Dynarmic dependency change cannot silently produce a
# link that works only on one machine.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/dm-dynarmic-libs.txt"
     CONTENT "$<TARGET_FILE_NAME:dm_guest_cpu>\n$<TARGET_FILE_NAME:dynarmic>\n$<TARGET_FILE_NAME:fmt>\n$<TARGET_FILE_NAME:mcl>\n$<TARGET_FILE_NAME:Zydis>\n$<TARGET_FILE_NAME:Zycore>\n")
