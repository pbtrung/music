cmake_minimum_required(VERSION 3.30)
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
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libswresample libavutil)

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
    ${FFMPEG_INCLUDE_DIRS}
    include
)

target_link_directories(music PRIVATE
    ${LIBCURL_LIBRARY_DIRS}
    ${SQLITE3_LIBRARY_DIRS}
    ${PCRE2_LIBRARY_DIRS}
    ${FMT_LIBRARY_DIRS}
    ${FFMPEG_LIBRARY_DIRS}
)

target_link_libraries(music PRIVATE
    ${LIBCURL_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${PCRE2_LIBRARIES}
    ${FMT_LIBRARIES}
    ${FFMPEG_LIBRARIES}
)