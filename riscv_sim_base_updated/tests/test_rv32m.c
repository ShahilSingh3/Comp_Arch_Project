#include <stdint.h>
#include <stdio.h>
#include <assert.h>

#include "cpu_state.h"
#include "decoder.h"
#include "memory.h"
#include "isa_registry.h"

/* Include the rv32m implementation directly to access static handlers */
#include "../isa/rv32m.c"

int main(void) {
    CPUState cpu;
    cpu_reset(&cpu, 0);

    DecodedInstr instr = {.rs1 = 1, .rs2 = 2, .rd = 3};

    #define TEST_OP(func, v1, v2) \
        (cpu_write_reg(&cpu, 1, (v1)), cpu_write_reg(&cpu, 2, (v2)), func(&cpu, NULL, &instr), cpu_read_reg(&cpu, 3))

    // 1. mul tests
    assert(TEST_OP(exec_mul, 121, 121) == 14641);
    assert(TEST_OP(exec_mul, -3, 5) == (uint32_t)-15);
    assert(TEST_OP(exec_mul, 0xFFFFFFFF, 0xFFFFFFFF) == 1);
    assert(TEST_OP(exec_mul, 0x80000000, 2) == 0);
    assert(TEST_OP(exec_mul, 0x12345678, 0) == 0);

    // 2. mulh tests
    assert(TEST_OP(exec_mulh, 0, 0x12345678) == 0);
    assert(TEST_OP(exec_mulh, 1, 1) == 0);
    assert(TEST_OP(exec_mulh, -1, -1) == 0);
    assert(TEST_OP(exec_mulh, -1, 1) == 0xFFFFFFFF);
    assert(TEST_OP(exec_mulh, 0x80000000, 0x80000000) == 0x40000000);
    assert(TEST_OP(exec_mulh, 0x7FFFFFFF, 0x7FFFFFFF) == 0x3FFFFFFF);
    assert(TEST_OP(exec_mulh, 0x80000000, 2) == 0xFFFFFFFF);

    // 3. mulhsu tests
    assert(TEST_OP(exec_mulhsu, 0, 0xFFFFFFFF) == 0);
    assert(TEST_OP(exec_mulhsu, -1, 0xFFFFFFFF) == 0xFFFFFFFF);
    assert(TEST_OP(exec_mulhsu, -1, 1) == 0xFFFFFFFF);
    assert(TEST_OP(exec_mulhsu, 0x80000000, 1) == 0xFFFFFFFF);
    assert(TEST_OP(exec_mulhsu, 0x80000000, 2) == 0xFFFFFFFF);
    assert(TEST_OP(exec_mulhsu, 0x80000000, 3) == 0xFFFFFFFE);
    assert(TEST_OP(exec_mulhsu, 0x7FFFFFFF, 0xFFFFFFFF) == 0x7FFFFFFE);

    // 4. mulhu tests
    assert(TEST_OP(exec_mulhu, 0xFFFFFFFF, 0xFFFFFFFF) == 0xFFFFFFFE);
    assert(TEST_OP(exec_mulhu, 0x80000000, 2) == 1);
    assert(TEST_OP(exec_mulhu, 0, 0xFFFFFFFF) == 0);

    // 5. div tests
    assert(TEST_OP(exec_div, 14641, 7) == 2091);
    assert(TEST_OP(exec_div, -20, 7) == (uint32_t)-2);
    assert(TEST_OP(exec_div, 20, -7) == (uint32_t)-2);
    assert(TEST_OP(exec_div, -20, -7) == 2);
    assert(TEST_OP(exec_div, 14641, 0) == 0xFFFFFFFF);              // div by 0
    assert(TEST_OP(exec_div, 0x80000000, 0xFFFFFFFF) == 0x80000000); // INT32_MIN / -1
    assert(TEST_OP(exec_div, 0x80000000, 1) == 0x80000000);

    // 6. divu tests
    assert(TEST_OP(exec_divu, 14641, 7) == 2091);
    assert(TEST_OP(exec_divu, 14641, 0) == 0xFFFFFFFF);             // divu by 0
    assert(TEST_OP(exec_divu, 0xFFFFFFFF, 2) == 0x7FFFFFFF);
    assert(TEST_OP(exec_divu, 0x80000000, 0xFFFFFFFF) == 0);

    // 7. rem tests
    assert(TEST_OP(exec_rem, 14641, 7) == 4);
    assert(TEST_OP(exec_rem, -20, 7) == (uint32_t)-6);              // sign of dividend
    assert(TEST_OP(exec_rem, 20, -7) == 6);                         // sign of dividend
    assert(TEST_OP(exec_rem, -20, -7) == (uint32_t)-6);
    assert(TEST_OP(exec_rem, 14641, 0) == 14641);                   // rem by 0 -> dividend
    assert(TEST_OP(exec_rem, 0x80000000, 0xFFFFFFFF) == 0);        // INT32_MIN % -1 -> 0
    assert(TEST_OP(exec_rem, 0x80000000, 1) == 0);

    // 8. remu tests
    assert(TEST_OP(exec_remu, 14641, 7) == 4);
    assert(TEST_OP(exec_remu, 14641, 0) == 14641);                  // remu by 0 -> dividend
    assert(TEST_OP(exec_remu, 0xFFFFFFFF, 2) == 1);
    assert(TEST_OP(exec_remu, 0x80000000, 0xFFFFFFFF) == 0x80000000);

    // Additional comprehensive edge-case tests
    // div with INT32_MIN / INT32_MIN == 1
    assert(TEST_OP(exec_div, 0x80000000, 0x80000000) == 1);
    assert(TEST_OP(exec_rem, 0x80000000, 0x80000000) == 0);

    // div with 0 dividend
    assert(TEST_OP(exec_div, 0, 5) == 0);
    assert(TEST_OP(exec_div, 0, 0) == 0xFFFFFFFF);
    assert(TEST_OP(exec_rem, 0, 5) == 0);
    assert(TEST_OP(exec_rem, 0, 0) == 0);
    assert(TEST_OP(exec_divu, 0, 5) == 0);
    assert(TEST_OP(exec_divu, 0, 0) == 0xFFFFFFFF);
    assert(TEST_OP(exec_remu, 0, 5) == 0);
    assert(TEST_OP(exec_remu, 0, 0) == 0);

    // div with divisor = INT32_MIN
    assert(TEST_OP(exec_div, 5, 0x80000000) == 0);
    assert(TEST_OP(exec_rem, 5, 0x80000000) == 5);
    assert(TEST_OP(exec_div, -5, 0x80000000) == 0);
    assert(TEST_OP(exec_rem, -5, 0x80000000) == (uint32_t)-5);

    // mulhsu with positive rs1
    assert(TEST_OP(exec_mulhsu, 1, 0xFFFFFFFF) == 0);
    assert(TEST_OP(exec_mulhsu, 2, 0x80000000) == 1);

    // 9. x0 discard test
    instr.rd = 0;
    exec_mul(&cpu, NULL, &instr);
    assert(cpu_read_reg(&cpu, 0) == 0);

    // 10. Registration table checks
    isa_registry_init();
    register_rv32m();
    assert(isa_find_by_mnemonic("mul") != NULL);
    assert(isa_find_by_mnemonic("mulh") != NULL);
    assert(isa_find_by_mnemonic("mulhsu") != NULL);
    assert(isa_find_by_mnemonic("mulhu") != NULL);
    assert(isa_find_by_mnemonic("div") != NULL);
    assert(isa_find_by_mnemonic("divu") != NULL);
    assert(isa_find_by_mnemonic("rem") != NULL);
    assert(isa_find_by_mnemonic("remu") != NULL);

    assert(isa_find_by_opcode(0x33, 0x0, 0x01, 0) == isa_find_by_mnemonic("mul"));
    assert(isa_find_by_opcode(0x33, 0x1, 0x01, 0) == isa_find_by_mnemonic("mulh"));
    assert(isa_find_by_opcode(0x33, 0x2, 0x01, 0) == isa_find_by_mnemonic("mulhsu"));
    assert(isa_find_by_opcode(0x33, 0x3, 0x01, 0) == isa_find_by_mnemonic("mulhu"));
    assert(isa_find_by_opcode(0x33, 0x4, 0x01, 0) == isa_find_by_mnemonic("div"));
    assert(isa_find_by_opcode(0x33, 0x5, 0x01, 0) == isa_find_by_mnemonic("divu"));
    assert(isa_find_by_opcode(0x33, 0x6, 0x01, 0) == isa_find_by_mnemonic("rem"));
    assert(isa_find_by_opcode(0x33, 0x7, 0x01, 0) == isa_find_by_mnemonic("remu"));

    printf("All RV32M unit assertions (including extra edge cases and registration checks) passed successfully!\n");
    return 0;
}
