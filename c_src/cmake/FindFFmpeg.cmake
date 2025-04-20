# FindFFmpeg.cmake
#
# Finds the FFmpeg library components (libavcodec, libavformat, libavutil, etc.)
# using pkg-config.
#
# Sets the following variables:
# FFMPEG_FOUND - System has FFmpeg libraries and headers
# FFMPEG_INCLUDE_DIRS - Location of FFmpeg headers
# FFMPEG_LIBRARIES - Required FFmpeg libraries for linking

find_package(PkgConfig REQUIRED)

# List of FFmpeg components required by the project
set(FFMPEG_COMPONENTS
    libavcodec
    libavformat
    libavutil
    libswscale
    # Add other components if needed, e.g., libswresample, libavfilter
)

# Use pkg_search_module to find each component
set(FFMPEG_FOUND TRUE)
set(FFMPEG_INCLUDE_DIRS "")
set(FFMPEG_LIBRARIES "")

message(STATUS "Searching for FFmpeg components using pkg-config...")

foreach(COMPONENT ${FFMPEG_COMPONENTS})
    pkg_search_module(PC_${COMPONENT} QUIET ${COMPONENT})
    if(NOT PC_${COMPONENT}_FOUND)
        set(FFMPEG_FOUND FALSE)
        message(WARNING "Required FFmpeg component '${COMPONENT}' not found using pkg-config.")
        # Don't break immediately, report all missing components
    else()
        message(STATUS "  Found component: ${COMPONENT}")
        # Collect include directories (avoid duplicates)
        list(APPEND FFMPEG_INCLUDE_DIRS ${PC_${COMPONENT}_INCLUDE_DIRS})
        # Collect libraries
        list(APPEND FFMPEG_LIBRARIES ${PC_${COMPONENT}_LIBRARIES})
    endif()
endforeach()

# Remove duplicate include directories and libraries
if(FFMPEG_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIRS)
endif()
if(FFMPEG_LIBRARIES)
    list(REMOVE_DUPLICATES FFMPEG_LIBRARIES)
endif()

# Standard find_package handling
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    FOUND_VAR FFMPEG_FOUND
    REQUIRED_VARS FFMPEG_LIBRARIES FFMPEG_INCLUDE_DIRS
    FAIL_MESSAGE "FFmpeg not found. Could not find all required components using pkg-config. Please ensure FFmpeg development libraries (including .pc files for libavcodec, libavformat, libavutil, libswscale) are installed and accessible via pkg-config within the build environment."
)

# Mark variables as advanced so they don't clutter CMake GUI/ccmake
mark_as_advanced(FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES)

if(FFMPEG_FOUND)
    message(STATUS "FFmpeg found:")
    message(STATUS "  Includes: ${FFMPEG_INCLUDE_DIRS}")
    message(STATUS "  Libraries: ${FFMPEG_LIBRARIES}")
endif() 