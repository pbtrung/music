#!/bin/bash

find ./src ./include ./examples -regex ".*\.\(cpp\|hpp\|c\|h\|cc\|hh\)" -exec echo "{}" \; -exec clang-format -i "{}" \;
