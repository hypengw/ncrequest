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

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  add_compile_options(
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-direct-access-external-data>")
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(FetchContent)

set(WARN_OPTS -Wall -Wextra -Wpedantic -Werror=mismatched-tags)
set(WIN_DEFS "$<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN;NOMINMAX;NOGDI>")
add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:${WARN_OPTS}>")

if(NOT TARGET rstd::rstd)

if(EXISTS "${CMAKE_SOURCE_DIR}/rstd")
add_subdirectory(rstd)
else()
  FetchContent_Declare(
    rstd
    GIT_REPOSITORY https://github.com/hypengw/rstd.git
    GIT_TAG 030f89bd21d6d26e9698fcdb103f42fe616f993b
    GIT_SHALLOW 0
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(rstd)
endif()
endif()

if(NCREQUEST_CLIENT_BACKEND STREQUAL "qt")
  find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets)
else()
  find_package(CURL REQUIRED)
endif()

add_library(ncrequest STATIC)
add_library(ncrequest::ncrequest ALIAS ncrequest)

add_library(ncrequest_http_parser OBJECT)
target_compile_features(ncrequest_http_parser PUBLIC cxx_std_20)
target_sources(
  ncrequest_http_parser
  PUBLIC FILE_SET
         modules
         TYPE
         CXX_MODULES
         FILES
         src/http/parser/cursor.cppm
         src/http/parser/ascii.cppm
         src/http/parser/uri.cppm
         src/http/parser/http1.cppm
         src/http/parser/mod.cppm)
target_link_libraries(ncrequest_http_parser PUBLIC rstd::rstd)

target_compile_features(ncrequest PUBLIC cxx_std_20)

set(NCREQUEST_MODULE_FILES
    src/type.cppm
    src/error.cppm
    src/session_share.cppm
    src/request.cppm
    src/client/callback.cppm
    src/client/http_backend.cppm
    src/client/websocket_backend.cppm
    src/response.cppm
    src/session.cppm
    src/http/error.cppm
    src/http/url.cppm
    src/http/header.cppm
    src/http/query.cppm
    src/http/cookie.cppm
    src/http/message.cppm
    src/http.cppm
    src/websocket.cppm
    src/coro/base.cppm
    src/coro/mod.cppm
    src/mod.cppm)

if(NCREQUEST_CLIENT_BACKEND STREQUAL "curl")
  list(APPEND NCREQUEST_MODULE_FILES
       src/client/curl/session_share.cppm
       src/client/curl/curl_easy.cppm
       src/client/curl/curl_multi.cppm
       src/client/curl/curl.cppm
       src/client/curl/init.cppm
       src/client/curl/mod.cppm
       src/client/curl/connection.cppm
       src/client/curl/response.cppm
       src/client/curl/session.cppm
       src/client/curl/websocket.cppm)
else()
  list(APPEND NCREQUEST_MODULE_FILES src/client/qt_network/qt.cppm
       src/client/qt_network/session_share.cppm
       src/client/qt_network/backend.cppm
       src/client/qt_network/websocket.cppm
       src/client/qt_network/mod.cppm)
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
  PRIVATE src/http/url.cpp
          src/http/header.cpp
          src/http/query.cpp
          src/http/cookie.cpp
          src/http/message.cpp
          src/request.cpp)

if(NCREQUEST_CLIENT_BACKEND STREQUAL "curl")
  target_sources(
    ncrequest
    PRIVATE src/session_share.cpp
            src/client/curl/init.cpp
            src/client/curl/response.cpp
            src/client/curl/session.cpp
            src/client/curl/websocket.cpp)
else()
  target_sources(ncrequest PRIVATE src/client/qt_network/session_share.cpp)
endif()

target_include_directories(ncrequest PRIVATE src)
target_compile_definitions(ncrequest PUBLIC ${WIN_DEFS})
target_compile_definitions(ncrequest PUBLIC NCREQUEST_USE_RSTD_CORO)

if(NCREQUEST_CLIENT_BACKEND STREQUAL "qt")
  target_compile_definitions(ncrequest PUBLIC NCREQUEST_CLIENT_BACKEND_QT_NETWORK)
else()
  target_compile_definitions(ncrequest PUBLIC NCREQUEST_CLIENT_BACKEND_CURL)
endif()

if(PROJECT_IS_TOP_LEVEL)
  install(
    TARGETS ncrequest
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

target_link_libraries(
  ncrequest
  PRIVATE ncrequest_http_parser
  PUBLIC rstd::rstd rstd.cppstd)

if(NCREQUEST_CLIENT_BACKEND STREQUAL "qt")
  target_link_libraries(ncrequest PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets)
else()
  target_link_libraries(ncrequest PRIVATE CURL::libcurl)
endif()

if(NCREQUEST_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(test)
endif()
