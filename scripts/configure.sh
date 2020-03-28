#!/bin/sh

if [ "$#" -ne 2 ]; then
    echo "Usage: *.sh <arm|thumb> <opt|no-opt>"
    exit 1
fi

if [ $1 = 'arm' ]; then
    thumb=""
else
    thumb="-DThumb=1"
fi

if [ $2 = 'opt' ]; then
    optimized="-DOptimized=1"
else
    optimized=""
fi

rm -rf build && mkdir build && cd build &&
    cmake ../ $thumb $optimized \
        -DDynamoRIO_DIR=../dynamorio/cmake \
        -DDrMemoryFramework_DIR=../dynamorio/drmemory/drmf \
        -DCMAKE_TOOLCHAIN_FILE=toolchain-arm32.cmake
