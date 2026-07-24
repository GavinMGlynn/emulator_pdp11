// CPU behaviour tests. Each test is named as a sentence stating a hardware fact
// (emulator-setup-guide.md §6). Word/octal notation follows PDP-11 convention.
#include "unity.h"

#include "cpu/cpu.h"

static pdp11_cpu *cpu;

void setUp(void) {
    cpu = pdp11_cpu_create();
    TEST_ASSERT_NOT_NULL(cpu);
    cpu->r[PDP11_PC] = 001000; // conventional load address
}

void tearDown(void) {
    pdp11_cpu_destroy(cpu);
}

// Deposit a sequence of words starting at `addr`.
static void deposit(uint32_t addr, const uint16_t *words, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        pdp11_mem_write_word(cpu->mem, addr + (uint32_t)(i * 2u), words[i]);
    }
}

static void test_mov_immediate_to_register_sets_the_value(void) {
    // MOV #123456, R0  ->  012700 123456
    const uint16_t prog[] = {0012700u, 0123456u};
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0123456u, cpu->r[PDP11_R0]);
}

static void test_mov_of_a_negative_value_sets_n_and_clears_z(void) {
    const uint16_t prog[] = {0012700u, 0100000u}; // MOV #100000, R0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_N);
    TEST_ASSERT_FALSE(cpu->psw & PDP11_PSW_Z);
}

static void test_mov_of_zero_sets_z_and_clears_n(void) {
    const uint16_t prog[] = {0012700u, 0000000u}; // MOV #0, R0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_Z);
    TEST_ASSERT_FALSE(cpu->psw & PDP11_PSW_N);
}

static void test_mov_always_clears_the_overflow_flag(void) {
    cpu->psw = PDP11_PSW_V;
    const uint16_t prog[] = {0012700u, 0000001u}; // MOV #1, R0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_FALSE(cpu->psw & PDP11_PSW_V);
}

static void test_add_of_two_immediates_accumulates_in_the_register(void) {
    // MOV #1,R0 ; ADD #2,R0  ->  R0 == 3
    const uint16_t prog[] = {0012700u, 0000001u, 0062700u, 0000002u};
    deposit(001000, prog, 4);
    pdp11_cpu_step(cpu);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0000003u, cpu->r[PDP11_R0]);
}

static void test_add_that_carries_out_of_bit_15_sets_the_carry_flag(void) {
    // MOV #177777,R0 ; ADD #1,R0  ->  result 0, carry set, zero set
    const uint16_t prog[] = {0012700u, 0177777u, 0062700u, 0000001u};
    deposit(001000, prog, 4);
    pdp11_cpu_step(cpu);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0u, cpu->r[PDP11_R0]);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_C);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_Z);
}

static void test_add_of_two_large_positives_sets_the_overflow_flag(void) {
    // MOV #077777,R0 ; ADD #1,R0  ->  0100000, V set (signed overflow), N set
    const uint16_t prog[] = {0012700u, 0077777u, 0062700u, 0000001u};
    deposit(001000, prog, 4);
    pdp11_cpu_step(cpu);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0100000u, cpu->r[PDP11_R0]);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_V);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_N);
}

static void test_add_via_register_deferred_reads_through_the_pointer(void) {
    // R1 -> 002000 holding 000005 ; ADD (R1), R0 with R0 = 000003 -> R0 = 000010
    cpu->r[PDP11_R0] = 0000003u;
    cpu->r[PDP11_R1] = 0002000u;
    pdp11_mem_write_word(cpu->mem, 0002000u, 0000005u);
    const uint16_t prog[] = {0061100u}; // ADD (R1), R0
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0000010u, cpu->r[PDP11_R0]);
}

static void test_autoincrement_advances_the_pointer_register_by_two(void) {
    // MOV (R1)+, R0  with R1 -> 002000 holding 000042
    cpu->r[PDP11_R1] = 0002000u;
    pdp11_mem_write_word(cpu->mem, 0002000u, 0000042u);
    const uint16_t prog[] = {0012100u}; // MOV (R1)+, R0
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0000042u, cpu->r[PDP11_R0]);
    TEST_ASSERT_EQUAL_HEX16(0002002u, cpu->r[PDP11_R1]);
}

static void test_halt_stops_execution_and_latches_the_halted_flag(void) {
    const uint16_t prog[] = {0000000u, 0012700u, 0000001u}; // HALT ; MOV #1,R0
    deposit(001000, prog, 3);
    pdp11_cpu_step(cpu); // HALT
    TEST_ASSERT_TRUE(cpu->halted);
    pdp11_cpu_step(cpu); // no-op while halted
    TEST_ASSERT_EQUAL_HEX16(0u, cpu->r[PDP11_R0]);
}

