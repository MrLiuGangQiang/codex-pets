add_executable(CodeXPets MACOSX_BUNDLE main.mm)
target_link_libraries(CodeXPets PRIVATE
    codexpets_core
    "-framework AppKit"
    "-framework CoreGraphics"
    "-framework ServiceManagement"
)
target_compile_options(CodeXPets PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -fobjc-arc -fno-rtti -Os
    -fvisibility=hidden -ffunction-sections -fdata-sections
)
target_link_options(CodeXPets PRIVATE
    "$<$<CONFIG:Release>:-Wl,-dead_strip>"
    "$<$<CONFIG:Release>:-Wl,-x>"
)
target_compile_definitions(CodeXPets PRIVATE CODEXPETS_MACOS=1)
set_target_properties(CodeXPets PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "CodeXPets"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.mrliugangqiang.codexpets"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/CodeXPets-Info.plist"
    MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    OUTPUT_NAME "CodeXPets"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
               "${CMAKE_BINARY_DIR}/CodeXPets-Info.plist" @ONLY)
add_custom_command(TARGET CodeXPets POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/audio"
            "$<TARGET_BUNDLE_DIR:CodeXPets>/Contents/Resources/audio"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_BUNDLE_DIR:CodeXPets>/Contents/Resources/cloud"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:CodeXPets>/Contents/Resources/cloud"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/cloud/cloud-bubble-540.png"
            "$<TARGET_BUNDLE_DIR:CodeXPets>/Contents/Resources/cloud/cloud-bubble-540.png"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/dock"
            "$<TARGET_BUNDLE_DIR:CodeXPets>/Contents/Resources/dock"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/floating"
            "$<TARGET_BUNDLE_DIR:CodeXPets>/Contents/Resources/floating"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/icons"
            "$<TARGET_BUNDLE_DIR:CodeXPets>/Contents/Resources/icons"
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/make-macos-icon.sh"
            "${CMAKE_SOURCE_DIR}/assets/app/AppIcon.png"
            "$<TARGET_BUNDLE_DIR:CodeXPets>/Contents/Resources/AppIcon.icns"
    VERBATIM
)

if(CODEXPETS_BUILD_TESTS)
    add_test(NAME codexpets_macos_resources
             COMMAND "$<TARGET_FILE:CodeXPets>" --validate-resources)
    add_test(NAME codexpets_macos_smoke
             COMMAND "$<TARGET_FILE:CodeXPets>" --smoke-test)
endif()
