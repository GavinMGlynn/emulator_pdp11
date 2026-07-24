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
    return UNITY_END();
}
