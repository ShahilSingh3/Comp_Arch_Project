#include "hazard.h"

/*
 * Naive interlock, no forwarding: the instruction in ID may only read the
 * register file once every older producer of its sources has written back.
 * (WB writes before ID reads in the same cycle.)
 */
static void decide_stall(const HazardInput *in, HazardDecision *out) {
    out->stall = 0;
    out->fwd1 = FWD_NONE;
    out->fwd2 = FWD_NONE;
    out->fwd3 = FWD_NONE;

    if (!in->id.valid) return;

    int s1 = in->id.ctrl.src1;
    int s2 = in->id.ctrl.src2;
    int s3 = in->id.ctrl.src3;

    /* 1. Hazard against instruction currently in EX */
    if (in->ex.valid && in->ex.ctrl.dst != REG_NONE) {
        int d = in->ex.ctrl.dst;
        if ((s1 != REG_NONE && s1 == d) ||
            (s2 != REG_NONE && s2 == d) ||
            (s3 != REG_NONE && s3 == d)) {
            out->stall = 1;
            return;
        }
    }

    /* 2. Hazard against instruction currently in MEM */
    if (in->mem.valid && in->mem.ctrl.dst != REG_NONE) {
        int d = in->mem.ctrl.dst;
        if ((s1 != REG_NONE && s1 == d) ||
            (s2 != REG_NONE && s2 == d) ||
            (s3 != REG_NONE && s3 == d)) {
            out->stall = 1;
            return;
        }
    }
}

const HazardDetector hazard_stall = {
    "stall",
    "stall in ID until producers write back; no forwarding",
    decide_stall,
};
