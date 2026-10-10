function(studio_duo_apply_dependency_patch source_directory patch_file)
    find_package(Git REQUIRED QUIET)
    # Archive dependencies must not inherit the enclosing project's Git prefix.
    get_filename_component(source_parent "${source_directory}" DIRECTORY)
    set(patch_command
        "${CMAKE_COMMAND}" -E env
        --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE
        "GIT_CEILING_DIRECTORIES=${source_parent}"
        "${GIT_EXECUTABLE}" apply --no-index --ignore-space-change
    )
    execute_process(
        COMMAND ${patch_command} --check "${patch_file}"
        WORKING_DIRECTORY "${source_directory}"
        RESULT_VARIABLE check_result
        ERROR_VARIABLE check_error
    )
    if(check_result EQUAL 0)
        execute_process(
            COMMAND ${patch_command} "${patch_file}"
            WORKING_DIRECTORY "${source_directory}"
            RESULT_VARIABLE apply_result
            ERROR_VARIABLE apply_error
        )
        if(NOT apply_result EQUAL 0)
            message(FATAL_ERROR "Could not apply dependency patch ${patch_file}: ${apply_error}")
        endif()
    else()
        execute_process(
            COMMAND ${patch_command} --reverse --check "${patch_file}"
            WORKING_DIRECTORY "${source_directory}"
            RESULT_VARIABLE reverse_result
            ERROR_VARIABLE reverse_error
        )
        if(NOT reverse_result EQUAL 0)
            message(FATAL_ERROR
                "Dependency patch ${patch_file} neither applies nor is already applied. "
                "Check the pinned dependency source or custom source override.\n"
                "${check_error}\n${reverse_error}"
            )
        endif()
    endif()
endfunction()
