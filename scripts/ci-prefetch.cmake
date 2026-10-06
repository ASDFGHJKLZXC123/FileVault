cmake_minimum_required(VERSION 3.24)

# Archives and SHA512 values from the pinned vcpkg baseline's gtest/gperf ports.
# Preload alternate endpoints for the origins that timed out in M7 CI.
if("$ENV{VCPKG_ROOT}" STREQUAL "")
    message(FATAL_ERROR "VCPKG_ROOT must name the bootstrapped vcpkg checkout")
endif()
find_program(CURL_EXECUTABLE NAMES curl REQUIRED)
file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}/downloads" downloads)
file(MAKE_DIRECTORY "${downloads}")

function(prefetch filename hash url)
    set(archive "${downloads}/${filename}")
    if(EXISTS "${archive}")
        file(SHA512 "${archive}" actual)
        if(actual STREQUAL hash)
            message(STATUS "Using verified ${filename}")
            return()
        endif()
    endif()
    execute_process(COMMAND "${CURL_EXECUTABLE}" --silent --show-error --fail --location --retry 2
        --retry-max-time 240 --connect-timeout 15 --max-time 120
        --output "${archive}.part" "${url}" RESULT_VARIABLE result TIMEOUT 360)
    if(NOT result EQUAL 0)
        file(REMOVE "${archive}.part")
        message(WARNING "Alternate download failed for ${filename}; vcpkg retains its own download path")
        return()
    endif()
    file(SHA512 "${archive}.part" actual)
    if(NOT actual STREQUAL hash)
        file(REMOVE "${archive}.part")
        message(FATAL_ERROR "Pinned SHA512 mismatch for ${filename}")
    endif()
    file(RENAME "${archive}.part" "${archive}")
    message(STATUS "Prefetched and verified ${filename}")
endfunction()

prefetch(google-googletest-v1.17.0.tar.gz
    0f57e9ef06925e5b7722df1eb92ef5850e8dce79220ea16a8aaff586a71c0b01460ef1713649ee24ffedb2e6ad5a51e9198c5a5ae1b2789e43feb1f494e7d45c
    https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.17.0)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    prefetch(gperf-3.3.tar.gz
        246b75b8ce7d77d6a8725cd15f1cf2e68da404812573af1d5bf32dbe6ad4228f48757baefc77bcb1f5597c2397043c04d31d8a04ab507bfa7a80f85e1ab6045f
        https://mirrors.kernel.org/gnu/gperf/gperf-3.3.tar.gz)
endif()
