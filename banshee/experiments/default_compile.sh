#!/bin/bash
cd $1
rm -rf experiments
rm -rf config
rm -rf examples
rm -rf tests
aclocal
autoconf
automake
./configure CC="/home/eecs/jkodumal/work/pldi05_experiments/banshee/experiments/gcc_subst.py"
make clean all CC="/home/eecs/jkodumal/work/pldi05_experiments/banshee/experiments/gcc_subst.py -P -save-temps"