cmake_minimum_required(VERSION 3.16)
project(rnic_ucx_transfer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UCX_ROOT "/usr/local/ucx" CACHE PATH "UCX installation prefix")

find_path(UCX_INCLUDE_DIR
  NAMES ucp/api/ucp.h
  HINTS "${UCX_ROOT}/include" /usr/local/ucx/include /usr/include)

find_library(UCP_LIBRARY
  NAMES ucp
  HINTS "${UCX_ROOT}/lib" /usr/local/ucx/lib /usr/lib /usr/lib/x86_64-linux-gnu)
find_library(UCS_LIBRARY
  NAMES ucs
  HINTS "${UCX_ROOT}/lib" /usr/local/ucx/lib /usr/lib /usr/lib/x86_64-linux-gnu)

if(NOT UCX_INCLUDE_DIR OR NOT UCP_LIBRARY OR NOT UCS_LIBRARY)
  message(FATAL_ERROR "UCX was not found. Set -DUCX_ROOT=/path/to/ucx, e.g. /usr/local/ucx")
endif()

add_library(rnic_common
  src/sha256.cpp
  src/protocol.cpp
  src/ucx_stream.cpp)
target_include_directories(rnic_common PUBLIC include "${UCX_INCLUDE_DIR}")
target_link_libraries(rnic_common PUBLIC "${UCP_LIBRARY}" "${UCS_LIBRARY}" pthread)
target_compile_options(rnic_common PRIVATE -Wall -Wextra -Wpedantic)

foreach(target rnic_send rnic_recv)
  add_executable(${target} src/${target}.cpp)
  target_link_libraries(${target} PRIVATE rnic_common)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "${UCX_ROOT}/lib"
    INSTALL_RPATH "${UCX_ROOT}/lib")
endforeach()
