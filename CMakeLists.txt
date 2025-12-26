cmake_minimum_required(VERSION 3.25)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type" FORCE)
endif()

project(avs_libplacebo VERSION 1.6.0 LANGUAGES CXX)

option(USE_SYSTEM_AVS_HELPER "Use an installed version of avs_c_api_loader" OFF)
option(USE_STATIC_LIBPLACEBO "Link libplacebo statically" ON)
option(USE_STATIC_DOVI "Link dovi statically" ON)
option(USE_STATIC_SHADERC "Link shaderc statically (shaderc_combined) instead of shared" ON)

if(USE_SYSTEM_AVS_HELPER)
    message(STATUS "Using system-provided avs_c_api_loader")
    find_package(avs_c_api_loader REQUIRED)
else()
    message(STATUS "Using submodule for avs_c_api_loader")
    if(NOT IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/avs_c_api_loader)
        message(FATAL_ERROR "avs_c_api_loader submodule not found...")
    endif()

    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/avs_c_api_loader/cmake")
    find_package(AvisynthPlus REQUIRED)

    add_subdirectory(avs_c_api_loader)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPLACEBO REQUIRED libplacebo)
pkg_check_modules(DOVI REQUIRED dovi)

add_library(${PROJECT_NAME} SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/avs_libplacebo.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deband.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resample.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tonemap.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

if(USE_STATIC_LIBPLACEBO)
    # Use the static variables populated by pkg-config
    set(PL_LIBS ${LIBPLACEBO_STATIC_LIBRARIES})
    set(PL_LIB_DIRS ${LIBPLACEBO_STATIC_LIBRARY_DIRS})
    set(PL_CFLAGS ${LIBPLACEBO_STATIC_CFLAGS})

    message(STATUS "Libplacebo Lib Dirs: ${PL_LIB_DIRS}")

    # Swap shaderc_shared for shaderc_combined if requested
    if(USE_STATIC_SHADERC)
        message(STATUS "Swapping shaderc_shared for shaderc_combined")
        list(TRANSFORM PL_LIBS REPLACE "shaderc_shared" "shaderc_combined")
    endif()

    # Fix library name mismatch (placebo.lib vs libplacebo.a)
    if(MSVC)
        set(FOUND_LIBPLACEBO_A FALSE)
        set(LIBPLACEBO_FULL_PATH "")

        # Search for libplacebo.a in the library directories
        foreach(dir ${PL_LIB_DIRS})
            if(EXISTS "${dir}/libplacebo.a")
                set(FOUND_LIBPLACEBO_A TRUE)
                set(LIBPLACEBO_FULL_PATH "${dir}/libplacebo.a")
                break()
            endif()
        endforeach()

        if(FOUND_LIBPLACEBO_A)
            message(STATUS "Found libplacebo.a at ${LIBPLACEBO_FULL_PATH}. Using explicit path to satisfy MSVC linker.")
            # Replace the short name "placebo" with the full path to the .a file
            # This stops MSVC from looking for "placebo.lib"
            list(TRANSFORM PL_LIBS REPLACE "^placebo$" "${LIBPLACEBO_FULL_PATH}")
        else()
            message(WARNING "Could not find libplacebo.a in ${PL_LIB_DIRS}. Linker might fail if it looks for placebo.lib.")
        endif()
    endif()

    target_link_directories(${PROJECT_NAME} PRIVATE ${PL_LIB_DIRS})
    target_compile_options(${PROJECT_NAME} PRIVATE ${PL_CFLAGS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${PL_LIBS})
else()
    # Dynamic linking
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBPLACEBO_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBPLACEBO_LIBRARIES})
endif()

