// CPU behaviour tests. Each test is named as a sentence stating a hardware fact
// (emulator-setup-guide.md §6). Word/octal notation follows PDP-11 convention.
#include "unity.h"

#include "clk/clk.h"
#include "console/console.h"
#include "cpu/cpu.h"
#include "devices/rk11.h"
#include "devices/tm11.h"

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

static void test_sbc_of_the_most_negative_value_sets_v_only_with_carry_in(void) {
    // SBC R0 with R0 = 0100000. With C clear there is no subtraction, so no
    // overflow (V=0); the differential fuzzer caught us setting V unconditionally.
    cpu->r[PDP11_R0] = 0100000u;
    cpu->psw = 0; // C clear
    const uint16_t sbc[] = {0005600u}; // SBC R0
    run1(sbc, 1);
    TEST_ASSERT_EQUAL_HEX16(0100000u, cpu->r[PDP11_R0]); // unchanged (dst - 0)
    TEST_ASSERT_FALSE(cpu->psw & PDP11_PSW_V);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_N);
    // With C set, subtracting 1 from 0100000 does overflow -> V set.
    pdp11_cpu_reset(cpu);
    cpu->r[PDP11_R0] = 0100000u;
    cpu->psw = PDP11_PSW_C;
    cpu->r[PDP11_PC] = 001000;
    run1(sbc, 1);
    TEST_ASSERT_EQUAL_HEX16(0077777u, cpu->r[PDP11_R0]);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_V);
}

static void test_writing_the_psw_via_177776_keeps_the_written_condition_codes(void) {
    // MOV #4, @#177776 loads the PSW with Z set. The MOV would normally recompute
    // Z=0 (value 4 != 0), but an explicit PSW store wins — the codes come from the
    // data written (matches SimH; the V6 idle loop restores the PSW this way).
    const uint16_t prog[] = {0012737u, 0000004u, 0177776u}; // MOV #4, @#177776
    deposit(001000, prog, 3);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0000004u, cpu->psw); // Z preserved, not clobbered
}

static void test_spl_sets_the_processor_priority_in_kernel_mode(void) {
    // SPL 6 (0002346) in Kernel mode sets PSW<7:5> = 6. The V6 idle loop relies
    // on this to mask interrupts while scanning the process table.
    cpu->psw = 0; // kernel mode, priority 0
    const uint16_t spl6[] = {0000236u}; // SPL 6 = 000230 | 6
    run1(spl6, 1);
    TEST_ASSERT_EQUAL_HEX16(6u, (cpu->psw >> 5) & 07u);
    // In a non-Kernel mode SPL is a no-op (privileged).
    pdp11_cpu_reset(cpu);
    cpu->psw = 0140000u; // current mode = User (PSW<15:14> = 11), priority 0
    cpu->r[PDP11_PC] = 001000;
    run1(spl6, 1);
    TEST_ASSERT_EQUAL_HEX16(0u, (cpu->psw >> 5) & 07u); // unchanged in User mode
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

static void test_trap_pushes_psw_then_pc_and_vectors_through_034(void) {
    cpu->r[PDP11_SP] = 0002000u;
    cpu->psw = PDP11_PSW_C; // a live flag to see preserved on the stack
    pdp11_mem_write_word(cpu->mem, 0034u, 0001500u); // vector PC
    pdp11_mem_write_word(cpu->mem, 0036u, 0000340u); // vector PSW (priority 7)
    const uint16_t prog[] = {0104401u}; // TRAP 1
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001500u, cpu->r[PDP11_PC]);        // new PC
    TEST_ASSERT_EQUAL_HEX16(0000340u, cpu->psw);               // new PSW
    TEST_ASSERT_EQUAL_HEX16(0001774u, cpu->r[PDP11_SP]);       // two words pushed
    TEST_ASSERT_EQUAL_HEX16(0001002u,
                            pdp11_mem_read_word(cpu->mem, 0001774u)); // old PC (top)
    TEST_ASSERT_EQUAL_HEX16(PDP11_PSW_C,
                            pdp11_mem_read_word(cpu->mem, 0001776u)); // old PSW
}

static void test_rti_pops_pc_then_psw(void) {
    cpu->r[PDP11_SP] = 0001774u;
    pdp11_mem_write_word(cpu->mem, 0001774u, 0001234u); // PC (top)
    pdp11_mem_write_word(cpu->mem, 0001776u, 0000004u); // PSW (Z)
    const uint16_t prog[] = {0000002u}; // RTI
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001234u, cpu->r[PDP11_PC]);
    TEST_ASSERT_EQUAL_HEX16(0000004u, cpu->psw);
    TEST_ASSERT_EQUAL_HEX16(0002000u, cpu->r[PDP11_SP]);
}

static void test_mul_produces_a_32_bit_product_across_the_register_pair(void) {
    cpu->r[PDP11_R0] = 0000010u; // 8
    cpu->r[PDP11_R1] = 0;
    const uint16_t prog[] = {0070027u, 0000005u}; // MUL #5, R0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0u, cpu->r[PDP11_R0]);      // high word
    TEST_ASSERT_EQUAL_HEX16(0000050u, cpu->r[PDP11_R1]); // low word = 40
    TEST_ASSERT_FALSE(cpu->psw & PDP11_PSW_C);
}

static void test_div_by_zero_sets_v_and_c(void) {
    cpu->r[PDP11_R0] = 0;
    cpu->r[PDP11_R1] = 0000012u;
    const uint16_t prog[] = {0071027u, 0000000u}; // DIV #0, R0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_V);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_C);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_Z);
    TEST_ASSERT_FALSE(cpu->psw & PDP11_PSW_N);
}

static void test_ash_shifts_left_and_records_the_last_bit_shifted_out(void) {
    cpu->r[PDP11_R0] = 0000005u; // 101b
    const uint16_t prog[] = {0072027u, 0000003u}; // ASH #3, R0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0000050u, cpu->r[PDP11_R0]); // 5 << 3 = 40
}

