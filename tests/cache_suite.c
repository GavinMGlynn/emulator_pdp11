// PDP-11/70 cache model tests (KB11-C sec. 2.2): two-way set-associative,
// 256 sets, 2-word (4-byte) blocks, write-through, round-robin victim.
#include "unity.h"

#include "cache/cache.h"

static pdp11_cache c;

void setUp(void) { pdp11_cache_reset(&c); }
void tearDown(void) {}

static void test_a_cold_read_misses_then_the_same_block_hits(void) {
    TEST_ASSERT_FALSE(pdp11_cache_read(&c, 0));  // cold -> miss
    TEST_ASSERT_TRUE(pdp11_cache_read(&c, 0));   // now resident -> hit
    TEST_ASSERT_EQUAL_UINT64(1, c.misses);
}

static void test_both_words_of_a_block_share_a_line(void) {
    TEST_ASSERT_FALSE(pdp11_cache_read(&c, 0)); // byte 0, word 0 -> miss
    TEST_ASSERT_TRUE(pdp11_cache_read(&c, 2));  // byte 2 = second word, same block
    TEST_ASSERT_EQUAL_UINT64(1, c.misses);
}

static void test_two_tags_in_a_set_both_fit_the_two_ways(void) {
    // Same set (index bits 9-2), different tags (bits 21-10): 0 and 020000.
    TEST_ASSERT_FALSE(pdp11_cache_read(&c, 0));
    TEST_ASSERT_FALSE(pdp11_cache_read(&c, 020000u));
    TEST_ASSERT_TRUE(pdp11_cache_read(&c, 0));       // still resident (way 0)
    TEST_ASSERT_TRUE(pdp11_cache_read(&c, 020000u)); // still resident (way 1)
    TEST_ASSERT_EQUAL_UINT64(2, c.misses);
}

static void test_a_third_tag_in_a_set_evicts_a_way(void) {
    pdp11_cache_read(&c, 0);        // way 0
    pdp11_cache_read(&c, 020000u);  // way 1
    pdp11_cache_read(&c, 040000u);  // third tag, same set -> evicts way 0 (RR)
    TEST_ASSERT_FALSE(pdp11_cache_read(&c, 0)); // way 0 was evicted -> miss
    TEST_ASSERT_EQUAL_UINT64(4, c.misses);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_cold_read_misses_then_the_same_block_hits);
    RUN_TEST(test_both_words_of_a_block_share_a_line);
    RUN_TEST(test_two_tags_in_a_set_both_fit_the_two_ways);
    RUN_TEST(test_a_third_tag_in_a_set_evicts_a_way);
    return UNITY_END();
}
