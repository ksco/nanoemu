#include "nanoemu.h"

struct plic*
plic_new() {
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
