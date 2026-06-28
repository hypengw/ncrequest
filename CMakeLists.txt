cmake_minimum_required(VERSION 3.28)

project(
  ncrequest
  VERSION 0.1.0
  LANGUAGES CXX
  HOMEPAGE_URL "https://github.com/hypengw/ncrequest"
  DESCRIPTION "")

option(NCREQUEST_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
set(NCREQUEST_CLIENT_BACKEND "qt" CACHE STRING "Client backend: curl or qt")
set(NCREQUEST_CLIENT_BACKEND_VALUES curl qt)
set_property(CACHE NCREQUEST_CLIENT_BACKEND
             PROPERTY STRINGS ${NCREQUEST_CLIENT_BACKEND_VALUES})

if(NOT NCREQUEST_CLIENT_BACKEND IN_LIST NCREQUEST_CLIENT_BACKEND_VALUES)
  list(JOIN NCREQUEST_CLIENT_BACKEND_VALUES ", " NCREQUEST_CLIENT_BACKEND_LIST)
  message(FATAL_ERROR
          "NCREQUEST_CLIENT_BACKEND must be one of: ${NCREQUEST_CLIENT_BACKEND_LIST}"
  )
endif()

set(NCREQUEST_USE_QT_NETWORK OFF)
set(NCREQUEST_USE_QT_WEBSOCKETS OFF)
if(NCREQUEST_CLIENT_BACKEND STREQUAL "qt")
  set(NCREQUEST_USE_QT_NETWORK ON)
  set(NCREQUEST_USE_QT_WEBSOCKETS ON)
endif()

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

# curl
find_package(CURL REQUIRED)

# pegtl
if(NOT TARGET taocpp::pegtl)
  FetchContent_Declare(
    pegtl
    GIT_REPOSITORY https://github.com/taocpp/PEGTL.git
    GIT_TAG 3.2.8
    GIT_SHALLOW 1
    GIT_PROGRESS 1
    SYSTEM EXCLUDE_FROM_ALL FIND_PACKAGE_ARGS 3.2.7 QUIET GLOBAL)
  FetchContent_MakeAvailable(pegtl)
endif()

if(NOT TARGET rstd::rstd)

if(EXISTS "${CMAKE_SOURCE_DIR}/rstd")
add_subdirectory(rstd)
else()
  FetchContent_Declare(
    rstd
    GIT_REPOSITORY https://github.com/hypengw/rstd.git
    GIT_TAG 5878b2058eb67adb615f691889d93e3ab13ba7d2
    GIT_SHALLOW 0
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(rstd)
endif()
endif()

set(NCREQUEST_QT_COMPONENTS)
if(NCREQUEST_USE_QT_NETWORK)
  list(APPEND NCREQUEST_QT_COMPONENTS Core Network)
endif()
if(NCREQUEST_USE_QT_WEBSOCKETS)
  list(APPEND NCREQUEST_QT_COMPONENTS Core Network WebSockets)
endif()
if(NCREQUEST_QT_COMPONENTS)
  list(REMOVE_DUPLICATES NCREQUEST_QT_COMPONENTS)
  find_package(Qt6 REQUIRED COMPONENTS ${NCREQUEST_QT_COMPONENTS})
endif()

add_library(ncrequest STATIC)
add_library(ncrequest::ncrequest ALIAS ncrequest)

target_compile_features(ncrequest PUBLIC cxx_std_20)

set(NCREQUEST_MODULE_FILES
    src/type.cppm
    src/type_list.cppm
    src/error.cppm
    src/request.cppm
    src/client/http_backend.cppm
    src/client/websocket_backend.cppm
    src/response.cppm
    src/session.cppm
    src/session_share.cppm
    src/http.cppm
    src/websocket.cppm
    src/client/curl/curl_error.cppm
    src/client/curl/curl_easy.cppm
    src/client/curl/curl_multi.cppm
    src/client/curl/curl.cppm
    src/client/curl/init.cppm
    src/client/curl/mod.cppm
    src/coro/base.cppm
    src/coro/rstd.cppm
    src/coro/mod.cppm
    src/mod.cppm)

if(NCREQUEST_CLIENT_BACKEND STREQUAL "curl")
  list(APPEND NCREQUEST_MODULE_FILES
       src/client/curl/connection.cppm
       src/client/curl/response.cppm
       src/client/curl/session.cppm)
endif()

if(NOT NCREQUEST_USE_QT_WEBSOCKETS)
  list(APPEND NCREQUEST_MODULE_FILES src/client/curl/websocket.cppm)
endif()

if(NCREQUEST_USE_QT_NETWORK)
  list(APPEND NCREQUEST_MODULE_FILES src/client/qt_network/backend.cppm
       src/client/qt_network/mod.cppm)
endif()

if(NCREQUEST_USE_QT_WEBSOCKETS)
  list(APPEND NCREQUEST_MODULE_FILES src/client/qt_websockets/backend.cppm
       src/client/qt_websockets/mod.cppm)
endif()

target_sources(
  ncrequest
  PUBLIC FILE_SET
         all
         TYPE
         CXX_MODULES
         FILES
         ${NCREQUEST_MODULE_FILES})

target_sources(
  ncrequest
  PRIVATE src/peg/uri.hpp
          src/peg/http.hpp
          src/client/curl/init.cpp
          src/http.cpp
          src/request.cpp
          src/session_share.cpp
          src/type.cpp)

if(NCREQUEST_CLIENT_BACKEND STREQUAL "curl")
  target_sources(ncrequest PRIVATE src/client/curl/response.cpp
                                   src/client/curl/session.cpp)
endif()

if(NOT NCREQUEST_USE_QT_WEBSOCKETS)
  target_sources(ncrequest PRIVATE src/client/curl/websocket.cpp)
endif()

target_include_directories(ncrequest PRIVATE src)
target_compile_definitions(ncrequest PUBLIC ${WIN_DEFS})
target_compile_definitions(ncrequest PUBLIC NCREQUEST_USE_RSTD_CORO)

if(NCREQUEST_USE_QT_NETWORK)
  target_compile_definitions(ncrequest PUBLIC NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
else()
  target_compile_definitions(ncrequest PUBLIC NCREQUEST_CLIENT_BACKEND_CURL)
endif()

if(NCREQUEST_USE_QT_WEBSOCKETS)
  target_compile_definitions(ncrequest
                             PUBLIC NCREQUEST_WEBSOCKET_BACKEND_QT_WEBSOCKETS)
else()
  target_compile_definitions(ncrequest PUBLIC NCREQUEST_WEBSOCKET_BACKEND_CURL)
endif()

if(PROJECT_IS_TOP_LEVEL)
  install(
    TARGETS ncrequest
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

target_link_libraries(
  ncrequest
  PUBLIC rstd::rstd rstd.cppstd;
  PRIVATE taocpp::pegtl CURL::libcurl)

if(NCREQUEST_USE_QT_NETWORK OR NCREQUEST_USE_QT_WEBSOCKETS)
  target_link_libraries(ncrequest PRIVATE Qt6::Core Qt6::Network)
endif()

if(NCREQUEST_USE_QT_WEBSOCKETS)
  target_link_libraries(ncrequest PRIVATE Qt6::WebSockets)
endif()

if(NCREQUEST_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(test)
endif()
