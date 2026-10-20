cmake_minimum_required(VERSION 3.20)
project(ou_http_server)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB HTTP_LIB_SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h ${CMAKE_SOURCE_DIR}/include/ou/http/*.h)

add_compile_options(-Wall -Wextra -Werror)

set(SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined or thread")
if (SANITIZE)
	add_compile_options(-fsanitize=${SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
	add_link_options(-fsanitize=${SANITIZE})
endif()

option(COVERAGE "Instrument for clang source-based coverage" OFF)
if (COVERAGE)
	add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
	add_link_options(-fprofile-instr-generate)
endif()

option(FUZZ "Build the libFuzzer targets in fuzz/" OFF)
if (FUZZ)
	add_compile_options(-fsanitize=fuzzer-no-link)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
	add_compile_options(-fexperimental-library)
	add_link_options(-fexperimental-library)
endif()

find_package(OpenSSL REQUIRED)

add_library(http_lib ${HTTP_LIB_SOURCES})
add_library(ou::http ALIAS http_lib)
target_link_libraries(http_lib PRIVATE OpenSSL::SSL OpenSSL::Crypto pthread)
target_include_directories(http_lib PUBLIC ${CMAKE_SOURCE_DIR}/include PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_library(demo_kvstore STATIC demo/KVStore.cpp)
target_link_libraries(demo_kvstore PUBLIC ou::http)
target_include_directories(demo_kvstore PUBLIC ${CMAKE_SOURCE_DIR}/demo)

add_executable(ou_http_server demo/main.cpp)
target_link_libraries(ou_http_server PRIVATE demo_kvstore)

enable_testing()
add_subdirectory(tests)
if (FUZZ)
	add_subdirectory(fuzz)
endif()

find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)

if (CLANG_TIDY_EXECUTABLE)
	get_target_property(TIDY_TEST_SOURCES test_http SOURCES)
	list(TRANSFORM TIDY_TEST_SOURCES PREPEND ${CMAKE_SOURCE_DIR}/tests/)
	set(TIDY_SOURCES ${HTTP_LIB_SOURCES} ${CMAKE_SOURCE_DIR}/demo/main.cpp ${CMAKE_SOURCE_DIR}/demo/KVStore.cpp ${TIDY_TEST_SOURCES})
	list(FILTER TIDY_SOURCES INCLUDE REGEX "\\.cpp$")
	
	if (APPLE)
		execute_process(COMMAND xcrun --show-sdk-path
			OUTPUT_VARIABLE MACOSX_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
		set(TIDY_EXTRA_ARGS "--extra-arg-before=-isysroot${MACOSX_SDK_PATH}")
	endif()

	add_custom_target(tidy
		COMMAND ${CLANG_TIDY_EXECUTABLE} -p ${CMAKE_BINARY_DIR} --quiet
			"--header-filter=${CMAKE_SOURCE_DIR}/(src|include|demo|tests)/" ${TIDY_EXTRA_ARGS} ${TIDY_SOURCES}
		COMMENT "Running clang-tidy"
		VERBATIM)
else()
	message(WARNING "Clang-tidy not found; `tidy` target unavailable.")
endif()
