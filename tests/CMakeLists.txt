cmake_minimum_required(VERSION 3.14)
cmake_policy(SET CMP0167 NEW)

find_package(Boost REQUIRED COMPONENTS unit_test_framework)

add_executable(test_http test_http.cpp)
target_include_directories(test_http PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_http PRIVATE http_lib Boost::unit_test_framework pthread)

add_test(NAME test_http COMMAND test_http)
