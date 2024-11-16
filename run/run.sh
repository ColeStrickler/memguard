#!/bin/bash

if [ $# -ne 2 ]; then
    echo "Usage: $0 <attacker bw(MB/s)> <do setup(yes,no)>"
    exit 1
fi


ATTACKERBW=$1
if [$2 != "yes" ]; then
# enable huge pages
echo 2000 > /proc/sys/vm/nr_hugepages
################
#MEMGUARD SETUP#
################
insmod memguard.ko
VICTIMBW=100000000
mount -t debugfs none /sys/kernel/debug
##################
#BkPLL PARAMETERS#
##################
MEM_SIZE=33554432 # 32MB
RW=read
# Color 0 = dram bank = 0, cache bank = 0
# Color 1 = dram bank = 0, cache bank = 1
# Color 2 = dram bank = 0, cache bank = 2
# Color 3 = dram bank = 0, cache bank = 3



DRAM_BANK_MASK=0xe0c0
# Color 0 = bank 0, set partion 0
# Color 1 = bank 0, set partion 1
# Color 2 = bank 1, set partion 0
# Color 3 = bank 1, set partion 1
#DRAM_BANK_MASK=0x78000
MAX_MLP=6 # 6 MSHRS
fi



echo "$ATTACKERBW $VICTIMBW" > /sys/kernel/debug/memguard/read_limit
devmem 0x20103010 8 0 # disable victim interrupts
./BkPLL -a write -c 0 -m 202400 -i 50 -l 6 -b 0x3e040 -e 1 -x &
./BkPLL -a read -c 1 -m 202400 -i 50 -l 6 -b 0x3e040 -e 0 -x
killall BkPLL

################
#RUN EXPERIMENT#
################
echo "Same Bank attacker"
./BkPLL -x -m $MEM_SIZE -a $RW -b $DRAM_BANK_MASK -l $MAX_MLP -i 10000000000 -e 0,1 -c 0 1>/dev/null 2>/dev/null & 
./BkPLL -x -m $MEM_SIZE -a $RW -b $DRAM_BANK_MASK -l $MAX_MLP -i 100000 -e 2,3 -c 1 

killall BkPLL

echo "Diff Bank attacker"
./BkPLL -x -m $MEM_SIZE -a $RW -b $DRAM_BANK_MASK -l $MAX_MLP -i 1000000000 -e 0,1 -c 0 1>/dev/null 2>/dev/null & 
./BkPLL -x -m $MEM_SIZE -a $RW -b $DRAM_BANK_MASK -l $MAX_MLP -i 100000 -e 2,3 -c 1 

killall BkPLL