/*
 * Timer wake-list benchmark. This deliberately calls picoquic's real wake-list
 * API rather than duplicating its data structures. It is an out-of-tree tool:
 * see timer_bench.md for the link command.
 */
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "picoquic_internal.h"

typedef struct st_fixture_t {
    picoquic_quic_t quic;
    picoquic_cnx_t* cnx;
    size_t count;
    uint64_t now;
} fixture_t;

/* quicctx keeps these wake-tree callbacks file-local. The fixture needs the
 * same node representation so the public wake API can operate on minimum
 * objects without bringing up TLS, sockets, or full connection state. */
#ifndef PICOQUIC_TIMER_HEAP
static int64_t wake_compare(void* left, void* right)
{
    uint64_t ltime = ((picoquic_cnx_t*)left)->next_wake_time;
    uint64_t rtime = ((picoquic_cnx_t*)right)->next_wake_time;
    return ltime < rtime ? -1 : (ltime > rtime ? 1 : 0);
}

static picosplay_node_t* wake_create(void* value)
{
    return &((picoquic_cnx_t*)value)->cnx_wake_node;
}

static void wake_delete(void* tree, picosplay_node_t* node)
{
    (void)tree;
    memset(node, 0, sizeof(*node));
}

static void* wake_value(picosplay_node_t* node)
{
    return node == NULL ? NULL : (void*)((char*)node - offsetof(picoquic_cnx_t, cnx_wake_node));
}

#endif