if(USE_STATIC_DOVI)
    target_link_directories(${PROJECT_NAME} PRIVATE ${DOVI_STATIC_LIBRARY_DIRS})
    target_compile_options(${PROJECT_NAME} PRIVATE ${DOVI_STATIC_CFLAGS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${DOVI_STATIC_LIBRARIES})
else()
    target_include_directories(${PROJECT_NAME} PRIVATE ${DOVI_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${DOVI_LIBRARIES})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE avs_c_api_loader::avs_c_api_loader)

if(CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
    target_link_libraries(${PROJECT_NAME} PRIVATE "libmmds")
endif()

if (NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    string(TOLOWER ${CMAKE_BUILD_TYPE} build_type)
    if (build_type STREQUAL Debug)
        target_compile_definitions(${PROJECT_NAME} PRIVATE DEBUG_BUILD)
    else (build_type STREQUAL Release)
        target_compile_definitions(${PROJECT_NAME} PRIVATE RELEASE_BUILD)
    endif()

    if (NOT MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:-s>)
    endif()

    message(STATUS "Build type - ${CMAKE_BUILD_TYPE}")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
    target_compile_options(${PROJECT_NAME} PRIVATE "/fp:precise")
endif()

if(MINGW)
    set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
elseif(UNIX)
    string(TOLOWER "${PROJECT_NAME}" FILE_NAME_OUTPUT)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${FILE_NAME_OUTPUT}.${PROJECT_VERSION}")
endif()

if (WIN32)
    string(REGEX MATCH "^([0-9.]+)" CORE_VERSION_STRING "${PROJECT_VERSION}")
    if(NOT CORE_VERSION_STRING)
        message(WARNING "Could not parse core version from '${PROJECT_VERSION}'. Defaulting to 0.0.0.")
        set(CORE_VERSION_STRING "0.0.0")
    endif()

    string(REPLACE "." ";" VERSION_NUMERIC_PARTS_LIST "${CORE_VERSION_STRING}")
    list(LENGTH VERSION_NUMERIC_PARTS_LIST NUM_PARTS)

    set(VERSION_MAJOR 0)
    set(VERSION_MINOR 0)
    set(VERSION_PATCH 0)
    set(VERSION_BUILD 0)

    if(NUM_PARTS GREATER_EQUAL 1)
        list(GET VERSION_NUMERIC_PARTS_LIST 0 VERSION_MAJOR)
    endif()

    if(NUM_PARTS GREATER_EQUAL 2)
        list(GET VERSION_NUMERIC_PARTS_LIST 1 VERSION_MINOR)
    endif()

    if(NUM_PARTS GREATER_EQUAL 3)
        list(GET VERSION_NUMERIC_PARTS_LIST 2 VERSION_PATCH)
    endif()

    if(NUM_PARTS GREATER_EQUAL 4)
        list(GET VERSION_NUMERIC_PARTS_LIST 3 VERSION_BUILD)
    endif()

    if(PROJECT_VERSION MATCHES "-")
        set(PROJECT_IS_PRERELEASE 1)
    else()
        set(PROJECT_IS_PRERELEASE 0)
    endif()

    if(NOT CMAKE_CONFIGURATION_TYPES)
        if(CMAKE_BUILD_TYPE MATCHES "^Debug$")
            set(PROJECT_DEBUG_BUILD_VALUE 1)
        else()
            set(PROJECT_DEBUG_BUILD_VALUE 0)
        endif()
    else()
        set(PROJECT_DEBUG_BUILD_VALUE -1)
    endif()

    set(PROJECT_DEBUG_BUILD ${PROJECT_DEBUG_BUILD_VALUE})

    set(FILE_DESCRIPTION "AviSynth+ plugin interface to libplacebo")
    set(INTERNAL_NAME "${PROJECT_NAME}")
    set(ORIGINAL_FILENAME "${PROJECT_NAME}.dll")
    set(PRODUCT_NAME "${PROJECT_NAME}")
    set(PROJECT_VERSION_STRING_FULL "${PROJECT_VERSION}")

    set(RC_FILE_OUT "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.rc") # Output to build dir

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/${PROJECT_NAME}.rc.in"
        "${RC_FILE_OUT}"
        @ONLY
    )

    target_sources(${PROJECT_NAME} PRIVATE "${RC_FILE_OUT}")

    if(MSVC)
        set_source_files_properties("${RC_FILE_OUT}" PROPERTIES
            VS_RESOURCE_GENERATOR "RC")
    endif()
endif()

if(UNIX)
    include(GNUInstallDirs)

    INSTALL(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/avisynth")

    # uninstall target
    if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY)

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")
    endif()
endif()
