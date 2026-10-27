# Helper functions for the xenia build.

include(CMakeParseArguments)

set(XE_PLATFORM_SUFFIXES
  _win _linux _posix _gnulinux _x11 _gtk _android _mac _amd64 _x64 _arm64
)

# xe_platform_sources(target base_path [RECURSIVE])
#
# Globs source files from base_path and adds them to target. Excludes
# *_main.cc / *_test.cc / *_demo.cc, and any file with a platform suffix
# that doesn't match the current platform. RECURSIVE descends into
# subdirectories.
function(xe_platform_sources target base_path)
  set(options RECURSIVE)
  cmake_parse_arguments(ARG "${options}" "" "" ${ARGN})

  if(ARG_RECURSIVE)
    set(glob_mode GLOB_RECURSE)
  else()
    set(glob_mode GLOB)
  endif()

  file(${glob_mode} _all_sources
    "${base_path}/*.h"
    "${base_path}/*.cc"
    "${base_path}/*.cpp"
    "${base_path}/*.c"
    "${base_path}/*.inc"
  )

  set(_excluded)
  foreach(src ${_all_sources})
    get_filename_component(_name_we ${src} NAME_WE)
    if(_name_we MATCHES "_main$" OR _name_we MATCHES "_test$" OR _name_we MATCHES "_demo$")
      list(APPEND _excluded ${src})
      continue()
    endif()
    foreach(suffix ${XE_PLATFORM_SUFFIXES})
      if(_name_we MATCHES "${suffix}$")
        list(APPEND _excluded ${src})
        break()
      endif()
    endforeach()
  endforeach()

  set(_sources ${_all_sources})
  if(_excluded)
    list(REMOVE_ITEM _sources ${_excluded})
  endif()

  if(WIN32)
    file(${glob_mode} _plat_sources
      "${base_path}/*_win.h"
      "${base_path}/*_win.cc"
    )
  elseif(APPLE)
    file(${glob_mode} _plat_sources
      "${base_path}/*_posix.h"
      "${base_path}/*_posix.cc"
      "${base_path}/*_mac.h"
      "${base_path}/*_mac.cc"
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(${glob_mode} _plat_sources
      "${base_path}/*_posix.h"
      "${base_path}/*_posix.cc"
      "${base_path}/*_linux.h"
      "${base_path}/*_linux.cc"
      "${base_path}/*_gnulinux.h"
      "${base_path}/*_gnulinux.cc"
      "${base_path}/*_x11.h"
      "${base_path}/*_x11.cc"
      "${base_path}/*_gtk.h"
      "${base_path}/*_gtk.cc"
    )
  endif()

  list(APPEND _sources ${_plat_sources})

  # Add back architecture-specific files
  if(XE_TARGET_X86_64)
    file(${glob_mode} _arch_sources "${base_path}/*_amd64.h" "${base_path}/*_amd64.cc"
      "${base_path}/*_x64.h" "${base_path}/*_x64.cc")
  elseif(XE_TARGET_AARCH64)
    file(${glob_mode} _arch_sources "${base_path}/*_arm64.h" "${base_path}/*_arm64.cc")
  endif()
  if(_arch_sources)
    list(APPEND _sources ${_arch_sources})
  endif()

  target_sources(${target} PRIVATE ${_sources})
endfunction()

# xe_target_defaults(target)
#
# Applies xenia-wide compile defaults to a target: project-root include
# directories and warnings-as-errors. GCC is excluded from -Werror because
# it's too noisy to keep clean across the codebase.
function(xe_target_defaults target)
  target_include_directories(${target} PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/third_party
  )
  if(MSVC)
    target_compile_options(${target} PRIVATE /WX)
  elseif(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# xe_windowed_app_main(target)
#
# Adds the per-platform wx entry point a WindowedApp executable needs.
# src/xenia/ui/CMakeLists.txt keeps these out of the xenia-ui library, so every
# windowed executable has to name one itself.
function(xe_windowed_app_main target)
  if(WIN32)
    set(_main windowed_app_main_win.cc)
  elseif(APPLE)
    set(_main windowed_app_main_mac.cc)
  else()
    set(_main windowed_app_main_linux.cc)
  endif()
  target_sources(${target} PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/${_main})
endfunction()

# xe_embed_binary_assets(target source_dir output_namespace)
#
# Embeds every file in source_dir as a static byte array in a generated
# embedded_<namespace>.{h,cc} pair. Symbol names mirror the filename with
# '-' and '.' replaced by '_'. Generated files land in the build dir and
# are added to target's sources / include path. Changing an asset re-runs
# configure on the next build; adding or removing one needs a reconfigure, like
# sources (no CONFIGURE_DEPENDS, which re-checks the glob on every build).
function(xe_embed_binary_assets target source_dir output_namespace)
  file(GLOB asset_files "${source_dir}/*")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      ${asset_files})
  set(header_path "${CMAKE_CURRENT_BINARY_DIR}/embedded_${output_namespace}.h")
  set(source_path "${CMAKE_CURRENT_BINARY_DIR}/embedded_${output_namespace}.cc")
  set(ns "xe::ui::embedded_${output_namespace}")

  # Buffer to a single string; unquoted expansion to file(WRITE) would split
  # on ';' and silently drop every C++ semicolon.
  set(header_buf "// Auto-generated from ${source_dir}.\n")
  string(APPEND header_buf "#pragma once\n#include <cstddef>\n")
  string(APPEND header_buf "namespace ${ns} {\n")
  string(APPEND header_buf "struct Asset { const char* name; const unsigned char* data; size_t size; };\n")

  set(source_buf "// Auto-generated from ${source_dir}.\n")
  string(APPEND source_buf "#include \"embedded_${output_namespace}.h\"\n")
  string(APPEND source_buf "namespace ${ns} {\n")

  set(index_lines "")
  foreach(path ${asset_files})
    if(IS_DIRECTORY "${path}")
      continue()
    endif()
    get_filename_component(name "${path}" NAME)
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" symbol "${name}")
    if("${symbol}" MATCHES "^[0-9]")
      set(symbol "_${symbol}")
    endif()
    file(READ "${path}" hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE ",$" "" bytes "${bytes}")
    string(APPEND header_buf "extern const unsigned char ${symbol}_data[];\n")
    string(APPEND header_buf "extern const size_t ${symbol}_size;\n")
    string(APPEND source_buf "const unsigned char ${symbol}_data[] = { ${bytes} };\n")
    string(APPEND source_buf "const size_t ${symbol}_size = sizeof(${symbol}_data);\n")
    list(APPEND index_lines
        "  { \"${name}\", ${symbol}_data, sizeof(${symbol}_data) }")
  endforeach()

  string(JOIN ",\n" index_body ${index_lines})
  string(APPEND header_buf "extern const Asset kAll[];\n")
  string(APPEND header_buf "extern const size_t kAllCount;\n")
  string(APPEND header_buf "}  // namespace ${ns}\n")
  string(APPEND source_buf "const Asset kAll[] = {\n${index_body}\n};\n")
  string(APPEND source_buf "const size_t kAllCount = sizeof(kAll) / sizeof(kAll[0]);\n")
  string(APPEND source_buf "}  // namespace ${ns}\n")

  # configure_file COPYONLY skips the copy when bytes match, preserving mtime
  # so reconfigures don't force a multi-MB embedded_*.cc recompile.
  file(WRITE "${header_path}.in" "${header_buf}")
  configure_file("${header_path}.in" "${header_path}" COPYONLY)
  file(REMOVE "${header_path}.in")
  file(WRITE "${source_path}.in" "${source_buf}")
  configure_file("${source_path}.in" "${source_path}" COPYONLY)
  file(REMOVE "${source_path}.in")

  target_sources(${target} PRIVATE "${source_path}")
  target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# xe_embed_compressed_bundle(target source_dir output_namespace)
#
# Packs every file in source_dir into a zlib-compressed concat blob and
# embeds it as a single byte array in embedded_bundle_<namespace>.{h,cc}.
# Symbols: xe::embedded_bundle_<ns>::kBundleData / kBundleSize. Decompress
# at runtime with EmbeddedBundle (xenia/patcher). Re-run cmake configure
# after adding/changing assets.
#
# Runs the generator at configure time. The script's write_if_changed keeps
# the .cc/.h untouched when the compressed bundle is byte-identical, so no
# downstream recompile happens on no-op runs. We don't use add_custom_command
# here because game-patches filenames contain spaces, which break CMake's
# ninja-generator dependency escaping.
function(xe_embed_compressed_bundle target source_dir output_namespace)
  set(script "${PROJECT_SOURCE_DIR}/tools/build/embed_bundle.py")
  execute_process(
    COMMAND ${Python3_EXECUTABLE} "${script}"
            "${source_dir}" "${CMAKE_CURRENT_BINARY_DIR}" "${output_namespace}"
    RESULT_VARIABLE _xe_eb_rc)
  if(NOT _xe_eb_rc EQUAL 0)
    message(FATAL_ERROR "embed_bundle.py failed for ${source_dir}")
  endif()
  target_sources(${target} PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}/embedded_bundle_${output_namespace}.cc")
  target_include_directories(${target} PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# xe_shader_rules_spirv(target shader_dir)
#
# Wires up SPIR-V shader compilation via the in-tree xenia-shader-cc host
# tool (glslang-based) as a prerequisite of target. Sources are *.xesl /
# *.glsl with a stage suffix (vs/hs/ds/gs/ps/cs); outputs land in the build
# tree at ${PROJECT_BINARY_DIR}/generated/<src-relative>/bytecode/vulkan_spirv/
# and the generated root is added to the target's include path so existing
# `#include "xenia/.../bytecode/vulkan_spirv/<id>.h"` continues to resolve.
function(xe_shader_rules_spirv target shader_dir)
  get_filename_component(shader_dir "${shader_dir}" ABSOLUTE)
  file(GLOB _sources
    "${shader_dir}/*.xesl" "${shader_dir}/*.glsl"
    "${shader_dir}/*.xesli" "${shader_dir}/*.glsli")
  file(RELATIVE_PATH _rel_dir "${PROJECT_SOURCE_DIR}/src" "${shader_dir}")
  set(_generated_root "${PROJECT_BINARY_DIR}/generated")
  set(_bytecode_dir "${_generated_root}/${_rel_dir}/bytecode/vulkan_spirv")
  set(_valid_stages vs hs ds gs ps cs)
  set(_outputs)
  file(MAKE_DIRECTORY "${_bytecode_dir}")
  foreach(src ${_sources})
    get_filename_component(_name ${src} NAME)
    string(REGEX REPLACE "\\.[^.]+$" "" _basename "${_name}")
    string(REPLACE "." "_" _id "${_basename}")
    string(LENGTH "${_id}" _len)
    if(_len LESS 3)
      continue()
    endif()
    math(EXPR _s "${_len} - 2")
    string(SUBSTRING "${_id}" ${_s} 2 _stage)
    if(NOT _stage IN_LIST _valid_stages)
      continue()
    endif()
    set(_out "${_bytecode_dir}/${_id}.h")
    set(_dep "${_out}.d")
    list(APPEND _outputs "${_out}")
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND $<TARGET_FILE:xenia-shader-cc> --depfile "${_dep}"
              "${src}" "${_out}"
      DEPENDS "${src}" xenia-shader-cc
      DEPFILE "${_dep}"
      COMMENT "SPIR-V: ${_name}"
      VERBATIM
    )
  endforeach()
  add_custom_target(${target}-spirv-shaders DEPENDS ${_outputs})
  add_dependencies(${target} ${target}-spirv-shaders)
  target_include_directories(${target} BEFORE PRIVATE "${_generated_root}")
  # Attach sources to the target for IDE visibility without letting VS
  # try to compile them as C++.
  set_source_files_properties(${_sources} PROPERTIES HEADER_FILE_ONLY TRUE)
  target_sources(${target} PRIVATE ${_sources})
endfunction()

# xe_shader_rules_metal(target shader_dir)
#
# Metal counterpart to xe_shader_rules_spirv. Runs
# xenia-shader-cc --msl on each cs/ps/vs-stage *.xesl file under shader_dir,
# emitting to the build tree under ${PROJECT_BINARY_DIR}/generated/<src-
# relative>/bytecode/metal/ with the metallib bytes embedded as
# `const uint8_t <id>_metallib[]`. .glsl / .hlsl / fxaa / ffx_ sources are
# skipped (no MSL branch in those polyglots).
function(xe_shader_rules_metal target shader_dir)
  if(NOT APPLE)
    return()
  endif()
  get_filename_component(shader_dir "${shader_dir}" ABSOLUTE)
  file(GLOB _sources "${shader_dir}/*.xesl" "${shader_dir}/*.xesli")
  file(RELATIVE_PATH _rel_dir "${PROJECT_SOURCE_DIR}/src" "${shader_dir}")
  set(_generated_root "${PROJECT_BINARY_DIR}/generated")
  set(_bytecode_dir "${_generated_root}/${_rel_dir}/bytecode/metal")
  set(_valid_stages vs ps cs)
  set(_outputs)
  file(MAKE_DIRECTORY "${_bytecode_dir}")
  foreach(src ${_sources})
    get_filename_component(_name ${src} NAME)
    if(_name MATCHES "^fxaa" OR _name MATCHES "ffx_")
      continue()
    endif()
    string(REGEX REPLACE "\\.[^.]+$" "" _basename "${_name}")
    string(REPLACE "." "_" _id "${_basename}")
    string(LENGTH "${_id}" _len)
    if(_len LESS 3)
      continue()
    endif()
    math(EXPR _s "${_len} - 2")
    string(SUBSTRING "${_id}" ${_s} 2 _stage)
    if(NOT _stage IN_LIST _valid_stages)
      continue()
    endif()
    set(_out "${_bytecode_dir}/${_id}.h")
    set(_dep "${_out}.d")
    list(APPEND _outputs "${_out}")
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND $<TARGET_FILE:xenia-shader-cc> --msl --depfile "${_dep}"
              "${src}" "${_out}"
      DEPENDS "${src}" xenia-shader-cc
      DEPFILE "${_dep}"
      COMMENT "Metal: ${_name}"
      VERBATIM
    )
  endforeach()
  add_custom_target(${target}-metal-shaders DEPENDS ${_outputs})
  add_dependencies(${target} ${target}-metal-shaders)
  target_include_directories(${target} BEFORE PRIVATE "${_generated_root}")
  set_source_files_properties(${_sources} PROPERTIES HEADER_FILE_ONLY TRUE)
  target_sources(${target} PRIVATE ${_sources})
