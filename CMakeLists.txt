cmake_minimum_required(VERSION 3.50)
project(music)

set(CMAKE_CXX_STANDARD 23)

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Set build flags for different build types
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wno-interference-size")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -Wno-interference-size")
add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

find_package(PkgConfig REQUIRED)
set(PKG_LIST
    libcurl
    libavcodec
    libavformat
    libavutil
    libswresample
    libssl
    libcrypto
    fmt
    spdlog
    libzstd
    sqlite3
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
    src/curl.cpp
    src/utils.cpp
    src/audio_decoder.cpp
    src/downloader.cpp
    src/httpvfs.cpp
    src/track.cpp
)
target_include_directories(music PRIVATE ${COMMON_INCLUDE_DIRS})
target_link_directories(music PRIVATE ${COMMON_LINK_DIRS})
target_link_libraries(music PRIVATE ${COMMON_LINK_LIBS})

add_executable(ex01
    examples/ex01.cpp
)
add_executable(ex02
    examples/ex02.cpp
    src/httpvfs.cpp
    src/curl.cpp
    src/track.cpp
    src/downloader.cpp
    src/utils.cpp
)
set(EXAMPLE_LIST
    ex01
    ex02
)
foreach(target ${EXAMPLE_LIST})
    target_include_directories(${target} PRIVATE ${COMMON_INCLUDE_DIRS})
    target_link_directories(${target} PRIVATE ${COMMON_LINK_DIRS})
    target_link_libraries(${target} PRIVATE ${COMMON_LINK_LIBS})
endforeach()

add_subdirectory(external/rapidyenc)
add_subdirectory(external/wirehair)
add_executable(data
    src/data.cpp
    src/downloader.cpp
    src/track.cpp
    src/httpvfs.cpp
    src/utils.cpp
    src/httpvfs.cpp
    src/curl.cpp
    src/file_block_reader.cpp
)
list(APPEND COMMON_INCLUDE_DIRS external)
target_link_libraries(data PRIVATE rapidyenc_static)
target_link_libraries(data PRIVATE wirehair)
target_link_libraries(data PRIVATE ${COMMON_LINK_LIBS})
target_include_directories(data PRIVATE ${COMMON_INCLUDE_DIRS})
target_link_directories(data PRIVATE ${COMMON_LINK_DIRS})
