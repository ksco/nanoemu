#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Xv6 uses only 128MiB of memory. */
#define DRAM_SIZE 1024 * 1024 * 128

/* Same as QEMU virt machine, DRAM starts at 0x80000000. */
#define DRAM_BASE 0x80000000

#define CLINT_BASE      0x2000000
#define CLINT_SIZE      0x10000
#define CLINT_MTIMECMP  CLINT_BASE + 0x4000
#define CLINT_MTIME     CLINT_BASE + 0xbff8

#define PLIC_BASE       0xc000000
#define PLIC_SIZE       0x4000000
#define PLIC_PENDING    PLIC_BASE + 0x1000
#define PLIC_SENABLE    PLIC_BASE + 0x2080
#define PLIC_SPRIORITY  PLIC_BASE + 0x201000
#define PLIC_SCLAIM     PLIC_BASE + 0x201004

#define UART_BASE   0x10000000
#define UART_SIZE   0x100
#define UART_RHR    UART_BASE + 0
#define UART_THR    UART_BASE + 0
#define UART_LCR    UART_BASE + 3
#define UART_LSR    UART_BASE + 5
#define UART_LSR_RX 1
#define UART_LSR_TX 1 << 5

#define VIRTIO_BASE             0x10001000
#define VIRTIO_SIZE             0x1000
#define VIRTIO_MAGIC            VIRTIO_BASE + 0x000
#define VIRTIO_VERSION          VIRTIO_BASE + 0x004
#define VIRTIO_DEVICE_ID        VIRTIO_BASE + 0x008
#define VIRTIO_VENDOR_ID        VIRTIO_BASE + 0x00c
#define VIRTIO_DEVICE_FEATURES  VIRTIO_BASE + 0x010
#define VIRTIO_DRIVER_FEATURES  VIRTIO_BASE + 0x020
#define VIRTIO_GUEST_PAGE_SIZE  VIRTIO_BASE + 0x028
#define VIRTIO_QUEUE_SEL        VIRTIO_BASE + 0x030
#define VIRTIO_QUEUE_NUM_MAX    VIRTIO_BASE + 0x034
#define VIRTIO_QUEUE_NUM        VIRTIO_BASE + 0x038
#define VIRTIO_QUEUE_PFN        VIRTIO_BASE + 0x040
#define VIRTIO_QUEUE_NOTIFY     VIRTIO_BASE + 0x050
#define VIRTIO_STATUS           VIRTIO_BASE + 0x070

#define VIRTIO_VRING_DESC_SIZE  16
#define VIRTIO_DESC_NUM         8

#define VIRTIO_IRQ  1
#define UART_IRQ    10

/* Machine level CSRs */
#define MSTATUS     0x300
#define MEDELEG     0x302
#define MIDELEG     0x303
#define MIE         0x304
#define MTVEC       0x305
#define MEPC        0x341
#define MCAUSE      0x342
#define MTVAL       0x343
#define MIP         0x344

/* Supervisor level CSRs */
#define SSTATUS     0x100
#define SIE         0x104
#define STVEC       0x105
#define SEPC        0x141
#define SCAUSE      0x142
#define STVAL       0x143
#define SIP         0x144
#define SATP        0x180

#define MIP_SSIP ((uint64_t)1 << 1)
#define MIP_MSIP ((uint64_t)1 << 3)
#define MIP_STIP ((uint64_t)1 << 5)
#define MIP_MTIP ((uint64_t)1 << 7)
#define MIP_SEIP ((uint64_t)1 << 9)
#define MIP_MEIP ((uint64_t)1 << 11)

#define PAGE_SIZE 4096

enum exception {
    OK                              = -1,
    INSTRUCTION_ADDRESS_MISALIGNED  = 0,
    INSTRUCTION_ACCESS_FAULT        = 1,
    ILLEGAL_INSTRUCTION             = 2,
    BREAKPOINT                      = 3,
    LOAD_ADDRESS_MISALIGNED         = 4,
    LOAD_ACCESS_FAULT               = 5,
    STORE_AMO_ADDRESS_MISALIGNED    = 6,
    STORE_AMO_ACCESS_FAULT          = 7,
    ECALL_FROM_UMODE                = 8,
    ECALL_FROM_SMODE                = 9,
    ECALL_FROM_MMODE                = 11,
    INSTRUCTION_PAGE_FAULT          = 12,
    LOAD_PAGE_FAULT                 = 13,
    STORE_AMO_PAGE_FAULT            = 15,
};

enum interrupt {
    NONE                            = -1,
    USER_SOFTWARE_INTERRUPT         = 0,
    SUPERVISOR_SOFTWARE_INTERRUPT   = 1,
    MACHINE_SOFTWARE_INTERRUPT      = 3,
    USER_TIMER_INTERRUPT            = 4,
    SUPERVISOR_TIMER_INTERRUPT      = 5,
    MACHINE_TIMER_INTERRUPT         = 7,
    USER_EXTERNAL_INTERRUPT         = 8,
    SUPERVISOR_EXTERNAL_INTERRUPT   = 9,
    MACHINE_EXTERNAL_INTERRUPT      = 11,
};

bool exception_is_fatal(enum exception exception) {
    if (exception == INSTRUCTION_ADDRESS_MISALIGNED ||
        exception == INSTRUCTION_ACCESS_FAULT ||
        exception == LOAD_ACCESS_FAULT ||
        exception == STORE_AMO_ADDRESS_MISALIGNED ||
        exception == STORE_AMO_ACCESS_FAULT) {
        return true;
    }
    return false;
}

struct dram {
    uint8_t* data;
};

struct dram* dram_new(uint8_t* code, size_t code_size) {
    struct dram* dram = calloc(1, sizeof *dram);
    dram->data = calloc(DRAM_SIZE, 1);
    memcpy(dram->data, code, code_size);
    return dram;
}

uint64_t dram_load8(struct dram* dram, uint64_t addr) {
    uint64_t index = addr - DRAM_BASE;
    return dram->data[index];
}

uint64_t dram_load16(struct dram* dram, uint64_t addr) {
    uint64_t index = addr - DRAM_BASE;
    return (uint64_t)(dram->data[index])
        | ((uint64_t)(dram->data[index + 1]) << 8);
}

uint64_t dram_load32(struct dram* dram, uint64_t addr) {
    uint64_t index = addr - DRAM_BASE;
    return (uint64_t)(dram->data[index])
        | ((uint64_t)(dram->data[index + 1]) << 8)
        | ((uint64_t)(dram->data[index + 2]) << 16)
        | ((uint64_t)(dram->data[index + 3]) << 24);
}

