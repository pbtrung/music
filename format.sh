#!/bin/bash

find ./src ./include -regex ".*\.\(cpp\|hpp\|c\|h\|cc\|hh\)" -exec echo "{}" \; -exec clang-format -i "{}" \;
