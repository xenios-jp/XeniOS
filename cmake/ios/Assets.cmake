set(_xe_ios_catalog "${PROJECT_SOURCE_DIR}/assets/apple/XeniOSAssets.xcassets")
set(_xe_ios_assets "${CMAKE_CURRENT_BINARY_DIR}/ios-assets")
file(GLOB_RECURSE _xe_ios_asset_inputs CONFIGURE_DEPENDS "${_xe_ios_catalog}/*")
add_custom_command(
  OUTPUT "${_xe_ios_assets}/Assets.car" "${_xe_ios_assets}/asset-info.plist"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_xe_ios_assets}"
  COMMAND xcrun actool "${_xe_ios_catalog}"
    --compile "${_xe_ios_assets}" --platform ${XE_IOS_SDK}
    --minimum-deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    --target-device iphone --target-device ipad
    --app-icon AppIcon --include-all-app-icons
    --output-partial-info-plist "${_xe_ios_assets}/asset-info.plist"
    --output-format human-readable-text
  DEPENDS ${_xe_ios_asset_inputs}
  VERBATIM)
add_custom_target(xenia-app-ios-assets DEPENDS
  "${_xe_ios_assets}/Assets.car" "${_xe_ios_assets}/asset-info.plist")
add_dependencies(xenia-app xenia-app-ios-assets)
add_custom_command(TARGET xenia-app POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/build/merge_ios_assets.py"
    "${_xe_ios_assets}" "$<TARGET_BUNDLE_DIR:xenia-app>"
  VERBATIM)
