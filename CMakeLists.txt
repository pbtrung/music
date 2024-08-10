cmake_minimum_required(VERSION 3.29)
project(music LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Set build flags for different build types
set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/usr/local/lib/pkgconfig:/opt/homebrew/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
pkg_check_modules(FMT REQUIRED fmt)
pkg_check_modules(TAGLIB2 REQUIRED taglib)
pkg_check_modules(OPUSFILE REQUIRED opusfile)
pkg_check_modules(MPG123 REQUIRED libmpg123)
pkg_check_modules(SOXR REQUIRED soxr)
pkg_check_modules(UV REQUIRED libuv)

add_executable(music
    src/main.cpp
    src/database.cpp
    src/random.cpp
    src/utils.cpp
    src/downloader.cpp
    src/dir.cpp
    src/decoder.cpp
)

target_include_directories(music PRIVATE
    ${LIBCURL_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${PCRE2_INCLUDE_DIRS}
    ${FMT_INCLUDE_DIRS}
    ${TAGLIB2_INCLUDE_DIRS}
    ${OPUSFILE_INCLUDE_DIRS}
    ${MPG123_INCLUDE_DIRS}
    ${SOXR_INCLUDE_DIRS}
    ${UV_INCLUDE_DIRS}
    include
)

target_link_directories(music PRIVATE
    ${LIBCURL_LIBRARY_DIRS}
    ${SQLITE3_LIBRARY_DIRS}
    ${PCRE2_LIBRARY_DIRS}
    ${FMT_LIBRARY_DIRS}
    ${TAGLIB2_LIBRARY_DIRS}
    ${OPUSFILE_LIBRARY_DIRS}
    ${MPG123_LIBRARY_DIRS}
    ${SOXR_LIBRARY_DIRS}
    ${UV_LIBRARY_DIRS}
)

target_link_libraries(music PRIVATE
    ${LIBCURL_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${PCRE2_LIBRARIES}
    ${FMT_LIBRARIES}
    ${TAGLIB2_LIBRARIES}
    ${OPUSFILE_LIBRARIES}
    ${MPG123_LIBRARIES}
    ${SOXR_LIBRARIES}
    ${UV_LIBRARIES}
)