// Run a one- or two-word instruction loaded at 001000 and stop.
static void run1(const uint16_t *prog, size_t n) {
    deposit(001000, prog, n);
    pdp11_cpu_step(cpu);
}

static void test_cmp_sets_carry_as_borrow_when_source_is_below_destination(void) {
    // CMP R0,R1 computes R0 - R1. With R0<R1 (unsigned) there is a borrow -> C=1.
    cpu->r[PDP11_R0] = 0000001u;
    cpu->r[PDP11_R1] = 0000002u;
    const uint16_t prog[] = {0020001u}; // CMP R0, R1
    run1(prog, 1);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_C);  // borrow
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_N);  // 1-2 = -1
    TEST_ASSERT_EQUAL_HEX16(0000001u, cpu->r[PDP11_R0]); // CMP does not store
}

static void test_sub_subtracts_source_from_destination(void) {
    // SUB R0,R1 computes R1 = R1 - R0 (opposite operand order to CMP).
    cpu->r[PDP11_R0] = 0000003u;
    cpu->r[PDP11_R1] = 0000010u;
    const uint16_t prog[] = {0160001u}; // SUB R0, R1
    run1(prog, 1);
    TEST_ASSERT_EQUAL_HEX16(0000005u, cpu->r[PDP11_R1]);
    TEST_ASSERT_FALSE(cpu->psw & PDP11_PSW_C); // no borrow
}

static void test_neg_of_the_most_negative_value_sets_overflow(void) {
    // NEG 0100000 has no positive representation -> V set, result unchanged.
    cpu->r[PDP11_R0] = 0100000u;
    const uint16_t prog[] = {0005400u}; // NEG R0
    run1(prog, 1);
    TEST_ASSERT_EQUAL_HEX16(0100000u, cpu->r[PDP11_R0]);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_V);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_C);
}

static void test_movb_into_a_register_sign_extends_the_byte(void) {
    // MOVB #0200,R0-equivalent: put 0377 low byte source -> sign-extend to -1.
    cpu->r[PDP11_R1] = 0000377u;       // low byte 0377 (bit 7 set)
    const uint16_t prog[] = {0110100u}; // MOVB R1, R0
    run1(prog, 1);
    TEST_ASSERT_EQUAL_HEX16(0177777u, cpu->r[PDP11_R0]); // sign-extended
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_N);
}

static void test_movb_to_a_register_of_a_positive_byte_clears_the_high_byte(void) {
    cpu->r[PDP11_R0] = 0177777u;       // dirty high byte
    cpu->r[PDP11_R1] = 0000101u;       // 'A', bit 7 clear
    const uint16_t prog[] = {0110100u}; // MOVB R1, R0
    run1(prog, 1);
    TEST_ASSERT_EQUAL_HEX16(0000101u, cpu->r[PDP11_R0]);
}

static void test_ror_rotates_bit0_into_carry_and_carry_into_bit15(void) {
    cpu->psw |= PDP11_PSW_C;            // carry in -> becomes bit 15
    cpu->r[PDP11_R0] = 0000001u;        // bit 0 set -> becomes new carry
    const uint16_t prog[] = {0006000u}; // ROR R0
    run1(prog, 1);
    TEST_ASSERT_EQUAL_HEX16(0100000u, cpu->r[PDP11_R0]);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_C);
}

static void test_swab_swaps_the_two_bytes_of_a_word(void) {
    cpu->r[PDP11_R0] = 0000377u;        // low byte only
    const uint16_t prog[] = {0000300u}; // SWAB R0
    run1(prog, 1);
    TEST_ASSERT_EQUAL_HEX16(0177400u, cpu->r[PDP11_R0]);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_Z); // new low byte is zero
}

static void test_sxt_fills_the_destination_from_the_n_flag(void) {
    cpu->psw |= PDP11_PSW_N;
    cpu->r[PDP11_R0] = 0000123u;
    const uint16_t prog[] = {0006700u}; // SXT R0
    run1(prog, 1);
    TEST_ASSERT_EQUAL_HEX16(0177777u, cpu->r[PDP11_R0]);
    TEST_ASSERT_FALSE(cpu->psw & PDP11_PSW_Z);
}

