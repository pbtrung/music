cmake_minimum_required(VERSION 3.50)
project(music)

set(CMAKE_C_STANDARD 11)

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Set build flags for different build types
set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(PkgConfig REQUIRED)
pkg_check_modules(APR REQUIRED apr-1)
pkg_check_modules(APRU REQUIRED apr-util-1)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(JANSSON REQUIRED jansson)
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswresample)
pkg_check_modules(OPENSSL REQUIRED libssl libcrypto)

add_executable(music
    src/main.c
    src/config.c
    src/log.c
    src/utils.c
    src/dir.c
    src/decode.c
    src/download.c
    src/cosmosdb.c
)
add_executable(ex01
    examples/ex01.c
    src/cosmosdb.c
    src/config.c
    src/log.c
    src/utils.c
    src/download.c
)

set(COMMON_INCLUDE_DIRS
    ${APR_INCLUDE_DIRS}
    ${APRU_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${JANSSON_INCLUDE_DIRS}
    ${PCRE2_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
    include
)

set(COMMON_LINK_DIRS
    ${APR_LIBRARY_DIRS}
    ${APRU_LIBRARY_DIRS}
    ${CURL_LIBRARY_DIRS}
    ${JANSSON_LIBRARY_DIRS}
    ${PCRE2_LIBRARY_DIRS}
    ${FFMPEG_LIBRARY_DIRS}
    ${OPENSSL_LIBRARY_DIRS}
)

set(COMMON_LINK_LIBS
    ${APR_LIBRARIES}
    ${APRU_LIBRARIES}
    ${CURL_LIBRARIES}
    ${JANSSON_LIBRARIES}
    ${PCRE2_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    ${OPENSSL_LIBRARIES}
)

foreach(target music ex01)
    target_include_directories(${target} PRIVATE ${COMMON_INCLUDE_DIRS})
    target_link_directories(${target} PRIVATE ${COMMON_LINK_DIRS})
    target_link_libraries(${target} PRIVATE ${COMMON_LINK_LIBS})
endforeach()