static void test_xor_toggles_bits_and_leaves_carry_alone(void) {
    cpu->psw = PDP11_PSW_C;
    cpu->r[PDP11_R0] = 0125252u;
    cpu->r[PDP11_R2] = 0052525u;
    const uint16_t prog[] = {0074002u}; // XOR R0, R2
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0177777u, cpu->r[PDP11_R2]);
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_C); // C unchanged
    TEST_ASSERT_TRUE(cpu->psw & PDP11_PSW_N);
}

static void test_the_psw_is_readable_at_its_io_page_address(void) {
    cpu->psw = PDP11_PSW_N | PDP11_PSW_C;
    // MOV @#177776, R0 : src mode 3 reg 7 (absolute), dst R0
    const uint16_t prog[] = {0013700u, 0177776u};
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    // R0 gets the PSW value; MOV then updates N,Z from it (C preserved).
    TEST_ASSERT_EQUAL_HEX16((uint16_t)(PDP11_PSW_N | PDP11_PSW_C),
                            cpu->r[PDP11_R0]);
}

static void test_a_word_write_to_an_odd_address_traps_through_vector_4(void) {
    cpu->r[PDP11_SP] = 0002000u;
    pdp11_mem_write_word(cpu->mem, 0004u, 0001500u); // vec 4 -> handler
    pdp11_mem_write_word(cpu->mem, 0006u, 0u);
    // MOV R1, @#1001  (odd destination address)
    const uint16_t prog[] = {0010137u, 0001001u};
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001500u, cpu->r[PDP11_PC]); // vectored to handler
    TEST_ASSERT_EQUAL_HEX16(0001774u, cpu->r[PDP11_SP]); // frame pushed
}

// A reference whose relocated physical address is at or above installed memory
// (and below the I/O page) is non-existent memory: the 11/70 aborts it through
// vector 4, exactly as SimH does (pdp11_cpu.c ReadW: pa >= MEMSIZE && pa <
// IOPAGEBASE). Unix sizes core by walking it until this trap fires; without it
// the memory-sizing loop never terminates. mem_top is shrunk so a reachable
// 16-bit address (MMU off) lands in the non-existent region.
static void test_a_word_access_to_nonexistent_memory_traps_through_vector_4(void) {
    cpu->r[PDP11_SP] = 0002000u;
    cpu->mem_top = 0100000u;                          // 32 KB installed RAM
    pdp11_mem_write_word(cpu->mem, 0004u, 0001500u);  // vec 4 -> handler
    pdp11_mem_write_word(cpu->mem, 0006u, 0u);
    // MOV @#140000, R0 — absolute 140000 is above installed RAM, below the I/O
    // page, so the operand read is non-existent memory.
    const uint16_t prog[] = {0013700u, 0140000u};
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001500u, cpu->r[PDP11_PC]); // vectored to the handler
    TEST_ASSERT_EQUAL_HEX16(0001774u, cpu->r[PDP11_SP]); // trap frame pushed
}

static void test_an_11_70_illegal_instruction_traps_through_vector_10(void) {
    cpu->r[PDP11_SP] = 0002000u;
    pdp11_mem_write_word(cpu->mem, 0010u, 0001600u); // vec 10 -> handler
    pdp11_mem_write_word(cpu->mem, 0012u, 0u);
    const uint16_t prog[] = {0106700u}; // MFPS — illegal on the 11/70
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001600u, cpu->r[PDP11_PC]);
}

static void test_a_pirq_above_the_cpu_priority_is_granted_through_240(void) {
    cpu->r[PDP11_SP] = 0002000u;
    cpu->psw = 0; // priority 0
    pdp11_mem_write_word(cpu->mem, 0240u, 0001600u); // vec -> handler
    pdp11_mem_write_word(cpu->mem, 0242u, 0000340u); // handler PSW: priority 7
    cpu->pirq = 0100000u; // request PIR7 (bit 15)
    // The next step takes the interrupt before fetching an instruction.
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001600u, cpu->r[PDP11_PC]);
    TEST_ASSERT_EQUAL_HEX16(0000340u, cpu->psw);
}

static void test_a_pirq_at_or_below_the_cpu_priority_is_masked(void) {
    cpu->psw = 0000340u; // priority 7
    cpu->pirq = 0100000u; // PIR7 -> level 7, not > 7
    const uint16_t prog[] = {0005202u}; // INC R2 runs normally
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(1u, cpu->r[PDP11_R2]);
    TEST_ASSERT_EQUAL_HEX16(0001002u, cpu->r[PDP11_PC]); // no vectoring
}

static void test_wait_idles_until_an_interrupt_is_granted(void) {
    cpu->psw = 0; // priority 0
    pdp11_mem_write_word(cpu->mem, 0240u, 0001600u);
    pdp11_mem_write_word(cpu->mem, 0242u, 0000340u);
    const uint16_t prog[] = {0000001u}; // WAIT
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu); // WAIT executes
    TEST_ASSERT_TRUE(cpu->waiting);
    TEST_ASSERT_EQUAL_HEX16(0001002u, cpu->r[PDP11_PC]);
    pdp11_cpu_step(cpu); // no interrupt: idles, PC does not advance
    TEST_ASSERT_TRUE(cpu->waiting);
    TEST_ASSERT_EQUAL_HEX16(0001002u, cpu->r[PDP11_PC]);
    cpu->r[PDP11_SP] = 0002000u;
    cpu->pirq = 0100000u; // PIR7 arrives
    pdp11_cpu_step(cpu); // interrupt granted; wait ends
    TEST_ASSERT_FALSE(cpu->waiting);
    TEST_ASSERT_EQUAL_HEX16(0001600u, cpu->r[PDP11_PC]);
}

