cmake_minimum_required(VERSION 3.28)

project(
  ncrequest
  VERSION 0.1.0
  LANGUAGES CXX
  HOMEPAGE_URL "https://github.com/hypengw/ncrequest"
  DESCRIPTION "")

option(NCREQUEST_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(FetchContent)

set(WARN_OPTS -Wall -Wextra -Wpedantic -Werror=mismatched-tags)
set(WIN_DEFS "$<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN;NOMINMAX;NOGDI>")
add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:${WARN_OPTS}>")

find_package(PkgConfig REQUIRED)

# curl
find_package(CURL REQUIRED)

# pegtl
FetchContent_Declare(
  pegtl
  GIT_REPOSITORY https://github.com/taocpp/PEGTL.git
  GIT_TAG 3.2.8
  GIT_SHALLOW 1
  GIT_PROGRESS 1
  SYSTEM EXCLUDE_FROM_ALL FIND_PACKAGE_ARGS 3.2.7 QUIET GLOBAL)
FetchContent_MakeAvailable(pegtl)

# asio
if(NOT TARGET asio)
  add_library(asio INTERFACE)
  target_compile_definitions(asio INTERFACE ASIO_NO_DEPRECATED ASIO_HAS_THREADS
                                            ${WIN_DEFS})
  if(MINGW)
    target_link_libraries(asio INTERFACE ws2_32)
  endif()
  pkg_check_modules(ASIO QUIET asio>=1.30.2)
  if(NOT ASIO_FOUND)
    FetchContent_Declare(
      asio
      GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
      GIT_TAG asio-1-30-2
      GIT_SHALLOW TRUE
      EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(asio)
    target_include_directories(asio SYSTEM
                               INTERFACE ${asio_SOURCE_DIR}/asio/include)
    message(STATUS "asio dir: ${asio_SOURCE_DIR}")
  else()
    target_include_directories(asio SYSTEM INTERFACE ${ASIO_INCLUDE_DIRS})
  endif()
endif()

FetchContent_Declare(
  rstd
  GIT_REPOSITORY https://github.com/hypengw/rstd.git
  GIT_TAG 42b14d29ccc664d4974b0017330da52a99d0f7a8
  GIT_SHALLOW 0
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(rstd)

add_library(ncrequest STATIC)
add_library(ncrequest::ncrequest ALIAS ncrequest)

target_compile_features(ncrequest PUBLIC cxx_std_20)

target_sources(
  ncrequest
  PUBLIC FILE_SET
         all
         TYPE
         CXX_MODULES
         FILES
         src/asio.cppm
         src/type.cppm
         src/type_list.cppm
         src/connection.cppm
         src/error.cppm
         src/request.cppm
         src/response.cppm
         src/session.cppm
         src/session_share.cppm
         src/http.cppm
         src/websocket.cppm
         src/event/asio.cppm
         src/event/interface.cppm
         src/event/mod.cppm
         src/curl/curl_error.cppm
         src/curl/curl_easy.cppm
         src/curl/curl_multi.cppm
         src/curl/init.cppm
         src/curl/mod.cppm
         src/mod.cppm)

target_sources(
  ncrequest
  PRIVATE src/peg/uri.hpp
          src/peg/http.hpp
          src/curl/init.cpp
          src/http.cpp
          src/request.cpp
          src/response.cpp
          src/session.cpp
          src/session_share.cpp
          src/websocket.cpp
          src/type.cpp)

target_include_directories(ncrequest PRIVATE src)
target_compile_definitions(ncrequest PUBLIC ${WIN_DEFS})

if(PROJECT_IS_TOP_LEVEL)
  install(
    TARGETS ncrequest
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

target_link_libraries(
  ncrequest
  PUBLIC rstd::rstd
  PRIVATE taocpp::pegtl CURL::libcurl asio)

if(NCREQUEST_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(test)
endif()
