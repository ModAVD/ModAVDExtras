cmake_minimum_required(VERSION 3.20)
project(ModAVDExtras VERSION 0.1.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  # Static CRT: the DLLs load into other programs' processes and must not need a VC++ runtime.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /WX /utf-8)
  # Reproducible output: no timestamps in objects and PE headers, so a DLL can be checked against its source.
  add_compile_options(/Brepro)
  add_link_options(/Brepro)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Werror)
endif()
if(NOT WIN32)
  # POSIX: shm_open, nanosleep.
  add_compile_definitions(_POSIX_C_SOURCE=200809L)
  if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
  endif()
endif()

set(MX_SOURCES src/core.c src/shm.c src/console.c src/log.c)

function(mx_link_system target)
  if(WIN32)
    target_link_libraries(${target} PRIVATE ws2_32)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(${target} PRIVATE rt)
  endif()
endfunction()

# The library: modavd_extras.dll / libmodavd_extras.so / libmodavd_extras.dylib
add_library(modavd_extras SHARED ${MX_SOURCES})
target_include_directories(modavd_extras PUBLIC include PRIVATE src)
target_compile_definitions(modavd_extras PRIVATE MODAVD_EXTRAS_BUILD MODAVD_EXTRAS_VERSION="${PROJECT_VERSION}")
mx_link_system(modavd_extras)

# The same code, linked statically into the MuMu-shaped shell and the tests.
add_library(mx_core STATIC ${MX_SOURCES})
target_include_directories(mx_core PUBLIC include src)
target_compile_definitions(mx_core PUBLIC MODAVD_EXTRAS_STATIC PRIVATE MODAVD_EXTRAS_VERSION="${PROJECT_VERSION}")
mx_link_system(mx_core)

enable_testing()

add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE mx_core)
mx_link_system(test_core)
add_test(NAME test_core COMMAND test_core)

if(WIN32)
  # external_renderer_ipc.dll: the compatibility shell for MaaCore's MuMu path.
  add_library(external_renderer_ipc SHARED shim/mumu_shim.c)
  target_link_libraries(external_renderer_ipc PRIVATE mx_core ws2_32)
  set_target_properties(external_renderer_ipc PROPERTIES PREFIX "")

  add_executable(test_shim tests/test_shim.c)
  target_link_libraries(test_shim PRIVATE mx_core)
  add_test(NAME test_shim COMMAND test_shim $<TARGET_FILE:external_renderer_ipc> ${CMAKE_CURRENT_BINARY_DIR})
endif()
