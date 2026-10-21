cmake_minimum_required(VERSION 3.24)
project(HKRegionHook VERSION 1.7.0 LANGUAGES C CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "This project targets Windows x86/x64.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(HKR_ARCH x64)
  set(HKR_HDE third_party/minhook/src/hde/hde64.c)
else()
  set(HKR_ARCH x86)
  set(HKR_HDE third_party/minhook/src/hde/hde32.c)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(payload IN ITEMS managed hmcl_agent)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${payload}_payload.h")
    message(FATAL_ERROR "Missing generated payload. Run scripts/build-release.ps1, or build-managed.ps1 and build-hmcl-agent.ps1 first.")
  endif()
endforeach()
foreach(pair IN ITEMS "managed/PclDownloadRestore.cs|managed" "java/HmclRegionAgent.java|hmcl_agent")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 source)
  list(GET parts 1 payload)
  if(payload STREQUAL "hmcl_agent")
    file(GLOB java_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/java/Hmcl*.java")
    set(java_hashes "")
    foreach(java_source IN LISTS java_sources)
      file(SHA256 "${java_source}" java_hash)
      get_filename_component(java_name "${java_source}" NAME)
      string(APPEND java_hashes "${java_name}:${java_hash}\n")
    endforeach()
    string(SHA256 source_hash "${java_hashes}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${java_sources})
  else()
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" source_hash)
  endif()
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/${payload}_payload.h" embedded_hash REGEX "^// Source SHA256: ")
  if(NOT embedded_hash STREQUAL "// Source SHA256: ${source_hash}")
    message(FATAL_ERROR "Stale ${payload} payload. Re-run scripts/build-release.ps1.")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}" "src/${payload}_payload.h")
endforeach()

add_library(minhook STATIC third_party/minhook/src/buffer.c third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c ${HKR_HDE})
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
  enable_language(ASM_MASM)
  set(HKR_ASSEMBLER masm)
  set(HKR_THUNKS proxy_thunks.asm)
else()
  enable_language(ASM)
  set(HKR_ASSEMBLER gas)
  set(HKR_THUNKS proxy_thunks.S)
endif()
set(HKR_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-proxy.py"
  --output "${HKR_GENERATED}" --architecture "${HKR_ARCH}" --assembler "${HKR_ASSEMBLER}"
  COMMAND_ERROR_IS_FATAL ANY)
configure_file(resources/licenses.rc.in "${HKR_GENERATED}/licenses.rc" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS scripts/generate-proxy.py src/proxy_exports.json)

add_library(VersionProxy SHARED src/hook.cpp src/universal_proxy.cpp src/pcl_restore.cpp
  src/hmcl_agent.cpp src/hmcl_launcher.cpp src/hmcl_archive.cpp "${HKR_GENERATED}/${HKR_THUNKS}"
  "${HKR_GENERATED}/proxy.def" "${HKR_GENERATED}/licenses.rc")
target_include_directories(VersionProxy PUBLIC include PRIVATE "${HKR_GENERATED}")
target_link_libraries(VersionProxy PRIVATE minhook advapi32 oleaut32 shell32)
target_compile_definitions(VersionProxy PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  WINVER=0x0A00 _WIN32_WINNT=0x0A00)
if(MSVC)
  target_compile_options(VersionProxy PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/W4;/permissive-;/utf-8;/EHsc>")
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_options(VersionProxy PRIVATE "$<$<COMPILE_LANGUAGE:ASM_MASM>:/safeseh>")
  endif()
else()
  target_compile_options(VersionProxy PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")
  target_link_options(VersionProxy PRIVATE -static -static-libgcc -static-libstdc++)
endif()
set_target_properties(VersionProxy PROPERTIES PREFIX "" OUTPUT_NAME version
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin")
add_custom_command(TARGET VersionProxy POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:VersionProxy>" "$<TARGET_FILE_DIR:VersionProxy>/winmm.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:VersionProxy>" "$<TARGET_FILE_DIR:VersionProxy>/winhttp.dll")

include(CTest)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  add_executable(ArchiveProbe tests/hmcl/ArchiveProbe.cpp src/hmcl_archive.cpp)
  target_include_directories(ArchiveProbe PRIVATE include)
  target_compile_definitions(ArchiveProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  if(MINGW)
    target_link_options(ArchiveProbe PRIVATE -municode -static -static-libgcc -static-libstdc++)
  endif()
  add_test(NAME hmcl_archive_structure COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hmcl/archive-tests.py" "$<TARGET_FILE:ArchiveProbe>" "${CMAKE_BINARY_DIR}/archive-tests")
  set_tests_properties(hmcl_archive_structure PROPERTIES TIMEOUT 30)
  find_program(HKR_CSC NAMES csc.exe PATHS
    "$ENV{WINDIR}/Microsoft.NET/Framework64/v4.0.30319"
    "$ENV{WINDIR}/Microsoft.NET/Framework/v4.0.30319" NO_DEFAULT_PATH REQUIRED)
  set(HKR_PROBE "${CMAKE_BINARY_DIR}/bin/ProxyArchitectureProbe.exe")
  # The Framework C# compiler misparses absolute paths containing forward slashes.
  file(TO_NATIVE_PATH "${HKR_PROBE}" HKR_PROBE_NATIVE)
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/ProxyArchitectureProbe.cs" HKR_PROBE_SOURCE)
  add_custom_command(OUTPUT "${HKR_PROBE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bin"
    COMMAND "${HKR_CSC}" /nologo "/platform:${HKR_ARCH}" "/out:${HKR_PROBE_NATIVE}"
            "${HKR_PROBE_SOURCE}"
    DEPENDS tests/ProxyArchitectureProbe.cs VERBATIM)
  add_custom_target(ArchitectureProbe ALL DEPENDS "${HKR_PROBE}")
  foreach(profile IN ITEMS version winhttp winmm)
    add_test(NAME architecture_${profile}
      COMMAND "${HKR_PROBE}" "$<TARGET_FILE_DIR:VersionProxy>/${profile}.dll" "${CMAKE_BINARY_DIR}/architecture-tests")
    set_tests_properties(architecture_${profile} PROPERTIES TIMEOUT 30)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      add_test(NAME universal_${profile}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/universal_proxy.py"
                "$<TARGET_FILE_DIR:VersionProxy>/${profile}.dll")
      add_test(NAME extraction_${profile}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/hmcl_agent.py"
                "$<TARGET_FILE_DIR:VersionProxy>/${profile}.dll" "${CMAKE_BINARY_DIR}/extraction-tests")
      set_tests_properties(universal_${profile} extraction_${profile} PROPERTIES TIMEOUT 30)
    endif()
  endforeach()
endif()
