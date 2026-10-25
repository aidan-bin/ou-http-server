if (POLICY CMP0167)
	cmake_policy(SET CMP0167 NEW)
endif()

find_package(Boost REQUIRED)

file(GLOB TEST_SUITE_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS test_*.cpp)
list(REMOVE_ITEM TEST_SUITE_SOURCES test_main.cpp)

add_executable(test_http test_main.cpp ${TEST_SUITE_SOURCES})
target_link_libraries(test_http PRIVATE demo_kvstore OpenSSL::SSL OpenSSL::Crypto Boost::headers)
target_include_directories(test_http PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_compile_definitions(test_http PRIVATE TEST_CERT_DIR="${PROJECT_SOURCE_DIR}/example/certs")

# One ctest entry per suite, so they run in parallel
foreach(source IN LISTS TEST_SUITE_SOURCES)
	string(REGEX REPLACE "^test_(.*)\\.cpp$" "\\1" suite ${source})
	add_test(NAME ${suite} COMMAND test_http --run_test=${suite})
	# Fallback for deadlocks/hangs
	set_tests_properties(${suite} PROPERTIES TIMEOUT 60)
endforeach()
