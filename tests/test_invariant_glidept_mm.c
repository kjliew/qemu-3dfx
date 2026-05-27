#include <check.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

/*
 * Security property: Vertex count parameters used in memory allocation
 * must be validated before use. Specifically, the expression (v + 1) * SIZE_GRVERTEX
 * must not overflow and must not result in an allocation that is too large
 * or too small for the intended use.
 *
 * This test simulates the validation logic that MUST exist before any
 * allocation of the form: g_malloc((v + 1) * SIZE_GRVERTEX)
 */

#define SIZE_GRVERTEX 156  /* typical size of GrVertex structure */
#define MAX_SAFE_VERTEX_COUNT 65536  /* reasonable upper bound for vertex count */
#define MAX_SAFE_ALLOC_SIZE (SIZE_MAX / 2)  /* half of SIZE_MAX to avoid overflow */

/*
 * This function represents the validation that MUST be performed before
 * allocating memory based on a guest-supplied vertex count.
 * Returns 1 if the vertex count is safe to use, 0 otherwise.
 */
static int is_safe_vertex_count(uint64_t v) {
    /* Check for integer overflow in (v + 1) */
    if (v == UINT64_MAX) {
        return 0;
    }
    uint64_t v_plus_one = v + 1;

    /* Check for multiplication overflow */
    if (v_plus_one > MAX_SAFE_ALLOC_SIZE / SIZE_GRVERTEX) {
        return 0;
    }

    /* Check against reasonable maximum */
    if (v > MAX_SAFE_VERTEX_COUNT) {
        return 0;
    }

    return 1;
}

/*
 * Simulate the allocation that would occur in glidept_mm.c
 * Returns allocated pointer on success, NULL on failure or invalid input.
 * This represents what the code SHOULD do.
 */
static void *safe_vtx_cache_alloc(uint64_t v) {
    /* INVARIANT: Must validate before allocating */
    if (!is_safe_vertex_count(v)) {
        return NULL;
    }

    size_t alloc_size = (size_t)(v + 1) * SIZE_GRVERTEX;

    /* Additional sanity check */
    if (alloc_size == 0) {
        return NULL;
    }

    void *ptr = malloc(alloc_size);
    return ptr;
}

START_TEST(test_vertex_count_allocation_safety)
{
    /* Invariant: Adversarial vertex counts must never cause unsafe allocations.
     * The allocation (v+1)*SIZE_GRVERTEX must be validated to prevent:
     * 1. Integer overflow in (v+1)
     * 2. Integer overflow in multiplication
     * 3. Excessively large allocations
     * 4. Zero-size allocations used maliciously
     */

    /* Adversarial vertex count values as strings (parsed to uint64_t) */
    struct {
        uint64_t value;
        const char *description;
        int should_be_safe;
    } payloads[] = {
        /* Overflow cases - MUST be rejected */
        { UINT64_MAX,           "UINT64_MAX",                    0 },
        { UINT64_MAX - 1,       "UINT64_MAX - 1",                0 },
        { UINT32_MAX,           "UINT32_MAX",                    0 },
        { UINT32_MAX - 1,       "UINT32_MAX - 1",                0 },
        { 0xFFFFFFFF00000000ULL,"high bits set",                 0 },
        { 0x8000000000000000ULL,"MSB set",                       0 },
        { 0x7FFFFFFFFFFFFFFFULL,"INT64_MAX",                     0 },
        { SIZE_MAX,             "SIZE_MAX",                      0 },
        { SIZE_MAX / SIZE_GRVERTEX + 1, "overflow boundary+1",  0 },
        { SIZE_MAX / SIZE_GRVERTEX,     "overflow boundary",     0 },
        { 0x100000000ULL,       "2^32",                          0 },
        { 0x10000000ULL,        "large count 268M",              0 },
        { 0x1000000ULL,         "large count 16M",               0 },
        { MAX_SAFE_VERTEX_COUNT + 1, "just over max safe",       0 },
        /* Valid cases - MUST be accepted */
        { 0,                    "zero vertices",                 1 },
        { 1,                    "one vertex",                    1 },
        { 100,                  "100 vertices",                  1 },
        { 1000,                 "1000 vertices",                 1 },
        { MAX_SAFE_VERTEX_COUNT,"max safe vertex count",         1 },
    };

    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        uint64_t v = payloads[i].value;
        int expected_safe = payloads[i].should_be_safe;

        /* Test the safety check itself */
        int actually_safe = is_safe_vertex_count(v);

        if (expected_safe) {
            /* INVARIANT: Valid inputs must be accepted */
            ck_assert_msg(actually_safe == 1,
                "SECURITY VIOLATION: Valid vertex count %llu ('%s') was incorrectly rejected",
                (unsigned long long)v, payloads[i].description);

            /* INVARIANT: Safe allocation must succeed and return non-NULL */
            void *ptr = safe_vtx_cache_alloc(v);
            ck_assert_msg(ptr != NULL,
                "SECURITY VIOLATION: Allocation failed for valid vertex count %llu ('%s')",
                (unsigned long long)v, payloads[i].description);
            free(ptr);
        } else {
            /* INVARIANT: Adversarial inputs must be rejected */
            ck_assert_msg(actually_safe == 0,
                "SECURITY VIOLATION: Dangerous vertex count %llu ('%s') was not rejected by validation",
                (unsigned long long)v, payloads[i].description);

            /* INVARIANT: Unsafe allocation must return NULL (not crash or corrupt) */
            void *ptr = safe_vtx_cache_alloc(v);
            ck_assert_msg(ptr == NULL,
                "SECURITY VIOLATION: Allocation was performed for dangerous vertex count %llu ('%s')",
                (unsigned long long)v, payloads[i].description);
        }
    }
}
END_TEST

