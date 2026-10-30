if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
   NOT CMAKE_OSX_SYSROOT MATCHES "[iI][pP]hone[oO][sS]")
  message(FATAL_ERROR
    "The iOS app supports devices only: use arm64 and the iPhoneOS SDK.")
endif()
if(XENIA_BUILD_MISC OR XENIA_BUILD_TESTS)
  message(FATAL_ERROR "Desktop misc/test targets are not part of the iOS app build.")
endif()

# Use Edge's existing generator without invoking bootstrap or downloading data.
add_custom_target(xenia-ios-version
  COMMAND "${Python3_EXECUTABLE}" -c
    "import runpy; runpy.run_path('xenia-build.py')['generate_version_h']('${CMAKE_BINARY_DIR}')"
  BYPRODUCTS "${CMAKE_BINARY_DIR}/version.h"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  VERBATIM)

function(xe_ios_target_defaults target)
  add_dependencies(${target} xenia-ios-version)
  target_compile_options(${target} PRIVATE
    "$<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fblocks>"
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-switch>")
endfunction()
