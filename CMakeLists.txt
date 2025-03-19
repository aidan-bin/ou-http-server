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

find_program(CPPCHECK_EXE NAMES cppcheck)
if (CPPCHECK_EXE)
	add_custom_target(run_cppcheck
		COMMAND ${CPPCHECK_EXE} --enable=all --inconclusive --quiet --error-exitcode=1 --suppress=missingIncludeSystem --check-level=exhaustive --inline-suppr --checkers-report=${CMAKE_BINARY_DIR}/checkers.txt ${CMAKE_SOURCE_DIR}/src
		COMMENT "Running cppcheck"
	)
	add_dependencies(toy_http_server run_cppcheck)
else()
	message(WARNING "Cppcheck not found!")
endif()

add_compile_options(-Wall -Wextra -Werror)

# Tests
enable_testing()
add_subdirectory(tests)
