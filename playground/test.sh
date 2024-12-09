#!/bin/bash

fusermount -u /home/wjh/CS537/P6/playground/mnt
rm -rf mnt
mkdir mnt
cd ../solution
make clean
make

./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200
echo "mkfs finished"
valgrind ./wfs ../playground/disk1.img ../playground/disk2.img -f -s ../playground/mnt
cd ../playground