static uint64_t rng_next(uint64_t* state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static uint64_t wall_ns(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t timeval_ns(const struct timeval* tv)
{
    return (uint64_t)tv->tv_sec * UINT64_C(1000000000) + (uint64_t)tv->tv_usec * UINT64_C(1000);
}

static uint64_t cpu_ns(void)
{
    struct rusage usage;
    (void)getrusage(RUSAGE_SELF, &usage);
    return timeval_ns(&usage.ru_utime) + timeval_ns(&usage.ru_stime);
}

static int fixture_init(fixture_t* fixture, size_t count, uint64_t now)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->cnx = (picoquic_cnx_t*)calloc(count, sizeof(*fixture->cnx));
    if (fixture->cnx == NULL) {
        return -1;
    }
    fixture->count = count;
    fixture->now = now;
    fixture->quic.p_simulated_time = &fixture->now;
#ifdef PICOQUIC_TIMER_HEAP
    fixture->quic.cnx_wake_heap = calloc(count, sizeof(*fixture->quic.cnx_wake_heap));
    if (fixture->quic.cnx_wake_heap == NULL) { free(fixture->cnx); return -1; }
    fixture->quic.cnx_wake_heap_capacity = count;
    fixture->quic.cnx_wake_heap_next_sequence = 1;
#else
    picosplay_init_tree(&fixture->quic.cnx_wake_tree, wake_compare, wake_create, wake_delete, wake_value);
#endif
    for (size_t i = 0; i < count; i++) {
        fixture->cnx[i].quic = &fixture->quic;
    }
    return 0;
}

static void fixture_clear(fixture_t* fixture)
{
    if (fixture->cnx != NULL) {
        /* Move every ready entry out before deleting tree nodes and freeing the backing array. */
        for (size_t i = 0; i < fixture->count; i++) {
            picoquic_reinsert_by_wake_time(&fixture->quic, &fixture->cnx[i], UINT64_MAX);
        }
#ifdef PICOQUIC_TIMER_HEAP
        free(fixture->quic.cnx_wake_heap);
#else
        picosplay_empty_tree(&fixture->quic.cnx_wake_tree);
#endif
        free(fixture->cnx);
    }
    memset(fixture, 0, sizeof(*fixture));
}

static void schedule_spread(fixture_t* fixture, uint64_t seed, int randomized)
{
    for (size_t i = 0; i < fixture->count; i++) {
        uint64_t offset = 100 + (uint64_t)i * 37;
        if (randomized) {
            offset = 100 + rng_next(&seed) % (fixture->count * 53 + 1);
        }
        picoquic_reinsert_by_wake_time(&fixture->quic, &fixture->cnx[i], fixture->now + offset);
    }
}

static uint64_t query_wake(fixture_t* fixture, uint64_t max_wake_time)
{
    picoquic_cnx_t* cnx = picoquic_get_earliest_cnx_to_wake(&fixture->quic, max_wake_time);
    uint64_t next = picoquic_get_next_wake_time(&fixture->quic, fixture->now);
    return (cnx == NULL ? UINT64_C(0x9e3779b97f4a7c15) : (uint64_t)(cnx - fixture->cnx) + 1) ^ next;
}

static void prepare_workload(fixture_t* fixture, const char* workload)
{
    if (strncmp(workload, "ready-", 6) == 0) {
        size_t ready_count = strcmp(workload, "ready-only") == 0 ? fixture->count : (fixture->count < 8 ? fixture->count : 8);
        for (size_t i = 0; i < ready_count; i++) {
            picoquic_reinsert_by_wake_time(&fixture->quic, &fixture->cnx[i], fixture->now);
        }
    }
}

static uint64_t run_workload(fixture_t* fixture, const char* workload, size_t iterations, uint64_t seed)
{
    uint64_t checksum = 0;
    size_t cursor = 0;

    for (size_t i = 0; i < iterations; i++) {
        picoquic_cnx_t* cnx;
        if (strcmp(workload, "query") == 0) {
            /* Persistent-connection mix: one timer update for 32 lookups. */
            if ((i & 31) == 0) {
                cnx = &fixture->cnx[rng_next(&seed) % fixture->count];
                picoquic_reinsert_by_wake_time(&fixture->quic, cnx,
                    fixture->now + 100 + rng_next(&seed) % (fixture->count * 53 + 1));
            }
            checksum += query_wake(fixture, 0);
        }
        else if (strcmp(workload, "query-static") == 0 ||
            strcmp(workload, "query-random-static") == 0) {
            checksum += query_wake(fixture, 0);
        }
        else if (strcmp(workload, "update") == 0) {
            cnx = &fixture->cnx[rng_next(&seed) % fixture->count];
            picoquic_reinsert_by_wake_time(&fixture->quic, cnx,
                fixture->now + 100 + rng_next(&seed) % (fixture->count * 53 + 1));
            checksum += query_wake(fixture, 0);
        }
        else if (strncmp(workload, "ready-", 6) == 0) {
            cnx = picoquic_get_earliest_cnx_to_wake(&fixture->quic, fixture->now);
            checksum += cnx == NULL ? 0 : (uint64_t)(cnx - fixture->cnx) + 1;
            if (cnx != NULL) {
                picoquic_reinsert_by_wake_time(&fixture->quic, cnx, fixture->now);
            }
        }
        else if (strcmp(workload, "monotonic") == 0) {
            cnx = &fixture->cnx[cursor++ % fixture->count];
            picoquic_reinsert_by_wake_time(&fixture->quic, cnx,
                fixture->now + 100 + (uint64_t)i + fixture->count * 64);
            checksum += query_wake(fixture, 0);
        }
        else { /* randomized */
            cnx = &fixture->cnx[rng_next(&seed) % fixture->count];
            picoquic_reinsert_by_wake_time(&fixture->quic, cnx,
                fixture->now + 100 + rng_next(&seed) % (fixture->count * 53 + 1));
            checksum += query_wake(fixture, 0);
        }
    }
    return checksum;
}

static uint64_t reference_min(const uint64_t* deadlines, size_t count, uint64_t cutoff)
{
    uint64_t result = UINT64_MAX;
    for (size_t i = 0; i < count; i++) {
        if (deadlines[i] <= cutoff && deadlines[i] < result) {
            result = deadlines[i];
        }
    }
    return result;
}

static int randomized_oracle(void)
{
    fixture_t fixture;
    uint64_t deadlines[32];
    uint64_t seed = UINT64_C(0x1d1e5eed);
    int ret = -1;

    if (fixture_init(&fixture, 32, 1000) != 0) {
        return -1;
    }
    for (size_t i = 0; i < fixture.count; i++) {
        /* Deliberately small key space produces duplicate deadlines. */
        deadlines[i] = fixture.now + 100 + rng_next(&seed) % 17;
        picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[i], deadlines[i]);
    }
    for (size_t round = 0; round < 2048; round++) {
        picoquic_cnx_t* actual;
        uint64_t cutoff;
        uint64_t expected;
        size_t index = rng_next(&seed) % fixture.count;

        deadlines[index] = fixture.now + 100 + rng_next(&seed) % 17;
        /* Normalize after the previous cutoff may have promoted some entries.
         * This makes every oracle round start with only future-tree entries. */
        for (size_t i = 0; i < fixture.count; i++) {
            picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[i], deadlines[i]);
        }
        if (picoquic_get_next_wake_time(&fixture.quic, fixture.now) !=
            reference_min(deadlines, fixture.count, UINT64_MAX)) {
            goto exit;
        }
        cutoff = fixture.now + 90 + rng_next(&seed) % 40;
        expected = reference_min(deadlines, fixture.count, cutoff);
        actual = picoquic_get_earliest_cnx_to_wake(&fixture.quic, cutoff);
        if ((expected == UINT64_MAX && actual != NULL) ||
            (expected != UINT64_MAX && (actual == NULL || actual->next_wake_time != expected))) {
            goto exit;
        }
    }
    ret = 0;
