#!/bin/bash

if [ $# -lt 1 ]; then
    echo "Usage: $0 <attacker bw(MB/s)>"
    exit 1
fi
# enable huge pages
echo 50 > /proc/sys/vm/nr_hugepages

################
#MEMGUARD SETUP#
################
insmod memguard.ko
ATTACKERBW=$1
VICTIMBW=1000000
mount -t debugfs none /sys/kernel/debug
echo $ATTACKERBW $VICTIMBW > /sys/kernel/debug/memguard/read_limit

# disable victim interrupts
devmem 0x20103010 8 0

##################
#BkPLL PARAMETERS#
##################
MEM_SIZE=33554432 # 32MB
# Color 0 = bank 0, set partion 0
# Color 1 = bank 0, set partion 1
# Color 2 = bank 1, set partion 0
# Color 3 = bank 1, set partion 1
RW=read
DRAM_BANK_MASK=0x78000
MAX_MLP=6 # 6 MSHRS



################
#RUN EXPERIMENT#
################


echo "Same Bank attacker"
./BkPLL -x -m $MEM_SIZE -a $RW -b $DRAM_BANK_MASK -l $MAX_MLP -i 10000000000 -e 0 -c 0 1>/dev/null 2>/dev/null & 
./BkPLL -x -m $MEM_SIZE -a $RW -b $DRAM_BANK_MASK -l $MAX_MLP -i 100000 -e 1 -c 1 &

killall BkPLL

echo "Diff Bank attacker"
./BkPLL -x -m $MEM_SIZE -a $RW -b $DRAM_BANK_MASK -l $MAX_MLP -i 1000000000 -e 0 -c 0 1>/dev/null 2>/dev/null & 
./BkPLL -x -m $MEM_SIZE -a $RW -b $DRAM_BANK_MASK -l $MAX_MLP -i 100000 -e 3 -c 1 & 