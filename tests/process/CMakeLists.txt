if(TARGET lpr_process)
  add_executable(test_plate_processor test_plate_processor.cpp)
  target_link_libraries(test_plate_processor PRIVATE lpr_process)
  add_test(NAME plate_processor COMMAND test_plate_processor)

  add_executable(test_direction test_direction.cpp)
  target_link_libraries(test_direction PRIVATE lpr_process)
  add_test(NAME direction COMMAND test_direction)
endif()

# Pure exposure control law is header-only (no Pylon) -> always testable.
add_executable(test_exposure test_exposure.cpp)
target_include_directories(test_exposure PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME exposure COMMAND test_exposure)
