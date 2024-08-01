cmake_minimum_required(VERSION 3.30)
project(music)

set(CMAKE_C_STANDARD 11)

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Set build flags for different build types
set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/homebrew/lib/pkgconfig")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(JANSSON REQUIRED jansson)
pkg_check_modules(LIBSODIUM REQUIRED libsodium)
pkg_check_modules(MPV REQUIRED mpv)
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
pkg_check_modules(UV REQUIRED libuv)

include_directories(${LIBCURL_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS} ${LIBSODIUM_INCLUDE_DIRS} ${MPV_INCLUDE_DIRS} ${JANSSON_INCLUDE_DIRS} ${PCRE2_INCLUDE_DIRS} ${UV_INCLUDE_DIRS} include)

link_directories(${LIBCURL_LIBRARY_DIRS} ${SQLITE3_LIBRARY_DIRS} ${LIBSODIUM_LIBRARY_DIRS} ${MPV_LIBRARY_DIRS} ${JANSSON_LIBRARY_DIRS} ${PCRE2_LIBRARY_DIRS} ${UV_LIBRARY_DIRS})

add_executable(music
    src/main.c
    src/config.c
    src/database.c
    src/random.c
    src/dir.c
    src/mpv.c
    src/utils.c
    src/download.c
)

target_link_libraries(music ${LIBCURL_LIBRARIES} ${SQLITE3_LIBRARIES} ${LIBSODIUM_LIBRARIES} ${MPV_LIBRARIES} ${JANSSON_LIBRARIES} ${PCRE2_LIBRARIES} ${UV_LIBRARIES})
