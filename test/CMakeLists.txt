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

add_executable(ncrequest_test http.cpp main.cpp)

target_link_libraries(ncrequest_test PRIVATE ncrequest::ncrequest GTest::gtest_main)

if(NOT NCREQUEST_USE_QT_WEBSOCKETS)
  target_sources(ncrequest_test PRIVATE websocket.cpp)
endif()

if(NCREQUEST_USE_QT_NETWORK OR NCREQUEST_USE_QT_WEBSOCKETS)
  target_link_libraries(ncrequest_test PRIVATE Qt6::Core)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

include(GoogleTest)
gtest_discover_tests(ncrequest_test
                     TEST_FILTER "*-http.LocalHttp*:websocket.LocalEcho*")

if(Python3_EXECUTABLE)
  add_test(
    NAME ncrequest_local_http
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/http_server_test.py
            --test-executable $<TARGET_FILE:ncrequest_test>)
  set_tests_properties(ncrequest_local_http PROPERTIES LABELS "http;integration")
endif()

if((NOT NCREQUEST_USE_QT_WEBSOCKETS) AND Python3_EXECUTABLE)
  add_test(
    NAME ncrequest_websocket_local_echo
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/websocket_server_test.py
      --test-executable $<TARGET_FILE:ncrequest_test> --gtest-filter
      "websocket.LocalEcho*")
  set_tests_properties(ncrequest_websocket_local_echo
                       PROPERTIES LABELS "websocket;integration")
endif()

if(NCREQUEST_USE_QT_NETWORK)
  add_executable(ncrequest_qt_network_test qt_network.cpp qt_network_main.cpp)
  target_link_libraries(ncrequest_qt_network_test PRIVATE ncrequest::ncrequest
                                                          GTest::gtest Qt6::Core
                                                          Qt6::Network)

  gtest_discover_tests(ncrequest_qt_network_test
                       TEST_FILTER "*-qt_network.LocalHttp*")

  if(Python3_EXECUTABLE)
    add_test(
      NAME ncrequest_qt_network_local_http
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/http_server_test.py
        --test-executable $<TARGET_FILE:ncrequest_qt_network_test>
        --gtest-filter "qt_network.LocalHttp*")
    set_tests_properties(ncrequest_qt_network_local_http
                         PROPERTIES LABELS "http;integration;qt_network")
  endif()
endif()

if(NCREQUEST_USE_QT_WEBSOCKETS)
  add_executable(ncrequest_qt_websockets_test qt_websockets.cpp
                                              qt_websockets_main.cpp)
  target_link_libraries(ncrequest_qt_websockets_test
                        PRIVATE ncrequest::ncrequest GTest::gtest Qt6::Core
                                Qt6::WebSockets)

  gtest_discover_tests(ncrequest_qt_websockets_test
                       TEST_FILTER "*-qt_websockets.LocalEcho*")

  if(Python3_EXECUTABLE)
    add_test(
      NAME ncrequest_qt_websockets_local_echo
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/websocket_server_test.py
        --test-executable $<TARGET_FILE:ncrequest_qt_websockets_test>
        --gtest-filter "qt_websockets.LocalEcho*")
    set_tests_properties(ncrequest_qt_websockets_local_echo
                         PROPERTIES LABELS "websocket;integration;qt_websockets")
  endif()
endif()
