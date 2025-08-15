cmake_minimum_required(VERSION 3.50)
project(music)

set(CMAKE_CXX_STANDARD 20)

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Set build flags for different build types
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(PkgConfig REQUIRED)
set(PKG_LIST
    libcurl
    libpcre2-8
    libavcodec
    libavformat
    libavutil
    libswresample
    libssl
    libcrypto
    fmt
    spdlog
)
foreach(PKG ${PKG_LIST})
    pkg_check_modules(${PKG} REQUIRED ${PKG})
    list(APPEND COMMON_INCLUDE_DIRS ${${PKG}_INCLUDE_DIRS})
    list(APPEND COMMON_LINK_DIRS    ${${PKG}_LIBRARY_DIRS})
    list(APPEND COMMON_LINK_LIBS    ${${PKG}_LIBRARIES})
endforeach()
list(APPEND COMMON_INCLUDE_DIRS include)

add_executable(music
    src/main.cpp
    src/bounded_queue.cpp
    src/thread_pool.cpp
    src/curl.cpp
    src/utils.cpp
    src/audio_decoder.cpp
)
add_executable(ex01
    examples/ex01.cpp
    src/bounded_queue.cpp
)
add_executable(ex02
    examples/ex02.cpp
    src/bounded_queue.cpp
    src/thread_pool.cpp
)
add_executable(ex03
    examples/ex03.cpp
    src/curl.cpp
)
foreach(target music ex01 ex02 ex03)
    target_include_directories(${target} PRIVATE ${COMMON_INCLUDE_DIRS})
    target_link_directories(${target} PRIVATE ${COMMON_LINK_DIRS})
    target_link_libraries(${target} PRIVATE ${COMMON_LINK_LIBS})
endforeach()
