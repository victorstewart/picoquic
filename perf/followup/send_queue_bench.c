/* Real picoquic_add_to_stream benchmark and queue-lifecycle oracle.
 * Build against either baseline or the send_queue_tail.patch candidate. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "picoquic_internal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct st_fixture_t {
    picoquic_quic_t* quic;
    picoquic_cnx_t** cnx;
    size_t count;
    uint64_t now;
} fixture_t;

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
        return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
    }
}

static void fixture_free(fixture_t* f)
{
    if (f->cnx != NULL) {
        for (size_t i = 0; i < f->count; i++) {
            if (f->cnx[i] != NULL) {
                picoquic_delete_cnx(f->cnx[i]);
            }
        }
        free(f->cnx);
    }
    if (f->quic != NULL) {
        picoquic_free(f->quic);
    }
    memset(f, 0, sizeof(*f));
}

static void fixture_init(fixture_t* f, size_t count)
{
    struct sockaddr_in addr;
    memset(f, 0, sizeof(*f));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(4443);
    f->quic = picoquic_create((uint32_t)(count + 8), NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, f->now, &f->now, NULL, NULL, 0);
    CHECK(f->quic != NULL);
    f->cnx = (picoquic_cnx_t**)calloc(count, sizeof(*f->cnx));
    CHECK(f->cnx != NULL);
    f->count = count;
    for (size_t i = 0; i < count; i++) {
        f->cnx[i] = picoquic_create_cnx(f->quic, picoquic_null_connection_id,
            picoquic_null_connection_id, (struct sockaddr*)&addr, f->now, 0,
            "send-queue-bench", "hq-interop", 1);
        CHECK(f->cnx[i] != NULL);
        f->cnx[i]->maxdata_remote = UINT64_MAX;
        f->cnx[i]->max_streams_bidir_remote = UINT64_MAX;
        CHECK(picoquic_add_to_stream(f->cnx[i], 0, NULL, 0, 0) == 0);
        CHECK(picoquic_find_stream(f->cnx[i], 0) != NULL);
        picoquic_find_stream(f->cnx[i], 0)->maxdata_remote = UINT64_MAX;
    }
}

static void check_tail(picoquic_stream_head_t* s);

static uint64_t validate_and_clear_queues(fixture_t* f, size_t depth)
{
    uint64_t checksum = 0;
    for (size_t i = 0; i < f->count; i++) {
        picoquic_stream_head_t* s = picoquic_find_stream(f->cnx[i], 0);
        picoquic_stream_queue_node_t* n;
        size_t d = 0;
        CHECK(s != NULL);
        for (n = s->send_queue; n != NULL; n = n->next_stream_data, d++) {
            CHECK(d < depth && n->length == 1 && n->offset == 0);
            CHECK(n->bytes != NULL && n->bytes[0] == (uint8_t)(d + 1));
            checksum = checksum * UINT64_C(131) + n->bytes[0] + i;
        }
        CHECK(d == depth);
        check_tail(s);
        picoquic_clear_stream(s);
        /* This fixture owns one app stream per connection. clear_stream does not
         * decrement this telemetry counter, so restore its batch-start value. */
        s->cnx->nb_bytes_queued = 0;
        CHECK(s->send_queue == NULL);
#ifdef PICOQUIC_SEND_QUEUE_TAIL
        CHECK(s->send_queue_last == NULL);
#endif
    }
    return checksum;
}

static void check_tail(picoquic_stream_head_t* s)
{
#ifdef PICOQUIC_SEND_QUEUE_TAIL
    picoquic_stream_queue_node_t* n = s->send_queue;
    picoquic_stream_queue_node_t* last = NULL;
    while (n != NULL) { last = n; n = n->next_stream_data; }
    CHECK(s->send_queue_last == last);
#else
    (void)s;
#endif
}

static size_t format_and_check(picoquic_cnx_t* cnx, picoquic_stream_head_t* s,
    size_t cap, const uint8_t* expected, size_t expected_length, size_t expected_offset, int expected_fin)
{
    uint8_t out[128];
    uint8_t* end;
    uint64_t sid, offset;
    size_t length, consumed;
    int fin, more_data = 0, pure_ack = 1, active = 0, closed = 0, ret = 0;
    CHECK(cap <= sizeof(out));
    end = picoquic_format_stream_frame(cnx, s, out, out + cap, &more_data, &pure_ack, &active, &closed, &ret);
    CHECK(end != NULL && ret == 0);
    CHECK(picoquic_parse_stream_header(out, (size_t)(end - out), &sid, &offset, &length, &fin, &consumed) == 0);
    CHECK(sid == 0 && offset == expected_offset && length <= expected_length - expected_offset);
    CHECK(memcmp(out + consumed, expected + expected_offset, length) == 0);
    CHECK(fin == expected_fin);
    check_tail(s);
    return length;
}

static void seed_tls_queue(picoquic_stream_head_t* s, const uint8_t* bytes, size_t length)
{
    picoquic_stream_queue_node_t* n = (picoquic_stream_queue_node_t*)calloc(1, sizeof(*n));
    CHECK(n != NULL);
    n->bytes = (uint8_t*)malloc(length);
    CHECK(n->bytes != NULL);
    memcpy(n->bytes, bytes, length);
    n->length = length;
    CHECK(s->send_queue == NULL);
    s->send_queue = n;
#ifdef PICOQUIC_SEND_QUEUE_TAIL
    s->send_queue_last = n;
#endif
    check_tail(s);
}

