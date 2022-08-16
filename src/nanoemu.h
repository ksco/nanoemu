#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#define SUPRESS_RETURN(x) (void)((x)+1)

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

bool
exception_is_fatal(enum exception exception);

struct dram {
    uint8_t* data;
};

struct dram*
dram_new(uint8_t* code, size_t code_size);

enum exception
dram_load(struct dram* dram, uint64_t addr, uint64_t size, uint64_t *result);

enum exception
dram_store(struct dram* dram, uint64_t addr, uint64_t size, uint64_t value);

struct clint {
    uint64_t mtime;
    uint64_t mtimecmp;
};

struct clint*
clint_new();

enum exception
clint_load(struct clint* clint, uint64_t addr, uint64_t size, uint64_t *result);

enum exception
clint_store(struct clint* clint, uint64_t addr, uint64_t size, uint64_t value);

struct plic {
    uint64_t pending;
    uint64_t senable;
    uint64_t spriority;
    uint64_t sclaim;
};

struct plic*
plic_new();

enum exception
plic_load(struct plic* plic, uint64_t addr, uint64_t size, uint64_t *result);

enum exception
plic_store(struct plic* plic, uint64_t addr, uint64_t size, uint64_t value);

struct uart {
    uint8_t data[UART_SIZE];
    bool interrupting;

    pthread_t tid;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

struct uart*
uart_new();

enum exception
uart_load(struct uart* uart, uint64_t addr, uint64_t size, uint64_t *result);

enum exception
uart_store(struct uart* uart, uint64_t addr, uint64_t size, uint64_t value);

bool
uart_interrupting(struct uart* uart);

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

struct virtio*
virtio_new(uint8_t* disk);

enum exception
virtio_load(struct virtio* virtio, uint64_t addr, uint64_t size, uint64_t *result);

enum exception
virtio_store(struct virtio* virtio, uint64_t addr, uint64_t size, uint64_t value);

bool
virtio_is_interrupting(struct virtio* virtio);

uint64_t
virtio_desc_addr(struct virtio* virtio);

uint64_t
virtio_disk_read(struct virtio* virtio, uint64_t addr);

void
virtio_disk_write(struct virtio* virtio, uint64_t addr, uint64_t value);

uint64_t
virtio_new_id(struct virtio* virtio);

struct bus {
    struct dram* dram;
    struct clint* clint;
    struct plic* plic;
    struct uart* uart;
    struct virtio *virtio;
};

struct bus*
bus_new(struct dram* dram, struct virtio* virtio);

enum exception
bus_load(struct bus* bus, uint64_t addr, uint64_t size, uint64_t *result);

enum exception
bus_store(struct bus* bus, uint64_t addr, uint64_t size, uint64_t value);

void
bus_disk_access(struct bus* bus);

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

struct cpu*
cpu_new(uint8_t* code, size_t code_size, uint8_t* disk);

void
cpu_update_paging(struct cpu* cpu, uint16_t csr_addr);

enum exception
cpu_translate(struct cpu* cpu, uint64_t addr, enum exception e, uint64_t *result);

enum exception
cpu_fetch(struct cpu* cpu, uint64_t* result);

uint64_t
cpu_load_csr(struct cpu* cpu, uint16_t addr);

void
cpu_store_csr(struct cpu* cpu, uint16_t addr, uint64_t value);

enum exception
cpu_load(struct cpu* cpu, uint64_t addr, uint64_t size, uint64_t *result);

enum exception
cpu_store(struct cpu* cpu, uint64_t addr, uint64_t size, uint64_t value);

void
cpu_dump_csrs(struct cpu* cpu);

enum exception
cpu_execute(struct cpu* cpu, uint64_t inst);

void
cpu_dump_registers(struct cpu* cpu);

void
cpu_take_trap(struct cpu* cpu, enum exception exception, enum interrupt interrupt);

enum interrupt
cpu_check_pending_interrupt(struct cpu* cpu);

size_t
read_file(FILE* f, uint8_t** r);
