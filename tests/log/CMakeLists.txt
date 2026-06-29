add_executable(test_log test_log.cpp)
target_link_libraries(test_log PRIVATE lpr_log)
add_test(NAME log COMMAND test_log)
