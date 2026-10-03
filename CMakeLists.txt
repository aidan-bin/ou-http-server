cmake_minimum_required(VERSION 3.20)
project(toy_http_server)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB HTTP_LIB_SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h)
list(REMOVE_ITEM HTTP_LIB_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

option(DISABLE_HTTPS "Disable HTTPS support" OFF)

add_compile_options(-Wall -Wextra -Werror)

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

add_library(http_lib ${HTTP_LIB_SOURCES})
target_link_libraries(http_lib PRIVATE pthread ${SSL_LIBS})
target_include_directories(http_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_executable(toy_http_server src/main.cpp)
target_link_libraries(toy_http_server PRIVATE http_lib ${SSL_LIBS})

find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)

if (CLANG_TIDY_EXECUTABLE)
	set(TIDY_SOURCES ${HTTP_LIB_SOURCES} ${CMAKE_SOURCE_DIR}/src/main.cpp ${CMAKE_SOURCE_DIR}/tests/test_http.cpp)
	list(FILTER TIDY_SOURCES INCLUDE REGEX "\\.cpp$")
	
	if (APPLE)
		execute_process(COMMAND xcrun --show-sdk-path
			OUTPUT_VARIABLE MACOSX_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
		set(TIDY_EXTRA_ARGS "--extra-arg-before=-isysroot${MACOSX_SDK_PATH}")
	endif()

	add_custom_target(tidy
		COMMAND ${CLANG_TIDY_EXECUTABLE} -p ${CMAKE_BINARY_DIR} --quiet
			"--header-filter=${CMAKE_SOURCE_DIR}/(src|tests)/" ${TIDY_EXTRA_ARGS} ${TIDY_SOURCES}
		COMMENT "Running clang-tidy over src/ and tests/"
		VERBATIM)
else()
	message(WARNING "Clang-tidy not found; `tidy` target unavailable.")
endif()

enable_testing()
add_subdirectory(tests)