exit:
    fixture_clear(&fixture);
    return ret;
}

static int expect_cnx(fixture_t* fixture, uint64_t max_wake_time, size_t expected)
{
    picoquic_cnx_t* actual = picoquic_get_earliest_cnx_to_wake(&fixture->quic, max_wake_time);
    return actual == &fixture->cnx[expected] ? 0 : -1;
}

static int correctness(void)
{
    fixture_t fixture;
    int ret = -1;

    if (fixture_init(&fixture, 16, 1000) != 0) {
        return -1;
    }
#ifdef PICOQUIC_TIMER_HEAP
    fixture.quic.cnx_wake_heap_next_sequence = UINT64_MAX - 3;
#endif
    /* Future ordering, max-wake cutoff, and promotion to the ready FIFO. */
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[0], 1500);
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[1], 1200);
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[2], 1300);
    if (picoquic_get_next_wake_time(&fixture.quic, fixture.now) != 1200 ||
        picoquic_get_earliest_cnx_to_wake(&fixture.quic, 1199) != NULL ||
        expect_cnx(&fixture, 1200, 1) != 0 || !fixture.cnx[1].is_wake_ready) {
        goto exit;
    }
    if (expect_cnx(&fixture, 0, 1) != 0 || picoquic_get_next_wake_time(&fixture.quic, fixture.now) != 1200) {
        goto exit;
    }

    /* Equal deadlines are promoted in insertion order, yielding FIFO tie order. */
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[3], 1400);
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[4], 1400);
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[5], 1400);
    if (expect_cnx(&fixture, 1400, 1) != 0) {
        goto exit;
    }
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[1], 2000);
    if (expect_cnx(&fixture, 1400, 2) != 0) {
        goto exit;
    }
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[2], 2000);
    if (expect_cnx(&fixture, 1400, 3) != 0) {
        goto exit;
    }
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[3], 2000);
    if (expect_cnx(&fixture, 1400, 4) != 0) {
        goto exit;
    }
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[4], 2000);
    if (expect_cnx(&fixture, 1400, 5) != 0) {
        goto exit;
    }

    /* A ready/future move and inactive-timer reschedule leave no stale ready entry. */
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[5], UINT64_MAX);
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[6], fixture.now);
    if (expect_cnx(&fixture, fixture.now, 6) != 0 ||
        picoquic_get_next_wake_time(&fixture.quic, fixture.now) != fixture.now) {
        goto exit;
    }
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[6], 1600);
    if (expect_cnx(&fixture, 1599, 0) != 0 ||
        expect_cnx(&fixture, 1600, 0) != 0) {
        goto exit;
    }
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[0], UINT64_MAX);
    if (expect_cnx(&fixture, 1600, 6) != 0) {
        goto exit;
    }
    picoquic_reinsert_by_wake_time(&fixture.quic, &fixture.cnx[6], UINT64_MAX);
    if (picoquic_get_earliest_cnx_to_wake(&fixture.quic, 1999) != NULL ||
        picoquic_get_next_wake_time(&fixture.quic, fixture.now) != 2000) {
        goto exit;
    }
    if (randomized_oracle() != 0) {
        goto exit;
    }
    ret = 0;
