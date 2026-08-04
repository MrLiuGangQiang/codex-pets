enable_language(RC)

add_executable(CodeXPets WIN32
    main.cpp
    native_app.cpp
    renderer.cpp
    resources.rc
)
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
)
target_compile_definitions(CodeXPets PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
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
    target_link_options(CodeXPets PRIVATE -municode -Wl,--gc-sections -Wl,--strip-all -Wl,--subsystem,windows)
endif()