static void test_mark_restores_pc_from_r5_and_cleans_the_stack(void) {
    cpu->r[5] = 0001234u;
    pdp11_mem_write_word(cpu->mem, 0001002u, 0004321u); // mem[i], i = PC after MARK
    const uint16_t prog[] = {0006400u}; // MARK 0
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001234u, cpu->r[PDP11_PC]); // PC <- old R5
    TEST_ASSERT_EQUAL_HEX16(0004321u, cpu->r[5]);        // R5 <- mem[i]
    TEST_ASSERT_EQUAL_HEX16(0001004u, cpu->r[PDP11_SP]); // SP <- i + 2
}

static void test_jmp_to_a_register_is_illegal_on_the_11_70(void) {
    cpu->r[PDP11_SP] = 0002000u;
    pdp11_mem_write_word(cpu->mem, 0010u, 0001600u); // vec 10 -> handler
    pdp11_mem_write_word(cpu->mem, 0012u, 0u);
    const uint16_t prog[] = {0000100u}; // JMP R0 (register mode)
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001600u, cpu->r[PDP11_PC]); // trapped to handler
}

static void test_the_mmu_relocates_a_virtual_write_to_its_physical_page(void) {
    cpu->pdr[0] = 0077406u; // code page: read/write, full length
    cpu->pdr[1] = 0077406u; // target page: read/write, full length
    cpu->par[1] = 0001000u; // VA page 1 (020000) -> PA 0100000
    cpu->mmr3 = 0000020u;   // 22-bit enable
    cpu->mmr0 = 0000001u;   // management enable
    // MOV #123456, @#020000  (code page 0 maps identity via PAR[0]=0)
    const uint16_t prog[] = {0012737u, 0123456u, 0020000u};
    deposit(001000, prog, 3);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0123456u,
                            pdp11_mem_read_word(cpu->mem, 0100000u));
    // And nothing landed at the unrelocated virtual address.
    TEST_ASSERT_EQUAL_HEX16(0u, pdp11_mem_read_word(cpu->mem, 0020000u));
}

static void test_a_write_to_a_read_only_page_aborts_through_vector_250(void) {
    cpu->r[PDP11_SP] = 0017000u;  // stack in page 0
    cpu->pdr[0] = 0077406u;       // code/stack page: read/write
    cpu->pdr[2] = 0077402u;       // page 2: read-only (ACF = 2)
    cpu->mmr3 = 0000020u;         // 22-bit
    cpu->mmr0 = 0000001u;         // enable
    pdp11_mem_write_word(cpu->mem, 0250u, 0001600u); // MMU-abort vector
    pdp11_mem_write_word(cpu->mem, 0252u, 0u);
    // MOV #123, @#040000  — write into the read-only page 2
    const uint16_t prog[] = {0012737u, 0000123u, 0040000u};
    deposit(001000, prog, 3);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0001600u, cpu->r[PDP11_PC]); // vectored to handler
    TEST_ASSERT_TRUE(cpu->mmr0 & 0020000u);              // read-only error
    TEST_ASSERT_TRUE(cpu->mmr0 & 0000200u);              // instruction complete
    TEST_ASSERT_EQUAL_HEX16(0000004u, cpu->mmr0 & 0000176u); // page 2 recorded
}

static void test_r0_r5_are_banked_by_the_psw_register_set_bit(void) {
    cpu->r[PDP11_R0] = 0001111u; // set 0
    // MOV #4000, @#177776 ; MOV #0, @#177776  (switch to set 1, then back)
    const uint16_t prog[] = {0012737u, 0004000u, 0177776u,
                             0012737u, 0000000u, 0177776u};
    deposit(001000, prog, 6);
    pdp11_cpu_step(cpu); // -> set 1: R0 becomes the (empty) alternate set
    TEST_ASSERT_EQUAL_HEX16(0u, cpu->r[PDP11_R0]);
    cpu->r[PDP11_R0] = 0002222u; // set 1
    pdp11_cpu_step(cpu); // -> back to set 0
    TEST_ASSERT_EQUAL_HEX16(0001111u, cpu->r[PDP11_R0]);
}

static void test_mfpi_reads_the_previous_modes_space_and_pushes_it(void) {
    // Kernel current, user previous. User page 1 (VA 020000) -> PA 0100000.
    cpu->psw = 0030000u;          // cm=Kernel, pm=User
    cpu->r[PDP11_SP] = 0001000u;
    cpu->pdr[0] = 0077406u;       // kernel code/stack
    cpu->pdr[(3 << 4) | 1] = 0077406u; // user page 1
    cpu->par[(3 << 4) | 1] = 0001000u; // user page 1 -> PA 0100000
    cpu->mmr3 = 0000020u;
    cpu->mmr0 = 0000001u;
    pdp11_mem_write_word(cpu->mem, 0100000u, 0123456u); // value in user space
    const uint16_t prog[] = {0006537u, 0020000u}; // MFPI @#020000
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0000776u, cpu->r[PDP11_SP]);          // pushed one word
    TEST_ASSERT_EQUAL_HEX16(0123456u,
                            pdp11_mem_read_word(cpu->mem, 0000776u)); // user value
}

static void test_data_references_use_d_space_when_it_is_enabled(void) {
    cpu->pdr[0] = 0077406u;            // kernel I code page
    cpu->pdr[(1 << 3) | 2] = 0077406u; // kernel D page 2 (idx 10)
    cpu->par[(1 << 3) | 2] = 0001000u; // kernel D page 2 -> PA 0100000
    cpu->mmr3 = 0000024u;             // KDS | M22E (kernel D-space enabled)
    cpu->mmr0 = 0000001u;
    pdp11_mem_write_word(cpu->mem, 0100000u, 0122222u);
    cpu->r[PDP11_R1] = 0040000u;      // VA page 2
    const uint16_t prog[] = {0011100u}; // MOV (R1), R0 — a data reference
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0122222u, cpu->r[PDP11_R0]); // read via D-space
}

