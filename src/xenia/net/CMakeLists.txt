add_library(xenia-net STATIC)
xe_platform_sources(xenia-net ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-net PUBLIC mbedtls xenia-base PRIVATE asio)
if(WIN32)
  target_link_libraries(xenia-net PRIVATE iphlpapi)
elseif(APPLE)
  # xe_platform_sources globs *.cc only; .mm sources need explicit listing.
  target_sources(xenia-net PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/device_signal_apple.mm
    ${CMAKE_CURRENT_SOURCE_DIR}/lobby_socket_apple.mm
    ${CMAKE_CURRENT_SOURCE_DIR}/network_path_apple.mm
  )
  target_link_libraries(xenia-net PRIVATE "-framework Foundation" "-framework Network" "-framework Security")
  if(NOT XE_PLATFORM_IOS)
    target_link_libraries(xenia-net PRIVATE "-framework IOKit")
  endif()
endif()
xe_target_defaults(xenia-net)

if(XENIA_BUILD_TESTS)
  add_subdirectory(testing)
endif()
