enable_language(RC)

set(CODEXPETS_WINDOWS_ARCH "${CODEXPETS_WINDOWS_ARCH}" CACHE STRING
    "Windows target architecture (x64 or arm64)")
if(CODEXPETS_WINDOWS_ARCH STREQUAL "")
    if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64")
        set(CODEXPETS_WINDOWS_ARCH "arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(CODEXPETS_WINDOWS_ARCH "arm64")
    else()
        set(CODEXPETS_WINDOWS_ARCH "x64")
    endif()
endif()
string(TOLOWER "${CODEXPETS_WINDOWS_ARCH}" CODEXPETS_WINDOWS_ARCH)
if(NOT CODEXPETS_WINDOWS_ARCH MATCHES "^(x64|arm64)$")
    message(FATAL_ERROR "Unsupported Windows architecture: ${CODEXPETS_WINDOWS_ARCH}")
endif()
set(CODEXPETS_WEBVIEW2_LOADER
    "${CMAKE_SOURCE_DIR}/third_party/webview2/${CODEXPETS_WINDOWS_ARCH}/WebView2Loader.dll")
if(NOT EXISTS "${CODEXPETS_WEBVIEW2_LOADER}")
    message(FATAL_ERROR "Missing WebView2Loader.dll: ${CODEXPETS_WEBVIEW2_LOADER}")
endif()
file(TO_CMAKE_PATH "${CODEXPETS_WEBVIEW2_LOADER}" CODEXPETS_WEBVIEW2_LOADER_RC_PATH)

add_executable(CodeXPets WIN32
    main.cpp
    native_app.cpp
    renderer.cpp
    resources.rc
    xiaomi_transport.cpp
    xiaomi_browser_login.cpp
    xiaomi_credentials.cpp
)
target_include_directories(CodeXPets PRIVATE "${CMAKE_SOURCE_DIR}/third_party/webview2/include")
target_link_libraries(CodeXPets PRIVATE
    codexpets_core
    advapi32
    comctl32
    crypt32
    gdiplus
    gdi32
    ole32
    psapi
    shell32
    shlwapi
    shcore
    user32
    uuid
    winmm
    winhttp
)
target_compile_definitions(CodeXPets PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    CODEXPETS_WEBVIEW2_LOADER_PATH="${CODEXPETS_WEBVIEW2_LOADER_RC_PATH}"
)
set_target_properties(CodeXPets PROPERTIES
    OUTPUT_NAME "CodeXPets"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(MSVC)
    set_property(TARGET CodeXPets PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(CodeXPets PRIVATE /W4 /WX /utf-8 /EHsc /GR- /O1 /Gy /Gw)
    target_link_options(CodeXPets PRIVATE /OPT:REF /OPT:ICF)
else()
    target_compile_options(CodeXPets PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-rtti -Os
                           -Wno-missing-field-initializers -Wno-missing-braces
                           -ffunction-sections -fdata-sections)
    # The vendored WebView2 SDK header contains an MSVC-only #pragma warning.
    # Scope the compatibility suppression to the single source that includes it.
    set_source_files_properties(xiaomi_browser_login.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-unknown-pragmas")
    target_link_options(CodeXPets PRIVATE -municode -Wl,--gc-sections -Wl,--strip-all -Wl,--subsystem,windows)
endif()
