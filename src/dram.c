#include "nanoemu.h"

struct dram*
dram_new(uint8_t* code, size_t code_size) {
    struct dram* dram = calloc(1, sizeof *dram);
    dram->data = calloc(DRAM_SIZE, 1);
    memcpy(dram->data, code, code_size);
    return dram;
}

static inline uint64_t
dram_load8(struct dram* dram, uint64_t addr) {
    uint64_t index = addr - DRAM_BASE;
    return dram->data[index];
}

static inline uint64_t
dram_load16(struct dram* dram, uint64_t addr) {
    uint64_t index = addr - DRAM_BASE;
    return (uint64_t)(dram->data[index])
        | ((uint64_t)(dram->data[index + 1]) << 8);
}

static inline uint64_t
dram_load32(struct dram* dram, uint64_t addr) {
    uint64_t index = addr - DRAM_BASE;
    return (uint64_t)(dram->data[index])
        | ((uint64_t)(dram->data[index + 1]) << 8)
        | ((uint64_t)(dram->data[index + 2]) << 16)
        | ((uint64_t)(dram->data[index + 3]) << 24);
}

static inline uint64_t
dram_load64(struct dram* dram, uint64_t addr) {
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

static inline void
dram_store8(struct dram* dram, uint64_t addr, uint64_t value) {
    uint64_t index = addr - DRAM_BASE;
    dram->data[index] = value;
}

static inline void
dram_store16(struct dram* dram, uint64_t addr, uint64_t value) {
    uint64_t index = addr - DRAM_BASE;
    dram->data[index + 0] = (value >> 0) & 0xff;
    dram->data[index + 1] = (value >> 8) & 0xff;
}

static inline void
dram_store32(struct dram* dram, uint64_t addr, uint64_t value) {
    uint64_t index = addr - DRAM_BASE;
    dram->data[index + 0] = (value >>  0) & 0xff;
    dram->data[index + 1] = (value >>  8) & 0xff;
    dram->data[index + 2] = (value >> 16) & 0xff;
    dram->data[index + 3] = (value >> 24) & 0xff;
}

static inline void
dram_store64(struct dram* dram, uint64_t addr, uint64_t value) {
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
