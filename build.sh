#!/bin/sh

set -xe

clang -O2 -Wall -Werror -pedantic-errors nanoemu.c -o nanoemu
./nanoemu ./xv6-kernel.bin ./xv6-fs.img