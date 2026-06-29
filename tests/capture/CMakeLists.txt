# SDK-free dispatch test: always builds.
add_executable(test_camera_kind test_camera_kind.cpp)
target_link_libraries(test_camera_kind PRIVATE lpr_util)   # include path only
add_test(NAME camera_kind COMMAND test_camera_kind)

# Capture-flow test needs OpenCV (FrameItem holds cv::Mat).
if(TARGET lpr_capture)
  add_executable(test_capture_flow test_capture_flow.cpp)
  target_link_libraries(test_capture_flow PRIVATE lpr_capture)
  add_test(NAME capture_flow COMMAND test_capture_flow)
endif()

if(TARGET lpr_capture)
  add_executable(test_dual_capture test_dual_capture.cpp)
  target_link_libraries(test_dual_capture PRIVATE lpr_capture)
  add_test(NAME dual_capture COMMAND test_dual_capture)
endif()
