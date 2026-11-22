cmake_minimum_required(VERSION 3.30)

file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/.." source_dir)
if(NOT EXISTS "${source_dir}/CMakeLists.txt"
   OR NOT EXISTS "${source_dir}/CMakePresets.json"
   OR NOT IS_DIRECTORY "${source_dir}/coretx/core")
    message(FATAL_ERROR "Cleanup must run from a CoreTX source tree")
endif()

set(directories
    build
    build-sanitize
    build-i6x
    build-i6s
    build-docs
    deps
    tools/__pycache__
    tests/__pycache__
)
set(files
    README.html
    README.backup.html
    COMPATIBILITY.html
    PERIPHERALS.html
)
file(GLOB temporary_files RELATIVE "${source_dir}"
    "${source_dir}/*.tmp"
    "${source_dir}/*.swp"
    "${source_dir}/*~"
    "${source_dir}/CoreTX.creator.user"
    "${source_dir}/CoreTX.creator.user.*"
    "${source_dir}/tools/*.pyc"
    "${source_dir}/tools/*.pyo"
    "${source_dir}/tests/*.pyc"
    "${source_dir}/tests/*.pyo"
)
list(APPEND files ${temporary_files})

# Check every target before deleting anything, including symlinked parents.
foreach(relative IN LISTS directories files)
    set(path "${source_dir}/${relative}")
    if(IS_SYMLINK "${path}")
        message(FATAL_ERROR "Refusing to remove symlink: ${path}")
    endif()
    file(REAL_PATH "${path}" resolved)
    cmake_path(IS_PREFIX source_dir "${resolved}" NORMALIZE inside_source)
    if(NOT inside_source OR resolved STREQUAL source_dir)
        message(FATAL_ERROR "Cleanup target outside source tree: ${path}")
    endif()
    if(relative IN_LIST files AND IS_DIRECTORY "${path}")
        message(FATAL_ERROR "Expected a temporary file, not a directory: ${path}")
    endif()
endforeach()

foreach(relative IN LISTS directories)
    set(path "${source_dir}/${relative}")
    if(EXISTS "${path}")
        message(STATUS "Removing ${relative}")
        file(REMOVE_RECURSE "${path}")
        if(EXISTS "${path}")
            message(FATAL_ERROR "Could not remove ${path}")
        endif()
    endif()
endforeach()
foreach(relative IN LISTS files)
    set(path "${source_dir}/${relative}")
    if(EXISTS "${path}")
        message(STATUS "Removing ${relative}")
        file(REMOVE "${path}")
        if(EXISTS "${path}")
            message(FATAL_ERROR "Could not remove ${path}")
        endif()
    endif()
endforeach()
