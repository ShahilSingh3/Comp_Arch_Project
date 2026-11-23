#include "hazard.h"

/*
 * Full forwarding unit:
 *   EX/MEM -> EX   producer one instruction ahead (ALU result)
 *   MEM/WB -> EX   producer two ahead (ALU result or loaded value)
 *   (three ahead needs nothing: WB writes before ID reads)
 * plus a 1-cycle load-use stall. FP registers (unified numbers 32..63) are
 * forwarded exactly like integer ones.
 */
static inline ForwardSel forward_for_reg(int src, const HazardSlot *mem, const HazardSlot *wb) {
    if (src == REG_NONE) return FWD_NONE;

    /* Priority 1: EX/MEM (younger producer) */
    if (mem->valid && mem->ctrl.dst != REG_NONE && mem->ctrl.dst == src) {
        if (mem->ctrl.mem_op != MEMOP_LOAD) {
            return FWD_EX_MEM;
        }
        return FWD_NONE;
    }

    /* Priority 2: MEM/WB (older producer) */
    if (wb->valid && wb->ctrl.dst != REG_NONE && wb->ctrl.dst == src) {
        return FWD_MEM_WB;
    }

    return FWD_NONE;
}

static void decide_forward(const HazardInput *in, HazardDecision *out) {
    out->stall = 0;
    out->fwd1 = FWD_NONE;
    out->fwd2 = FWD_NONE;
    out->fwd3 = FWD_NONE;

    /* 1. Forwarding for the instruction currently in EX */
    if (in->ex.valid) {
        out->fwd1 = forward_for_reg(in->ex.ctrl.src1, &in->mem, &in->wb);
        out->fwd2 = forward_for_reg(in->ex.ctrl.src2, &in->mem, &in->wb);
        out->fwd3 = forward_for_reg(in->ex.ctrl.src3, &in->mem, &in->wb);
    }

    /* 2. Load-use hazard detection for the instruction currently in ID */
    if (in->id.valid && in->ex.valid &&
        in->ex.ctrl.mem_op == MEMOP_LOAD && in->ex.ctrl.dst != REG_NONE) {
        int load_dst = in->ex.ctrl.dst;
        int s1 = in->id.ctrl.src1;
        int s2 = in->id.ctrl.src2;
        int s3 = in->id.ctrl.src3;

        if ((s1 != REG_NONE && s1 == load_dst) ||
            (s2 != REG_NONE && s2 == load_dst) ||
            (s3 != REG_NONE && s3 == load_dst)) {
            out->stall = 1;
        }
    }
}

const HazardDetector hazard_forward = {
    "forward",
    "EX->EX and MEM->EX forwarding, 1-cycle stall on load-use",
    decide_forward,
};
