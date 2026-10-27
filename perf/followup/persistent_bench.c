/* Native persistent-connection loopback benchmark. See persistent_bench.md. */
#include <arpa/inet.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "picoquic.h"
#ifdef PERSISTENT_DETERMINISTIC
#include "deterministic_random.h"
#endif

#define BENCH_ALPN "persistent-bench"
#define HEADER_BYTES 32
#define MAGIC UINT64_C(0x5043515045524631)
#define MAX_STREAMS 4
#define MAX_STEPS_PER_ROUND 2000000ULL

typedef struct st_bench_t bench_t;
typedef struct st_client_t client_t;

typedef struct st_parser_t {
    uint8_t header[HEADER_BYTES];
    size_t header_length;
    uint32_t client_id;
    uint32_t stream_number;
    uint64_t sequence;
    uint64_t sent_time;
    size_t payload_seen;
    int active;
} parser_t;

typedef struct st_server_t {
    bench_t* bench;
    client_t* owner;
    parser_t parser[MAX_STREAMS];
    struct st_server_t* next;
} server_t;

struct st_client_t {
    picoquic_cnx_t* cnx;
    bench_t* bench;
    uint32_t id;
    uint64_t next_sequence[MAX_STREAMS];
    uint64_t expected_sequence[MAX_STREAMS];
    int ready;
};

struct st_bench_t {
    picoquic_quic_t* quic;
    client_t* clients;
    server_t* servers;
    server_t** server_by_client;
    uint8_t* message;
    uint64_t* latencies;
    size_t population;
    size_t streams;
    size_t payload_bytes;
    size_t latency_capacity;
    uint64_t simulated_time;
    uint64_t workload_rng;
    uint64_t loss_rng;
    uint64_t original_seed;
    uint64_t packets;
    uint64_t setup_packets;
    FILE* wire_capture;
    uint64_t observed_messages;
    uint64_t delivered_messages;
    uint64_t delivered_bytes;
    uint64_t ready_clients;
    uint64_t checksum;
    int measuring;
    int stopping;
    int failed;
    char failure[160];
};