static void test_branch_timing_is_600ns_taken_and_300ns_not_taken(void) {
    cpu->psw |= PDP11_PSW_Z;             // Z set
    const uint16_t taken[] = {0001400u}; // BEQ .  (taken)
    deposit(001000, taken, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_UINT64(600, cpu->time_ns);

    pdp11_cpu_destroy(cpu);
    cpu = pdp11_cpu_create();
    cpu->r[PDP11_PC] = 001000;
    cpu->psw |= PDP11_PSW_Z;
    const uint16_t nottaken[] = {0001000u}; // BNE (Z set -> not taken)
    deposit(001000, nottaken, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_UINT64(300, cpu->time_ns);
}

static void test_ldfps_and_cfcc_move_the_fp_status_and_condition_codes(void) {
    // LDFPS #240 ; CFCC   (FPS = N | Z... 240 = D-bit + ... use CC bits)
    cpu->psw = 0;
    const uint16_t prog[] = {0170127u, 0000017u, 0170000u}; // LDFPS #17 ; CFCC
    deposit(001000, prog, 3);
    pdp11_cpu_step(cpu); // LDFPS #17 -> FPS low nibble = N Z V C
    TEST_ASSERT_EQUAL_HEX16(0000017u, cpu->fps);
    pdp11_cpu_step(cpu); // CFCC -> PSW condition codes = FPS condition codes
    TEST_ASSERT_EQUAL_HEX16(0000017u, (uint16_t)(cpu->psw & 017u));
}

static void test_setd_and_setf_toggle_the_fps_double_bit(void) {
    const uint16_t setd[] = {0170011u}; // SETD
    deposit(001000, setd, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_TRUE(cpu->fps & 0000200u); // D bit set
    cpu->r[PDP11_PC] = 001000;
    const uint16_t setf[] = {0170001u}; // SETF
    deposit(001000, setf, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_FALSE(cpu->fps & 0000200u); // D bit cleared
}

static void test_ldf_stf_round_trip_a_single_through_an_accumulator(void) {
    // mem[2000] = 1.0 (040200 000000); LDF @#2000,AC0 ; STF AC0,@#2010
    pdp11_mem_write_word(cpu->mem, 0002000u, 0040200u);
    pdp11_mem_write_word(cpu->mem, 0002002u, 0u);
    const uint16_t prog[] = {0172437u, 0002000u,  // LDF @#2000, AC0
                             0174037u, 0002010u};  // STF AC0, @#2010
    deposit(001000, prog, 4);
    pdp11_cpu_step(cpu); // LDF
    TEST_ASSERT_EQUAL_HEX64(0x4080000000000000ULL, cpu->fac[0]);
    pdp11_cpu_step(cpu); // STF
    TEST_ASSERT_EQUAL_HEX16(0040200u, pdp11_mem_read_word(cpu->mem, 0002010u));
    TEST_ASSERT_EQUAL_HEX16(0u, pdp11_mem_read_word(cpu->mem, 0002012u));
}

static void test_negf_flips_the_sign_of_an_accumulator(void) {
    cpu->fac[0] = 0x4080000000000000ULL; // 1.0
    const uint16_t prog[] = {0170700u}; // NEGF AC0
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX64(0xC080000000000000ULL, cpu->fac[0]); // sign set
    TEST_ASSERT_TRUE(cpu->fps & 0000010u); // FP N set
}

static void test_addf_adds_two_single_floats(void) {
    // AC0 = 1.0 ; ADDF @#2000 (1.0) -> 2.0 (exp 130 -> high word 040400)
    cpu->fac[0] = 0x4080000000000000ULL; // 1.0
    pdp11_mem_write_word(cpu->mem, 0002000u, 0040200u); // 1.0
    pdp11_mem_write_word(cpu->mem, 0002002u, 0u);
    const uint16_t prog[] = {0172037u, 0002000u}; // ADDF @#2000, AC0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0040400u, (uint16_t)(cpu->fac[0] >> 48)); // 2.0
    TEST_ASSERT_FALSE(cpu->fps & 0000004u); // FP Z clear (nonzero)
}

static void test_mulf_multiplies_two_single_floats(void) {
    // AC0 = 2.0 ; MULF @#2000 (3.0) -> 6.0 (high word 040700)
    cpu->fac[0] = 0x4100000000000000ULL; // 2.0
    pdp11_mem_write_word(cpu->mem, 0002000u, 0040500u); // 3.0
    pdp11_mem_write_word(cpu->mem, 0002002u, 0u);
    const uint16_t prog[] = {0171037u, 0002000u}; // MULF @#2000, AC0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0040700u, (uint16_t)(cpu->fac[0] >> 48)); // 6.0
}

static void test_divf_divides_two_single_floats(void) {
    // AC0 = 6.0 ; DIVF @#2000 (2.0) -> 3.0 (high word 040500)
    cpu->fac[0] = 0x41C0000000000000ULL; // 6.0
    pdp11_mem_write_word(cpu->mem, 0002000u, 0040400u); // 2.0
    pdp11_mem_write_word(cpu->mem, 0002002u, 0u);
    const uint16_t prog[] = {0174437u, 0002000u}; // DIVF @#2000, AC0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0040500u, (uint16_t)(cpu->fac[0] >> 48)); // 3.0
}

static void test_divf_by_zero_traps_through_the_fpe_vector(void) {
    // FPE vector 244 -> handler 3000; AC0 = 2.0 ; DIVF @#2000 (0.0) -> trap
    pdp11_mem_write_word(cpu->mem, 0000244u, 0003000u);
    pdp11_mem_write_word(cpu->mem, 0000246u, 0u);
    cpu->fac[0] = 0x4100000000000000ULL; // 2.0
    pdp11_mem_write_word(cpu->mem, 0002000u, 0u); // 0.0
    pdp11_mem_write_word(cpu->mem, 0002002u, 0u);
    const uint16_t prog[] = {0174437u, 0002000u}; // DIVF @#2000, AC0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0003000u, cpu->r[7]);  // vectored to the handler
    TEST_ASSERT_EQUAL_HEX16(4u, cpu->fec);         // FEC_DZRO
    TEST_ASSERT_EQUAL_HEX16(0001000u, cpu->fea);   // faulting DIVF address
    TEST_ASSERT_TRUE(cpu->fps & 0100000u);         // FPS_ER
}

static void test_cmpf_sets_n_when_the_source_is_below_the_accumulator(void) {
    // AC0 = 3.0 ; CMPF @#2000 (2.0): fsrc 2.0 < fac 3.0 -> N set
    cpu->fac[0] = 0x4140000000000000ULL; // 3.0
    pdp11_mem_write_word(cpu->mem, 0002000u, 0040400u); // 2.0
    pdp11_mem_write_word(cpu->mem, 0002002u, 0u);
    const uint16_t prog[] = {0173437u, 0002000u}; // CMPF @#2000, AC0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_TRUE(cpu->fps & 0000010u);  // FP N set (source below accumulator)
    TEST_ASSERT_FALSE(cpu->fps & 0000004u); // FP Z clear (unequal)
}

static void test_kw11l_powers_up_with_the_monitor_bit_set(void) {
    // Reading LKS right after reset returns the DONE/monitor bit (0200).
    const uint16_t prog[] = {0013700u, 0177546u}; // MOV @#177546, R0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0000200u, cpu->r[PDP11_R0]);
}

static void test_kw11l_tick_with_interrupts_enabled_vectors_through_100(void) {
    pdp11_mem_write_word(cpu->mem, 0000100u, 0003000u); // vector PC -> handler
    pdp11_mem_write_word(cpu->mem, 0000102u, 0000340u); // vector PSW (priority 7)
    cpu->r[PDP11_SP] = 0004000u;        // a real stack (0 would push onto the I/O page)
    pdp11_clk_write(cpu, KW11L_IE);     // enable clock interrupts
    pdp11_clk_tick(cpu);                // a tick sets DONE and requests the BR6 int
    const uint16_t prog[] = {0010000u}; // MOV R0,R0 (would run absent an interrupt)
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0003000u, cpu->r[PDP11_PC]); // vectored to the handler
    TEST_ASSERT_EQUAL_HEX16(0000340u, cpu->psw);         // handler PSW (priority 7)
}

static void test_kw11l_tick_without_ie_sets_done_but_does_not_interrupt(void) {
    pdp11_mem_write_word(cpu->mem, 0000100u, 0003000u);
    pdp11_mem_write_word(cpu->mem, 0000102u, 0000340u);
    pdp11_clk_tick(cpu);                // IE off: a tick sets DONE only
    const uint16_t prog[] = {0000000u}; // HALT
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_TRUE(cpu->halted);                    // ran the HALT, took no vector
    TEST_ASSERT_TRUE(cpu->r[PDP11_PC] != 0003000u);
    TEST_ASSERT_TRUE(cpu->clk_csr & KW11L_DONE);      // DONE set by the tick
}

static void test_kw11l_writing_zero_to_done_clears_the_monitor_bit(void) {
    // DONE powers up set; writing a 0 to LKS clears the monitor bit.
    pdp11_clk_write(cpu, 0);
    TEST_ASSERT_FALSE(cpu->clk_csr & KW11L_DONE);
}

static uint8_t tx_capture[64];
static size_t tx_len;
static void tx_sink(void *ctx, uint8_t ch) {
    (void)ctx;
    if (tx_len < sizeof tx_capture) {
        tx_capture[tx_len++] = ch;
    }
}

static void test_dl11_receiver_latches_input_and_reading_rbuf_clears_done(void) {
    pdp11_console_input(cpu, 'A');
    TEST_ASSERT_TRUE(cpu->tti_csr & DL11_DONE);
    const uint16_t prog[] = {0013700u, 0177562u}; // MOV @#177562 (RBUF), R0
    deposit(001000, prog, 2);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16('A', cpu->r[PDP11_R0]);
    TEST_ASSERT_FALSE(cpu->tti_csr & DL11_DONE); // reading RBUF clears DONE
}

static void test_dl11_receiver_interrupt_vectors_through_060(void) {
    pdp11_mem_write_word(cpu->mem, 0000060u, 0003000u); // receiver vector -> handler
    pdp11_mem_write_word(cpu->mem, 0000062u, 0000340u);
    cpu->r[PDP11_SP] = 0004000u;
    pdp11_console_write(cpu, DL11_RCSR, DL11_IE); // enable receiver interrupts
    pdp11_console_input(cpu, 'Z');                // a character arrives -> BR4 int
    const uint16_t prog[] = {0010000u};           // MOV R0,R0 (runs absent an int)
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0003000u, cpu->r[PDP11_PC]); // vectored to the handler
}

static void test_dl11_transmitter_emits_to_the_sink_then_completes(void) {
    tx_len = 0;
    pdp11_console_set_sink(cpu, tx_sink, NULL);
    pdp11_console_write(cpu, DL11_XBUF, 'X'); // transmit a character
    TEST_ASSERT_EQUAL_UINT(1u, tx_len);
    TEST_ASSERT_EQUAL_HEX8('X', tx_capture[0]);
    TEST_ASSERT_FALSE(cpu->tto_csr & DL11_DONE); // busy: DONE clear during transmit
    cpu->time_ns = cpu->tto_done_ns;             // a character-time elapses
    pdp11_console_tx_poll(cpu);
    TEST_ASSERT_TRUE(cpu->tto_csr & DL11_DONE);  // ready again
}

// A small RK backing store: 4 sectors (1024 words) is enough to exercise the
// DMA without allocating a whole 1.2M-word drive.
static uint16_t rk_disk[1024];

// Word count register value for an N-word transfer (two's-complement count).
static uint16_t rk_wc(unsigned n) { return (uint16_t)(0200000u - n); }

static void test_rk11_read_transfers_a_sector_from_disk_to_memory(void) {
    for (int i = 0; i < 256; ++i) {
        rk_disk[i] = (uint16_t)(0100000u + (unsigned)i); // pattern in sector 0
    }
    pdp11_rk_attach(cpu, rk_disk, 1024);
    pdp11_rk_write(cpu, RK_RKBA, 0010000u);      // memory address
    pdp11_rk_write(cpu, RK_RKDA, 0);             // disk block 0
    pdp11_rk_write(cpu, RK_RKWC, rk_wc(256));    // 256 words
    pdp11_rk_write(cpu, RK_RKCS, (2u << 1) | 1u); // FUNC=READ, GO
    cpu->time_ns = cpu->rk.done_ns;              // let the transfer complete
    pdp11_rk_poll(cpu);
    TEST_ASSERT_TRUE(cpu->rk.rkcs & 0000200u);   // DONE
    TEST_ASSERT_EQUAL_HEX16(0100000u, pdp11_mem_read_word(cpu->mem, 0010000u));
    TEST_ASSERT_EQUAL_HEX16(0100000u + 255u,
                            pdp11_mem_read_word(cpu->mem, 0010000u + 255u * 2u));
    TEST_ASSERT_EQUAL_HEX16(0u, cpu->rk.rkwc);   // word count exhausted
}

static void test_rk11_write_transfers_memory_to_disk(void) {
    for (int i = 0; i < 1024; ++i) {
        rk_disk[i] = 0;
    }
    pdp11_rk_attach(cpu, rk_disk, 1024);
    for (unsigned i = 0; i < 256; ++i) {
        pdp11_mem_write_word(cpu->mem, 0010000u + i * 2u, (uint16_t)(040000u + i));
    }
    pdp11_rk_write(cpu, RK_RKBA, 0010000u);
    pdp11_rk_write(cpu, RK_RKDA, 0);
    pdp11_rk_write(cpu, RK_RKWC, rk_wc(256));
    pdp11_rk_write(cpu, RK_RKCS, (1u << 1) | 1u); // FUNC=WRITE, GO
    cpu->time_ns = cpu->rk.done_ns;
    pdp11_rk_poll(cpu);
    TEST_ASSERT_EQUAL_HEX16(040000u, rk_disk[0]);
    TEST_ASSERT_EQUAL_HEX16(040000u + 255u, rk_disk[255]);
}

static void test_rk11_completion_interrupts_through_220_when_enabled(void) {
    pdp11_mem_write_word(cpu->mem, 0000220u, 0003000u); // RK vector -> handler
    pdp11_mem_write_word(cpu->mem, 0000222u, 0000340u);
    cpu->r[PDP11_SP] = 0004000u;
    pdp11_rk_attach(cpu, rk_disk, 1024);
    pdp11_rk_write(cpu, RK_RKBA, 0010000u);
    pdp11_rk_write(cpu, RK_RKDA, 0);
    pdp11_rk_write(cpu, RK_RKWC, rk_wc(256));
    pdp11_rk_write(cpu, RK_RKCS, (2u << 1) | 1u | 0000100u); // READ, GO, IE
    cpu->time_ns = cpu->rk.done_ns;
    pdp11_rk_poll(cpu);                          // completes -> requests BR5 int
    const uint16_t prog[] = {0010000u};          // MOV R0,R0 (runs absent an int)
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0003000u, cpu->r[PDP11_PC]); // vectored to the handler
}

// Granting a device interrupt acknowledges the bus request and drops it, so a
// still-asserted level (DONE & IE both set, handler yet to clear DONE) is taken
// exactly once — it does not re-vector on the next instruction. The vector PSW
// here keeps priority 0 so only the acknowledge, not priority masking, can stop
// a re-storm. Regression for the RK interrupt storm that stalled the V6 boot.
static void test_a_granted_device_interrupt_is_acknowledged_and_not_restormed(void) {
    pdp11_mem_write_word(cpu->mem, 0000220u, 0003000u); // RK vector -> handler
    pdp11_mem_write_word(cpu->mem, 0000222u, 0000000u); // vector PSW: priority 0
    pdp11_mem_write_word(cpu->mem, 0003000u, 0000240u); // handler instruction: NOP
    cpu->r[PDP11_SP] = 0004000u;
    pdp11_rk_attach(cpu, rk_disk, 1024);
    pdp11_rk_write(cpu, RK_RKBA, 0010000u);
    pdp11_rk_write(cpu, RK_RKDA, 0);
    pdp11_rk_write(cpu, RK_RKWC, rk_wc(256));
    pdp11_rk_write(cpu, RK_RKCS, (2u << 1) | 1u | 0000100u); // READ, GO, IE
    cpu->time_ns = cpu->rk.done_ns;
    pdp11_rk_poll(cpu);                                  // completes -> BR5 request
    pdp11_cpu_step(cpu);                                 // grant + acknowledge
    TEST_ASSERT_EQUAL_HEX16(0003000u, cpu->r[PDP11_PC]); // vectored once
    pdp11_cpu_step(cpu);                                 // handler NOP, no re-storm
    TEST_ASSERT_EQUAL_HEX16(0003002u, cpu->r[PDP11_PC]); // ran on, did not re-vector
}

static uint16_t rp_disk[1024];

static void test_rp04_read_transfers_a_sector_from_disk_to_memory(void) {
    for (int i = 0; i < 256; ++i) {
        rp_disk[i] = (uint16_t)(0140000u + (unsigned)i);
    }
    pdp11_rp_attach(cpu, rp_disk, 1024);
    pdp11_rp_write(cpu, 0176704u, 0010000u);  // RPBA
    pdp11_rp_write(cpu, 0176750u, 0);         // RPBAE
    pdp11_rp_write(cpu, 0176706u, 0);         // RPDA (sector 0, track 0)
    pdp11_rp_write(cpu, 0176734u, 0);         // RPDC (cylinder 0)
    pdp11_rp_write(cpu, 0176702u, rk_wc(256)); // RPWC
    pdp11_rp_write(cpu, 0176700u, (034u << 1) | 1u); // RPCS1: READ, GO
    cpu->time_ns = cpu->rp.done_ns;
    pdp11_rp_poll(cpu);
    TEST_ASSERT_TRUE(cpu->rp.cs1 & 0000200u); // ready
    TEST_ASSERT_EQUAL_HEX16(0140000u, pdp11_mem_read_word(cpu->mem, 0010000u));
    TEST_ASSERT_EQUAL_HEX16(0140000u + 255u,
                            pdp11_mem_read_word(cpu->mem, 0010000u + 255u * 2u));
}

static void test_rp04_write_transfers_memory_to_disk(void) {
    for (int i = 0; i < 1024; ++i) {
        rp_disk[i] = 0;
    }
    pdp11_rp_attach(cpu, rp_disk, 1024);
    for (unsigned i = 0; i < 256; ++i) {
        pdp11_mem_write_word(cpu->mem, 0010000u + i * 2u, (uint16_t)(050000u + i));
    }
    pdp11_rp_write(cpu, 0176704u, 0010000u);  // RPBA
    pdp11_rp_write(cpu, 0176750u, 0);         // RPBAE
    pdp11_rp_write(cpu, 0176706u, 0);         // RPDA
    pdp11_rp_write(cpu, 0176734u, 0);         // RPDC
    pdp11_rp_write(cpu, 0176702u, rk_wc(256)); // RPWC
    pdp11_rp_write(cpu, 0176700u, (030u << 1) | 1u); // RPCS1: WRITE, GO
    cpu->time_ns = cpu->rp.done_ns;
    pdp11_rp_poll(cpu);
    TEST_ASSERT_EQUAL_HEX16(050000u, rp_disk[0]);
    TEST_ASSERT_EQUAL_HEX16(050000u + 255u, rp_disk[255]);
}

static void test_rp04_completion_interrupts_through_254_when_enabled(void) {
    pdp11_mem_write_word(cpu->mem, 0000254u, 0003000u); // RP vector -> handler
    pdp11_mem_write_word(cpu->mem, 0000256u, 0000340u);
    cpu->r[PDP11_SP] = 0004000u;
    pdp11_rp_attach(cpu, rp_disk, 1024);
    pdp11_rp_write(cpu, 0176704u, 0010000u);
    pdp11_rp_write(cpu, 0176750u, 0);
    pdp11_rp_write(cpu, 0176706u, 0);
    pdp11_rp_write(cpu, 0176734u, 0);
    pdp11_rp_write(cpu, 0176702u, rk_wc(256));
    pdp11_rp_write(cpu, 0176700u, (034u << 1) | 1u | 0000100u); // READ, GO, IE
    cpu->time_ns = cpu->rp.done_ns;
    pdp11_rp_poll(cpu);
    const uint16_t prog[] = {0010000u};
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0003000u, cpu->r[PDP11_PC]); // vectored to handler
}

