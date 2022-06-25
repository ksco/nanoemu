#include "nanoemu.h"

int
main(int argc, char** argv) {
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
