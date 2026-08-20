# Hardware-AOI crop test against Basler camera emulation (PYLON_CAMEMU).
# Only builds when the Pylon backend (lpr_basler) is enabled.
if(TARGET lpr_basler)
  add_executable(test_aoi_emulated test_aoi_emulated.cpp)
  target_link_libraries(test_aoi_emulated PRIVATE lpr_basler)
  add_test(NAME aoi_emulated COMMAND test_aoi_emulated)
  # The emulation transport layer is picked up from PYLON_CAMEMU at runtime; the
  # test sets it itself, but also set it here so `ctest` runs are self-contained.
  set_tests_properties(aoi_emulated PROPERTIES ENVIRONMENT "PYLON_CAMEMU=2")

  # Colour-camera path: settings -> computeAoiCrop -> applyRoi (needs the facade).
  add_executable(test_aoi_rgb_emulated test_aoi_rgb_emulated.cpp)
  target_link_libraries(test_aoi_rgb_emulated PRIVATE lpr_basler)
  add_test(NAME aoi_rgb_emulated COMMAND test_aoi_rgb_emulated)
  set_tests_properties(aoi_rgb_emulated PROPERTIES ENVIRONMENT "PYLON_CAMEMU=2")

  # Two-camera pair: mono (always, ROI bbox) + colour (optional) cropped together.
  add_executable(test_aoi_pair_emulated test_aoi_pair_emulated.cpp)
  target_link_libraries(test_aoi_pair_emulated PRIVATE lpr_basler)
  add_test(NAME aoi_pair_emulated COMMAND test_aoi_pair_emulated)
  set_tests_properties(aoi_pair_emulated PROPERTIES ENVIRONMENT "PYLON_CAMEMU=2")
endif()
