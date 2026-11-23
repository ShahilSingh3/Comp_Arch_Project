#include "pipeline.h"
#include "decoder.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define OPC_BRANCH 0x63
#define OPC_JAL    0x6F
#define OPC_JALR   0x67

/*
 * The stages are evaluated in REVERSE order (WB, MEM, EX, ID, IF) each
 * cycle, updating the latches in place. Going backwards means every stage
 * reads its input latch before the stage behind it overwrites that latch
 * with next cycle's value -- the software equivalent of all the pipeline
 * registers clocking at the same instant.
 *
 * Lab 2: the hazard unit and the forwarding muxes need the latch contents
 * as they were at the START of the cycle (the MEM/WB latch, for one, has
 * already been overwritten by the time EX runs). pipeline_step() therefore
 * takes a snapshot `cur` first; hazard decisions and forwarded values come
 * from it.
 */

/* ------------------------------------------------------------------ */
/* Faults                                                             */
/* ------------------------------------------------------------------ */

static inline void fault_clear(PipeFault *f) {
    f->status = CPU_RUNNING;
    f->addr = 0;
    f->msg[0] = '\0';
}

static inline void fault_set(PipeFault *f, CPUStatus status, uint32_t addr, const char *fmt, ...) {
    f->status = status;
    f->addr = addr;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->msg, sizeof(f->msg), fmt, ap);
    va_end(ap);
}

static inline int has_fault(const PipeFault *f) {
    return f->status != CPU_RUNNING;
}

/* ------------------------------------------------------------------ */
/* Reset                                                              */
/* ------------------------------------------------------------------ */

void pipeline_reset(PipelineState *p, uint32_t start_pc, const PipelineConfig *cfg) {
    memset(p, 0, sizeof(*p)); /* every latch starts as a bubble (valid = 0) */
    p->fetch_pc = start_pc;
    p->cfg = *cfg;
    if (!p->cfg.hazard) p->cfg.hazard = &hazard_forward;
    if (!p->cfg.predictor) p->cfg.predictor = &bp_2bit;
    p->cfg.predictor->reset(&p->bp);
}

/* ------------------------------------------------------------------ */
/* WB                                                                 */
/* ------------------------------------------------------------------ */

