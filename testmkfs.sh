#!/bin/bash

cd solution
make clean
make mkfs
cd ../tests
./run-tests.sh