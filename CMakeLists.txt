cmake_minimum_required(VERSION 3.24)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CODEXPETS_VERSION_FILE)
string(STRIP "${CODEXPETS_VERSION_FILE}" CODEXPETS_VERSION_FILE)
if(NOT CODEXPETS_VERSION_FILE MATCHES "^[0-9]+\.[0-9]+\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a semantic version, got: ${CODEXPETS_VERSION_FILE}")
endif()
project(CodeXPets VERSION "${CODEXPETS_VERSION_FILE}" LANGUAGES CXX)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(CODEXPETS_BUILD_APP "Build the native desktop application" ON)
option(CODEXPETS_BUILD_TESTS "Build native core tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum supported macOS version")
endif()

add_library(codexpets_core STATIC
    src/core/app_logic.cpp
    src/core/json.cpp
    src/core/paths.cpp
    src/core/platform_text.cpp
    src/core/presentation.cpp
    src/core/render_layout.cpp
    src/core/session_monitor.cpp
    src/core/settings.cpp
    src/core/visual_state.cpp
)
target_include_directories(codexpets_core PUBLIC src/core)
target_compile_definitions(codexpets_core PUBLIC CODEXPETS_VERSION="${PROJECT_VERSION}")

if(APPLE)
    find_library(PROC_LIBRARY proc REQUIRED)
    target_link_libraries(codexpets_core PUBLIC "${PROC_LIBRARY}")
endif()

if(MSVC)
    target_compile_options(codexpets_core PRIVATE /W4 /WX /utf-8 /EHsc /GR- /O1 /Gy /Gw)
else()
    target_compile_options(codexpets_core PRIVATE
        -Wall -Wextra -Wpedantic -Werror -fno-rtti -Os
        -ffunction-sections -fdata-sections)
endif()

if(CODEXPETS_BUILD_TESTS)
    enable_testing()
    add_executable(codexpets_tests tests/native_tests.cpp)
    target_link_libraries(codexpets_tests PRIVATE codexpets_core)
    if(MSVC)
        target_compile_options(codexpets_tests PRIVATE /W4 /WX /utf-8 /EHsc /GR-)
    else()
        target_compile_options(codexpets_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-rtti)
    endif()
    add_test(NAME codexpets_core_tests COMMAND codexpets_tests)
endif()

if(CODEXPETS_BUILD_APP)
    if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/main.cpp")
        add_subdirectory(src/platform/windows)
    elseif(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/main.mm")
        enable_language(OBJCXX)
        add_subdirectory(src/platform/macos)
    endif()
endif()