uint64_t dram_load64(struct dram* dram, uint64_t addr) {
    uint64_t index = addr - DRAM_BASE;
    return (uint64_t)(dram->data[index])
        | ((uint64_t)(dram->data[index + 1]) << 8)
        | ((uint64_t)(dram->data[index + 2]) << 16)
        | ((uint64_t)(dram->data[index + 3]) << 24)
        | ((uint64_t)(dram->data[index + 4]) << 32)
        | ((uint64_t)(dram->data[index + 5]) << 40)
        | ((uint64_t)(dram->data[index + 6]) << 48)
        | ((uint64_t)(dram->data[index + 7]) << 56);
}

enum exception
dram_load(struct dram* dram, uint64_t addr, uint64_t size, uint64_t *result) {
    switch (size) {
    case 8:
        *result = dram_load8(dram, addr);
        return OK;
    case 16:
        *result = dram_load16(dram, addr);
        return OK;
    case 32:
        *result = dram_load32(dram, addr);
        return OK;
    case 64:
        *result = dram_load64(dram, addr);
        return OK;
    default:
        return LOAD_ACCESS_FAULT;
    }
}

void dram_store8(struct dram* dram, uint64_t addr, uint64_t value) {
    uint64_t index = addr - DRAM_BASE;
    dram->data[index] = value;
}

void dram_store16(struct dram* dram, uint64_t addr, uint64_t value) {
    uint64_t index = addr - DRAM_BASE;
    dram->data[index + 0] = (value >> 0) & 0xff;
    dram->data[index + 1] = (value >> 8) & 0xff;
}

void dram_store32(struct dram* dram, uint64_t addr, uint64_t value) {
    uint64_t index = addr - DRAM_BASE;
    dram->data[index + 0] = (value >>  0) & 0xff;
    dram->data[index + 1] = (value >>  8) & 0xff;
    dram->data[index + 2] = (value >> 16) & 0xff;
    dram->data[index + 3] = (value >> 24) & 0xff;
}

void dram_store64(struct dram* dram, uint64_t addr, uint64_t value) {
    uint64_t index = addr - DRAM_BASE;
    dram->data[index + 0] = (value >>  0) & 0xff;
    dram->data[index + 1] = (value >>  8) & 0xff;
    dram->data[index + 2] = (value >> 16) & 0xff;
    dram->data[index + 3] = (value >> 24) & 0xff;
    dram->data[index + 4] = (value >> 32) & 0xff;
    dram->data[index + 5] = (value >> 40) & 0xff;
    dram->data[index + 6] = (value >> 48) & 0xff;
    dram->data[index + 7] = (value >> 56) & 0xff;
}

enum exception
dram_store(struct dram* dram, uint64_t addr, uint64_t size, uint64_t value) {
    switch (size) {
    case 8:
        dram_store8(dram, addr, value);
        return OK;
    case 16:
        dram_store16(dram, addr, value);
        return OK;
    case 32:
        dram_store32(dram, addr, value);
        return OK;
    case 64:
        dram_store64(dram, addr, value);
        return OK;
    default:
        return STORE_AMO_ACCESS_FAULT;
    }
}

struct clint {
    uint64_t mtime;
    uint64_t mtimecmp;
};

struct clint* clint_new() {
    struct clint* clint = calloc(1, sizeof *clint);
    return clint;
}

enum exception
clint_load(struct clint* clint, uint64_t addr, uint64_t size, uint64_t *result) {
    switch (size) {
    case 64:
        switch (addr) {
        case CLINT_MTIMECMP:
            *result = clint->mtimecmp;
            break;
        case CLINT_MTIME:
            *result = clint->mtime;
            break;
        default: *result = 0;
        }
        return OK;
    default:
        return LOAD_ACCESS_FAULT;
    }
}

enum exception
clint_store(struct clint* clint, uint64_t addr, uint64_t size, uint64_t value) {
    switch (size) {
    case 64:
        switch (addr) {
        case CLINT_MTIMECMP:
            clint->mtimecmp = value;
            break;
        case CLINT_MTIME:
            clint->mtime = value;
            break;
        }
        return OK;
    default:
        return STORE_AMO_ACCESS_FAULT;
    }
}

struct plic {
    uint64_t pending;
    uint64_t senable;
    uint64_t spriority;
    uint64_t sclaim;
};

struct plic* plic_new() {
    struct plic* plic = calloc(1, sizeof *plic);
    return plic;
}

enum exception
plic_load(struct plic* plic, uint64_t addr, uint64_t size, uint64_t *result) {
    switch (size) {
    case 32:
        switch (addr) {
        case PLIC_PENDING:
            *result = plic->pending;
            break;
        case PLIC_SENABLE:
            *result = plic->senable;
            break;
        case PLIC_SPRIORITY:
            *result = plic->spriority;
            break;
        case PLIC_SCLAIM:
            *result = plic->sclaim;
            break;
        default: *result = 0;
        }
        return OK;
    default:
        return LOAD_ACCESS_FAULT;
    }
}

enum exception
plic_store(struct plic* plic, uint64_t addr, uint64_t size, uint64_t value) {
    switch (size) {
    case 32:
        switch (addr) {
        case PLIC_PENDING:
            plic->pending = value;
            break;
        case PLIC_SENABLE:
            plic->senable = value;
            break;
        case PLIC_SPRIORITY:
            plic->spriority = value;
            break;
        case PLIC_SCLAIM:
            plic->sclaim = value;
            break;
        }
        return OK;
    default:
        return STORE_AMO_ACCESS_FAULT;
    }
}

struct uart {
    uint8_t data[UART_SIZE];
    bool interrupting;

    pthread_t tid;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

void* uart_thread(void* opaque) {
    struct uart* uart = opaque;
    while (1) {
        char c;
        read(STDIN_FILENO, &c, 1);
        pthread_mutex_lock(&uart->lock);
        while ((uart->data[UART_LSR - UART_BASE] & UART_LSR_RX) == 1) {
            pthread_cond_wait(&uart->cond, &uart->lock);
        }
        uart->data[0] = c;
        uart->interrupting = true;
        uart->data[UART_LSR - UART_BASE] |= UART_LSR_RX;
        pthread_mutex_unlock(&uart->lock);
    }

    /* Unreachable. */
    return NULL;
}

struct uart* uart_new() {
    struct uart* uart = calloc(1, sizeof *uart);
    uart->data[UART_LSR - UART_BASE] |= UART_LSR_TX;
    pthread_mutex_init(&uart->lock, NULL);
    pthread_cond_init(&uart->cond, NULL);

