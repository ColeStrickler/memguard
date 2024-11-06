#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>


#define WRITE_BOOL(addr, value)(*(bool*)(addr) = value)
#define WRITE_UINT8(addr, value)(*(uint8_t*)(addr) = value)
#define WRITE_UINT16(addr, value)(*(uint16_t*)(addr) = value)
#define WRITE_UINT32(addr, value)(*(uint32_t*)(addr) = value)
#define WRITE_UINT64(addr, value)(*(uint64_t*)(addr) = value)

#define READ_BOOL(addr)(*(bool*)(addr))
#define READ_UINT8(addr)(*(uint8_t*)(addr))
#define READ_UINT16(addr)(*(uint16_t*)(addr))
#define READ_UINT32(addr)(*(uint32_t*)(addr))
#define READ_UINT64(addr)(*(uint64_t*)(addr))

#define PLIC_BASE_ADDR 0xC000000
#define MAX_OFFSET 0x3FFFFFC
/*
    There is up to 1024 sources and this function will only work for the first 32 of them
*/
void EnableSource(uint64_t base, int ctx, int src) 
{
    uint64_t addr = base + 0x2000 + (ctx*0x80);
    uint32_t currVal = READ_UINT32(addr);
    currVal |= (1 << ctx);
    WRITE_UINT32(addr, currVal);
}

void SetSourcePriority(uint64_t base, int src, int priority)
{
    uint64_t addr = base + (src*0x4);
    WRITE_UINT32(addr, priority);
}

void SetContextThreshold(uint64_t base, int ctx, int threshold)
{
    uint64_t addr = base + 0x200000 + (0x1000 * ctx);
    WRITE_UINT32(addr, threshold);
}

/*
    Only works for interrupt sources 0-31
*/
int ReadPendingBit(uint64_t base, int src)
{
    uint64_t addr = base + 0x1000;
    uint32_t val = READ_UINT32(addr);
    return (val >> src) & 0x1;
}

int ClearPendingBit(uint64_t base, int src)
{
    uint64_t addr = base + 0x1000;
    uint32_t val = READ_UINT32(addr);
    val &= ~(1 << src); // clear bit
    WRITE_UINT32(addr, val);
}


int OpenDevmem()
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd == -1) {
        perror("Failed to open /dev/mem");
        return -1;
    }
    return fd;
}



int main(int argc, char** argv)
{
    if (argc != 5)
    {
        printf("Usage: ./plic_en <context> <interrupt source> <context threshold> <interrupt priority>");
        exit(EXIT_SUCCESS);
    }


    int ctx = atoi(argv[1]);
    int src = atoi(argv[2]);
    int threshold = atoi(argv[3]); 
    int priority = atoi(argv[4]);



    int devMemFd = OpenDevmem();
    if (devMemFd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }
    // Map physical memory into the process's address space
    uint64_t PLIC_Base = (uint64_t)mmap(NULL, MAX_OFFSET, PROT_READ | PROT_WRITE, MAP_SHARED, devMemFd, PLIC_BASE_ADDR);
    if (PLIC_Base == (uint64_t)MAP_FAILED) {
        perror("mmap");
        close(devMemFd);
        exit(EXIT_FAILURE);
    }


    int pending = ReadPendingBit(PLIC_Base, src);
    printf("Interrupt Source pending: %d", pending);
    if (pending)
    {
        ClearPendingBit(PLIC_Base, src);
    }

    SetContextThreshold(PLIC_Base, ctx, threshold);
    SetSourcePriority(PLIC_Base, src, priority);
    EnableSource(PLIC_Base, ctx, src);

    return 0;
}