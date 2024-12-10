#!/bin/bash

fusermount -u mnt
rm -rf mnt
mkdir mnt
cd ../solution
make clean
make

./mkfs -r 0 -d ../playground/disk1.img -d ../playground/disk2.img -d ../playground/disk3.img -i 32 -b 200
echo "mkfs finished"
valgrind ./wfs ../playground/disk3.img ../playground/disk1.img ../playground/disk2.img -f -s ../playground/mnt
cd ../playground