static uint64_t read64(const uint8_t* p)
{
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint32_t read32(const uint8_t* p)
{
    uint32_t v = 0;
    for (size_t i = 0; i < 4; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void write64(uint8_t* p, uint64_t v)
{
    for (size_t i = 0; i < 8; i++) {
        p[7 - i] = (uint8_t)(v >> (i * 8));
    }
}

static void write32(uint8_t* p, uint32_t v)
{
    for (size_t i = 0; i < 4; i++) {
        p[3 - i] = (uint8_t)(v >> (i * 8));
    }
}

static uint64_t rng_next(uint64_t* state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static uint64_t monotonic_ns(void)
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

static uint64_t rss_bytes(void)
{
    struct rusage usage;
    (void)getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    return (uint64_t)usage.ru_maxrss;
#else
    return (uint64_t)usage.ru_maxrss * 1024;
#endif
}

static int latency_compare(const void* left, const void* right)
{
    uint64_t a = *(const uint64_t*)left;
    uint64_t b = *(const uint64_t*)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static void fail(bench_t* bench, const char* reason)
{
    if (!bench->failed) {
        snprintf(bench->failure, sizeof(bench->failure), "%s", reason);
    }
    bench->failed = 1;
}

static void record_close(bench_t* bench, picoquic_cnx_t* cnx, const char* side, int event)
{
    uint64_t local_reason = 0, remote_reason = 0, local_app_reason = 0, remote_app_reason = 0;
    picoquic_get_close_reasons(cnx, &local_reason, &remote_reason, &local_app_reason, &remote_app_reason);
    snprintf(bench->failure, sizeof(bench->failure),
        "%s callback event=%d close local=%" PRIu64 " remote=%" PRIu64
        " local_app=%" PRIu64 " remote_app=%" PRIu64 " sim_us=%" PRIu64,
        side, event, local_reason, remote_reason, local_app_reason, remote_app_reason,
        bench->simulated_time);
    bench->failed = 1;
}

static int parse_message_header(server_t* server, parser_t* parser)
{
    bench_t* bench = server->bench;
    uint64_t magic = read64(parser->header);
    uint32_t client_id = read32(parser->header + 8);
    uint32_t stream_number = read32(parser->header + 12);

    if (magic != MAGIC || client_id >= bench->population || stream_number >= bench->streams ||
        (server->owner != NULL && server->owner != &bench->clients[client_id])) {
        return -1;
    }
    if (server->owner == NULL) {
        if (bench->server_by_client[client_id] != NULL) {
            return -1;
        }
        server->owner = &bench->clients[client_id];
        bench->server_by_client[client_id] = server;
    }
    parser->client_id = client_id;
    parser->stream_number = stream_number;
    parser->sequence = read64(parser->header + 16);
    parser->sent_time = read64(parser->header + 24);
    if (parser->sequence != server->owner->expected_sequence[stream_number] ||
        parser->sent_time > bench->simulated_time) {
        return -1;
    }
    parser->payload_seen = 0;
    parser->active = 1;
    return 0;
}

static int consume_stream_data(server_t* server, uint64_t stream_id, const uint8_t* bytes, size_t length)
{
    bench_t* bench = server->bench;
    size_t stream_number;

    if ((stream_id & 3) != 0 || stream_id / 4 >= bench->streams) {
        return -1;
    }
    stream_number = (size_t)(stream_id / 4);
    parser_t* parser = &server->parser[stream_number];
    while (length != 0) {
        if (!parser->active) {
            size_t take = HEADER_BYTES - parser->header_length;
            if (take > length) {
                take = length;
            }
            memcpy(parser->header + parser->header_length, bytes, take);
            parser->header_length += take;
            bytes += take;
            length -= take;
            if (parser->header_length != HEADER_BYTES) {
                continue;
            }
            if (parse_message_header(server, parser) != 0 || parser->stream_number != stream_number) {
                return -1;
            }
        }
        size_t remaining = bench->payload_bytes - parser->payload_seen;
        size_t take = remaining < length ? remaining : length;
        for (size_t i = 0; i < take; i++) {
            uint8_t expected = (uint8_t)(parser->client_id ^ parser->stream_number ^
                parser->sequence ^ (parser->payload_seen + i));
            if (bytes[i] != expected) {
                return -1;
            }
        }
        parser->payload_seen += take;
        bytes += take;
        length -= take;
        if (parser->payload_seen == bench->payload_bytes) {
            bench->observed_messages++;
            if (bench->measuring) {
                if (bench->delivered_messages >= bench->latency_capacity) {
                    return -1;
                }
                bench->latencies[bench->delivered_messages] = bench->simulated_time - parser->sent_time;
                bench->checksum += parser->sequence ^ ((uint64_t)parser->client_id << 32) ^ parser->stream_number;
                bench->delivered_messages++;
                bench->delivered_bytes += bench->payload_bytes;
            }
            server->owner->expected_sequence[stream_number]++;
            memset(parser, 0, sizeof(*parser));
        }
    }
    return 0;
}

static int server_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t event, void* callback_ctx, void* stream_ctx)
{
    (void)stream_ctx;
    server_t* server = (server_t*)callback_ctx;
    int ret = 0;
    if (event == picoquic_callback_stream_data || event == picoquic_callback_stream_fin) {
        ret = consume_stream_data(server, stream_id, bytes, length);
    }
    else if (event == picoquic_callback_stream_reset || event == picoquic_callback_stop_sending ||
        event == picoquic_callback_close || event == picoquic_callback_application_close ||
        event == picoquic_callback_stateless_reset) {
        ret = -1;
    }
    if (ret != 0) {
        if (!server->bench->stopping) {
            if (event == picoquic_callback_close || event == picoquic_callback_application_close ||
                event == picoquic_callback_stateless_reset) {
                record_close(server->bench, cnx, "server", (int)event);
            }
            else {
                snprintf(server->bench->failure, sizeof(server->bench->failure),
                    "server callback event=%d validation=%d sim_us=%" PRIu64,
                    (int)event, ret, server->bench->simulated_time);
                server->bench->failed = 1;
            }
        }
    }
    return ret;
}

static int server_bootstrap_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t event, void* callback_ctx, void* stream_ctx)
{
    bench_t* bench = (bench_t*)callback_ctx;
    server_t* server = (server_t*)calloc(1, sizeof(*server));
    if (server == NULL) {
        fail(bench, "server callback allocation failed");
        return -1;
    }
    server->bench = bench;
    server->next = bench->servers;
    bench->servers = server;
    picoquic_set_callback(cnx, server_callback, server);
    return server_callback(cnx, stream_id, bytes, length, event, server, stream_ctx);
}

static int client_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t event, void* callback_ctx, void* stream_ctx)
{
    (void)stream_id;
    (void)bytes;
    (void)length;
    (void)stream_ctx;
    client_t* client = (client_t*)callback_ctx;
    if (event == picoquic_callback_ready && !client->ready) {
        client->ready = 1;
        client->bench->ready_clients++;
    }
    else if (event == picoquic_callback_close || event == picoquic_callback_application_close ||
        event == picoquic_callback_stateless_reset || event == picoquic_callback_stream_data ||
        event == picoquic_callback_stream_fin || event == picoquic_callback_stream_reset) {
        if (!client->bench->stopping) {
            if (event == picoquic_callback_close || event == picoquic_callback_application_close ||
                event == picoquic_callback_stateless_reset) {
                record_close(client->bench, cnx, "client", (int)event);
            }
            else {
                snprintf(client->bench->failure, sizeof(client->bench->failure),
                    "client=%u callback event=%d sim_us=%" PRIu64,
                    client->id, (int)event, client->bench->simulated_time);
                client->bench->failed = 1;
            }
        }
        return -1;
    }
    return 0;
}

static int should_drop(bench_t* bench, int loss)
{
    return loss != 0 && (rng_next(&bench->loss_rng) & 63) == 0;
}

static int pump_one(bench_t* bench, int loss, uint64_t deadline)
{
    uint8_t packet[1536];
    struct sockaddr_storage addr_to;
    struct sockaddr_storage addr_from;
    picoquic_connection_id_t log_cid;
    picoquic_cnx_t* last_cnx = NULL;
    picoquic_cnx_t* first_cnx = NULL;
    size_t length = 0;
    size_t send_msg_size = 0;
    int if_index = 0;
    int ret = picoquic_prepare_next_packet_ex(bench->quic, bench->simulated_time, packet, sizeof(packet),
        &length, &addr_to, &addr_from, &if_index, &log_cid, &last_cnx, &send_msg_size);
    (void)last_cnx;
    (void)log_cid;
    if (ret != 0) {
        return ret;
    }
    if (length == 0) {
        uint64_t next = picoquic_get_next_wake_time(bench->quic, bench->simulated_time);
        if (next == UINT64_MAX || next > deadline) {
            bench->simulated_time = deadline;
            return 1;
        }
        bench->simulated_time = next > bench->simulated_time ? next : bench->simulated_time + 1;
        return 0;
    }
    size_t segment = send_msg_size == 0 || send_msg_size > length ? length : send_msg_size;
    for (size_t offset = 0; offset < length; offset += segment) {
        size_t this_length = length - offset;
        if (this_length > segment) {
            this_length = segment;
        }
#ifdef PERSISTENT_DETERMINISTIC
        if (bench->wire_capture != NULL) {
            uint8_t size_bytes[8];
            write64(size_bytes, this_length);
            if (fwrite(size_bytes, 1, 8, bench->wire_capture) != 8 ||
                fwrite(packet + offset, 1, this_length, bench->wire_capture) != this_length) {
                return -1;
            }
        }
#endif
        bench->packets++;
        bench->simulated_time += 1;
        if (!should_drop(bench, loss)) {
            ret = picoquic_incoming_packet_ex(bench->quic, packet + offset, this_length,
                (struct sockaddr*)&addr_from, (struct sockaddr*)&addr_to, if_index, 0,
                &first_cnx, bench->simulated_time);
            if (ret != 0) {
                return ret;
            }
        }
    }
    return 0;
}

static int pump_until(bench_t* bench, uint64_t wanted, int loss, uint64_t limit)
{
    for (uint64_t step = 0; step < limit && !bench->failed; step++) {
        if (bench->delivered_messages >= wanted) {
            return 0;
        }
        int ret = pump_one(bench, loss, UINT64_MAX);
        if (ret != 0) {
            return -1;
        }
    }
    return -1;
}

static int pump_until_observed(bench_t* bench, uint64_t wanted, int loss, uint64_t limit)
{
    for (uint64_t step = 0; step < limit && !bench->failed; step++) {
        if (bench->observed_messages >= wanted) {
            return 0;
        }
        if (pump_one(bench, loss, UINT64_MAX) != 0) {
            return -1;
        }
    }
    return -1;
}

/* Deliver protocol work due no later than target without jumping past it. */
static int pump_until_time(bench_t* bench, uint64_t target, int loss, uint64_t limit)
{
    for (uint64_t step = 0; step < limit && !bench->failed; step++) {
        if (bench->simulated_time >= target) {
            return 0;
        }
        int ret = pump_one(bench, loss, target);
        if (ret == 1) {
            return bench->simulated_time == target ? 0 : -1;
        }
        if (ret != 0) {
            return -1;
        }
    }
    return -1;
}

static int send_message(bench_t* bench, client_t* client, size_t stream_number)
{
    uint64_t sequence = client->next_sequence[stream_number]++;
    uint8_t* message = bench->message;
    uint64_t stream_id = (uint64_t)stream_number * 4;
    write64(message, MAGIC);
    write32(message + 8, client->id);
    write32(message + 12, (uint32_t)stream_number);
    write64(message + 16, sequence);
    write64(message + 24, bench->simulated_time);
    for (size_t i = 0; i < bench->payload_bytes; i++) {
        message[HEADER_BYTES + i] = (uint8_t)(client->id ^ stream_number ^ sequence ^ i);
    }
    return picoquic_add_to_stream(client->cnx, stream_id, message, HEADER_BYTES + bench->payload_bytes, 0);
}

static int connect_all(bench_t* bench, const char* cert, const char* key, const char* root)
{
    struct sockaddr_in server_addr;
    picoquic_connection_id_t null_id = { 0 };
    uint64_t setup_limit;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(4321);
    server_addr.sin_addr.s_addr = htonl(0x0a000001);
    bench->quic = picoquic_create((uint32_t)(bench->population * 2 + 8), cert, key, root,
        BENCH_ALPN, server_bootstrap_callback, bench, NULL, NULL, NULL, bench->simulated_time,
        &bench->simulated_time, NULL, NULL, 0);
    if (bench->quic == NULL) {
        return -1;
    }
    picoquic_set_random_initial(bench->quic, 0);
    picoquic_set_default_idle_timeout(bench->quic, 600000);
    picoquic_set_max_half_open_retry_threshold(bench->quic, (uint32_t)bench->population + 16);
    for (size_t i = 0; i < bench->population; i++) {
        client_t* client = &bench->clients[i];
        client->bench = bench;
        client->id = (uint32_t)i;
        client->cnx = picoquic_create_cnx(bench->quic, null_id,
            null_id, (struct sockaddr*)&server_addr, bench->simulated_time, 0,
            "test.example.com", BENCH_ALPN, 1);
        if (client->cnx == NULL) {
            return -1;
        }
        picoquic_set_callback(client->cnx, client_callback, client);
        if (picoquic_start_client_cnx(client->cnx) != 0) {
            return -1;
        }
    }
    setup_limit = bench->population > UINT64_MAX / 2000 ? UINT64_MAX : bench->population * 2000;
    for (uint64_t step = 0; step < setup_limit && !bench->failed; step++) {
        if (bench->ready_clients == bench->population) {
            return 0;
        }
        if (pump_one(bench, 0, UINT64_MAX) != 0) {
            fail(bench, "setup packet loop");
            return -1;
        }
    }
    return -1;
}

static int prime_streams(bench_t* bench)
{
    uint64_t wanted = bench->observed_messages + bench->population * bench->streams;
    for (size_t i = 0; i < bench->population; i++) {
        for (size_t stream = 0; stream < bench->streams; stream++) {
            if (send_message(bench, &bench->clients[i], stream) != 0) {
                return -1;
            }
        }
    }
    if (pump_until_observed(bench, wanted, 0, MAX_STEPS_PER_ROUND) != 0) {
        return -1;
    }
    return pump_until_time(bench, bench->simulated_time + 20000, 0, MAX_STEPS_PER_ROUND);
}

static void cleanup(bench_t* bench)
{
    server_t* server = bench->servers;
    bench->stopping = 1;
    if (bench->quic != NULL) {
        picoquic_free(bench->quic);
    }
    while (server != NULL) {
        server_t* next = server->next;
        free(server);
        server = next;
    }
    if (bench->wire_capture != NULL) fclose(bench->wire_capture);
    free(bench->message);
    free(bench->latencies);
    free(bench->server_by_client);
    free(bench->clients);
    memset(bench, 0, sizeof(*bench));
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
    bench_t bench;
    size_t rounds, burst;
    int loss;
    char* end = NULL;
    uint64_t setup_cpu_start, setup_wall_start, message_cpu_start, message_wall_start;
    uint64_t setup_cpu, setup_wall, message_cpu, message_wall;
    const char* cert = getenv("PICOQUIC_BENCH_CERT");
    const char* key = getenv("PICOQUIC_BENCH_KEY");
    const char* root = getenv("PICOQUIC_BENCH_ROOT");

    if (argc != 8 || parse_size(argv[1], &bench.population) != 0 ||
        parse_size(argv[2], &bench.streams) != 0 || parse_size(argv[3], &rounds) != 0 ||
        parse_size(argv[4], &bench.payload_bytes) != 0 || parse_size(argv[5], &burst) != 0 ||
        (loss = (int)strtol(argv[6], &end, 0), *argv[6] == '\0' || *end != '\0' || (loss != 0 && loss != 1)) ||
        (bench.original_seed = strtoull(argv[7], &end, 0), *argv[7] == '\0' || *end != '\0')) {
        fprintf(stderr, "Usage: %s <population> <streams:1|4> <rounds> <payload_bytes>=1.. <burst> <loss:0|1> <seed>\n", argv[0]);
        return 2;
    }
    if ((bench.population != 1000 && bench.population != 10000) ||
        (bench.streams != 1 && bench.streams != 4) || bench.payload_bytes > 65536 ||
        burst > bench.population || rounds > SIZE_MAX / burst ||
        bench.payload_bytes > SIZE_MAX - HEADER_BYTES) {
        fprintf(stderr, "invalid bounded workload\n");
        return 2;
    }
    memset(&bench, 0, sizeof(bench));
    bench.population = (size_t)strtoull(argv[1], NULL, 0);
    bench.streams = (size_t)strtoull(argv[2], NULL, 0);
    bench.payload_bytes = (size_t)strtoull(argv[4], NULL, 0);
    bench.original_seed = strtoull(argv[7], NULL, 0);
#ifdef PERSISTENT_DETERMINISTIC
    bench_deterministic_random_init();
    if (getenv("PICOQUIC_WIRE_CAPTURE") != NULL) {
        bench.wire_capture = fopen(getenv("PICOQUIC_WIRE_CAPTURE"), "wb");
        if (bench.wire_capture == NULL) return 1;
    }
#endif
    bench.workload_rng = bench.original_seed;
    bench.loss_rng = bench.original_seed ^ UINT64_C(0x9e3779b97f4a7c15);
    cert = cert == NULL ? "certs/cert.pem" : cert;
    key = key == NULL ? "certs/key.pem" : key;
    root = root == NULL ? "certs/test-ca.crt" : root;
    bench.clients = (client_t*)calloc(bench.population, sizeof(*bench.clients));
    bench.server_by_client = (server_t**)calloc(bench.population, sizeof(*bench.server_by_client));
    bench.message = (uint8_t*)malloc(HEADER_BYTES + bench.payload_bytes);
    bench.latency_capacity = rounds * burst;
    bench.latencies = (uint64_t*)calloc(bench.latency_capacity, sizeof(*bench.latencies));
    if (bench.clients == NULL || bench.server_by_client == NULL || bench.message == NULL || bench.latencies == NULL) {
        fprintf(stderr, "allocation failed\n");
        cleanup(&bench);
        return 1;
    }
    setup_cpu_start = cpu_ns();
    setup_wall_start = monotonic_ns();
    if (connect_all(&bench, cert, key, root) != 0) {
        fprintf(stderr, "connection setup failed: %s\n",
            bench.failure[0] == '\0' ? "connection create or readiness limit" : bench.failure);
        cleanup(&bench);
        return 1;
    }
    if (prime_streams(&bench) != 0) {
        fprintf(stderr, "stream prime failed: %s\n", bench.failure[0] == '\0' ? "packet loop" : bench.failure);
        cleanup(&bench);
        return 1;
    }
    setup_cpu = cpu_ns() - setup_cpu_start;
    setup_wall = monotonic_ns() - setup_wall_start;
    bench.setup_packets = bench.packets;
    bench.measuring = 1;
    message_cpu_start = cpu_ns();
    message_wall_start = monotonic_ns();
    for (size_t round = 0; round < rounds && !bench.failed; round++) {
        size_t count = (round % 8 == 0) ? burst : (burst + 15) / 16;
        uint64_t wanted = bench.delivered_messages + count;
        size_t first = (size_t)(rng_next(&bench.workload_rng) % bench.population);
        for (size_t j = 0; j < count; j++) {
            client_t* client = &bench.clients[(first + j * 9973) % bench.population];
            if (send_message(&bench, client, (round + j) % bench.streams) != 0) {
                bench.failed = 1;
                break;
            }
        }
        if (!bench.failed && pump_until(&bench, wanted, loss, MAX_STEPS_PER_ROUND) != 0) {
            bench.failed = 1;
        }
        /* Process all due timers in the 20 ms idle interval without skipping them. */
        if (!bench.failed && pump_until_time(&bench, bench.simulated_time + 20000,
            loss, MAX_STEPS_PER_ROUND) != 0) {
            fail(&bench, "idle packet loop");
        }
    }
    message_cpu = cpu_ns() - message_cpu_start;
    message_wall = monotonic_ns() - message_wall_start;
    if (bench.failed || bench.delivered_messages == 0) {
        fprintf(stderr, "delivery validation failed delivered=%" PRIu64 " reason=%s\n",
            bench.delivered_messages, bench.failure[0] == '\0' ? "packet loop" : bench.failure);
        cleanup(&bench);
        return 1;
    }
    qsort(bench.latencies, (size_t)bench.delivered_messages, sizeof(*bench.latencies), latency_compare);
    size_t p99_index = (size_t)((bench.delivered_messages * 99 + 99) / 100 - 1);
    printf("population=%zu streams=%zu rounds=%zu payload_bytes=%zu burst=%zu loss=%d seed=%" PRIu64
        " setup_cpu_ns=%" PRIu64 " setup_wall_ns=%" PRIu64
        " cpu_ns_op=%.3f wall_ns_op=%.3f checksum=%" PRIu64
        " delivered_messages=%" PRIu64 " delivered_bytes=%" PRIu64
        " simulated_p99_delivery_us=%" PRIu64 " rss_max_bytes=%" PRIu64 " packets=%" PRIu64 " measured_packets=%" PRIu64 "\n",
        bench.population, bench.streams, rounds, bench.payload_bytes, burst, loss, bench.original_seed,
        setup_cpu, setup_wall, (double)message_cpu / bench.delivered_messages,
        (double)message_wall / bench.delivered_messages, bench.checksum, bench.delivered_messages,
        bench.delivered_bytes, bench.latencies[p99_index], rss_bytes(), bench.packets, bench.packets - bench.setup_packets);
    cleanup(&bench);
    return 0;
}
