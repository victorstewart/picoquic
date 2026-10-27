/* Scheduler regression controls using real Picoquic contexts and connections. */
#include <arpa/inet.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "picoquic_internal.h"

#define CONTEXTS 4

typedef struct st_poll_fixture_t {
    picoquic_quic_t* quic[CONTEXTS];
    picoquic_cnx_t* cnx[CONTEXTS];
    uint64_t now;
} poll_fixture_t;

static uint64_t timeval_ns(const struct timeval* tv)
{
    return (uint64_t)tv->tv_sec * UINT64_C(1000000000) + (uint64_t)tv->tv_usec * 1000;
}

static uint64_t cpu_ns(void)
{
    struct rusage usage;
    (void)getrusage(RUSAGE_SELF, &usage);
    return timeval_ns(&usage.ru_utime) + timeval_ns(&usage.ru_stime);
}

static uint64_t wall_ns(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static picoquic_quic_t* create_quic(uint64_t* now)
{
    return picoquic_create(1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        *now, now, NULL, NULL, 0);
}

static picoquic_cnx_t* create_cnx(picoquic_quic_t* quic, uint64_t now, uint16_t port)
{
    picoquic_connection_id_t null_id = { 0 };
    struct sockaddr_in peer = { 0 };
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(0x7f000001);
    peer.sin_port = htons(port);
    return picoquic_create_cnx(quic, null_id, null_id, (struct sockaddr*)&peer,
        now, 0, NULL, NULL, 1);
}

static void poll_clear(poll_fixture_t* fixture)
{
    for (size_t i = 0; i < CONTEXTS; i++) {
        if (fixture->quic[i] != NULL) {
            picoquic_free(fixture->quic[i]);
        }
    }
    memset(fixture, 0, sizeof(*fixture));
}

static int poll_init(poll_fixture_t* fixture)
{
    static const uint64_t expected[CONTEXTS] = { 1000, 700, 0, 100 };
    static const int64_t caps[CONTEXTS] = { 1000, 1000, 1000, 100 };

    memset(fixture, 0, sizeof(*fixture));
    fixture->now = 1000;
    for (size_t i = 0; i < CONTEXTS; i++) {
        fixture->quic[i] = create_quic(&fixture->now);
        if (fixture->quic[i] == NULL) {
            poll_clear(fixture);
            return -1;
        }
    }
    for (size_t i = 1; i < CONTEXTS; i++) {
        fixture->cnx[i] = create_cnx(fixture->quic[i], fixture->now, (uint16_t)(9000 + i));
        if (fixture->cnx[i] == NULL) {
            poll_clear(fixture);
            return -1;
        }
    }
    picoquic_reinsert_by_wake_time(fixture->quic[1], fixture->cnx[1], fixture->now + 700);
    picoquic_reinsert_by_wake_time(fixture->quic[2], fixture->cnx[2], fixture->now);
    picoquic_reinsert_by_wake_time(fixture->quic[3], fixture->cnx[3], fixture->now + 10000);
    for (size_t i = 0; i < CONTEXTS; i++) {
        if (picoquic_get_next_wake_delay(fixture->quic[i], fixture->now, caps[i]) != (int64_t)expected[i]) {
            poll_clear(fixture);
            return -1;
        }
    }
    return 0;
}

static int run_poll(poll_fixture_t* fixture, size_t iterations, int selected, uint64_t* checksum, uint64_t* operations)
{
    static const int64_t caps[CONTEXTS] = { 1000, 1000, 1000, 100 };
    for (size_t round = 0; round < iterations; round++) {
        for (size_t i = selected < 0 ? 0 : (size_t)selected;
            i < (selected < 0 ? CONTEXTS : (size_t)selected + 1); i++) {
            int64_t delay = picoquic_get_next_wake_delay(fixture->quic[i], fixture->now, caps[i]);
            if (delay < 0 || delay > caps[i]) {
                return -1;
            }
            *checksum += (uint64_t)delay + i;
        }
    }
    *operations = iterations * (selected < 0 ? CONTEXTS : 1);
    static const uint64_t sums[CONTEXTS] = {1000, 701, 2, 103};
    return *checksum == iterations * (selected < 0 ? 1806 : sums[selected]) ? 0 : -1;
}

static int run_churn(size_t iterations, size_t batch, uint64_t* checksum, uint64_t* operations)
{
    picoquic_cnx_t** cnx = (picoquic_cnx_t**)calloc(batch, sizeof(*cnx));
    uint64_t now = 1000;
    if (cnx == NULL) {
        return -1;
    }
    for (size_t round = 0; round < iterations; round++) {
        picoquic_quic_t* quic = create_quic(&now);
        if (quic == NULL) {
            free(cnx);
            return -1;
        }
        memset(cnx, 0, batch * sizeof(*cnx));
        for (size_t i = 0; i < batch; i++) {
            cnx[i] = create_cnx(quic, now, (uint16_t)(10000 + i));
            if (cnx[i] == NULL) {
                picoquic_free(quic);
                free(cnx);
                return -1;
            }
            picoquic_reinsert_by_wake_time(quic, cnx[i], now + 100 + i);
            picoquic_reinsert_by_wake_time(quic, cnx[i],
                (i % 3 == 0) ? now : ((i % 3 == 1) ? UINT64_MAX : now + 200 + i));
            *checksum += now + i;
        }
        for (size_t i = 0; i < batch; i++) {
            picoquic_delete_cnx(cnx[i]);
        }
        if (picoquic_current_number_connections(quic) != 0 ||
            picoquic_get_earliest_cnx_to_wake(quic, 0) != NULL ||
            picoquic_get_next_wake_time(quic, now) != UINT64_MAX) {
            picoquic_free(quic);
            free(cnx);
            return -1;
        }
        picoquic_free(quic);
        now += 1000;
    }
    *operations = iterations * (4 * batch + 2); /* create, two reschedules, delete, context lifecycle */
    free(cnx);
    return 0;
}

static int parse_size(const char* text, size_t* value)
{
    char* end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (*text == '\0' || *end != '\0' || parsed == 0 || parsed > SIZE_MAX) {
        return -1;
    }
    *value = (size_t)parsed;
    return 0;
}

int main(int argc, char** argv)
{
    poll_fixture_t poll_fixture;
    size_t iterations, batch;
    uint64_t checksum = 0, operations = 0, before_cpu, before_wall, elapsed_cpu, elapsed_wall;
    const char* mode;
    int selected = -1;
    if (argc != 4 || parse_size(argv[2], &iterations) != 0 || parse_size(argv[3], &batch) != 0 ||
        (strcmp(argv[1], "poll") != 0 && strcmp(argv[1], "poll-empty") != 0 &&
         strcmp(argv[1], "poll-future") != 0 && strcmp(argv[1], "poll-ready") != 0 &&
         strcmp(argv[1], "poll-capped") != 0 && strcmp(argv[1], "churn") != 0) ||
        batch > 64 || iterations > 1000000000) {
        fprintf(stderr, "Usage: %s <poll[-empty|-future|-ready|-capped]|churn> <iterations> <batch:1..64>\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    if (strcmp(mode, "poll-empty") == 0) selected = 0;
    else if (strcmp(mode, "poll-future") == 0) selected = 1;
    else if (strcmp(mode, "poll-ready") == 0) selected = 2;
    else if (strcmp(mode, "poll-capped") == 0) selected = 3;
    memset(&poll_fixture, 0, sizeof(poll_fixture));
    if (strncmp(mode, "poll", 4) == 0 && poll_init(&poll_fixture) != 0) {
        fprintf(stderr, "scheduler poll setup failed\n");
        return 1;
    }
    before_cpu = cpu_ns();
    before_wall = wall_ns();
    int ret = strncmp(mode, "poll", 4) == 0 ? run_poll(&poll_fixture, iterations, selected, &checksum, &operations) :
        run_churn(iterations, batch, &checksum, &operations);
    elapsed_cpu = cpu_ns() - before_cpu;
    elapsed_wall = wall_ns() - before_wall;
    if (ret != 0 || operations == 0) {
        poll_clear(&poll_fixture);
        fprintf(stderr, "scheduler control failed mode=%s\n", mode);
        return 1;
    }
    printf("mode=%s iterations=%zu batch=%zu operations=%" PRIu64
        " cpu_ns=%" PRIu64 " wall_ns=%" PRIu64 " cpu_ns_op=%.3f wall_ns_op=%.3f checksum=%" PRIu64 "\n",
        mode, iterations, batch, operations, elapsed_cpu, elapsed_wall,
        (double)elapsed_cpu / operations, (double)elapsed_wall / operations, checksum);
    poll_clear(&poll_fixture);
    return 0;
}
