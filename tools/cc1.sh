#!/bin/sh
# compile one generated file: cc1.sh SRC OBJDIR
o="$2/$(basename "$1" .c).o"
clang -O2 -ffp-contract=off -fno-strict-aliasing -w -c -Iruntime/include -Ibuild/gen "$1" -o "$o" 2> "${o%.o}.err" || echo "FAIL $1"
