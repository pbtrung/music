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
pkg_check_modules(LIBUV REQUIRED libuv)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(LIBSODIUM REQUIRED libsodium)
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
pkg_check_modules(OPUSFILE REQUIRED opusfile)
pkg_check_modules(OPUS REQUIRED opus)
pkg_check_modules(TAGLIB REQUIRED taglib_c)
pkg_check_modules(JANSSON REQUIRED jansson)
pkg_check_modules(MPG123 REQUIRED libmpg123)
pkg_check_modules(LWS REQUIRED libwebsockets)

include_directories(${LIBUV_INCLUDE_DIRS} ${LIBCURL_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS} ${LIBSODIUM_INCLUDE_DIRS} ${PCRE2_INCLUDE_DIRS} ${OPUSFILE_INCLUDE_DIRS} ${OPUS_INCLUDE_DIRS} ${TAGLIB_INCLUDE_DIRS} ${JANSSON_INCLUDE_DIRS} ${MPG123_INCLUDE_DIRS} ${LWS_INCLUDE_DIRS} include)

link_directories(${LIBUV_LIBRARY_DIRS} ${LIBCURL_LIBRARY_DIRS} ${SQLITE3_LIBRARY_DIRS} ${LIBSODIUM_LIBRARY_DIRS} ${PCRE2_LIBRARY_DIRS} ${OPUSFILE_LIBRARY_DIRS} ${OPUS_LIBRARY_DIRS} ${TAGLIB_LIBRARY_DIRS} ${JANSSON_LIBRARY_DIRS} ${MPG123_LIBRARY_DIRS} ${LWS_LIBRARY_DIRS})

add_executable(music
    src/main.c
    src/config.c
    src/database.c
    src/random.c
    src/downloader.c
    src/dir.c
    src/track.c
)

target_link_libraries(music ${LIBUV_LIBRARIES} ${LIBCURL_LIBRARIES} ${SQLITE3_LIBRARIES} ${LIBSODIUM_LIBRARIES} ${PCRE2_LIBRARIES} ${OPUS_LIBRARIES} ${OPUSFILE_LIBRARIES} ${TAGLIB_LIBRARIES} ${JANSSON_LIBRARIES} ${MPG123_LIBRARIES} ${LWS_LIBRARIES})
