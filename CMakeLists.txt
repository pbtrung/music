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
set(PKG_LIST
    apr-1
    apr-util-1
    libcurl
    jansson
    libpcre2-8
    libavcodec
    libavformat
    libavutil
    libswresample
    libssl
    libcrypto
)
foreach(PKG ${PKG_LIST})
    pkg_check_modules(${PKG} REQUIRED ${PKG})
    list(APPEND COMMON_INCLUDE_DIRS ${${PKG}_INCLUDE_DIRS})
    list(APPEND COMMON_LINK_DIRS    ${${PKG}_LIBRARY_DIRS})
    list(APPEND COMMON_LINK_LIBS    ${${PKG}_LIBRARIES})
endforeach()
list(APPEND COMMON_INCLUDE_DIRS include)


add_executable(music
    src/main.c
    src/config.c
    src/log.c
    src/utils.c
    src/dir.c
    src/decode.c
    src/download.c
    src/cosmosdb.c
    src/file_task.c
    src/file_info.c
)
add_executable(ex01
    examples/ex01.c
    src/cosmosdb.c
    src/config.c
    src/log.c
    src/utils.c
    src/download.c
)
foreach(target music ex01)
    target_include_directories(${target} PRIVATE ${COMMON_INCLUDE_DIRS})
    target_link_directories(${target} PRIVATE ${COMMON_LINK_DIRS})
    target_link_libraries(${target} PRIVATE ${COMMON_LINK_LIBS})
endforeach()
