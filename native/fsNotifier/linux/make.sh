#!/bin/sh
set -e

CC_FLAGS="-O2 -Wall -Wextra -Wpedantic -std=c++11 -D_DEFAULT_SOURCE"

VER=$(date "+%Y%m%d.%H%M")
sed -i.bak "s/#define VERSION .*/#define VERSION \"${VER}\"/" fsnotifier.h && rm fsnotifier.h.bak

if [ -f "/usr/include/gnu/stubs-32.h" ] ; then
  echo "compiling 32-bit version"
  g++ -m32 ${CC_FLAGS} -o fsnotifier main.cpp inotify.cpp util.cpp
  chmod 755 fsnotifier
fi

if [ -f "/usr/include/gnu/stubs-64.h" ] ; then
  echo "compiling 64-bit version"
  g++ -m64 ${CC_FLAGS} -o fsnotifier64 main.cpp inotify.cpp util.cpp
  chmod 755 fsnotifier64
fi