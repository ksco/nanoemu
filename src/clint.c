#include "nanoemu.h"

struct clint*
clint_new() {
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
