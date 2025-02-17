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

add_library(
  ncrequest STATIC

  include/ncrequest/request.hpp
  include/ncrequest/response.hpp
  include/ncrequest/response_p.hpp
  include/ncrequest/session.hpp
  include/ncrequest/session_p.hpp
  include/ncrequest/type.hpp
  include/ncrequest/type_list.hpp
  include/ncrequest/uri.hpp

  src/peg/uri.hpp
  src/peg/http.hpp
  src/connection.hpp
  src/request_p.hpp
  src/request.cpp
  src/response.cpp
  src/session.cpp
  src/session_share.cpp
  src/curl_easy.hpp
  src/curl_multi.hpp
  src/curl_error.hpp
  src/type.cpp
  src/peg.cpp)
add_library(ncrequest::ncrequest ALIAS ncrequest)

target_include_directories(
  ncrequest 
  PUBLIC include
  PRIVATE include/ncrequest src)

if(PROJECT_IS_TOP_LEVEL)
install(
  TARGETS ncrequest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

target_link_libraries(ncrequest PRIVATE taocpp::pegtl CURL::libcurl asio)
