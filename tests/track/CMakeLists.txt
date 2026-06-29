if(TARGET lpr_track)
  add_executable(test_vehicle_decode test_vehicle_decode.cpp)
  target_link_libraries(test_vehicle_decode PRIVATE lpr_track)
  add_test(NAME vehicle_decode COMMAND test_vehicle_decode)

  add_executable(test_vehicle_tracker test_vehicle_tracker.cpp)
  target_link_libraries(test_vehicle_tracker PRIVATE lpr_track)
  add_test(NAME vehicle_tracker COMMAND test_vehicle_tracker)

  add_executable(test_vehicle_pipeline test_vehicle_pipeline.cpp)
  target_link_libraries(test_vehicle_pipeline PRIVATE lpr_track)
  add_test(NAME vehicle_pipeline COMMAND test_vehicle_pipeline)
endif()

if(TARGET lpr_track)
  add_executable(test_plate_tracking test_plate_tracking.cpp)
  target_link_libraries(test_plate_tracking PRIVATE lpr_track)
  add_test(NAME plate_tracking COMMAND test_plate_tracking)
endif()
