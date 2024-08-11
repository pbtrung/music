cmake_minimum_required(VERSION 3.29)
project(music)

set(CMAKE_C_STANDARD 11)

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Set build flags for different build types
set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/apr/lib/pkgconfig:/opt/homebrew/opt/apr-util/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(JANSSON REQUIRED jansson)
pkg_check_modules(APR REQUIRED apr-1)
pkg_check_modules(APRU REQUIRED apr-util-1)
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswresample)

add_executable(music
    src/main.c
    src/config.c
    src/database.c
    src/util.c
    src/download.c
    src/dir.c
    src/decode.c
)

target_include_directories(music PRIVATE
    ${SQLITE3_INCLUDE_DIRS}
    ${JANSSON_INCLUDE_DIRS}
    ${APR_INCLUDE_DIRS}
    ${APRU_INCLUDE_DIRS}
    ${PCRE2_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    include
)

target_link_directories(music PRIVATE
    ${SQLITE3_LIBRARY_DIRS}
    ${JANSSON_LIBRARY_DIRS}
    ${APR_LIBRARY_DIRS}
    ${APRU_LIBRARY_DIRS}
    ${PCRE2_LIBRARY_DIRS}
    ${CURL_LIBRARY_DIRS}
    ${FFMPEG_LIBRARY_DIRS}
)

target_link_libraries(music PRIVATE
    ${SQLITE3_LIBRARIES}
    ${JANSSON_LIBRARIES}
    ${APR_LIBRARIES}
    ${APRU_LIBRARIES}
    ${PCRE2_LIBRARIES}
    ${CURL_LIBRARIES}
    ${FFMPEG_LIBRARIES}
)