endfunction()

# xe_shader_rules_slang(target shader_dir TARGET dxil|spirv|msl)
#
# Compiles *.slang files via xenia-shader-cc, which drives slangc. slangc is
# taken from SLANGC_PATH, or the .slang/ tree populated by
# `./xenia-build.py slang`. One call per backend; per-backend CMakeLists invokes it with
# the appropriate TARGET option to populate its bytecode tree:
#   dxil  -> bytecode/d3d12_dxil/<id>.h     (const uint8_t)
#   spirv -> bytecode/vulkan_spirv/<id>.h   (const uint32_t)
#   msl   -> bytecode/metal/<id>.h          (const uint8_t, _metallib suffix)
function(xe_shader_rules_slang target shader_dir)
  cmake_parse_arguments(ARG "" "TARGET" "" ${ARGN})
  if(NOT ARG_TARGET)
    message(FATAL_ERROR "xe_shader_rules_slang: TARGET is required")
  endif()
  if(ARG_TARGET STREQUAL "dxil")
    set(_subdir "d3d12_dxil")
    set(_flag "--slang-dxil")
    set(_label "DXIL")
  elseif(ARG_TARGET STREQUAL "spirv")
    set(_subdir "vulkan_spirv")
    set(_flag "--slang-spirv")
    set(_label "SPIR-V")
  elseif(ARG_TARGET STREQUAL "msl")
    if(NOT APPLE)
      return()
    endif()
    set(_subdir "metal")
    set(_flag "--slang-msl")
    set(_label "Metal")
  else()
    message(FATAL_ERROR
            "xe_shader_rules_slang: TARGET must be dxil|spirv|msl")
  endif()

  get_filename_component(shader_dir "${shader_dir}" ABSOLUTE)
  file(GLOB _sources "${shader_dir}/*.slang")
  file(RELATIVE_PATH _rel_dir "${PROJECT_SOURCE_DIR}/src" "${shader_dir}")
  set(_generated_root "${PROJECT_BINARY_DIR}/generated")
  set(_bytecode_dir "${_generated_root}/${_rel_dir}/bytecode/${_subdir}")
  set(_valid_stages vs hs ds gs ps cs)
  set(_outputs)
  file(MAKE_DIRECTORY "${_bytecode_dir}")

  # Locate slangc: an explicit SLANGC_PATH wins, otherwise use the copy
  # downloaded by `./xenia-build.py slang` under .slang/<version>/.
  if(DEFINED ENV{SLANGC_PATH} AND NOT "$ENV{SLANGC_PATH}" STREQUAL "")
    set(_slangc "$ENV{SLANGC_PATH}")
  else()
    file(GLOB _slangc_found
         "${PROJECT_SOURCE_DIR}/.slang/*/bin/slangc"
         "${PROJECT_SOURCE_DIR}/.slang/*/bin/slangc.exe")
    if(_slangc_found)
      list(GET _slangc_found 0 _slangc)
    else()
      set(_slangc "")
    endif()
  endif()

  foreach(src ${_sources})
    get_filename_component(_name ${src} NAME)
    string(REGEX REPLACE "\\.[^.]+$" "" _basename "${_name}")
    string(REPLACE "." "_" _id "${_basename}")
    string(LENGTH "${_id}" _len)
    if(_len LESS 3)
      continue()
    endif()
    math(EXPR _s "${_len} - 2")
    string(SUBSTRING "${_id}" ${_s} 2 _stage)
    if(NOT _stage IN_LIST _valid_stages)
      continue()
    endif()
    # `// XE_MSL_ONLY` — skip dxil and spirv (Metal-only shader).
    if(NOT ARG_TARGET STREQUAL "msl")
      file(STRINGS "${src}" _xe_msl_only LIMIT_COUNT 1
           REGEX "^[ \t]*//[ \t]*XE_MSL_ONLY")
      if(_xe_msl_only)
        continue()
      endif()
    endif()
    # For spirv/msl: defer to the legacy spirv/metal rules when a hand-tuned
    # .glsl/.xesl twin still exists at the same id, so we don't double-generate
    # the same output header. Once the .slang version is proven for those
    # backends and the twin is deleted, this skip stops kicking in.
    if(ARG_TARGET STREQUAL "spirv" OR ARG_TARGET STREQUAL "msl")
      if(EXISTS "${shader_dir}/${_basename}.glsl"
         OR EXISTS "${shader_dir}/${_basename}.xesl")
        continue()
      endif()
      # Source-level opt-outs:
      #   `// XE_DXIL_ONLY` — skip both spirv and msl (D3D12-only shader).
      #   `// XE_NO_MSL`    — skip only msl (cross-target sans Metal).
      file(STRINGS "${src}" _xe_dxil_only LIMIT_COUNT 1
           REGEX "^[ \t]*//[ \t]*XE_DXIL_ONLY")
      if(_xe_dxil_only)
        continue()
      endif()
      if(ARG_TARGET STREQUAL "msl")
        file(STRINGS "${src}" _xe_no_msl LIMIT_COUNT 1
             REGEX "^[ \t]*//[ \t]*XE_NO_MSL")
        if(_xe_no_msl)
          continue()
        endif()
      endif()
    endif()
    if(NOT _slangc)
      message(FATAL_ERROR
        "slangc not found. Run `./xenia-build.py slang` to download it, or "
        "set SLANGC_PATH to an existing slangc.")
    endif()
    set(_out "${_bytecode_dir}/${_id}.h")
    set(_dep "${_out}.d")
    list(APPEND _outputs "${_out}")
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND ${CMAKE_COMMAND} -E env "SLANGC_PATH=${_slangc}"
              $<TARGET_FILE:xenia-shader-cc> ${_flag} --depfile "${_dep}"
              "${src}" "${_out}"
      DEPENDS "${src}" xenia-shader-cc
      DEPFILE "${_dep}"
      COMMENT "Slang/${_label}: ${_name}"
      VERBATIM
    )
  endforeach()
  if(NOT _outputs)
    return()
  endif()
  add_custom_target(${target}-slang-${ARG_TARGET}-shaders DEPENDS ${_outputs})
  add_dependencies(${target} ${target}-slang-${ARG_TARGET}-shaders)
  target_include_directories(${target} BEFORE PRIVATE "${_generated_root}")
  set_source_files_properties(${_sources} PROPERTIES HEADER_FILE_ONLY TRUE)
  target_sources(${target} PRIVATE ${_sources})
