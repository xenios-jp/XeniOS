set(XENIA_IOS_HOST_SHADER_COMPILER "" CACHE FILEPATH
    "Native macOS xenia-shader-cc built from this source revision with --ios")
if(NOT EXISTS "${XENIA_IOS_HOST_SHADER_COMPILER}")
  message(FATAL_ERROR
    "Set XENIA_IOS_HOST_SHADER_COMPILER to the native macOS tool. "
    "The cmake/ios/host_tools project can rebuild it with cached host libraries.")
endif()
add_executable(xenia-shader-cc IMPORTED GLOBAL)
set_target_properties(xenia-shader-cc PROPERTIES
  IMPORTED_LOCATION "${XENIA_IOS_HOST_SHADER_COMPILER}")