// A small SimH .tap image: one 4-byte record [11 22 33 44] then a file mark.
static uint8_t tm_tape[64];
static void tm_build_tape(void) {
    for (unsigned i = 0; i < sizeof tm_tape; ++i) {
        tm_tape[i] = 0;
    }
    tm_tape[0] = 4;                                  // record length
    tm_tape[4] = 0x11; tm_tape[5] = 0x22;
    tm_tape[6] = 0x33; tm_tape[7] = 0x44;            // data
    tm_tape[8] = 4;                                  // trailing length
    // bytes 12-15 stay zero: a file mark
}

static void test_tm11_read_record_transfers_to_memory(void) {
    tm_build_tape();
    pdp11_tm_attach(cpu, tm_tape, sizeof tm_tape, false);
    pdp11_tm_write(cpu, TM_MTCMA, 0010000u);
    pdp11_tm_write(cpu, TM_MTBRC, (uint16_t)(0200000u - 4u));
    pdp11_tm_write(cpu, TM_MTC, (01u << 1) | 1u); // READ, GO
    cpu->time_ns = cpu->tm.done_ns;
    pdp11_tm_poll(cpu);
    TEST_ASSERT_TRUE(cpu->tm.cmd & 0000200u); // DONE
    TEST_ASSERT_EQUAL_HEX16(0x2211u, pdp11_mem_read_word(cpu->mem, 0010000u));
    TEST_ASSERT_EQUAL_HEX16(0x4433u, pdp11_mem_read_word(cpu->mem, 0010002u));
    TEST_ASSERT_EQUAL_HEX16(0u, cpu->tm.bc); // count exhausted
}

