#!/usr/bin/env bash
./clean.sh
cmake -DCMAKE_BUILD_TYPE=Debug .
make -j

DIRECTORY=/tmp/memMount

if [ ! -d "$DIRECTORY" ]; then
  mkdir /tmp/memMount
fi

./bin/MemoryFS -d -s -f /tmp/memMount
