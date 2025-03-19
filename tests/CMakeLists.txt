cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0167 NEW)

find_package(Boost REQUIRED COMPONENTS unit_test_framework)
find_package(OpenSSL)

if (NOT DISABLE_HTTPS AND OpenSSL_FOUND)
	message(STATUS "HTTPS support enabled.")
	set(SSL_LIBS OpenSSL::SSL OpenSSL::Crypto)
else()
	message(STATUS "HTTPS support disabled.")
	add_definitions(-DDISABLE_HTTPS)
	set(SSL_LIBS "")
endif()

add_executable(test_http test_http.cpp)
target_include_directories(test_http PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_http PRIVATE http_lib ${SSL_LIBS} Boost::unit_test_framework pthread)

add_test(NAME test_http COMMAND test_http)
