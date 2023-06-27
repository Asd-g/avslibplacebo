cmake_minimum_required(VERSION 3.16)

project(avs_libplacebo LANGUAGES CXX)

set(sources
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deband.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resample.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tonemap.cpp
)

if (WIN32)
    set(sources ${sources} ${CMAKE_CURRENT_SOURCE_DIR}/src/avs_libplacebo.rc)
endif()

add_library(avs_libplacebo SHARED ${sources})

target_include_directories(avs_libplacebo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if (UNIX)
    target_include_directories(avs_libplacebo PRIVATE
    /usr/local/include/avisynth
    /usr/local/include
    )
else()
    target_include_directories(avs_libplacebo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../AviSynthPlus/avs_core/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/deps_gcc/include"
    )
endif()

if (NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    if (NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
    endif()

    string(TOLOWER ${CMAKE_BUILD_TYPE} build_type)
    if (build_type STREQUAL Debug)
        target_compile_definitions(avs_libplacebo PRIVATE DEBUG_BUILD)
    else (build_type STREQUAL Release)
        target_compile_definitions(avs_libplacebo PRIVATE RELEASE_BUILD)
    endif()

    target_compile_options(avs_libplacebo PRIVATE $<$<CONFIG:Release>:-s -DPL_STATIC>)

    message(STATUS "Build type - ${CMAKE_BUILD_TYPE}")
endif()

target_compile_features(avs_libplacebo PRIVATE cxx_std_17)

target_link_libraries(avs_libplacebo PRIVATE
    avisynth
    placebo
    vulkan-1
    dovi
    shaderc_combined
)

if (MINGW)
    target_link_libraries(avs_libplacebo PRIVATE
        ws2_32
        bcrypt
        userenv
        shlwapi
    )

    set_target_properties(avs_libplacebo PROPERTIES PREFIX "")

    target_link_libraries(avs_libplacebo PRIVATE -static-libgcc -static-libstdc++ -s)
endif()

if (UNIX)
    find_package (Git)

    if (GIT_FOUND)
        execute_process (COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
            OUTPUT_VARIABLE ver
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set_target_properties(avs_libplacebo PROPERTIES OUTPUT_NAME "avs_libplacebo.${ver}")
    else()
        message (STATUS "GIT not found")
    endif()

    include(GNUInstallDirs)

    INSTALL(TARGETS avs_libplacebo LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/avisynth")

    # uninstall target
    if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY)

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
    endif()
endif()
