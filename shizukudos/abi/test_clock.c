/* SPDX-License-Identifier: GPL-2.0-only -- portable Core clock controls. */
#include "shz_clock.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %u: %s\n", (unsigned)__LINE__, #x); exit(1); \
} } while (0)
static unsigned checks;
__extension__ typedef unsigned __int128 wide_t;

/* The runner extracts clock_sample and the HC15 case from actual domain.c.
 * Only the platform TSC read and domain registers are modeled here. */
static uint64_t g_tsc_hz, g_start_tsc, host_tsc;
static unsigned tsc_reads;
static uint64_t rdtsc(void) { ++tsc_reads; return host_tsc; }
enum { GPR_RAX, GPR_RBX, GPR_RCX };
#include "domain_clock.inc"

static void numeric(uint64_t ticks, uint64_t hz)
{
    uint64_t value = UINT64_C(0xfedcba9876543210);
    const uint64_t sentinel = value;
    int32_t expected = SHZ_OK;
    wide_t oracle = 0;
    if (!hz) expected = SHZ_E_INVALID;
    else if (hz > UINT64_MAX / SHZ_CLOCK_FREQUENCY) expected = SHZ_E_RANGE;
    else {
        /* Wide multiplication is an independent oracle, unlike production's
         * quotient/remainder arithmetic that must fit plain uint64_t. */
        oracle = (wide_t)ticks * SHZ_CLOCK_FREQUENCY / hz;
        if (oracle > INT64_MAX) expected = SHZ_E_RANGE;
    }
    CHECK(shz_clock_ticks_ns(ticks, hz, &value) == expected);
    CHECK(value == (expected == SHZ_OK ? (uint64_t)oracle : sentinel));
}

static void test_numeric(void)
{
    static const uint64_t rates[] = {0, 1, 3, 10, UINT64_C(1000000),
        SHZ_CLOCK_FREQUENCY, UINT64_C(3000000000),
        UINT64_MAX / SHZ_CLOCK_FREQUENCY,
        UINT64_MAX / SHZ_CLOCK_FREQUENCY + 1, UINT64_MAX};
    static const uint64_t ticks[] = {0, 1, 2, 15, UINT32_MAX,
        UINT64_C(0x100000001), (uint64_t)INT64_MAX,
        (uint64_t)INT64_MAX + 1, UINT64_MAX};
    uint64_t seed = UINT64_C(0xabc098123456789);
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; ++i)
        for (unsigned j = 0; j < sizeof ticks / sizeof ticks[0]; ++j)
            numeric(ticks[j], rates[i]);
    for (unsigned i = 0; i < 10000; ++i) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        numeric(seed, rates[i % (sizeof rates / sizeof rates[0])]);
    }
    CHECK(shz_clock_ticks_ns(1, 3, NULL) == SHZ_E_INVALID);
    CHECK(SHZ_CLOCK_FREQUENCY == UINT64_C(1000000000));
    CHECK(sizeof(shz_clock_reply_t) == 32 && SHZ_CLOCK_VERSION == 1);
    CHECK(SHZ_HC_TIME == 6 && SHZ_HC_CHANNEL_INFO == 13 && SHZ_HC_CLOCK_SPLIT == 15);
}

struct sample_state { uint64_t value; int32_t status; unsigned calls; };
static int32_t sample(void *opaque, uint64_t *value)
{
    struct sample_state *state = opaque;
    ++state->calls;
    *value = state->value;
    return state->status;
}

static void test_split(void)
{
    struct sample_state s = {UINT64_C(0x12345678fedcba98), SHZ_OK, 0};
    uint64_t low = 12, high = 34;
    CHECK(shz_clock_split(0, 0, sample, &s, &low, &high) == SHZ_E_UNSUPPORTED);
    CHECK(shz_clock_split(SHZ_CLOCK_VERSION + 1u, 0, sample, &s, &low, &high) == SHZ_E_UNSUPPORTED);
    CHECK(shz_clock_split(SHZ_CLOCK_VERSION, 1, sample, &s, &low, &high) == SHZ_E_INVALID);
    CHECK(shz_clock_split(SHZ_CLOCK_VERSION, 0, NULL, &s, &low, &high) == SHZ_E_INVALID);
    CHECK(shz_clock_split(SHZ_CLOCK_VERSION, 0, sample, &s, NULL, &high) == SHZ_E_INVALID);
    CHECK(shz_clock_split(SHZ_CLOCK_VERSION, 0, sample, &s, &low, NULL) == SHZ_E_INVALID);
    CHECK(shz_clock_split(SHZ_CLOCK_VERSION, 0, sample, &s, &low, &low) == SHZ_E_INVALID);
    CHECK(s.calls == 0 && low == 12 && high == 34);
    CHECK(shz_clock_split(SHZ_CLOCK_VERSION, 0, sample, &s, &low, &high) == SHZ_OK);
    CHECK(s.calls == 1 && low == UINT32_C(0xfedcba98) && high == UINT32_C(0x12345678));
    low = 12; high = 34; s.status = SHZ_E_TIMEOUT;
    CHECK(shz_clock_split(SHZ_CLOCK_VERSION, 0, sample, &s, &low, &high) == SHZ_E_TIMEOUT);
    CHECK(s.calls == 2 && low == 12 && high == 34);
    s.status = SHZ_OK; s.value = (uint64_t)INT64_MAX + 1;
    CHECK(shz_clock_split(SHZ_CLOCK_VERSION, 0, sample, &s, &low, &high) == SHZ_E_RANGE);
    CHECK(s.calls == 3 && low == 12 && high == 34);
    s.value = (uint64_t)INT64_MAX;
    CHECK(shz_clock_split(SHZ_CLOCK_VERSION, 0, sample, &s, &low, &high) == SHZ_OK);
    CHECK(s.calls == 4 && low == UINT32_MAX && high == INT32_MAX);
}

