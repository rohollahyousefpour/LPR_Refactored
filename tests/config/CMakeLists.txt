# Dependency-light tests: build the source directly, no OpenCV needed.
add_executable(test_app_config test_app_config.cpp ${CMAKE_SOURCE_DIR}/src/config/AppConfig.cpp)
target_include_directories(test_app_config PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME app_config COMMAND test_app_config)

add_executable(test_lpr_settings test_lpr_settings.cpp ${CMAKE_SOURCE_DIR}/src/config/LprSettings.cpp)
target_include_directories(test_lpr_settings PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_lpr_settings PRIVATE lpr_json)
add_test(NAME lpr_settings COMMAND test_lpr_settings)
