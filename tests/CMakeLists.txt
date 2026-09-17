cmake_minimum_required(VERSION 3.20)
if (POLICY CMP0167)
	cmake_policy(SET CMP0167 NEW)
endif()

find_package(Boost REQUIRED COMPONENTS unit_test_framework)

add_executable(test_http test_http.cpp)
target_link_libraries(test_http PRIVATE http_lib ${SSL_LIBS} Boost::unit_test_framework pthread)

add_test(NAME test_http COMMAND test_http)

# Fallback for deadlocks/hangs
set_tests_properties(test_http PROPERTIES TIMEOUT 60)
