set(gtest_force_shared_crt ON)
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG release-1.12.1)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(ncrequest_test websocket.cpp http.cpp main.cpp)

target_link_libraries(ncrequest_test PRIVATE asio ncrequest::ncrequest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(ncrequest_test)