START_TEST(test_multiplication_overflow_detection)
{
    /* Invariant: The product (v+1) * SIZE_GRVERTEX must never overflow size_t */

    uint64_t overflow_candidates[] = {
        UINT64_MAX,
        UINT64_MAX - 1,
        UINT64_MAX / SIZE_GRVERTEX,
        UINT64_MAX / SIZE_GRVERTEX + 1,
        SIZE_MAX,
        SIZE_MAX / SIZE_GRVERTEX,
        SIZE_MAX / SIZE_GRVERTEX + 1,
        (uint64_t)UINT32_MAX,
        (uint64_t)UINT32_MAX * 2,
        0xDEADBEEFDEADBEEFULL,
        0xCAFEBABECAFEBABEULL,
    };

    int num_candidates = sizeof(overflow_candidates) / sizeof(overflow_candidates[0]);

    for (int i = 0; i < num_candidates; i++) {
        uint64_t v = overflow_candidates[i];

        /* INVARIANT: Overflow must be detected before allocation */
        int safe = is_safe_vertex_count(v);

        if (!safe) {
            /* Verify that if we were to compute (v+1)*SIZE_GRVERTEX it would overflow */
            /* This confirms our detection is correct */
            void *ptr = safe_vtx_cache_alloc(v);
            ck_assert_msg(ptr == NULL,
                "SECURITY VIOLATION: Overflow-inducing vertex count %llu resulted in allocation",
                (unsigned long long)v);
        }
        /* If safe==1, the value is within bounds and that's fine */
    }
}
END_TEST

START_TEST(test_boundary_vertex_counts)
{
    /* Invariant: Boundary values around the safe limit must be handled correctly */

    /* Values just at and around the safe boundary */
    uint64_t boundary_values[] = {
        MAX_SAFE_VERTEX_COUNT - 1,
        MAX_SAFE_VERTEX_COUNT,
        MAX_SAFE_VERTEX_COUNT + 1,
        MAX_SAFE_VERTEX_COUNT + 2,
        MAX_SAFE_VERTEX_COUNT * 2,
    };

    int num_values = sizeof(boundary_values) / sizeof(boundary_values[0]);

    for (int i = 0; i < num_values; i++) {
        uint64_t v = boundary_values[i];
        void *ptr = safe_vtx_cache_alloc(v);

        if (v <= MAX_SAFE_VERTEX_COUNT) {
            /* INVARIANT: Values within safe range must allocate successfully */
            ck_assert_msg(ptr != NULL,
                "SECURITY VIOLATION: Safe boundary vertex count %llu failed to allocate",
                (unsigned long long)v);
            free(ptr);
        } else {
            /* INVARIANT: Values beyond safe range must be rejected */
            ck_assert_msg(ptr == NULL,
                "SECURITY VIOLATION: Unsafe boundary vertex count %llu was allocated",
                (unsigned long long)v);
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security_3dfx_VertexAllocation");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_vertex_count_allocation_safety);
    tcase_add_test(tc_core, test_multiplication_overflow_detection);
    tcase_add_test(tc_core, test_boundary_vertex_counts);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}