endfunction()

# xe_force_c(files...) — compile the given sources as C.
function(xe_force_c)
  set_source_files_properties(${ARGN} PROPERTIES LANGUAGE C)
endfunction()

# xe_force_cxx(files...) — compile the given sources as C++.
function(xe_force_cxx)
  set_source_files_properties(${ARGN} PROPERTIES LANGUAGE CXX)
endfunction()

# xe_test_suite(name base_path LINKS lib1 lib2 ...)
#
# Creates a Catch2 test executable from *_test.cc files in base_path.
# Returns early (no target) if no test sources are found.
function(xe_test_suite name base_path)
  cmake_parse_arguments(ARG "" "" "LINKS" ${ARGN})

  file(GLOB _test_sources "${base_path}/*_test.cc")
  if(NOT _test_sources)
    return()
  endif()

  add_executable(${name}
    ${_test_sources}
    ${PROJECT_SOURCE_DIR}/tools/build/src/test_suite_main.cc
  )

  if(WIN32)
    target_sources(${name} PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc)
  else()
    target_sources(${name} PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc)
  endif()

  target_compile_definitions(${name} PRIVATE
    "XE_TEST_SUITE_NAME=\"${name}\""
  )
  target_include_directories(${name} PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/build
    ${PROJECT_SOURCE_DIR}/tools/build/src
    ${PROJECT_SOURCE_DIR}/tools/build/third_party/catch/include
  )
  if(ARG_LINKS)
    target_link_libraries(${name} PRIVATE ${ARG_LINKS})
  endif()
  xe_target_defaults(${name})
endfunction()
