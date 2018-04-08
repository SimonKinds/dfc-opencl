#!/bin/sh
source_files=$(find src | grep '\.c\|\.h\|\.cpp\|\.hpp\|\.cl')

while read -r source; do
  clang-format -i -style=file $source
done <<< $source_files