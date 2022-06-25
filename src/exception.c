#include "nanoemu.h"

bool
exception_is_fatal(enum exception exception) {
    if (exception == INSTRUCTION_ADDRESS_MISALIGNED ||
        exception == INSTRUCTION_ACCESS_FAULT ||
        exception == LOAD_ACCESS_FAULT ||
        exception == STORE_AMO_ADDRESS_MISALIGNED ||
        exception == STORE_AMO_ACCESS_FAULT) {
        return true;
    }
    return false;
}
