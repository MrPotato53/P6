#!/bin/bash

fusermount -u mnt
rm -rf mnt
mkdir mnt
cd ../solution
make clean
make

./mkfs -r 1 -d ../playground/disk1.img -d ../playground/disk2.img -i 32 -b 200
echo "mkfs finished"
valgrind ./wfs ../playground/disk1.img ../playground/disk2.img -f -s ../playground/mnt
cd ../playground