    pthread_create(&uart->tid, NULL, uart_thread, (void*)uart);
    return uart;
}

enum exception
uart_load(struct uart* uart, uint64_t addr, uint64_t size, uint64_t *result) {
    switch (size) {
    case 8:
        pthread_mutex_lock(&uart->lock);
        switch (addr) {
        case UART_RHR:
            pthread_cond_broadcast(&uart->cond);
            uart->data[UART_LSR - UART_BASE] &= ~UART_LSR_RX;
        default:
            *result = uart->data[addr - UART_BASE];
        }
        pthread_mutex_unlock(&uart->lock);
        return OK;
    default:
        return LOAD_ACCESS_FAULT;
    }
}

enum exception
uart_store(struct uart* uart, uint64_t addr, uint64_t size, uint64_t value) {
    switch (size) {
    case 8:
        pthread_mutex_lock(&uart->lock);
        switch (addr) {
        case UART_THR:
            printf("%c", (char)(value & 0xff));
            fflush(stdout);
            break;
        default:
            uart->data[addr - UART_BASE] = value & 0xff;
        }
        pthread_mutex_unlock(&uart->lock);
        return OK;
    default:
        return STORE_AMO_ACCESS_FAULT;
    }
}

bool uart_interrupting(struct uart* uart) {
    pthread_mutex_lock(&uart->lock);
    bool interrupting = uart->interrupting;
    uart->interrupting = false;
    pthread_mutex_unlock(&uart->lock);
    return interrupting;
}

struct virtio {
    uint64_t id;
    uint32_t driver_features;
    uint32_t page_size;
    uint32_t queue_sel;
    uint32_t queue_num;
    uint32_t queue_pfn;
    uint32_t queue_notify;
    uint32_t status;
    uint8_t *disk;
};

struct virtio* virtio_new(uint8_t* disk) {
    struct virtio* virtio = calloc(1, sizeof *virtio);
    virtio->disk = disk;
    virtio->queue_notify = -1;
    return virtio;
}


enum exception
virtio_load(struct virtio* virtio, uint64_t addr, uint64_t size, uint64_t *result) {
    switch (size) {
    case 32:
        switch (addr) {
        case VIRTIO_MAGIC:
            *result = 0x74726976;
            break;
        case VIRTIO_VERSION:
            *result = 0x1;
            break;
        case VIRTIO_DEVICE_ID:
            *result = 0x2;
            break;
        case VIRTIO_VENDOR_ID:
            *result = 0x554d4551;
            break;
        case VIRTIO_DEVICE_FEATURES:
            *result = 0;
            break;
        case VIRTIO_DRIVER_FEATURES:
            *result = virtio->driver_features;
            break;
        case VIRTIO_QUEUE_NUM_MAX:
            *result = 8;
            break;
        case VIRTIO_QUEUE_PFN:
            *result = virtio->queue_pfn;
            break;
        case VIRTIO_STATUS:
            *result = virtio->status;
            break;
        default: *result = 0;
        }
        return OK;
    default:
        return LOAD_ACCESS_FAULT;
    }
}

enum exception
virtio_store(struct virtio* virtio, uint64_t addr, uint64_t size, uint64_t value) {
    switch (size) {
    case 32:
        switch (addr) {
        case VIRTIO_DEVICE_FEATURES:
            virtio->driver_features = value;
            break;
        case VIRTIO_GUEST_PAGE_SIZE:
            virtio->page_size = value;
            break;
        case VIRTIO_QUEUE_SEL:
            virtio->queue_sel = value;
            break;
        case VIRTIO_QUEUE_NUM:
            virtio->queue_num = value;
            break;
        case VIRTIO_QUEUE_PFN:
            virtio->queue_pfn = value;
            break;
        case VIRTIO_QUEUE_NOTIFY:
            virtio->queue_notify = value;
            break;
        case VIRTIO_STATUS:
            virtio->status = value;
            break;
        }
        return OK;
    default:
        return STORE_AMO_ACCESS_FAULT;
    }
}

bool virtio_is_interrupting(struct virtio* virtio) {
    if (virtio->queue_notify != -1) {
        virtio->queue_notify = -1;
        return true;
    }
    return false;
}

uint64_t virtio_desc_addr(struct virtio* virtio) {
    return (uint64_t)virtio->queue_pfn * (uint64_t)virtio->page_size;
}

uint64_t virtio_disk_read(struct virtio* virtio, uint64_t addr) {
    return virtio->disk[addr];
}

void virtio_disk_write(struct virtio* virtio, uint64_t addr, uint64_t value) {
    virtio->disk[addr] = (uint8_t)value;
}

uint64_t virtio_new_id(struct virtio* virtio) {
    virtio->id += 1;
    return virtio->id;
}

struct bus {
    struct dram* dram;
    struct clint* clint;
    struct plic* plic;
    struct uart* uart;
    struct virtio *virtio;
};

struct bus* bus_new(struct dram* dram, struct virtio* virtio) {
    struct bus* bus = calloc(1, sizeof *bus);
    bus->dram = dram;
    bus->virtio = virtio;
    bus->clint = clint_new();
    bus->plic = plic_new();
    bus->uart = uart_new();
    return bus;
}

enum exception
bus_load(struct bus* bus, uint64_t addr, uint64_t size, uint64_t *result) {
    if (CLINT_BASE <= addr && addr < CLINT_BASE + CLINT_SIZE) {
        return clint_load(bus->clint, addr, size, result);
    }
    if (PLIC_BASE <= addr && addr < PLIC_BASE + PLIC_SIZE) {
        return plic_load(bus->plic, addr, size, result);
    }
    if (UART_BASE <= addr && addr < UART_BASE + UART_SIZE) {
        return uart_load(bus->uart, addr, size, result);
    }
    if (VIRTIO_BASE <= addr && addr < VIRTIO_BASE + VIRTIO_SIZE) {
        return virtio_load(bus->virtio, addr, size, result);
    }
    if (DRAM_BASE <= addr) {
        return dram_load(bus->dram, addr, size, result);
    }

    return LOAD_ACCESS_FAULT;
}

enum exception
bus_store(struct bus* bus, uint64_t addr, uint64_t size, uint64_t value) {
    if (CLINT_BASE <= addr && addr < CLINT_BASE + CLINT_SIZE) {
        return clint_store(bus->clint, addr, size, value);
    }
    if (PLIC_BASE <= addr && addr < PLIC_BASE + PLIC_SIZE) {
        return plic_store(bus->plic, addr, size, value);
    }
    if (UART_BASE <= addr && addr < UART_BASE + UART_SIZE) {
        return uart_store(bus->uart, addr, size, value);
    }
    if (VIRTIO_BASE <= addr && addr < VIRTIO_BASE + VIRTIO_SIZE) {
        return virtio_store(bus->virtio, addr, size, value);
    }
    if (DRAM_BASE <= addr) {
        return dram_store(bus->dram, addr, size, value);
    }

    return STORE_AMO_ACCESS_FAULT;
}

void bus_disk_access(struct bus* bus) {
    uint64_t desc_addr = virtio_desc_addr(bus->virtio);
    uint64_t avail_addr = desc_addr + 0x40;
    uint64_t used_addr = desc_addr + 4096;

    uint64_t offset;
    if (bus_load(bus, avail_addr + 1, 16, &offset) != OK) {
        printf("ERROR: failed to read offset.\n");
        exit(1);
    }

    uint64_t index;
    if (bus_load(bus, avail_addr + (offset % VIRTIO_DESC_NUM) + 2, 16, &index) != OK) {
        printf("ERROR: failed to read index.\n");
        exit(1);
    }

    uint64_t desc_addr0 = desc_addr + VIRTIO_VRING_DESC_SIZE * index;
    uint64_t addr0;
    if (bus_load(bus, desc_addr0, 64, &addr0) != OK) {
        printf("ERROR: failed to read address field in descriptor.\n");
        exit(1);
    }
    uint64_t next0;
    if (bus_load(bus, desc_addr0 + 14, 16, &next0) != OK) {
        printf("ERROR: failed to read next field in descriptor.\n");
        exit(1);
    }
    uint64_t desc_addr1 = desc_addr + VIRTIO_VRING_DESC_SIZE * next0;
    uint64_t addr1;
    if (bus_load(bus, desc_addr1, 64, &addr1) != OK) {
        printf("ERROR: failed to read address field in descriptor.\n");
        exit(1);
    }
    uint64_t len1;
    if (bus_load(bus, desc_addr1 + 8, 32, &len1) != OK) {
        printf("ERROR: failed to read length field in descriptor.\n");
        exit(1);
    }
    uint64_t flags1;
    if (bus_load(bus, desc_addr1 + 12, 16, &flags1) != OK) {
        printf("ERROR: failed to read flags field in descriptor.\n");
        exit(1);
    }

    uint64_t blk_sector;
    if (bus_load(bus, addr0 + 8, 64, &blk_sector) != OK) {
        printf("ERROR: failed to read sector field in virtio_blk_outhdr.\n");
        exit(1);
    }

    if ((flags1 & 2) == 0) {
        /* Read dram data and write it to a disk directly (DMA). */
        for (uint64_t i = 0; i < len1; i++) {
            uint64_t data;
            if (bus_load(bus, addr1 + i, 8, &data) != OK) {
                printf("ERROR: failed to read from dram.\n");
                exit(1);
            }
            virtio_disk_write(bus->virtio, blk_sector * 512 + i, data);
        }
    } else {
        /* Read disk data and write it to dram directly (DMA). */
        for (uint64_t i = 0; i < len1; i++) {
            uint64_t data = virtio_disk_read(bus->virtio, blk_sector * 512 + i);
            if (bus_store(bus, addr1 +i, 8, data) != OK) {
                printf("ERROR: failed to write to dram.\n");
                exit(1);
            }
        }
    }

    uint64_t new_id = virtio_new_id(bus->virtio);
    if (bus_store(bus, used_addr + 2, 16, new_id % 8) != OK) {
        printf("ERROR: failed to write to dram.\n");
        exit(1);
    }
}

enum mode {
    USER = 0x0,
    SUPERVISOR = 0x1,
    MACHINE = 0x3
};

struct cpu {
    uint64_t regs[32];
    uint64_t pc;
    uint64_t csrs[4096];
    enum mode mode;
    struct bus* bus;
    bool enable_paging;
    uint64_t pagetable;
};

struct cpu* cpu_new(uint8_t* code, size_t code_size, uint8_t* disk) {
    struct cpu* cpu = calloc(1, sizeof *cpu);

