#include "nanoemu.h"

struct bus*
bus_new(struct dram* dram, struct virtio* virtio) {
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

void
bus_disk_access(struct bus* bus) {
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
