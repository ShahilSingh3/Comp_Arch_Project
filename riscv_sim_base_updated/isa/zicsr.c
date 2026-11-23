#include "zicsr.h"
#include "isa_registry.h"
#include <stdio.h>

#define OP_SYSTEM 0x73

/*
 * TODO(lab5): the Zicsr instructions, for the three FP CSRs only:
 *   fflags (CSR_FFLAGS, 0x001)  = fcsr bits 4..0   (NV DZ OF UF NX)
 *   frm    (CSR_FRM,    0x002)  = fcsr bits 7..5   (rounding mode)
 *   fcsr   (CSR_FCSR,   0x003)  = frm << 5 | fflags
 * The CSR number is in i->imm. Every instruction writes the OLD CSR value
 * to rd. csrrw/csrrwi write the source; csrrs/csrrc (and the i forms) set /
 * clear the source's bits, but must not write at all when the source is
 * x0 (or uimm 0). In the immediate forms the rs1 field IS the 5-bit
 * immediate. Any other CSR number: CPU_ILLEGAL_INSTRUCTION with a message
 * in c->fault_msg. Masks/shifts: FCSR_* in core/cpu_state.h.
 */

static int csr_get(CPUState *c, uint32_t csr, uint32_t *val) {
    switch (csr) {
        case CSR_FFLAGS:
            *val = c->fcsr & FCSR_FFLAGS_MASK;
            return 0;
        case CSR_FRM:
            *val = (c->fcsr >> FCSR_FRM_SHIFT) & FCSR_FRM_MASK;
            return 0;
        case CSR_FCSR:
            *val = c->fcsr & FCSR_MASK;
            return 0;
        default:
            c->status = CPU_ILLEGAL_INSTRUCTION;
            c->fault_addr = c->pc;
            snprintf(c->fault_msg, sizeof(c->fault_msg), "unknown CSR 0x%03x", (unsigned)csr);
            return -1;
    }
}

static int csr_set(CPUState *c, uint32_t csr, uint32_t val) {
    switch (csr) {
        case CSR_FFLAGS:
            c->fcsr = (c->fcsr & ~FCSR_FFLAGS_MASK) | (val & FCSR_FFLAGS_MASK);
            return 0;
        case CSR_FRM:
            c->fcsr = (c->fcsr & ~(FCSR_FRM_MASK << FCSR_FRM_SHIFT)) | ((val & FCSR_FRM_MASK) << FCSR_FRM_SHIFT);
            return 0;
        case CSR_FCSR:
            c->fcsr = val & FCSR_MASK;
            return 0;
        default:
            c->status = CPU_ILLEGAL_INSTRUCTION;
            c->fault_addr = c->pc;
            snprintf(c->fault_msg, sizeof(c->fault_msg), "unknown CSR 0x%03x", (unsigned)csr);
            return -1;
    }
}

static void exec_csrrw(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t old_val;
    if (csr_get(c, i->imm, &old_val) != 0) return;
    uint32_t src = cpu_read_reg(c, i->rs1);
    if (csr_set(c, i->imm, src) != 0) return;
    cpu_write_reg(c, i->rd, old_val);
}

static void exec_csrrs(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t old_val;
    if (csr_get(c, i->imm, &old_val) != 0) return;
    if (i->rs1 != 0) {
        uint32_t mask = cpu_read_reg(c, i->rs1);
        if (csr_set(c, i->imm, old_val | mask) != 0) return;
    }
    cpu_write_reg(c, i->rd, old_val);
}

static void exec_csrrc(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t old_val;
    if (csr_get(c, i->imm, &old_val) != 0) return;
    if (i->rs1 != 0) {
        uint32_t mask = cpu_read_reg(c, i->rs1);
        if (csr_set(c, i->imm, old_val & ~mask) != 0) return;
    }
    cpu_write_reg(c, i->rd, old_val);
}

/* Immediate forms: the rs1 field holds a 5-bit unsigned immediate. */
static void exec_csrrwi(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t old_val;
    if (csr_get(c, i->imm, &old_val) != 0) return;
    uint32_t uimm = i->rs1 & 0x1F;
    if (csr_set(c, i->imm, uimm) != 0) return;
    cpu_write_reg(c, i->rd, old_val);
}

static void exec_csrrsi(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t old_val;
    if (csr_get(c, i->imm, &old_val) != 0) return;
    uint32_t uimm = i->rs1 & 0x1F;
    if (uimm != 0) {
        if (csr_set(c, i->imm, old_val | uimm) != 0) return;
    }
    cpu_write_reg(c, i->rd, old_val);
}

static void exec_csrrci(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t old_val;
    if (csr_get(c, i->imm, &old_val) != 0) return;
    uint32_t uimm = i->rs1 & 0x1F;
    if (uimm != 0) {
        if (csr_set(c, i->imm, old_val & ~uimm) != 0) return;
    }
    cpu_write_reg(c, i->rd, old_val);
}

void register_zicsr(void) {
    register_instruction((InstructionHandler){"csrrw",  OP_SYSTEM, 0x1, -1, FMT_CSR,  exec_csrrw});
    register_instruction((InstructionHandler){"csrrs",  OP_SYSTEM, 0x2, -1, FMT_CSR,  exec_csrrs});
    register_instruction((InstructionHandler){"csrrc",  OP_SYSTEM, 0x3, -1, FMT_CSR,  exec_csrrc});
    register_instruction((InstructionHandler){"csrrwi", OP_SYSTEM, 0x5, -1, FMT_CSRI, exec_csrrwi});
    register_instruction((InstructionHandler){"csrrsi", OP_SYSTEM, 0x6, -1, FMT_CSRI, exec_csrrsi});
    register_instruction((InstructionHandler){"csrrci", OP_SYSTEM, 0x7, -1, FMT_CSRI, exec_csrrci});
}