    /* Initialize the sp(x2) register. */
    cpu->regs[2] = DRAM_BASE + DRAM_SIZE;

    cpu->bus = bus_new(dram_new(code, code_size), virtio_new(disk));
    cpu->pc = DRAM_BASE;
    cpu->mode = MACHINE;

    return cpu;
}

uint64_t cpu_load_csr(struct cpu* cpu, uint16_t addr);
void cpu_update_paging(struct cpu* cpu, uint16_t csr_addr) {
    if (csr_addr != SATP) return;

    cpu->pagetable = (cpu_load_csr(cpu, SATP) & (((uint64_t)1 << 44) - 1)) * PAGE_SIZE;
    uint64_t mode = cpu_load_csr(cpu, SATP) >> 60;

    if (mode == 8)
        cpu->enable_paging = true;
    else
        cpu->enable_paging = false;
}

enum exception
cpu_translate(struct cpu* cpu, uint64_t addr, enum exception e, uint64_t *result) {
    if (!cpu->enable_paging) {
        *result = addr;
        return OK;
    }

    int levels = 3;
    uint64_t vpn[] = {
        (addr >> 12) & 0x1ff,
        (addr >> 21) & 0x1ff,
        (addr >> 30) & 0x1ff
    };

    uint64_t a = cpu->pagetable;
    int i = levels - 1;
    uint64_t pte;
    enum exception exception;
    while (1) {
        if ((exception = bus_load(cpu->bus, a + vpn[i] * 8, 64, &pte)) != OK) {
            return exception;
        }
        bool v = pte & 1;
        bool r = (pte >> 1) & 1;
        bool w = (pte >> 2) & 1;
        bool x = (pte >> 3) & 1;
        if (v == false || (r == false && w == true)) {
            return e;
        }

        if (r == true || x == true) break;

        i -= 1;
        uint64_t ppn = (pte >> 10) & 0x0fffffffffff;
        a = ppn * PAGE_SIZE;
        if (i < 0) return e;
    }

    uint64_t ppn[] = {
        (pte >> 10) & 0x1ff,
        (pte >> 19) & 0x1ff,
        (pte >> 28) & 0x03ffffff
    };

    uint64_t offset = addr & 0xfff;
    switch (i) {
    case 0: {
        uint64_t ppn = (pte >> 10) & 0x0fffffffffff;
        *result = (ppn << 12) | offset;
        return OK;
    }
    case 1: {
        *result = (ppn[2] << 30) | (ppn[1] << 21) | (vpn[0] << 12) | offset;
        return OK;
    }
    case 2: {
        *result = (ppn[2] << 30) | (vpn[1] << 21) | (vpn[0] << 12) | offset;
        return OK;
    }
    default: {
        return e;
    }
    }
}

/* Fetch an instruction from current PC from DRAM. */
enum exception cpu_fetch(struct cpu* cpu, uint64_t* result) {
    uint64_t ppc;
    enum exception exception;
    if ((exception = cpu_translate(cpu, cpu->pc, INSTRUCTION_PAGE_FAULT, &ppc)) != OK) {
        return exception;
    }
    if (bus_load(cpu->bus, ppc, 32, result) != OK) {
        return INSTRUCTION_ACCESS_FAULT;
    }
    return OK;
}

uint64_t cpu_load_csr(struct cpu* cpu, uint16_t addr) {
    if (addr == SIE) {
        return cpu->csrs[MIE] & cpu->csrs[MIDELEG];
    }
    return cpu->csrs[addr];
}

void cpu_store_csr(struct cpu* cpu, uint16_t addr, uint64_t value) {
    if (addr == SIE) {
        cpu->csrs[MIE] = (cpu->csrs[MIE] & ~cpu->csrs[MIDELEG]) | (value & cpu->csrs[MIDELEG]);
        return;
    }
    cpu->csrs[addr] = value;
}

enum exception
cpu_load(struct cpu* cpu, uint64_t addr, uint64_t size, uint64_t *result) {
    uint64_t pa;
    enum exception exception;
    if ((exception = cpu_translate(cpu, addr, LOAD_PAGE_FAULT, &pa)) != OK) {
        return exception;
    }
    return bus_load(cpu->bus, pa, size, result);
}

enum exception
cpu_store(struct cpu* cpu, uint64_t addr, uint64_t size, uint64_t value) {
    uint64_t pa;
    enum exception exception;
    if ((exception = cpu_translate(cpu, addr, STORE_AMO_PAGE_FAULT, &pa)) != OK) {
        return exception;
    }
    return bus_store(cpu->bus, pa, size, value);
}

void cpu_dump_csrs(struct cpu* cpu) {
    printf("mstatus=0x%016llx mtvec=0x%016llx mepc=0x%016llx mcause=0x%016llx\n",
        cpu_load_csr(cpu, MSTATUS),
        cpu_load_csr(cpu, MTVEC),
        cpu_load_csr(cpu, MEPC),
        cpu_load_csr(cpu, MCAUSE));
    printf("sstatus=0x%016llx stvec=0x%016llx sepc=0x%016llx scause=0x%016llx\n",
        cpu_load_csr(cpu, SSTATUS),
        cpu_load_csr(cpu, STVEC),
        cpu_load_csr(cpu, SEPC),
        cpu_load_csr(cpu, SCAUSE));
}

enum exception cpu_execute(struct cpu* cpu, uint64_t inst) {
    uint64_t opcode = inst & 0x7f;
    uint64_t rd = (inst >> 7) & 0x1f;
    uint64_t rs1 = (inst >> 15) & 0x1f;
    uint64_t rs2 = (inst >> 20) & 0x1f;
    uint64_t funct3 = (inst >> 12) & 0x7;
    uint64_t funct7 = (inst >> 25) & 0x7f;