static void test_beq_branches_backward_when_zero_is_set(void) {
    // BEQ with offset -2 (0377 = -1 word => target = PC). Use Z set.
    cpu->psw |= PDP11_PSW_Z;
    const uint16_t prog[] = {0001776u}; // BEQ .-2  (offset 0376 = -2 words)
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    // PC after fetch is 001002; offset -2 words => 001002 - 4 = 000776.
    TEST_ASSERT_EQUAL_HEX16(0000776u, cpu->r[PDP11_PC]);
}

static void test_bne_is_not_taken_when_zero_is_set(void) {
    cpu->psw |= PDP11_PSW_Z;
    const uint16_t prog[] = {0001000u}; // BNE .+2 area; Z set => not taken
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001002u, cpu->r[PDP11_PC]); // fell through
}

static void test_sob_decrements_and_loops_until_the_register_is_zero(void) {
    cpu->r[PDP11_R0] = 3;
    // SOB R0,.  targeting itself: offset 1 => PC back to the SOB word.
    const uint16_t prog[] = {0077001u}; // SOB R0, .-2
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu); // 3 -> 2, loop
    TEST_ASSERT_EQUAL_HEX16(2u, cpu->r[PDP11_R0]);
    TEST_ASSERT_EQUAL_HEX16(0001000u, cpu->r[PDP11_PC]);
}

static void test_jsr_pushes_the_register_and_saves_the_return_address(void) {
    cpu->r[PDP11_SP] = 0002000u;
    cpu->r[PDP11_R5] = 0123456u;
    // JSR R5, @#001020 : reg=5, dst mode 3 reg 7 (absolute) = 037
    const uint16_t prog[] = {0004537u, 0001020u}; // JSR R5, @#1020
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001020u, cpu->r[PDP11_PC]);   // jumped
    TEST_ASSERT_EQUAL_HEX16(0001004u, cpu->r[PDP11_R5]);   // R5 = old PC
    TEST_ASSERT_EQUAL_HEX16(0001776u, cpu->r[PDP11_SP]);   // pushed one word
    TEST_ASSERT_EQUAL_HEX16(0123456u,
                            pdp11_mem_read_word(cpu->mem, 0001776u)); // old R5
}

static void test_sec_sets_and_clc_clears_the_carry_flag(void) {
    const uint16_t sec[] = {0000261u}; // SEC
    deposit(001000, sec, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_C);
    cpu->r[PDP11_PC] = 001000;
    const uint16_t clc[] = {0000241u}; // CLC
    deposit(001000, clc, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_FALSE(cpu->psw & PDP11_PSW_C);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mov_immediate_to_register_sets_the_value);
    RUN_TEST(test_mov_of_a_negative_value_sets_n_and_clears_z);
    RUN_TEST(test_mov_of_zero_sets_z_and_clears_n);
    RUN_TEST(test_mov_always_clears_the_overflow_flag);
    RUN_TEST(test_add_of_two_immediates_accumulates_in_the_register);
    RUN_TEST(test_add_that_carries_out_of_bit_15_sets_the_carry_flag);
    RUN_TEST(test_add_of_two_large_positives_sets_the_overflow_flag);
    RUN_TEST(test_add_via_register_deferred_reads_through_the_pointer);
    RUN_TEST(test_autoincrement_advances_the_pointer_register_by_two);
    RUN_TEST(test_halt_stops_execution_and_latches_the_halted_flag);
    RUN_TEST(test_cmp_sets_carry_as_borrow_when_source_is_below_destination);
    RUN_TEST(test_sub_subtracts_source_from_destination);
    RUN_TEST(test_neg_of_the_most_negative_value_sets_overflow);
    RUN_TEST(test_movb_into_a_register_sign_extends_the_byte);
    RUN_TEST(test_movb_to_a_register_of_a_positive_byte_clears_the_high_byte);
    RUN_TEST(test_ror_rotates_bit0_into_carry_and_carry_into_bit15);
    RUN_TEST(test_swab_swaps_the_two_bytes_of_a_word);
    RUN_TEST(test_sxt_fills_the_destination_from_the_n_flag);
    RUN_TEST(test_beq_branches_backward_when_zero_is_set);
    RUN_TEST(test_bne_is_not_taken_when_zero_is_set);
    RUN_TEST(test_sob_decrements_and_loops_until_the_register_is_zero);
    RUN_TEST(test_jsr_pushes_the_register_and_saves_the_return_address);
    RUN_TEST(test_sec_sets_and_clc_clears_the_carry_flag);
    return UNITY_END();
}