static void test_tm11_write_record_stores_to_tape(void) {
    for (unsigned i = 0; i < sizeof tm_tape; ++i) {
        tm_tape[i] = 0;
    }
    pdp11_tm_attach(cpu, tm_tape, sizeof tm_tape, false);
    pdp11_mem_write_word(cpu->mem, 0010000u, 0x2211u);
    pdp11_mem_write_word(cpu->mem, 0010002u, 0x4433u);
    pdp11_tm_write(cpu, TM_MTCMA, 0010000u);
    pdp11_tm_write(cpu, TM_MTBRC, (uint16_t)(0200000u - 4u));
    pdp11_tm_write(cpu, TM_MTC, (02u << 1) | 1u); // WRITE, GO
    cpu->time_ns = cpu->tm.done_ns;
    pdp11_tm_poll(cpu);
    TEST_ASSERT_EQUAL_HEX8(4u, tm_tape[0]);    // record-length header
    TEST_ASSERT_EQUAL_HEX8(0x11u, tm_tape[4]);
    TEST_ASSERT_EQUAL_HEX8(0x44u, tm_tape[7]);
    TEST_ASSERT_EQUAL_HEX8(4u, tm_tape[8]);    // record-length trailer
}

static void test_tm11_reading_a_file_mark_sets_eof(void) {
    tm_build_tape();
    pdp11_tm_attach(cpu, tm_tape, sizeof tm_tape, false);
    cpu->tm.pos = 12; // positioned at the file mark
    pdp11_tm_write(cpu, TM_MTBRC, (uint16_t)(0200000u - 4u));
    pdp11_tm_write(cpu, TM_MTC, (01u << 1) | 1u); // READ, GO
    cpu->time_ns = cpu->tm.done_ns;
    pdp11_tm_poll(cpu);
    TEST_ASSERT_TRUE(cpu->tm.sta & 0040000u); // STA_EOF
}