    /* The x0 register is always zero. */
    cpu->regs[0] = 0;

    enum exception exception;
    switch (opcode) {
    case 0x03: {
        uint64_t imm = (int32_t)inst >> 20;
        uint64_t addr = cpu->regs[rs1] + imm;
        switch (funct3)
        {
        case 0x0: /* lb */ {
            uint64_t result;
            if ((exception = cpu_load(cpu, addr, 8, &result)) != OK) {
                return exception;
            }
            cpu->regs[rd] = (int8_t)result;
            break;
        }
        case 0x1: /* lh */ {
            uint64_t result;
            if ((exception = cpu_load(cpu, addr, 16, &result)) != OK) {
                return exception;
            }
            cpu->regs[rd] = (int16_t)result;
            break;
        }
        case 0x2: /* lw */ {
            uint64_t result;
            if ((exception = cpu_load(cpu, addr, 32, &result)) != OK) {
                return exception;
            }
            cpu->regs[rd] = (int32_t)result;
            break;
        }
        case 0x3: /* ld */ {
            uint64_t result;
            if ((exception = cpu_load(cpu, addr, 64, &result)) != OK) {
                return exception;
            }
            cpu->regs[rd] = result;
            break;
        }
        case 0x4: /* lbu */ {
            uint64_t result;
            if ((exception = cpu_load(cpu, addr, 8, &result)) != OK) {
                return exception;
            }
            cpu->regs[rd] = result;
            break;
        }
        case 0x5: /* lhu */ {
            uint64_t result;
            if ((exception = cpu_load(cpu, addr, 16, &result)) != OK) {
                return exception;
            }
            cpu->regs[rd] = result;
            break;
        }
        case 0x6: /* lwu */ {
            uint64_t result;
            if ((exception = cpu_load(cpu, addr, 32, &result)) != OK) {
                return exception;
            }
            cpu->regs[rd] = result;
            break;
        }
        default: return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x0f: {
        switch (funct3) {
        case 0x0: /* fence */
            /* Do nothing. */
            break;
        default: return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x13: {
        uint64_t imm = (int32_t)(inst & 0xfff00000) >> 20;
        uint32_t shamt = imm & 0x3f;

        switch (funct3)
        {
        case 0x0: /* addi */
            cpu->regs[rd] = cpu->regs[rs1] + imm;
            break;
        case 0x1: /* slli */
            cpu->regs[rd] = cpu->regs[rs1] << shamt;
            break;
        case 0x2: /* slti */
            cpu->regs[rd] = (int64_t)cpu->regs[rs1] < (int64_t)imm ? 1 : 0;
            break;
        case 0x3: /* sltiu */
            cpu->regs[rd] = cpu->regs[rs1] < imm ? 1 : 0;
            break;
        case 0x4: /* xori */
            cpu->regs[rd] = cpu->regs[rs1] ^ imm;
            break;
        case 0x5:
            switch (funct7 >> 1)
            {
            case 0x00: /* srli */
                cpu->regs[rd] = cpu->regs[rs1] >> shamt;
                break;
            case 0x10: /* srai */
                cpu->regs[rd] = (int64_t)(cpu->regs[rs1]) >> shamt;
                break;
            default: return ILLEGAL_INSTRUCTION;
            }
            break;
        case 0x6: /* ori */
            cpu->regs[rd] = cpu->regs[rs1] | imm;
            break;
        case 0x7: /* andi */
            cpu->regs[rd] = cpu->regs[rs1] & imm;
            break;
        default: return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x17: /* auipc */ {
        uint64_t imm = (int32_t)(inst & 0xfffff000);
        cpu->regs[rd] = cpu->pc + imm - 4;
        break;
    }
    case 0x1b: {
        uint64_t imm = (int32_t)inst >> 20;
        uint32_t shamt = imm & 0x1f;
        switch (funct3)
        {
        case 0x0: /* addiw */
            cpu->regs[rd] = (int32_t)(cpu->regs[rs1] + imm);
            break;
        case 0x1: /* slliw */
            cpu->regs[rd] = (int32_t)(cpu->regs[rs1] << shamt);
            break;
        case 0x5: {
            switch (funct7) {
            case 0x00: /* srliw */
                cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] >> shamt);
                break;
            case 0x20: /* sraiw */
                cpu->regs[rd] = (int32_t)(cpu->regs[rs1]) >> shamt;
                break;
            default: return ILLEGAL_INSTRUCTION;
            }
            break;
        }
        default: return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x23: {
        uint64_t imm = (uint64_t)((int32_t)(inst & 0xfe000000) >> 20) | ((inst >> 7) & 0x1f);
        uint64_t addr = cpu->regs[rs1] + imm;
        switch (funct3)
        {
        case 0x0: /* sb */
            if ((exception = cpu_store(cpu, addr, 8, cpu->regs[rs2])) != OK) {
                return exception;
            }
            break;
        case 0x1: /* sh */
            if ((exception = cpu_store(cpu, addr, 16, cpu->regs[rs2])) != OK) {
                return exception;
            }
            break;
        case 0x2: /* sw */
            if ((exception = cpu_store(cpu, addr, 32, cpu->regs[rs2])) != OK) {
                return exception;
            }
            break;
        case 0x3: /* sd */
            if ((exception = cpu_store(cpu, addr, 64, cpu->regs[rs2])) != OK) {
                return exception;
            }
            break;
        default: return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x2f: {
        uint64_t funct5 = (funct7 & 0x7c) >> 2;
        if (funct3 == 0x2 && funct5 == 0x00) { /* amoadd.w */
            uint64_t t;
            if ((exception = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK) {
                return exception;
            }
            if ((exception = cpu_store(cpu, cpu->regs[rs1], 32, t + cpu->regs[rs2])) != OK) {
                return exception;
            }
            cpu->regs[rd] = t;
        } else if (funct3 == 0x3 && funct5 == 0x00) { /* amoadd.d */
            uint64_t t;
            if ((exception = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK) {
                return exception;
            }
            if ((exception = cpu_store(cpu, cpu->regs[rs1], 64, t + cpu->regs[rs2])) != OK) {
                return exception;
            }
            cpu->regs[rd] = t;
        } else if (funct3 == 0x2 && funct5 == 0x01) { /* amoswap.w */
            uint64_t t;
            if ((exception = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK) {
                return exception;
            }
            if ((exception = cpu_store(cpu, cpu->regs[rs1], 32, cpu->regs[rs2])) != OK) {
                return exception;
            }
            cpu->regs[rd] = t;
        } else if (funct3 == 0x3 && funct5 == 0x01) { /* amoswap.d */
            uint64_t t;
            if ((exception = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK) {
                return exception;
            }
            if ((exception = cpu_store(cpu, cpu->regs[rs1], 64, cpu->regs[rs2])) != OK) {
                return exception;
            }
            cpu->regs[rd] = t;
        } else {
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x33: {
        uint32_t shamt = cpu->regs[rs2] & 0x3f;
        if (funct3 == 0x0 && funct7 == 0x00) { /* add */
            cpu->regs[rd] = cpu->regs[rs1] + cpu->regs[rs2];
        } else if (funct3 == 0x0 && funct7 == 0x01) { /* mul */
            cpu->regs[rd] = cpu->regs[rs1] * cpu->regs[rs2];
        } else if (funct3 == 0x0 && funct7 == 0x20) { /* sub */
            cpu->regs[rd] = cpu->regs[rs1] - cpu->regs[rs2];
        } else if (funct3 == 0x1 && funct7 == 0x00) { /* sll */
            cpu->regs[rd] = cpu->regs[rs1] << shamt;
        } else if (funct3 == 0x2 && funct7 == 0x00) { /* slt */
            cpu->regs[rd] = (int64_t)cpu->regs[rs1] < (int64_t)cpu->regs[rs2] ? 1 : 0;
        } else if (funct3 == 0x3 && funct7 == 0x00) { /* sltu */
            cpu->regs[rd] = cpu->regs[rs1] < cpu->regs[rs2] ? 1 : 0;
        } else if (funct3 == 0x4 && funct7 == 0x00) { /* xor */
            cpu->regs[rd] = cpu->regs[rs1] ^ cpu->regs[rs2];
        } else if (funct3 == 0x5 && funct7 == 0x00) { /* srl */
            cpu->regs[rd] = cpu->regs[rs1] >> shamt;
        } else if (funct3 == 0x5 && funct7 == 0x20) { /* sra */
            cpu->regs[rd] = (int64_t)cpu->regs[rs1] >> shamt;
        } else if (funct3 == 0x6 && funct7 == 0x00) { /* or */
            cpu->regs[rd] = cpu->regs[rs1] | cpu->regs[rs2];
        } else if (funct3 == 0x7 && funct7 == 0x00) { /* and */
            cpu->regs[rd] = cpu->regs[rs1] & cpu->regs[rs2];
        } else {
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x37: { /* lui */
        cpu->regs[rd] = (int32_t)(inst & 0xfffff000);
        break;
    }
    case 0x3b: {
        uint32_t shamt = cpu->regs[rs2] & 0x1f;
        if (funct3 == 0x0 && funct7 == 0x00) { /* addw */
            cpu->regs[rd] = (int32_t)(cpu->regs[rs1] + cpu->regs[rs2]);
        } else if (funct3 == 0x0 && funct7 == 0x20) { /* subw */
            cpu->regs[rd] = (int32_t)(cpu->regs[rs1] - cpu->regs[rs2]);
        } else if (funct3 == 0x1 && funct7 == 0x00) { /* sllw */
            cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] << shamt);
        } else if (funct3 == 0x5 && funct7 == 0x00) { /* srlw */
            cpu->regs[rd] = (int32_t)((uint32_t)cpu->regs[rs1] >> shamt);
        } else if (funct3 == 0x5 && funct7 == 0x01) { /* divu */
            cpu->regs[rd] = cpu->regs[rs2] == 0
                ? -1
                : cpu->regs[rs1] / cpu->regs[rs2];
        } else if (funct3 == 0x5 && funct7 == 0x20) { /* sraw */
            cpu->regs[rd] = (int32_t)cpu->regs[rs1] >> (int32_t)shamt;
        } else if (funct3 == 0x7 && funct7 == 0x01) { /* remuw */
            cpu->regs[rd] = cpu->regs[rs2] == 0
                ? cpu->regs[rs1]
                : (int32_t)((uint32_t)cpu->regs[rs1] % (uint32_t)cpu->regs[rs2]);
        } else {
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x63: {
        uint64_t imm = (uint64_t)((int32_t)(inst & 0x80000000) >> 19)
            | ((inst & 0x80) << 4)
            | ((inst >> 20) & 0x7e0)
            | ((inst >> 7) & 0x1e);

        switch (funct3)
        {
        case 0x0: /* beq */
            if (cpu->regs[rs1] == cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        case 0x1: /* bne */
            if (cpu->regs[rs1] != cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        case 0x4: /* blt */
            if ((int64_t)cpu->regs[rs1] < (int64_t)cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        case 0x5: /* bge */
            if ((int64_t)cpu->regs[rs1] >= (int64_t)cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        case 0x6: /* bltu */
            if (cpu->regs[rs1] < cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        case 0x7: /* bgeu */
            if (cpu->regs[rs1] >= cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        default: return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x67: { /* jalr */
        uint64_t t = cpu->pc;
        uint64_t imm = (int32_t)(inst & 0xfff00000) >> 20;
        cpu->pc = (cpu->regs[rs1] + imm) & ~1;

        cpu->regs[rd] = t;
        break;
    }
    case 0x6f: { /* jal */
        cpu->regs[rd] = cpu->pc;

        uint64_t imm = (uint64_t)((int32_t)(inst & 0x80000000) >> 11)
            | (inst & 0xff000)
            | ((inst >> 9) & 0x800)
            | ((inst >> 20) & 0x7fe);

        cpu->pc += imm - 4;
        break;
    }
    case 0x73: {
        uint16_t addr = (inst & 0xfff00000) >> 20;
        switch (funct3) {
        case 0x0: {
            if (rs2 == 0x0 && funct7 == 0x0) { /* ecall */
                switch (cpu->mode) {
                case USER: return ECALL_FROM_UMODE;
                case SUPERVISOR: return ECALL_FROM_SMODE;
                case MACHINE: return ECALL_FROM_MMODE;
                }
            } else if (rs2 == 0x1 && funct7 == 0x0) { /* ebreak */
                return BREAKPOINT;
            } else if (rs2 == 0x2 && funct7 == 0x8) { /* sret */
                cpu->pc = cpu_load_csr(cpu, SEPC);
                cpu->mode = ((cpu_load_csr(cpu, SSTATUS) >> 8) & 1) == 1
                    ? SUPERVISOR
                    : USER;
                cpu_store_csr(cpu, SSTATUS, ((cpu_load_csr(cpu, SSTATUS) >> 5) & 1) == 1
                    ? cpu_load_csr(cpu, SSTATUS) | (1 << 1)
                    : cpu_load_csr(cpu, SSTATUS) & ~(1 << 1));
                cpu_store_csr(cpu, SSTATUS, cpu_load_csr(cpu, SSTATUS) | (1 << 5));
                cpu_store_csr(cpu, SSTATUS, cpu_load_csr(cpu, SSTATUS) & ~(1 << 8));
            } else if (rs2 == 0x2 && funct7 == 0x18) { /* mret */
                cpu->pc = cpu_load_csr(cpu, MEPC);
                uint64_t mpp = (cpu_load_csr(cpu, MSTATUS) >> 11) & 3;
                cpu->mode = mpp == 2 ? MACHINE : (mpp == 1 ? SUPERVISOR : USER);
                cpu_store_csr(cpu, MSTATUS, (((cpu_load_csr(cpu, MSTATUS) >> 7) & 1) == 1)
                    ? cpu_load_csr(cpu, MSTATUS) | (1 << 3)
                    : cpu_load_csr(cpu, MSTATUS) & ~(1 << 3));
                cpu_store_csr(cpu, MSTATUS, cpu_load_csr(cpu, MSTATUS) | (1 << 7));
                cpu_store_csr(cpu, MSTATUS, cpu_load_csr(cpu, MSTATUS) & ~(3 << 11));
            } else if (funct7 == 0x9) { /* sfence.vma */
                /* Do nothing. */
            } else {
                return ILLEGAL_INSTRUCTION;
            }
            break;
        }
        case 0x1: { /* csrrw */
            uint64_t t = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, cpu->regs[rs1]);
            cpu->regs[rd] = t;
            cpu_update_paging(cpu, addr);
            break;
        }
        case 0x2: { /* csrrs */
            uint64_t t = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, t | cpu->regs[rs1]);
            cpu->regs[rd] = t;
            cpu_update_paging(cpu, addr);
            break;
        }
        case 0x3: { /* csrrc */
            uint64_t t = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, t & ~cpu->regs[rs1]);
            cpu->regs[rd] = t;
            cpu_update_paging(cpu, addr);
            break;
        }
        case 0x5: { /* csrrwi */
            cpu->regs[rd] = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, rs1);
            cpu_update_paging(cpu, addr);
            break;
        }
        case 0x6: { /* csrrsi */
            uint64_t t = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, t | rs1);
            cpu->regs[rd] = t;
            cpu_update_paging(cpu, addr);
            break;
        }
        case 0x7: { /* csrrci */
            uint64_t t = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, t & ~rs1);
            cpu->regs[rd] = t;
            cpu_update_paging(cpu, addr);
            break;
        }
        default: return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    default: return ILLEGAL_INSTRUCTION;
    }

    return OK;
}

void cpu_dump_registers(struct cpu* cpu) {
    char* abi[32] = {
        "zero", " ra ", " sp ", " gp ", " tp ", " t0 ", " t1 ", " t2 ", " s0 ", " s1 ", " a0 ",
        " a1 ", " a2 ", " a3 ", " a4 ", " a5 ", " a6 ", " a7 ", " s2 ", " s3 ", " s4 ", " s5 ",
        " s6 ", " s7 ", " s8 ", " s9 ", " s10", " s11", " t3 ", " t4 ", " t5 ", " t6 ",
    };

    for (int i = 0; i < 32; i += 4) {
        printf("x%-2d(%4s)=0x%016llx  ", i + 0, abi[i + 0], cpu->regs[i + 0]);
        printf("x%-2d(%4s)=0x%016llx  ", i + 1, abi[i + 1], cpu->regs[i + 1]);
        printf("x%-2d(%4s)=0x%016llx  ", i + 2, abi[i + 2], cpu->regs[i + 2]);
        printf("x%-2d(%4s)=0x%016llx\n", i + 3, abi[i + 3], cpu->regs[i + 3]);
    }
}

void cpu_take_trap(struct cpu* cpu, enum exception exception, enum interrupt interrupt) {
    uint64_t exception_pc = cpu->pc - 4;
    enum mode previous_mode = cpu->mode;

    bool is_interrupt = interrupt != NONE;
    uint64_t cause = exception;
    if (is_interrupt) {
        cause = ((uint64_t)1 << 63) | (uint64_t)interrupt;
    }

    if (previous_mode <= SUPERVISOR && (((cpu_load_csr(cpu, MEDELEG) >> (uint32_t)cause) & 1) != 0)) {
        cpu->mode = SUPERVISOR;
        if (is_interrupt) {
            uint64_t vec = (cpu_load_csr(cpu, STVEC) & 1) == 1 ? (4 * cause) : 0;
            cpu->pc = (cpu_load_csr(cpu, STVEC) & ~1) + vec;
        } else {
            cpu->pc = cpu_load_csr(cpu, STVEC) & ~1;
        }
        cpu_store_csr(cpu, SEPC, exception_pc & ~1);
        cpu_store_csr(cpu, SCAUSE, cause);
        cpu_store_csr(cpu, STVAL, 0);
        cpu_store_csr(cpu, SSTATUS, ((cpu_load_csr(cpu, SSTATUS) >> 1) & 1) == 1
            ? cpu_load_csr(cpu, SSTATUS) | (1 << 5)
            : cpu_load_csr(cpu, SSTATUS) & ~(1 << 5));
        cpu_store_csr(cpu, SSTATUS, cpu_load_csr(cpu, SSTATUS) & ~(1 << 1));
        if (previous_mode == USER) {
            cpu_store_csr(cpu, SSTATUS, cpu_load_csr(cpu, SSTATUS) & ~(1 << 8));
        } else {
            cpu_store_csr(cpu, SSTATUS, cpu_load_csr(cpu, SSTATUS) | (1 << 8));
        }
    } else {
        cpu->mode = MACHINE;

        if (is_interrupt) {
            uint64_t vec = (cpu_load_csr(cpu, MTVEC) & 1) == 1 ? 4 * cause : 0;
            cpu->pc = (cpu_load_csr(cpu, MTVEC) & ~1) + vec;
        } else {
            cpu->pc = cpu_load_csr(cpu, MTVEC) & ~1;
        }
        cpu_store_csr(cpu, MEPC, exception_pc & ~1);
        cpu_store_csr(cpu, MCAUSE, cause);
        cpu_store_csr(cpu, MTVAL, 0);
        cpu_store_csr(cpu, MSTATUS, ((cpu_load_csr(cpu, MSTATUS) >> 3) & 1) == 1
            ? cpu_load_csr(cpu, MSTATUS) | (1 << 7)
            : cpu_load_csr(cpu, MSTATUS) & ~(1 << 7));
        cpu_store_csr(cpu, MSTATUS, cpu_load_csr(cpu, MSTATUS) & ~(1 << 3));
        cpu_store_csr(cpu, MSTATUS, cpu_load_csr(cpu, MSTATUS) & ~(3 << 11));
    }
}

enum interrupt cpu_check_pending_interrupt(struct cpu* cpu) {
    if (cpu->mode == MACHINE) {
        if (((cpu_load_csr(cpu, MSTATUS) >> 3) & 1) == 0) {
            return NONE;
        }
    } else if (cpu->mode == SUPERVISOR) {
        if (((cpu_load_csr(cpu, SSTATUS) >> 1) & 1) == 0) {
            return NONE;
        }
    }

    uint64_t irq = 0;
    if (uart_interrupting(cpu->bus->uart)) {
        irq = UART_IRQ;
    } else if (virtio_is_interrupting(cpu->bus->virtio)) {
        bus_disk_access(cpu->bus);
        irq = VIRTIO_IRQ;
    }

    if (irq != 0) {
        bus_store(cpu->bus, PLIC_SCLAIM, 32, irq);
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) | MIP_SEIP);
    }

    uint64_t pending = cpu_load_csr(cpu, MIE) & cpu_load_csr(cpu, MIP);
    if (pending & MIP_MEIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_MEIP);
        return MACHINE_EXTERNAL_INTERRUPT;
    }
    if (pending & MIP_MSIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_MSIP);
        return MACHINE_SOFTWARE_INTERRUPT;
    }
    if (pending & MIP_MTIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_MTIP);
        return MACHINE_TIMER_INTERRUPT;
    }
    if (pending & MIP_SEIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_SEIP);
        return SUPERVISOR_EXTERNAL_INTERRUPT;
    }
    if (pending & MIP_SSIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_SSIP);
        return SUPERVISOR_SOFTWARE_INTERRUPT;
    }
    if (pending & MIP_STIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_STIP);
        return SUPERVISOR_TIMER_INTERRUPT;
    }

    return NONE;
}

size_t read_file(FILE* f, uint8_t** r) {
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* content = malloc(fsize + 1);
    fread(content, fsize, 1, f);
    fclose(f);
    content[fsize] = 0;
    *r = content;

    return fsize;
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        printf("Usage: nanoemu <filename> [<image>]\n");
        exit(1);
    }

    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        printf("ERROR: %s\n", strerror(errno));
    }

    uint8_t* binary = NULL;
    uint8_t* disk = NULL;
    size_t fsize = read_file(f, &binary);

    if (argc == 3) {
        FILE *f = fopen(argv[2], "rb");
        if (f == NULL) {
            printf("ERROR: %s\n", strerror(errno));
        }
        read_file(f, &disk);
    }

    struct cpu* cpu = cpu_new(binary, fsize, disk);
    free(binary);

    while (1) {
        /* Fetch instruction. */
        uint64_t inst;
        enum exception exception;
        enum interrupt interrupt;
        if ((exception = cpu_fetch(cpu, &inst)) != OK) {
            cpu_take_trap(cpu, exception, NONE);
            if (exception_is_fatal(exception)) {
                break;
            }
            inst = 0;
        }

        /* Advance PC. */
        cpu->pc += 4;

        /* Decode & execute. */
        if ((exception = cpu_execute(cpu, inst)) != OK) {
            cpu_take_trap(cpu, exception, NONE);
            if (exception_is_fatal(exception)) {
                break;
            }
        }

        if ((interrupt = cpu_check_pending_interrupt(cpu)) != NONE) {
            cpu_take_trap(cpu, OK, interrupt);
        }
    }

    cpu_dump_registers(cpu);
    printf("----------------------------------------------------------------------------------------------------------------------\n");
    cpu_dump_csrs(cpu);
    return 0;
}
