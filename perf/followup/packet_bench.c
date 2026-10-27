/* Packet-pool microbenchmark. It intentionally exercises only
 * picoquic_create_packet()/picoquic_recycle_packet(), not packet formatting. */
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include "picoquic_internal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed: %s\n", #x); return 1; } } while (0)

static volatile uint64_t packet_sink;

static uint64_t elapsed_ns(int cpu)
{
    if (cpu) {
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
        return ((uint64_t)usage.ru_utime.tv_sec + (uint64_t)usage.ru_stime.tv_sec) * UINT64_C(1000000000) +
            ((uint64_t)usage.ru_utime.tv_usec + (uint64_t)usage.ru_stime.tv_usec) * 1000;
    }
    else {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
        return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    }
}

static uint64_t write_payload(picoquic_packet_t *packet, size_t payload, uint64_t tag)
{
    size_t i;
    uint64_t sum = 0;
    for (i = 0; i < payload; i++) packet->bytes[i] = (uint8_t)(tag + i * 17);
    for (i = 0; i < payload; i++) sum = (sum << 5) - sum + packet->bytes[i];
    return sum;
}

static void drain_pool(picoquic_quic_t *quic)
{
    picoquic_packet_t *packet;
    while ((packet = quic->p_first_packet) != NULL) {
        quic->p_first_packet = packet->packet_previous;
        free(packet);
    }
    quic->nb_packets_in_pool = 0;
    quic->nb_packets_allocated = 0;
}

static int correctness(void)
{
    picoquic_quic_t quic;
    picoquic_packet_t *packet;
    size_t i;
    memset(&quic, 0, sizeof(quic));
    packet = picoquic_create_packet(&quic);
    CHECK(packet != NULL);
    packet->length = 77;
    packet->sequence_number = 99;
    packet->is_ack_eliciting = 1;
    memset(packet->bytes, 0xa5, sizeof(packet->bytes));
    picoquic_recycle_packet(&quic, packet);
    CHECK(quic.nb_packets_in_pool == 1 && quic.nb_packets_allocated == 1);
    packet = picoquic_create_packet(&quic);
    CHECK(packet != NULL && quic.nb_packets_in_pool == 0);
    /* Metadata must reset; payload is deliberately unspecified. */
    CHECK(packet->length == 0 && packet->sequence_number == 0 && packet->is_ack_eliciting == 0);
    packet_sink ^= write_payload(packet, 64, 7);
    picoquic_recycle_packet(&quic, packet);
    for (i = 0; i < 1024; i++) {
        packet = picoquic_create_packet(&quic);
        CHECK(packet != NULL && packet->length == 0 && packet->packet_next == NULL);
        packet_sink ^= write_payload(packet, 1200, i);
        picoquic_recycle_packet(&quic, packet);
    }
    CHECK(quic.nb_packets_in_pool == 1 && quic.nb_packets_allocated == 1);
    drain_pool(&quic);
    return 0;
}

static int benchmark(size_t depth, size_t payload, size_t iterations)
{
    picoquic_quic_t quic;
    picoquic_packet_t **packets;
    uint64_t wall, cpu;
    size_t i, j;
    memset(&quic, 0, sizeof(quic));
    packets = calloc(depth, sizeof(*packets));
    CHECK(packets != NULL);
    for (i = 0; i < depth; i++) { packets[i] = picoquic_create_packet(&quic); CHECK(packets[i] != NULL); }
    for (i = 0; i < depth; i++) picoquic_recycle_packet(&quic, packets[i]);
    wall = elapsed_ns(0); cpu = elapsed_ns(1);
    for (i = 0; i < iterations; i++) {
        for (j = 0; j < depth; j++) { packets[j] = picoquic_create_packet(&quic); packet_sink ^= write_payload(packets[j], payload, i * depth + j); }
        for (j = 0; j < depth; j++) picoquic_recycle_packet(&quic, packets[j]);
    }
    wall = elapsed_ns(0) - wall; cpu = elapsed_ns(1) - cpu;
    printf("depth=%zu payload=%zu operations=%zu checksum=%" PRIu64 " wall_ns_op=%.2f cpu_ns_op=%.2f\n", depth, payload, iterations * depth, packet_sink, (double)wall / (iterations * depth), (double)cpu / (iterations * depth));
    free(packets); drain_pool(&quic); return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--correctness")) return correctness();
    if (argc != 5 || strcmp(argv[1], "--bench")) { fprintf(stderr, "usage: %s --correctness | %s --bench DEPTH PAYLOAD ITERATIONS\n", argv[0], argv[0]); return 2; }
    size_t depth = strtoull(argv[2], NULL, 0), payload = strtoull(argv[3], NULL, 0), iterations = strtoull(argv[4], NULL, 0);
    CHECK((depth == 1 || depth == 64 || depth == 1024) && (payload == 64 || payload == 1200) && iterations > 0 && iterations <= SIZE_MAX / depth);
    return benchmark(depth, payload, iterations);
}
