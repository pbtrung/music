#!/bin/bash

cmake -S . -B build/release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target music

example_list=(
    ex02
)
cmake -S . -B build/examples -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
for target in "${example_list[@]}"; do
    cmake --build build/examples --target "$target"
done

# Build tests in Debug
# cmake -S . -B build/tests -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
# cmake --build build/tests --target test_music