static void test_tm11_completion_interrupts_through_224(void) {
    pdp11_mem_write_word(cpu->mem, 0000224u, 0003000u); // TM vector -> handler
    pdp11_mem_write_word(cpu->mem, 0000226u, 0000340u);
    cpu->r[PDP11_SP] = 0004000u;
    tm_build_tape();
    pdp11_tm_attach(cpu, tm_tape, sizeof tm_tape, false);
    pdp11_tm_write(cpu, TM_MTCMA, 0010000u);
    pdp11_tm_write(cpu, TM_MTBRC, (uint16_t)(0200000u - 4u));
    pdp11_tm_write(cpu, TM_MTC, (01u << 1) | 1u | 0000100u); // READ, GO, IE
    cpu->time_ns = cpu->tm.done_ns;
    pdp11_tm_poll(cpu);
    const uint16_t prog[] = {0010000u};
    deposit(001000, prog, 1);
    pdp11_cpu_step(cpu);
    TEST_ASSERT_EQUAL_HEX16(0003000u, cpu->r[PDP11_PC]); // vectored to handler
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
    RUN_TEST(test_sbc_of_the_most_negative_value_sets_v_only_with_carry_in);
    RUN_TEST(test_writing_the_psw_via_177776_keeps_the_written_condition_codes);
    RUN_TEST(test_spl_sets_the_processor_priority_in_kernel_mode);
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
    RUN_TEST(test_trap_pushes_psw_then_pc_and_vectors_through_034);
    RUN_TEST(test_rti_pops_pc_then_psw);
    RUN_TEST(test_mul_produces_a_32_bit_product_across_the_register_pair);
    RUN_TEST(test_div_by_zero_sets_v_and_c);
    RUN_TEST(test_ash_shifts_left_and_records_the_last_bit_shifted_out);
    RUN_TEST(test_xor_toggles_bits_and_leaves_carry_alone);
    RUN_TEST(test_the_psw_is_readable_at_its_io_page_address);
    RUN_TEST(test_a_word_write_to_an_odd_address_traps_through_vector_4);
    RUN_TEST(test_a_word_access_to_nonexistent_memory_traps_through_vector_4);
    RUN_TEST(test_an_11_70_illegal_instruction_traps_through_vector_10);
    RUN_TEST(test_a_pirq_above_the_cpu_priority_is_granted_through_240);
    RUN_TEST(test_a_pirq_at_or_below_the_cpu_priority_is_masked);
    RUN_TEST(test_wait_idles_until_an_interrupt_is_granted);
    RUN_TEST(test_mark_restores_pc_from_r5_and_cleans_the_stack);
    RUN_TEST(test_jmp_to_a_register_is_illegal_on_the_11_70);
    RUN_TEST(test_the_mmu_relocates_a_virtual_write_to_its_physical_page);
    RUN_TEST(test_a_write_to_a_read_only_page_aborts_through_vector_250);
    RUN_TEST(test_r0_r5_are_banked_by_the_psw_register_set_bit);
    RUN_TEST(test_mfpi_reads_the_previous_modes_space_and_pushes_it);
    RUN_TEST(test_data_references_use_d_space_when_it_is_enabled);
    RUN_TEST(test_branch_timing_is_600ns_taken_and_300ns_not_taken);
    RUN_TEST(test_ldfps_and_cfcc_move_the_fp_status_and_condition_codes);
    RUN_TEST(test_setd_and_setf_toggle_the_fps_double_bit);
    RUN_TEST(test_ldf_stf_round_trip_a_single_through_an_accumulator);
    RUN_TEST(test_negf_flips_the_sign_of_an_accumulator);
    RUN_TEST(test_addf_adds_two_single_floats);
    RUN_TEST(test_mulf_multiplies_two_single_floats);
    RUN_TEST(test_divf_divides_two_single_floats);
    RUN_TEST(test_divf_by_zero_traps_through_the_fpe_vector);
    RUN_TEST(test_cmpf_sets_n_when_the_source_is_below_the_accumulator);
    RUN_TEST(test_kw11l_powers_up_with_the_monitor_bit_set);
    RUN_TEST(test_kw11l_tick_with_interrupts_enabled_vectors_through_100);
    RUN_TEST(test_kw11l_tick_without_ie_sets_done_but_does_not_interrupt);
    RUN_TEST(test_kw11l_writing_zero_to_done_clears_the_monitor_bit);
    RUN_TEST(test_dl11_receiver_latches_input_and_reading_rbuf_clears_done);
    RUN_TEST(test_dl11_receiver_interrupt_vectors_through_060);
    RUN_TEST(test_dl11_transmitter_emits_to_the_sink_then_completes);
    RUN_TEST(test_rk11_read_transfers_a_sector_from_disk_to_memory);
    RUN_TEST(test_rk11_write_transfers_memory_to_disk);
    RUN_TEST(test_rk11_completion_interrupts_through_220_when_enabled);
    RUN_TEST(test_a_granted_device_interrupt_is_acknowledged_and_not_restormed);
    RUN_TEST(test_rp04_read_transfers_a_sector_from_disk_to_memory);
    RUN_TEST(test_rp04_write_transfers_memory_to_disk);
    RUN_TEST(test_rp04_completion_interrupts_through_254_when_enabled);
    RUN_TEST(test_tm11_read_record_transfers_to_memory);
    RUN_TEST(test_tm11_write_record_stores_to_tape);
    RUN_TEST(test_tm11_reading_a_file_mark_sets_eof);
    RUN_TEST(test_tm11_completion_interrupts_through_224);
    return UNITY_END();
}
