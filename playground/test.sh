#!/bin/bash

fusermount -u /home/wjh/CS537/P6/playground/mnt
rm -rf mnt/*
cd ../solution
make clean
make

./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200
echo "mkfs finished"
./wfs /home/wjh/CS537/P6/playground/disk1.img /home/wjh/CS537/P6/playground/disk2.img -f -s /home/wjh/CS537/P6/playground/mnt
cd ../playground
