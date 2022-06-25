#include "nanoemu.h"

struct cpu*
cpu_new(uint8_t* code, size_t code_size, uint8_t* disk) {
    struct cpu* cpu = calloc(1, sizeof *cpu);

    /* Initialize the sp(x2) register. */
    cpu->regs[2] = DRAM_BASE + DRAM_SIZE;

    cpu->bus = bus_new(dram_new(code, code_size), virtio_new(disk));
    cpu->pc = DRAM_BASE;
    cpu->mode = MACHINE;

    return cpu;
}

void
cpu_update_paging(struct cpu* cpu, uint16_t csr_addr) {
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
enum exception
cpu_fetch(struct cpu* cpu, uint64_t* result) {
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

uint64_t
cpu_load_csr(struct cpu* cpu, uint16_t addr) {
    if (addr == SIE) {
        return cpu->csrs[MIE] & cpu->csrs[MIDELEG];
    }
    return cpu->csrs[addr];
}

void
cpu_store_csr(struct cpu* cpu, uint16_t addr, uint64_t value) {
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

void
cpu_dump_csrs(struct cpu* cpu) {
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

enum exception
cpu_execute(struct cpu* cpu, uint64_t inst) {
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

void
cpu_dump_registers(struct cpu* cpu) {
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

void
cpu_take_trap(struct cpu* cpu, enum exception exception, enum interrupt interrupt) {
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

enum interrupt
cpu_check_pending_interrupt(struct cpu* cpu) {
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