static void check_tls_refill(picoquic_cnx_t* cnx)
{
    picoquic_stream_head_t* s = &cnx->tls_stream[picoquic_epoch_initial];
    uint8_t out[64];
    int more_data = 0, pure_ack = 1;
    static const uint8_t first[] = { 1, 2, 3 };
    static const uint8_t second[] = { 4, 5 };
    seed_tls_queue(s, first, sizeof(first));
    CHECK(picoquic_format_crypto_hs_frame(s, out, out + sizeof(out), &more_data, &pure_ack) != NULL);
    CHECK(s->send_queue == NULL);
    check_tail(s);
    seed_tls_queue(s, second, sizeof(second));
    CHECK(picoquic_format_crypto_hs_frame(s, out, out + sizeof(out), &more_data, &pure_ack) != NULL);
    CHECK(s->send_queue == NULL);
    check_tail(s);
}

static void correctness(void)
{
    fixture_t f;
    static const uint8_t payload[] = "ABCDEFGHIJK";
    picoquic_stream_head_t* s;
    picoquic_stream_head_t* reset_stream;
    size_t sent = 0;
    fixture_init(&f, 1);
    s = picoquic_find_stream(f.cnx[0], 0);
    CHECK(picoquic_add_to_stream(f.cnx[0], 0, payload, 5, 0) == 0);
    CHECK(picoquic_add_to_stream(f.cnx[0], 0, payload + 5, 2, 0) == 0);
    check_tail(s);
    /* A five-byte packet forces a partial drain of the first queued write. */
    sent += format_and_check(f.cnx[0], s, 5, payload, sizeof(payload) - 1, sent, 0);
    /* Append behind a partially consumed head node. */
    CHECK(picoquic_add_to_stream(f.cnx[0], 0, payload + 7, 2, 0) == 0);
    check_tail(s);
    while (s->send_queue != NULL) {
        sent += format_and_check(f.cnx[0], s, 64, payload, 9, sent, 0);
    }
    CHECK(sent == 9 && !s->fin_sent);
    check_tail(s);
    /* Refill an entirely drained, still-open stream. */
    CHECK(picoquic_add_to_stream(f.cnx[0], 0, payload + 9, 2, 0) == 0);
    sent += format_and_check(f.cnx[0], s, 64, payload, sizeof(payload) - 1, sent, 0);
    CHECK(sent == sizeof(payload) - 1 && s->send_queue == NULL && !s->fin_sent);
    check_tls_refill(f.cnx[0]);
    CHECK(picoquic_add_to_stream(f.cnx[0], 4, payload, 3, 0) == 0);
    CHECK(picoquic_add_to_stream(f.cnx[0], 4, payload + 3, 3, 0) == 0);
    reset_stream = picoquic_find_stream(f.cnx[0], 4);
    CHECK(reset_stream != NULL);
    reset_stream->reset_requested = 1;
    {
        uint8_t reset_frame[32];
        int more_data = 0, pure_ack = 1;
        CHECK(picoquic_format_reset_stream_frame(reset_stream, reset_frame,
            reset_frame + sizeof(reset_frame), &more_data, &pure_ack) != NULL);
    }
    CHECK(reset_stream->send_queue == NULL);
    check_tail(reset_stream);
    fixture_free(&f);
    puts("correctness=ok");
}

int main(int argc, char** argv)
{
    fixture_t f;
    uint64_t cpu = 0, wall = 0, checksum = 0;
    size_t population, depth, batches;
    if (argc == 2 && strcmp(argv[1], "--correctness") == 0) { correctness(); return 0; }
    if (argc != 4) {
        fprintf(stderr, "usage: %s --correctness | population depth batches\n", argv[0]);
        return 2;
    }
    population = (size_t)strtoull(argv[1], NULL, 10);
    depth = (size_t)strtoull(argv[2], NULL, 10);
    batches = (size_t)strtoull(argv[3], NULL, 10);
    CHECK((population == 1 || population == 1000) &&
        (depth == 1 || depth == 8 || depth == 64 || depth == 256) && batches > 0);
    fixture_init(&f, population);
    for (size_t batch = 0; batch < batches; batch++) {
        uint64_t batch_wall = now_ns(0);
        uint64_t batch_cpu = now_ns(1);
        for (size_t d = 0; d < depth; d++) {
            uint8_t byte = (uint8_t)(d + 1);
            for (size_t i = 0; i < population; i++) {
                CHECK(picoquic_add_to_stream(f.cnx[i], 0, &byte, 1, 0) == 0);
            }
        }
        cpu += now_ns(1) - batch_cpu;
        wall += now_ns(0) - batch_wall;
        /* Validation and clearing are outside the timed append interval. */
        checksum = checksum * UINT64_C(65537) + validate_and_clear_queues(&f, depth);
    }
    printf("population=%zu depth=%zu batches=%zu appends=%zu checksum=%" PRIu64
        " cpu_ns=%" PRIu64 " wall_ns=%" PRIu64 " cpu_ns_append=%.3f wall_ns_append=%.3f\n",
        population, depth, batches, population * depth * batches, checksum, cpu, wall,
        (double)cpu / (population * depth * batches), (double)wall / (population * depth * batches));
    fixture_free(&f);
    return 0;
}