static uint64_t dispatch_ok(uint64_t tsc)
{
    uint64_t r[] = {SHZ_HC_CLOCK_SPLIT, SHZ_CLOCK_VERSION, 0};
    host_tsc = tsc; tsc_reads = 0;
    CHECK(core_clock_dispatch(r) == SHZ_OK);
    CHECK(tsc_reads == 1);
    CHECK(r[GPR_RBX] <= UINT32_MAX && r[GPR_RCX] <= INT32_MAX);
    return (r[GPR_RCX] << 32) | r[GPR_RBX];
}

static void test_domain(void)
{
    uint64_t r[] = {SHZ_HC_CLOCK_SPLIT, SHZ_CLOCK_VERSION, 0};
    uint64_t last;
    g_tsc_hz = SHZ_CLOCK_FREQUENCY; g_start_tsc = 29;
    last = dispatch_ok(g_start_tsc + UINT32_MAX);
    CHECK(last == UINT32_MAX);
    CHECK(dispatch_ok(g_start_tsc + UINT64_C(0x100000000)) == UINT64_C(0x100000000));
    CHECK(dispatch_ok(g_start_tsc + UINT64_C(0x100000001)) == UINT64_C(0x100000001));
    last = 0;
    for (uint64_t i = 0; i < 1000; ++i) {
        uint64_t now = dispatch_ok(g_start_tsc + i * UINT64_C(4311123));
        CHECK(now >= last); last = now;
    }
    /* A wrap of the unsigned raw TSC origin subtraction remains elapsed time. */
    g_start_tsc = UINT64_MAX - 5;
    CHECK(dispatch_ok(4) == 10);
    g_tsc_hz = 3; g_start_tsc = 100;
    CHECK(dispatch_ok(101) == UINT64_C(333333333));
    CHECK(dispatch_ok(103) == SHZ_CLOCK_FREQUENCY);
    tsc_reads = 0; r[GPR_RBX] = SHZ_CLOCK_VERSION + 1u;
    CHECK(core_clock_dispatch(r) == SHZ_E_UNSUPPORTED);
    CHECK(tsc_reads == 0 && r[GPR_RBX] == SHZ_CLOCK_VERSION + 1u && r[GPR_RCX] == 0);
    r[GPR_RBX] = SHZ_CLOCK_VERSION; r[GPR_RCX] = 1;
    CHECK(core_clock_dispatch(r) == SHZ_E_INVALID);
    CHECK(tsc_reads == 0 && r[GPR_RBX] == SHZ_CLOCK_VERSION && r[GPR_RCX] == 1);
    r[GPR_RCX] = 0; g_tsc_hz = 0;
    CHECK(core_clock_dispatch(r) == SHZ_E_INVALID);
    CHECK(tsc_reads == 0 && r[GPR_RBX] == SHZ_CLOCK_VERSION && r[GPR_RCX] == 0);
    g_tsc_hz = SHZ_CLOCK_FREQUENCY; g_start_tsc = 0;
    host_tsc = (uint64_t)INT64_MAX + 1;
    CHECK(core_clock_dispatch(r) == SHZ_E_RANGE);
    CHECK(tsc_reads == 1 && r[GPR_RBX] == SHZ_CLOCK_VERSION && r[GPR_RCX] == 0);
    tsc_reads = 0; g_tsc_hz = UINT64_MAX;
    CHECK(core_clock_dispatch(r) == SHZ_E_RANGE);
    CHECK(tsc_reads == 1 && r[GPR_RBX] == SHZ_CLOCK_VERSION && r[GPR_RCX] == 0);
    r[GPR_RAX] = 14; tsc_reads = 0;
    CHECK(core_clock_dispatch(r) == SHZ_E_UNSUPPORTED && tsc_reads == 0);
}

int main(void)
{
    test_numeric(); test_split(); test_domain();
    printf("PASS %u Core clock checks (actual header/sample/case; modeled TSC)\n", checks);
    return 0;
}
