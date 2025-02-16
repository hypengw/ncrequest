cmake_minimum_required(VERSION 3.14)

project(
  ncrequest
  VERSION 0.0.1
  LANGUAGES CXX
  HOMEPAGE_URL "https://github.com/hypengw/ncrequest"
  DESCRIPTION "")


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

add_library(
  ncrequest STATIC

  include/ncrequest/request.h
  include/ncrequest/response.h
  include/ncrequest/response_p.h
  include/ncrequest/session.h
  include/ncrequest/session_p.h
  include/ncrequest/type.h

  src/peg/uri.h
  src/peg/http.h
  src/connection.h
  src/request_p.h
  src/request.cpp
  src/response.cpp
  src/session.cpp
  src/session_share.cpp
  src/curl_easy.h
  src/curl_multi.h
  src/curl_error.h
  src/type.cpp
  src/peg.cpp)
add_library(ncrequest::ncrequest ALIAS ncrequest)

target_include_directories(
  ncrequest 
  PUBLIC include
  PRIVATE include/ncrequest .)

if(PROJECT_IS_TOP_LEVEL)
install(
  TARGETS ncrequest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

target_link_libraries(ncrequest PRIVATE taocpp::pegtl CURL::libcurl asio)
