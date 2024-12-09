#!/bin/bash

cd solution
make clean
make
cd ../tests
./run-tests.sh