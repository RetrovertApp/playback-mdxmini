# MDXmini Playback Plugin
#
# This plugin plays Sharp X68000 MDX music files using the mdxmini library.
# MDX uses YM2151 (OPM) FM synthesis + ADPCM for drums/samples.
# Files: .mdx (music) with optional .pdx (PCM drum samples)

cmake_minimum_required(VERSION 3.16)
project(playback_mdxmini C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

# Download mdxmini v2.0.0
download_library(
    NAME mdxmini
    URL "https://github.com/mistydemeo/mdxmini/archive/refs/tags/v2.0.0.tar.gz"
    FILENAME "mdxmini-2.0.0.tar.gz"
    HASH "9b623b365e893a769084f7a2effedc9ece453c6e3861c571ba503f045471a0e0"
    EXTRACT_DIR "mdxmini-2.0.0"
    PATCHES patches/mdxmini-strcasecmp-win32.patch patches/mdxmini-extern-c.patch patches/mdxmini-case-insensitive.patch patches/mdxmini-fix-memory-leaks.patch patches/mdxmini-fix-pcm8-leaks.patch patches/mdxmini-fix-ym2151-leak.patch patches/mdxmini-fix-ym2151-reinit-leak.patch patches/scope-capture.patch
)

# Build mdxmini as a static library
file(GLOB MDXMINI_SOURCES ${mdxmini_SOURCE_DIR}/src/*.c)
add_library(mdxmini_static STATIC ${MDXMINI_SOURCES})
target_include_directories(mdxmini_static PUBLIC ${mdxmini_SOURCE_DIR}/src)
suppress_external_warnings(mdxmini_static)
set_target_properties(mdxmini_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Plugin shared library
add_library(mdxmini_playback SHARED
    mdxmini_plugin.c
)

target_include_directories(mdxmini_playback PRIVATE
    ${mdxmini_SOURCE_DIR}/src
)

target_link_libraries(mdxmini_playback PRIVATE mdxmini_static)
if(UNIX)
    target_link_libraries(mdxmini_playback PRIVATE m)
endif()

target_compile_options(mdxmini_playback PRIVATE
    -Wno-unused-parameter
)

rv_set_plugin_properties(mdxmini_playback)
