// KB11-C instruction-timing tests. Each checks a computed time against the
// PDP-11/70 Processor Handbook Appendix C tables (the timing oracle). Times are
// in nanoseconds (handbook microseconds x 1000).
#include "unity.h"

#include "timing/timing.h"

void setUp(void) {}
void tearDown(void) {}

// Double operand, register/register: SRC 0 + DST 0 + EF 0.30 = 0.30 us.
static void test_add_reg_reg_is_300ns(void) {
    TEST_ASSERT_EQUAL_UINT32(300, pdp11_instr_timing(0060001u).ns); // ADD R0,R1
}

// The handbook's worked example (ADD mode 6/6): by the tables SRC .60 + DST .60
// + EF 1.20 = 2.40 us. (The prose example says 2.55 us / EF 1.35 — an errata in
// the manual; we implement the tables. See FINDINGS.)
static void test_add_mode6_mode6_is_2400ns_per_the_tables(void) {
    pdp11_timing t = pdp11_instr_timing(0066061u); // ADD X(R0), Y(R1)
    TEST_ASSERT_EQUAL_UINT32(2400, t.ns);
    TEST_ASSERT_EQUAL_UINT32(5, t.read_cycles); // 2 + 2 + 1
}

// MOV register/register uses SRC + its own EF (0.30), no DST Time.
static void test_mov_reg_reg_is_300ns(void) {
    TEST_ASSERT_EQUAL_UINT32(300, pdp11_instr_timing(0010001u).ns); // MOV R0,R1
}

// MOV to the PC (DST R7) takes the longer PC row (0.60 us).
static void test_mov_to_pc_is_600ns(void) {
    TEST_ASSERT_EQUAL_UINT32(600, pdp11_instr_timing(0010007u).ns); // MOV R0,R7
}

// Single operand to a register: DST 0 + EF 0.30.
static void test_clr_reg_is_300ns(void) {
    TEST_ASSERT_EQUAL_UINT32(300, pdp11_instr_timing(0005000u).ns); // CLR R0
}

// Single operand to memory (mode 1): DST 0.30 + EF 1.20 = 1.50 us.
static void test_clr_deferred_is_1500ns(void) {
    TEST_ASSERT_EQUAL_UINT32(1500, pdp11_instr_timing(0005010u).ns); // CLR (R0)
}

// NEG and TST have their own single-operand EF times.
static void test_neg_reg_is_750ns(void) {
    TEST_ASSERT_EQUAL_UINT32(750, pdp11_instr_timing(0005400u).ns); // NEG R0
}

static void test_tst_reg_is_300ns(void) {
    TEST_ASSERT_EQUAL_UINT32(300, pdp11_instr_timing(0005700u).ns); // TST R0
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_reg_reg_is_300ns);
    RUN_TEST(test_add_mode6_mode6_is_2400ns_per_the_tables);
    RUN_TEST(test_mov_reg_reg_is_300ns);
    RUN_TEST(test_mov_to_pc_is_600ns);
    RUN_TEST(test_clr_reg_is_300ns);
    RUN_TEST(test_clr_deferred_is_1500ns);
    RUN_TEST(test_neg_reg_is_750ns);
    RUN_TEST(test_tst_reg_is_300ns);
    return UNITY_END();
}
