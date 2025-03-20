cmake_minimum_required(VERSION 3.20)
project(toy_http_server)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB HTTP_LIB_SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h)
list(REMOVE_ITEM HTTP_LIB_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

option(DISABLE_HTTPS "Disable HTTPS support" OFF)

find_package(OpenSSL)

if (NOT DISABLE_HTTPS AND OpenSSL_FOUND)
	message(STATUS "HTTPS support enabled.")
	set(SSL_LIBS OpenSSL::SSL OpenSSL::Crypto)
else()
	message(STATUS "HTTPS support disabled.")
	add_definitions(-DDISABLE_HTTPS)
	set(SSL_LIBS "")
	list(REMOVE_ITEM HTTP_LIB_SOURCES 
		${CMAKE_SOURCE_DIR}/src/SSLSocketHandler.cpp 
		${CMAKE_SOURCE_DIR}/src/SSLSocketHandler.h
	)
endif()

# Shared library
add_library(http_lib ${HTTP_LIB_SOURCES})
target_link_libraries(http_lib PRIVATE pthread ${SSL_LIBS})
target_include_directories(http_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Example driver program
add_executable(toy_http_server src/main.cpp)
target_link_libraries(toy_http_server PRIVATE http_lib ${SSL_LIBS})

find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)

if (CLANG_TIDY_EXECUTABLE)
	message(STATUS "Clang-tidy found, enabling static analysis.")
	set(CMAKE_CXX_CLANG_TIDY_VERBOSE "ON")
	set_target_properties(http_lib PROPERTIES CXX_CLANG_TIDY "clang-tidy;-checks=-*,modernize-*,readability-*,cppcoreguidelines-*,performance-*,bugprone-*,-modernize-use-designated-initializers,-*-magic-numbers,-llvmlibc-callee-namespace,-modernize-use-trailing-return-type,-readability-identifier-length,-readability-braces-around-statements,-cppcoreguidelines-pro-type-reinterpret-cast;-use-color;-warnings-as-errors=*")
else()
	message(WARNING "Clang-tidy not found, skipping static analysis.")
endif()

add_compile_options(-Wall -Wextra -Werror)

# Tests
enable_testing()
add_subdirectory(tests)
