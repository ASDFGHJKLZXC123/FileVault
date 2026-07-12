function(localvault_enable_clang_tidy target)
    if(NOT LOCALVAULT_ENABLE_CLANG_TIDY)
        return()
    endif()

    find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set_target_properties(
        ${target}
        PROPERTIES
            CXX_CLANG_TIDY "${CLANG_TIDY_EXE}"
    )
endfunction()
