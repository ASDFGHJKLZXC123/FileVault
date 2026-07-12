qt_standard_project_setup()

add_executable(
    localvault_desktop
    main.cpp
    main_window.cpp
)

target_link_libraries(
    localvault_desktop
    PRIVATE
        LocalVault::core
        Qt6::Core
        Qt6::Widgets
)

set_target_properties(
    localvault_desktop
    PROPERTIES
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE TRUE
)

localvault_set_warnings(localvault_desktop)
localvault_enable_sanitizers(localvault_desktop)
localvault_enable_clang_tidy(localvault_desktop)