exit:
    fixture_clear(&fixture);
    return ret;
}

static void usage(const char* program)
{
    fprintf(stderr, "Usage: %s --correctness | <query|query-static|query-random-static|update|ready-only|ready-mixed|ready-mixed-random|monotonic|randomized> <size> <iterations> <seed>\n", program);
}

int main(int argc, char** argv)
{
    fixture_t fixture;
    uint64_t before_wall, before_cpu, checksum, elapsed_wall, elapsed_cpu;
    char* end = NULL;
    size_t size;
    size_t iterations;
    uint64_t seed;
    const char* workload;

    if (argc == 2 && strcmp(argv[1], "--correctness") == 0) {
        int ret = correctness();
        printf("correctness=%s\n", ret == 0 ? "ok" : "failed");
        return ret == 0 ? 0 : 1;
    }
    if (argc != 5) {
        usage(argv[0]);
        return 2;
    }
    workload = argv[1];
    if (strcmp(workload, "query") != 0 && strcmp(workload, "query-static") != 0 &&
        strcmp(workload, "query-random-static") != 0 && strcmp(workload, "update") != 0 &&
        strcmp(workload, "ready-only") != 0 && strcmp(workload, "ready-mixed") != 0 && strcmp(workload, "ready-mixed-random") != 0 && strcmp(workload, "monotonic") != 0 &&
        strcmp(workload, "randomized") != 0) {
        usage(argv[0]);
        return 2;
    }
    size = (size_t)strtoull(argv[2], &end, 10);
    if (*argv[2] == '\0' || *end != '\0' || size == 0) {
        usage(argv[0]);
        return 2;
    }
    iterations = (size_t)strtoull(argv[3], &end, 10);
    if (*argv[3] == '\0' || *end != '\0' || iterations == 0) {
        usage(argv[0]);
        return 2;
    }
    seed = strtoull(argv[4], &end, 0);
    if (*argv[4] == '\0' || *end != '\0') {
        usage(argv[0]);
        return 2;
    }
    if (fixture_init(&fixture, size, UINT64_C(1000000)) != 0) {
        fprintf(stderr, "fixture allocation failed\n");
        return 1;
    }
    schedule_spread(&fixture, seed, strcmp(workload, "query-random-static") == 0 || strcmp(workload, "randomized") == 0 || strcmp(workload, "ready-mixed-random") == 0);
    prepare_workload(&fixture, workload);
    before_wall = wall_ns();
    before_cpu = cpu_ns();
    checksum = run_workload(&fixture, workload, iterations, seed);
    elapsed_cpu = cpu_ns() - before_cpu;
    elapsed_wall = wall_ns() - before_wall;
    printf("workload=%s size=%zu iterations=%zu seed=%" PRIu64
        " wall_ns=%" PRIu64 " cpu_ns=%" PRIu64 " wall_ns_op=%.3f cpu_ns_op=%.3f checksum=%" PRIu64 "\n",
        workload, size, iterations, seed, elapsed_wall, elapsed_cpu,
        (double)elapsed_wall / (double)iterations, (double)elapsed_cpu / (double)iterations, checksum);
    fixture_clear(&fixture);
    return 0;
}