/* Returns 1 if the machine stopped (fault or exit) this cycle. */
static int stage_wb(PipelineState *p, CPUState *cpu) {
    if (!p->mem_wb.valid) return 0;

    if (has_fault(&p->mem_wb.fault)) {
        cpu->status = p->mem_wb.fault.status;
        cpu->fault_addr = p->mem_wb.fault.addr;
        strncpy(cpu->fault_msg, p->mem_wb.fault.msg, sizeof(cpu->fault_msg) - 1);
        cpu->fault_msg[sizeof(cpu->fault_msg) - 1] = '\0';
        cpu->pc = p->mem_wb.pc;
        return 1;
    }

    if (p->mem_wb.ctrl.dst != REG_NONE) {
        unified_write(cpu, p->mem_wb.ctrl.dst, p->mem_wb.wb_val);
    }
    p->stats.retired++;

    if (p->mem_wb.exit_requested) {
        cpu->status = CPU_ECALL_EXIT;
        cpu->pc = p->mem_wb.pc;
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* MEM                                                                */
/* ------------------------------------------------------------------ */

static inline uint32_t extend_load(uint32_t v, int size, int is_unsigned) {
    if (size == 1) return is_unsigned ? (v & 0xFF)   : (uint32_t)(int32_t)(int8_t)v;
    if (size == 2) return is_unsigned ? (v & 0xFFFF) : (uint32_t)(int32_t)(int16_t)v;
    return v;
}

static inline MemStatus mem_load(const Memory *mem, uint32_t addr, int size, uint32_t *out) {
    MemStatus s;
    if (size == 1) { uint8_t b;  s = memory_read_byte(mem, addr, &b); *out = b; }
    else if (size == 2) { uint16_t h; s = memory_read_half(mem, addr, &h); *out = h; }
    else s = memory_read_word(mem, addr, out);
    return s;
}

static inline MemStatus mem_store(Memory *mem, uint32_t addr, int size, uint32_t val) {
    if (size == 1) return memory_write_byte(mem, addr, (uint8_t)val);
    if (size == 2) return memory_write_half(mem, addr, (uint16_t)val);
    return memory_write_word(mem, addr, val);
}

/* Returns 1 if this instruction faulted here (younger instructions must
 * then be squashed and fetch stopped). */
static int stage_mem(PipelineState *p, Memory *mem) {
    if (!p->ex_mem.valid) {
        p->mem_wb.valid = 0;
        return 0;
    }

    p->mem_wb.valid = 1;
    p->mem_wb.pc = p->ex_mem.pc;
    p->mem_wb.raw = p->ex_mem.raw;
    p->mem_wb.fault = p->ex_mem.fault;
    p->mem_wb.ctrl = p->ex_mem.ctrl;
    p->mem_wb.wb_val = p->ex_mem.alu_result;
    p->mem_wb.exit_requested = p->ex_mem.exit_requested;

    if (has_fault(&p->ex_mem.fault)) {
        return 0;
    }

    if (p->ex_mem.ctrl.mem_op == MEMOP_LOAD) {
        uint32_t val = 0;
        if (mem_load(mem, p->ex_mem.alu_result, p->ex_mem.ctrl.mem_size, &val) != MEM_OK) {
            fault_set(&p->mem_wb.fault, CPU_MEM_FAULT, p->ex_mem.alu_result,
                      "data memory access out of bounds at 0x%08x (%s at pc=0x%08x)",
                      p->ex_mem.alu_result,
                      p->ex_mem.handler ? p->ex_mem.handler->mnemonic : "unknown",
                      p->ex_mem.pc);
            return 1;
        }
        p->mem_wb.wb_val = extend_load(val, p->ex_mem.ctrl.mem_size, p->ex_mem.ctrl.mem_unsigned);
    } else if (p->ex_mem.ctrl.mem_op == MEMOP_STORE) {
        if (mem_store(mem, p->ex_mem.alu_result, p->ex_mem.ctrl.mem_size, p->ex_mem.store_val) != MEM_OK) {
            fault_set(&p->mem_wb.fault, CPU_MEM_FAULT, p->ex_mem.alu_result,
                      "data memory access out of bounds at 0x%08x (%s at pc=0x%08x)",
                      p->ex_mem.alu_result,
                      p->ex_mem.handler ? p->ex_mem.handler->mnemonic : "unknown",
                      p->ex_mem.pc);
            return 1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* EX                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int      redirect;   /* PC must change to `target`: squash IF and ID */
    uint32_t target;
    int      stop;       /* exit or fault on the correct path: squash IF and ID, stop fetching */
} ExOutcome;

/* The forwarding mux in front of each ALU input. */
static inline uint32_t forward_mux(uint32_t from_regfile, ForwardSel sel, const PipelineState *cur,
                            PipelineStats *stats) {
    if (sel == FWD_EX_MEM) {
        if (stats) stats->forwards++;
        return cur->ex_mem.alu_result;
    }
    if (sel == FWD_MEM_WB) {
        if (stats) stats->forwards++;
        return cur->mem_wb.wb_val;
    }
    return from_regfile;
}

static void stage_ex(PipelineState *p, CPUState *cpu, Memory *mem, const PipelineState *cur,
                     const HazardDecision *hz, ExOutcome *o) {
    if (!p->id_ex.valid) {
        p->ex_mem.valid = 0;
        return;
    }

    p->ex_mem.valid = 1;
    p->ex_mem.pc = p->id_ex.pc;
    p->ex_mem.raw = p->id_ex.raw;
    p->ex_mem.fault = p->id_ex.fault;
    p->ex_mem.handler = p->id_ex.handler;
    p->ex_mem.ctrl = p->id_ex.ctrl;
    p->ex_mem.alu_result = 0;
    p->ex_mem.store_val = 0;
    p->ex_mem.exit_requested = 0;

    if (has_fault(&p->id_ex.fault)) {
        o->stop = 1;
        return;
    }

    uint32_t op1 = forward_mux(p->id_ex.src1_val, hz->fwd1, cur, &p->stats);
    uint32_t op2 = forward_mux(p->id_ex.src2_val, hz->fwd2, cur, &p->stats);
    uint32_t op3 = forward_mux(p->id_ex.src3_val, hz->fwd3, cur, &p->stats);

    if (p->id_ex.ctrl.mem_op == MEMOP_LOAD || p->id_ex.ctrl.mem_op == MEMOP_STORE) {
        p->ex_mem.alu_result = op1 + (uint32_t)p->id_ex.instr.imm;
        p->ex_mem.store_val = op2;
        return;
    }

    CPUState scratch;
    memset(&scratch, 0, sizeof(scratch));
    scratch.pc = p->id_ex.pc;
    scratch.next_pc = p->id_ex.pc + 4;
    scratch.fcsr = cpu->fcsr;
    scratch.status = CPU_RUNNING;

    if (p->id_ex.ctrl.src1 != REG_NONE) {
        unified_write(&scratch, p->id_ex.ctrl.src1, op1);
    }
    if (p->id_ex.ctrl.src2 != REG_NONE) {
        unified_write(&scratch, p->id_ex.ctrl.src2, op2);
    }
    if (p->id_ex.ctrl.src3 != REG_NONE) {
        unified_write(&scratch, p->id_ex.ctrl.src3, op3);
    }

    DecodedInstr instr = p->id_ex.instr;
    if (p->id_ex.handler && p->id_ex.handler->execute) {
        p->id_ex.handler->execute(&scratch, mem, &instr);
    }

    if (p->id_ex.ctrl.dst != REG_NONE) {
        p->ex_mem.alu_result = unified_read(&scratch, p->id_ex.ctrl.dst);
    }

    cpu->fcsr = scratch.fcsr;

    if (scratch.status == CPU_ECALL_EXIT) {
        p->ex_mem.exit_requested = 1;
        o->stop = 1;
    } else if (scratch.status != CPU_RUNNING) {
        fault_set(&p->ex_mem.fault, scratch.status, scratch.fault_addr, "%s", scratch.fault_msg);
        o->stop = 1;
    }

    if (p->id_ex.ctrl.is_branch) {
        p->stats.branches++;
        int taken = (scratch.next_pc != p->id_ex.pc + 4);
        if (p->cfg.predictor && p->cfg.predictor->update) {
            uint32_t target = p->id_ex.pc + (uint32_t)p->id_ex.instr.imm;
            p->cfg.predictor->update(&p->bp, p->id_ex.pc, target, taken);
        }
        if (scratch.next_pc != p->id_ex.pred_next) {
            p->stats.branch_mispredicts++;
        }
    }

    if (p->id_ex.ctrl.is_jump) {
        if (p->id_ex.instr.opcode == OPC_JAL) {
            p->stats.jal++;
        } else if (p->id_ex.instr.opcode == OPC_JALR) {
            p->stats.jalr++;
        }
    }

    if (!o->stop && scratch.next_pc != p->id_ex.pred_next) {
        o->redirect = 1;
        o->target = scratch.next_pc;
        p->stats.redirects++;
    }
}

/* ------------------------------------------------------------------ */
/* ID                                                                 */
/* ------------------------------------------------------------------ */

/* `squash`: discard the instruction in IF/ID. `wrong_path`: the reason is a
 * PC redirect (counted in stats.squashed), not an exit/fault draining.
 * `stall`: the hazard unit says wait -- IF/ID is left untouched (IF holds
 * it too) and a bubble goes into EX. */
static void stage_id(PipelineState *p, const CPUState *cpu, int squash, int wrong_path, int stall) {
    if (squash) {
        if (wrong_path && p->if_id.valid) {
            p->stats.squashed++;
        }
        p->id_ex.valid = 0;
        p->if_id.valid = 0;
        return;
    }

    if (stall) {
        p->id_ex.valid = 0;
        return;
    }

    if (!p->if_id.valid) {
        p->id_ex.valid = 0;
        return;
    }

    memset(&p->id_ex, 0, sizeof(p->id_ex));
    p->id_ex.valid = 1;
    p->id_ex.pc = p->if_id.pc;
    p->id_ex.raw = p->if_id.raw;
    p->id_ex.fault = p->if_id.fault;
    p->id_ex.pred_next = p->if_id.pred_next;

    if (has_fault(&p->if_id.fault)) {
        return;
    }

    DecodedInstr d;
    const InstructionHandler *h = NULL;
    if (decode_instruction(p->if_id.raw, &d, &h) != DECODE_OK) {
        fault_set(&p->id_ex.fault, CPU_ILLEGAL_INSTRUCTION, p->if_id.pc,
                  "unknown instruction 0x%08x at pc=0x%08x (no handler registered)",
                  p->if_id.raw, p->if_id.pc);
        return;
    }

    p->id_ex.handler = h;
    p->id_ex.instr = d;
    control_from_instr(h, &d, &p->id_ex.ctrl);

    if (p->id_ex.ctrl.src1 != REG_NONE) {
        p->id_ex.src1_val = unified_read(cpu, p->id_ex.ctrl.src1);
    }
    if (p->id_ex.ctrl.src2 != REG_NONE) {
        p->id_ex.src2_val = unified_read(cpu, p->id_ex.ctrl.src2);
    }
    if (p->id_ex.ctrl.src3 != REG_NONE) {
        p->id_ex.src3_val = unified_read(cpu, p->id_ex.ctrl.src3);
    }
}

/* ------------------------------------------------------------------ */
/* IF                                                                 */
/* ------------------------------------------------------------------ */

/* IF-stage pre-decoder: just enough of the fetched word to find branch and
 * jal targets (in hardware: a few gates, or a BTB lookup). */
static inline int32_t imm_b(uint32_t raw) {
    int32_t v = (int32_t)((((raw >> 31) & 0x1) << 12) | (((raw >> 7) & 0x1) << 11) |
                          (((raw >> 25) & 0x3F) << 5) | (((raw >> 8) & 0xF) << 1));
    if (v & 0x1000) v |= (int32_t)0xFFFFE000;
    return v;
}

static inline int32_t imm_j(uint32_t raw) {
    int32_t v = (int32_t)((((raw >> 31) & 0x1) << 20) | (((raw >> 12) & 0xFF) << 12) |
                          (((raw >> 20) & 0x1) << 11) | (((raw >> 21) & 0x3FF) << 1));
    if (v & 0x100000) v |= (int32_t)0xFFE00000;
    return v;
}

/* Where to fetch after the word `raw` at `pc`. */
static inline uint32_t predict_next(const PipelineState *p, uint32_t pc, uint32_t raw) {
    uint32_t opcode = raw & 0x7F;
    if (opcode == OPC_BRANCH) {
        if (p->cfg.predictor && p->cfg.predictor->predict) {
            uint32_t target = pc + (uint32_t)imm_b(raw);
            if (p->cfg.predictor->predict(&p->bp, pc, target)) {
                return target;
            }
        }
        return pc + 4;
    }
    if (opcode == OPC_JAL) {
        return pc + (uint32_t)imm_j(raw);
    }
    return pc + 4;
}

static void stage_if(PipelineState *p, const Memory *mem, const ExOutcome *o, int stall) {
    if (o->stop) {
        p->fetch_stopped = 1;
        p->if_id.valid = 0;
        return;
    }

    if (p->fetch_stopped) {
        p->if_id.valid = 0;
        return;
    }

    if (o->redirect) {
        p->stats.squashed++;
        p->fetch_pc = o->target;
        p->if_id.valid = 0;
        return;
    }

    if (stall) {
        return;
    }

    uint32_t raw = 0;
    memset(&p->if_id, 0, sizeof(p->if_id));
    p->if_id.valid = 1;
    p->if_id.pc = p->fetch_pc;

    if (memory_read_word(mem, p->fetch_pc, &raw) != MEM_OK) {
        fault_set(&p->if_id.fault, CPU_MEM_FAULT, p->fetch_pc,
                  "instruction fetch out of bounds at pc=0x%08x", p->fetch_pc);
        p->if_id.pred_next = p->fetch_pc + 4;
        p->fetch_pc += 4;
        return;
    }

    p->if_id.raw = raw;
    p->if_id.pred_next = predict_next(p, p->fetch_pc, raw);
    p->fetch_pc = p->if_id.pred_next;
}

/* ------------------------------------------------------------------ */
/* One clock cycle                                                    */
/* ------------------------------------------------------------------ */

/* Control signals of the instruction sitting in IF/ID, for the hazard unit
 * (hardware would get these from the ID stage's decoder). An instruction
 * that will fault has no operands: it never executes. */
static HazardSlot peek_id_slot(const IF_ID_Latch *f) {
    HazardSlot s;
    memset(&s, 0, sizeof(s));
    if (!f->valid || has_fault(&f->fault)) return s;
    DecodedInstr d;
    const InstructionHandler *h;
    if (decode_instruction(f->raw, &d, &h) != DECODE_OK) return s;
    s.valid = 1;
    control_from_instr(h, &d, &s.ctrl);
    return s;
}

static HazardSlot slot_of(int valid, const PipeFault *fault, const Control *ctrl) {
    HazardSlot s;
    memset(&s, 0, sizeof(s));
    if (valid && !has_fault(fault)) { s.valid = 1; s.ctrl = *ctrl; }
    return s;
}

CPUStatus pipeline_step(PipelineState *p, CPUState *cpu, Memory *mem) {
    if (cpu->status != CPU_RUNNING) return cpu->status;
    cpu->cycle_count++;

    /* Start-of-cycle snapshot + hazard unit decision for this cycle. */
    const PipelineState cur = *p;
    HazardInput hin;
    hin.id  = peek_id_slot(&cur.if_id);
    hin.ex  = slot_of(cur.id_ex.valid,  &cur.id_ex.fault,  &cur.id_ex.ctrl);
    hin.mem = slot_of(cur.ex_mem.valid, &cur.ex_mem.fault, &cur.ex_mem.ctrl);
    hin.wb  = slot_of(cur.mem_wb.valid, &cur.mem_wb.fault, &cur.mem_wb.ctrl);
    HazardDecision hz = {0, FWD_NONE, FWD_NONE, FWD_NONE};
    p->cfg.hazard->decide(&hin, &hz);

    if (stage_wb(p, cpu)) return cpu->status;

    ExOutcome o = {0, 0, 0};
    if (stage_mem(p, mem)) {
        /* A load/store just faulted: the instruction behind it in EX is
         * younger and must not execute. */
        p->ex_mem.valid = 0;
        o.stop = 1;
    } else {
        stage_ex(p, cpu, mem, &cur, &hz, &o);
    }

    /* A redirect or stop squashes ID anyway, so a stall only matters
     * when neither happened. */
    int stall = hz.stall && !o.redirect && !o.stop && hin.id.valid;
    if (stall) p->stats.stalls++;

    stage_id(p, cpu, o.redirect || o.stop, o.redirect, stall);
    stage_if(p, mem, &o, stall);

    cpu->pc = p->fetch_pc;
    return cpu->status;
}

/* ------------------------------------------------------------------ */
/* Views / stats / engine wrapper                                     */
/* ------------------------------------------------------------------ */

int pipeline_stage_views(const PipelineState *p, PipelineStageView *out, int max) {
    if (max < 5) return 0;
    out[0] = (PipelineStageView){"IF",  p->fetch_pc,  p->fetch_stopped};
    out[1] = (PipelineStageView){"ID",  p->if_id.pc,  !p->if_id.valid};
    out[2] = (PipelineStageView){"EX",  p->id_ex.pc,  !p->id_ex.valid};
    out[3] = (PipelineStageView){"MEM", p->ex_mem.pc, !p->ex_mem.valid};
    out[4] = (PipelineStageView){"WB",  p->mem_wb.pc, !p->mem_wb.valid};
    return 5;
}

void pipeline_format_stats(const PipelineState *p, const CPUState *cpu, char *buf, size_t n) {
    const PipelineStats *s = &p->stats;
    double cpi = s->retired ? (double)cpu->cycle_count / (double)s->retired : 0.0;
    double acc = s->branches ? 100.0 * (double)(s->branches - s->branch_mispredicts) / (double)s->branches
                             : 100.0;
    snprintf(buf, n,
             "pipeline        = 5-stage, hazard unit '%s', predictor '%s'\n"
             "retired         = %llu\n"
             "CPI             = %.3f\n"
             "stalls          = %llu\n"
             "forwards        = %llu\n"
             "branches        = %llu\n"
             "mispredicts     = %llu\n"
             "accuracy        = %.1f%%\n"
             "jal             = %llu\n"
             "jalr            = %llu\n"
             "redirects       = %llu\n"
             "squashed        = %llu\n",
             p->cfg.hazard->name, p->cfg.predictor->name,
             (unsigned long long)s->retired, cpi,
             (unsigned long long)s->stalls,
             (unsigned long long)s->forwards,
             (unsigned long long)s->branches,
             (unsigned long long)s->branch_mispredicts,
             acc,
             (unsigned long long)s->jal,
             (unsigned long long)s->jalr,
             (unsigned long long)s->redirects,
             (unsigned long long)s->squashed);
}

static CPUStatus engine_step(SimEngine *e) {
    return pipeline_step((PipelineState *)e->ctx, e->cpu, e->mem);
}

static int engine_views(const SimEngine *e, PipelineStageView *out, int max) {
    return pipeline_stage_views((const PipelineState *)e->ctx, out, max);
}

static void engine_stats(const SimEngine *e, char *buf, size_t n) {
    pipeline_format_stats((const PipelineState *)e->ctx, e->cpu, buf, n);
}

void pipeline_make_engine(SimEngine *e, PipelineState *p, CPUState *cpu, Memory *mem) {
    e->name = "5-stage pipeline";
    e->cpu = cpu;
    e->mem = mem;
    e->ctx = p;
    e->step = engine_step;
    e->stage_views = engine_views;
    e->format_stats = engine_stats;
}
