cmake_minimum_required(VERSION 3.28)

project(
  ncrequest
  VERSION 0.1.0
  LANGUAGES CXX
  HOMEPAGE_URL "https://github.com/hypengw/ncrequest"
  DESCRIPTION "")

option(NCREQUEST_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# curl
find_package(CURL REQUIRED)
# pegtl
find_package(pegtl QUIET)
if(NOT pegtl_FOUND)
  message(STATUS "ncrequest: pegtl not found, fetching it...")
  FetchContent_Declare(
    pegtl
    GIT_REPOSITORY https://github.com/taocpp/PEGTL.git
    GIT_TAG 3.2.8
    GIT_SHALLOW 1
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(pegtl)
endif()
# asio
if(NOT TARGET asio)
  add_library(asio INTERFACE)
  target_compile_definitions(asio INTERFACE ASIO_NO_DEPRECATED)
  pkg_check_modules(ASIO QUIET asio>=1.30.2)
  if(NOT ASIO_FOUND)
    message(STATUS "asio not found, fetching it...")
    FetchContent_Declare(
      asio
      GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
      GIT_TAG asio-1-30-2
      GIT_SHALLOW TRUE
      EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(asio)
    target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
  else()
    target_include_directories(asio INTERFACE ${ASIO_INCLUDE_DIRS})
  endif()
endif()

add_library(ncrequest STATIC)
add_library(ncrequest::ncrequest ALIAS ncrequest)

target_sources(
  ncrequest
  PUBLIC FILE_SET
         all
         TYPE
         CXX_MODULES
         FILES
         src/type.cppm
         src/type_list.cppm
         src/connection.cppm
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
  PUBLIC src/peg/uri.hpp
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

if(PROJECT_IS_TOP_LEVEL)
  install(
    TARGETS ncrequest
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

target_link_libraries(ncrequest PRIVATE taocpp::pegtl CURL::libcurl asio)

if(NCREQUEST_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(test)
endif()
