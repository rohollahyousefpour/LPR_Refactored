add_executable(test_blocking_queue test_blocking_queue.cpp)
target_link_libraries(test_blocking_queue PRIVATE lpr_util Threads::Threads)
add_test(NAME blocking_queue COMMAND test_blocking_queue)

add_executable(test_base64 test_base64.cpp)
target_link_libraries(test_base64 PRIVATE lpr_core)
add_test(NAME base64 COMMAND test_base64)

add_executable(test_jaro_winkler test_jaro_winkler.cpp)
target_link_libraries(test_jaro_winkler PRIVATE lpr_core)
add_test(NAME jaro_winkler COMMAND test_jaro_winkler)

add_executable(test_uuid test_uuid.cpp)
target_link_libraries(test_uuid PRIVATE lpr_util)
add_test(NAME uuid COMMAND test_uuid)

add_executable(test_capture_path test_capture_path.cpp)
target_link_libraries(test_capture_path PRIVATE lpr_core)
add_test(NAME capture_path COMMAND test_capture_path)
