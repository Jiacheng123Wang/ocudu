#!/usr/bin/env bash
# Rebuild the L1 harness (session tool, /tmp/l1/l1.mm) against the current engine + libs.
set -eu
BUILD=/Users/jiachengwang/dev/ocudu/build/lib/phy/upper/channel_processors/metal
DIR=CMakeFiles/ul_chain_replay.dir
cd "$BUILD"
INC=$(grep "^CXX_INCLUDES" $DIR/flags.make | sed 's/^CXX_INCLUDES = //')
DEF=$(grep "^CXX_DEFINES" $DIR/flags.make | sed 's/^CXX_DEFINES = //; s/\\"/"/g')
/usr/bin/c++ -c /tmp/l1/l1.mm -o /tmp/l1/l1.o $INC $DEF \
  -I/Users/jiachengwang/dev/ocudu/lib/phy/upper/signal_processors/channel_estimator/metal \
  -fno-rtti -O2 -std=gnu++17 -arch arm64 -fobjc-arc
LINK=$(cat $DIR/link.txt)
LINK=${LINK//CMakeFiles\/ul_chain_replay.dir\/test\/ul_chain_replay.cpp.o//tmp/l1/l1.o}
LINK=${LINK//-o ul_chain_replay/-o \/tmp\/l1\/l1}
LINK=${LINK//-Werror/}
eval "$LINK"
echo "built: $(ls -la /tmp/l1/l1)"
