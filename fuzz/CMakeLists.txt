# ctest replays the committed corpus; CI fuzzes for real
add_executable(fuzz_request_reader fuzz_request_reader.cpp)
target_link_libraries(fuzz_request_reader PRIVATE http_lib)
target_include_directories(fuzz_request_reader PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_options(fuzz_request_reader PRIVATE -fsanitize=fuzzer)
add_test(NAME fuzz_request_reader COMMAND fuzz_request_reader -runs=0 ${CMAKE_CURRENT_SOURCE_DIR}/corpus/request_reader)
