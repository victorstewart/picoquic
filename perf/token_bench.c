/* Actual token-reuse API benchmark. Setup/TLS creation is outside timing. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static uint64_t random_next(uint64_t* state)
{
    *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17;
    return *state;
}
static uint64_t now_ns(int cpu)
{
    if (cpu) {
        struct rusage r;
        CHECK(getrusage(RUSAGE_SELF, &r) == 0);
        return ((uint64_t)r.ru_utime.tv_sec + r.ru_stime.tv_sec) * UINT64_C(1000000000) +
            ((uint64_t)r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1000;
    }
    else {
        struct timespec t;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
        return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
    }
}
static int check(picoquic_quic_t* quic, uint64_t id, uint64_t expiry)
{
    uint8_t token[16] = { 0 };
    for (size_t j = 0; j < 8; j++) token[8 + j] = (uint8_t)(id >> (56 - 8 * j));
    return picoquic_registered_token_check_reuse(quic, token, sizeof(token), expiry);
}
static picoquic_quic_t* context(size_t population, uint64_t* time)
{
    picoquic_quic_t* quic = picoquic_create((uint32_t)(population / 32 + 1), NULL, NULL, NULL,
        "token-bench", NULL, NULL, NULL, NULL, NULL, *time, time, NULL, NULL, 0);
    CHECK(quic != NULL);
    return quic;
}
static void correctness(void)
{
    uint64_t time = 0;
    picoquic_quic_t* q = context(16, &time);
    uint8_t short_token[7] = { 0 };
    CHECK(picoquic_registered_token_check_reuse(q, short_token, sizeof(short_token), 12) == -1);
    for (uint64_t i = 0; i < 32; i++) CHECK(check(q, i, 100 + i % 4) == 0);
    CHECK(q->token_reuse_tree.size == 32);
    CHECK(check(q, 1000, 100) == -1);
    CHECK(check(q, 0, 100) == -1);
    picoquic_registered_token_clear(q, 100);
    CHECK(q->token_reuse_tree.size == 32);
    picoquic_registered_token_clear(q, 101);
    CHECK(q->token_reuse_tree.size == 24);
    CHECK(check(q, 0, 100) == 0);
    CHECK(check(q, 0, 101) == 0);
    CHECK(check(q, UINT64_MAX, UINT64_MAX) == 0);
    picoquic_registered_token_clear(q, UINT64_MAX);
    CHECK(q->token_reuse_tree.size == 1);
    CHECK(check(q, UINT64_MAX, UINT64_MAX) == -1);
    picoquic_free(q);
    /* Multi-page exact/predecessor boundaries and every clear threshold, with
     * a flat independent membership oracle. */
    q = context(2048, &time);
    for (uint64_t i = 0; i < 2048; i++) CHECK(check(q, i, (i * 997) % 2048) == 0);
    for (uint64_t i = 0; i < 2048; i++) CHECK(check(q, i, (i * 997) % 2048) == -1);
    for (uint64_t cutoff = 0; cutoff <= 2048; cutoff++) {
        picoquic_registered_token_clear(q, cutoff);
        CHECK(q->token_reuse_tree.size == (int)(2048 - cutoff));
    }
    for (uint64_t i = 0; i < 2048; i++) CHECK(check(q, i, (i * 997) % 2048) == 0);
    picoquic_free(q);
    puts("correctness=ok");
}
int main(int argc, char** argv)
{
    uint64_t time = 0, seed = 42, checksum = 0, wall, cpu;
    size_t population, work, operations = 0;
    picoquic_quic_t* quic;
    uint64_t* expiry;
    debug_printf_suspend();
    if (argc == 2 && strcmp(argv[1], "--correctness") == 0) { correctness(); return 0; }
    if (argc != 4 || (strcmp(argv[1], "admit-sequential") && strcmp(argv[1], "admit-shuffled") &&
        strcmp(argv[1], "lookup") && strcmp(argv[1], "hot"))) {
        fprintf(stderr, "usage: %s --correctness | admit-sequential|admit-shuffled|lookup|hot population work\n", argv[0]); return 2;
    }
    population = (size_t)strtoull(argv[2], NULL, 10);
    work = (size_t)strtoull(argv[3], NULL, 10);
    CHECK(population > 0 && population <= 1000000 && work > 0);
    expiry = calloc(population, sizeof(*expiry)); CHECK(expiry != NULL);
    for (size_t i = 0; i < population; i++) expiry[i] = 1000 + i;
    if (strcmp(argv[1], "admit-sequential") != 0) {
        for (size_t i = population; i > 1; i--) {
            size_t j = random_next(&seed) % i; uint64_t t = expiry[i - 1]; expiry[i - 1] = expiry[j]; expiry[j] = t;
        }
    }
    quic = context(population, &time);
    int admission = strncmp(argv[1], "admit-", 6) == 0;
    if (!admission) for (size_t i = 0; i < population; i++) CHECK(check(quic, i, expiry[i]) == 0);
    wall = now_ns(0); cpu = now_ns(1);
    if (admission) {
        for (size_t round = 0; round < work; round++) {
            for (size_t i = 0; i < population; i++) { CHECK(check(quic, i, expiry[i]) == 0); checksum += i + expiry[i]; }
            for (size_t i = 0; i < population; i++) { CHECK(check(quic, i, expiry[i]) == -1); checksum += i; }
            picoquic_registered_token_clear(quic, UINT64_MAX);
            CHECK(quic->token_reuse_tree.size == 0);
        }
        operations = work * population * 2;
    }
    else {
        for (size_t i = 0; i < work; i++) {
            size_t id = strcmp(argv[1], "hot") == 0 ? 0 : random_next(&seed) % population;
            CHECK(check(quic, id, expiry[id]) == -1); checksum += id;
        }
        operations = work;
    }
    cpu = now_ns(1) - cpu; wall = now_ns(0) - wall;
    printf("population=%zu operations=%zu checksum=%" PRIu64 " wall_ns=%" PRIu64 " cpu_ns=%" PRIu64
        " cpu_ns_op=%.3f wall_ns_op=%.3f token_bytes=%zu\n", population, operations, checksum, wall, cpu,
        (double)cpu / operations, (double)wall / operations, sizeof(picoquic_registered_token_t));
    picoquic_free(quic); free(expiry); return 0;
}
