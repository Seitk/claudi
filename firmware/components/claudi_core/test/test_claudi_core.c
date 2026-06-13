// Host unit test for claudi_core — compiles and runs with plain cc, no ESP-IDF.
//   cd firmware/components/claudi_core/test && make
//
// Exercises the state ladder (mirroring the hook's self-test sequence), overlay
// activation/expiry, staleness, transcript ring buffer, and name round-trips.
#include "claudi_core.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                               \
    do {                                                              \
        g_checks++;                                                  \
        if (!(cond)) {                                               \
            g_failures++;                                            \
            printf("  [FAIL] " __VA_ARGS__);                         \
            printf("   (at %s:%d)\n", __FILE__, __LINE__);           \
        }                                                            \
    } while (0)

#define CHECK_STATE(got, want)                                        \
    do {                                                              \
        claudi_state_t _g = (got), _w = (want);                     \
        CHECK(_g == _w, "expected %s, got %s\n",                     \
              claudi_state_name(_w), claudi_state_name(_g));         \
    } while (0)

// Helper: derive at a given "now" by setting updated=0 and now=elapsed.
static claudi_derived_t derive_after(claudi_snapshot_t *s, uint32_t elapsed_ms)
{
    s->updated_ms = 0;
    return claudi_derive(s, elapsed_ms);
}

static void test_ladder(void)
{
    printf("ladder:\n");
    claudi_snapshot_t s;

    // idle: fresh, nothing happening
    claudi_snapshot_init(&s);
    CHECK_STATE(derive_after(&s, 0).base, CLAUDI_STATE_IDLE);

    // working: a tool in flight
    claudi_snapshot_init(&s);
    s.running = 2;
    CHECK_STATE(derive_after(&s, 100).base, CLAUDI_STATE_WORKING);

    // attention via waiting flag
    claudi_snapshot_init(&s);
    s.waiting = true;
    CHECK_STATE(derive_after(&s, 100).base, CLAUDI_STATE_ATTENTION);

    // attention via pending prompt, and it OUTRANKS a running tool
    claudi_snapshot_init(&s);
    s.running = 1;
    s.prompt.set = true;
    CHECK_STATE(derive_after(&s, 100).base, CLAUDI_STATE_ATTENTION);

    // a running tool OUTRANKS staleness (long builds stay "working")
    claudi_snapshot_init(&s);
    s.running = 1;
    CHECK_STATE(derive_after(&s, CLAUDI_STALE_MS + 5000).base, CLAUDI_STATE_WORKING);
}

static void test_staleness(void)
{
    printf("staleness:\n");
    claudi_snapshot_t s;
    claudi_snapshot_init(&s);  // idle and quiet

    claudi_derived_t fresh = derive_after(&s, CLAUDI_STALE_MS - 1);
    CHECK(!fresh.stale, "should not be stale just under the threshold\n");
    CHECK_STATE(fresh.base, CLAUDI_STATE_IDLE);

    claudi_derived_t stale = derive_after(&s, CLAUDI_STALE_MS + 1);
    CHECK(stale.stale, "should be stale just over the threshold\n");
    CHECK_STATE(stale.base, CLAUDI_STATE_SLEEPY);
}

static void test_overlay(void)
{
    printf("overlay:\n");
    claudi_snapshot_t s;
    claudi_snapshot_init(&s);
    s.running = 1;                  // base would be working
    s.overlay = CLAUDI_STATE_IDEA;
    s.overlay_ms = 2500;

    claudi_derived_t during = derive_after(&s, 1000);
    CHECK(during.overlay_active, "overlay should be active within window\n");
    CHECK_STATE(during.effective, CLAUDI_STATE_IDEA);
    CHECK_STATE(during.base, CLAUDI_STATE_WORKING);

    claudi_derived_t after = derive_after(&s, 3000);
    CHECK(!after.overlay_active, "overlay should expire after overlay_ms\n");
    CHECK_STATE(after.effective, CLAUDI_STATE_WORKING);  // reverts to base

    // overlay == none sentinel never activates
    claudi_snapshot_init(&s);
    s.overlay = CLAUDI_STATE_COUNT;
    s.overlay_ms = 5000;
    CHECK(!derive_after(&s, 10).overlay_active, "none overlay must not activate\n");
}

static void test_entries(void)
{
    printf("entries:\n");
    claudi_snapshot_t s;
    claudi_snapshot_init(&s);

    claudi_snapshot_add_entry(&s, "you: build it");
    claudi_snapshot_add_entry(&s, "> Read");
    CHECK(s.n_entries == 2, "expected 2 entries, got %zu\n", s.n_entries);
    CHECK(strcmp(s.entries[0], "you: build it") == 0, "oldest entry wrong: %s\n", s.entries[0]);

    // overflow keeps only the newest CLAUDI_MAX_ENTRIES, newest last
    for (int i = 0; i < 10; ++i) {
        char buf[16];
        snprintf(buf, sizeof(buf), "e%d", i);
        claudi_snapshot_add_entry(&s, buf);
    }
    CHECK(s.n_entries == CLAUDI_MAX_ENTRIES, "ring should cap at %d, got %zu\n",
          CLAUDI_MAX_ENTRIES, s.n_entries);
    CHECK(strcmp(s.entries[CLAUDI_MAX_ENTRIES - 1], "e9") == 0,
          "newest should be e9, got %s\n", s.entries[CLAUDI_MAX_ENTRIES - 1]);

    // truncation to CLAUDI_ENTRY_LEN-1 chars
    claudi_snapshot_init(&s);
    char longstr[200];
    memset(longstr, 'x', sizeof(longstr));
    longstr[sizeof(longstr) - 1] = '\0';
    claudi_snapshot_add_entry(&s, longstr);
    CHECK(strlen(s.entries[0]) == CLAUDI_ENTRY_LEN - 1,
          "entry should truncate to %d, got %zu\n",
          CLAUDI_ENTRY_LEN - 1, strlen(s.entries[0]));

    // empty / NULL ignored
    claudi_snapshot_add_entry(&s, "");
    claudi_snapshot_add_entry(&s, NULL);
    CHECK(s.n_entries == 1, "empty/NULL entries must be ignored\n");
}

static void test_names(void)
{
    printf("names:\n");
    for (int i = 0; i < CLAUDI_STATE_COUNT; ++i) {
        const char *name = claudi_state_name((claudi_state_t)i);
        claudi_state_t back;
        CHECK(claudi_state_from_name(name, &back), "name %s should parse\n", name);
        CHECK_STATE(back, (claudi_state_t)i);
    }
    claudi_state_t out = CLAUDI_STATE_IDLE;
    CHECK(!claudi_state_from_name("bogus", &out), "unknown name must fail\n");
    CHECK(!claudi_state_from_name(NULL, &out), "NULL name must fail\n");
    CHECK(strcmp(claudi_state_name(CLAUDI_STATE_COUNT), "idle") == 0,
          "out-of-range name should default to idle\n");
}

int main(void)
{
    printf("claudi_core host test\n\n");
    test_ladder();
    test_staleness();
    test_overlay();
    test_entries();
    test_names();
    printf("\n%d checks, %d failures: %s\n", g_checks, g_failures,
           g_failures == 0 ? "PASS" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
