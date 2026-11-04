set(XENIA_IOS_SHADER_CONVERTER_LIBRARY "" CACHE FILEPATH
    "Compatible arm64 iPhoneOS libmetalirconverter.dylib (not the macOS library)")
if(NOT EXISTS "${XENIA_IOS_SHADER_CONVERTER_LIBRARY}")
  message(FATAL_ERROR "Set XENIA_IOS_SHADER_CONVERTER_LIBRARY to an iPhoneOS library.")
endif()
execute_process(COMMAND xcrun vtool -show-build "${XENIA_IOS_SHADER_CONVERTER_LIBRARY}"
  OUTPUT_VARIABLE _xe_msc_platform COMMAND_ERROR_IS_FATAL ANY)
# A simulator build (UI work only, no games) links a simulator library instead.
if(CMAKE_OSX_SYSROOT MATCHES "[iI][pP]hone[sS]imulator")
  if(NOT _xe_msc_platform MATCHES "platform IOSSIMULATOR")
    message(FATAL_ERROR "The selected shader converter must target the iOS Simulator.")
  endif()
elseif(NOT _xe_msc_platform MATCHES "platform IOS" OR
       _xe_msc_platform MATCHES "platform MACOS|platform IOSSIMULATOR")
  message(FATAL_ERROR "The selected shader converter must target iPhoneOS.")
endif()
set(_xe_msc_include "${PROJECT_SOURCE_DIR}/third_party/metal-shader-converter/include")
add_library(xenia-third-party-metal-shader-converter SHARED IMPORTED GLOBAL)
set_target_properties(xenia-third-party-metal-shader-converter PROPERTIES
  IMPORTED_LOCATION "${XENIA_IOS_SHADER_CONVERTER_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES
    "${_xe_msc_include};${_xe_msc_include}/metal_irconverter;${_xe_msc_include}/metal_irconverter_runtime"
  INTERFACE_COMPILE_DEFINITIONS IR_RUNTIME_METALCPP)
