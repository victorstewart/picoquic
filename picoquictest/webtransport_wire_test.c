/*
* Author: Christian Huitema
* Copyright (c) 2026, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <string.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "picoquictest_internal.h"
#include "demoserver.h"
#include "h3zero_common.h"
#include "pico_webtransport.h"

typedef struct st_picowt_wire_harness_t {
    picoquic_test_tls_api_ctx_t* test_ctx;
    h3zero_callback_ctx_t* client_h3_ctx;
    h3zero_stream_ctx_t* client_control_stream;
    picohttp_server_parameters_t server_param;
    uint64_t simulated_time;
    uint64_t loss_mask;
} picowt_wire_harness_t;

typedef enum {
    picowt_wire_fragment_stream_prefix,
    picowt_wire_fragment_settings,
    picowt_wire_fragment_h3_frame,
    picowt_wire_fragment_capsule,
    picowt_wire_fragment_datagram,
    picowt_wire_fragment_qpack_header
} picowt_wire_fragment_target_t;

typedef struct st_picowt_wire_fragment_chunk_t {
    picowt_wire_fragment_target_t target;
    const uint8_t* bytes;
    size_t length;
    size_t boundary;
    size_t chunk_index;
    int is_last;
} picowt_wire_fragment_chunk_t;

typedef int (*picowt_wire_fragment_cb_fn)(
    const picowt_wire_fragment_chunk_t* chunk, void* callback_ctx);

typedef struct st_picowt_wire_fragment_varint_ctx_t {
    uint64_t expected;
    uint64_t decoded;
    uint8_t buffer[16];
    size_t buffer_length;
} picowt_wire_fragment_varint_ctx_t;

typedef struct st_picowt_wire_fragment_frame_ctx_t {
    uint64_t expected_type;
    const uint8_t* expected_payload;
    size_t expected_payload_length;
    uint64_t frame_type;
    uint64_t frame_length;
    uint8_t buffer[16];
    size_t buffer_length;
    size_t payload_seen;
} picowt_wire_fragment_frame_ctx_t;

typedef struct st_picowt_wire_malformed_sample_t {
    picowt_wire_fragment_target_t target;
    uint8_t bytes[128];
    size_t length;
    size_t payload_offset;
    uint64_t type;
    size_t declared_length;
    size_t payload_length;
} picowt_wire_malformed_sample_t;

typedef enum {
    picowt_wire_packet_stream,
    picowt_wire_packet_datagram
} picowt_wire_packet_type_t;

typedef struct st_picowt_wire_network_config_t {
    uint64_t stream_loss_mask;
    uint64_t datagram_loss_mask;
    uint64_t base_delay;
} picowt_wire_network_config_t;

#define PICOWT_WIRE_NETWORK_QUEUE_SIZE 8
#define PICOWT_WIRE_NETWORK_PACKET_SIZE 64

typedef struct st_picowt_wire_network_packet_t {
    picowt_wire_packet_type_t packet_type;
    uint64_t sequence;
    uint64_t arrival_time;
    uint8_t bytes[PICOWT_WIRE_NETWORK_PACKET_SIZE];
    size_t length;
} picowt_wire_network_packet_t;

typedef struct st_picowt_wire_network_t {
    picowt_wire_network_config_t config;
    uint64_t stream_loss_mask;
    uint64_t datagram_loss_mask;
    uint64_t next_sequence;
    uint64_t packets_sent;
    uint64_t packets_dropped;
    picowt_wire_network_packet_t queue[PICOWT_WIRE_NETWORK_QUEUE_SIZE];
    size_t queue_size;
} picowt_wire_network_t;

static int picowt_wire_expect_uint64(char const* what, uint64_t expected, uint64_t actual)
{
    int ret = 0;

    if (expected != actual) {
        DBG_PRINTF("%s: expected 0x%" PRIx64 ", got 0x%" PRIx64, what, expected, actual);
        ret = -1;
    }

    return ret;
}

static void picowt_wire_network_init(picowt_wire_network_t* network,
    const picowt_wire_network_config_t* config)
{
    memset(network, 0, sizeof(picowt_wire_network_t));
    network->config = *config;
    network->stream_loss_mask = config->stream_loss_mask;
    network->datagram_loss_mask = config->datagram_loss_mask;
}

static uint64_t* picowt_wire_network_loss_mask(picowt_wire_network_t* network,
    picowt_wire_packet_type_t packet_type)
{
    return (packet_type == picowt_wire_packet_datagram) ?
        &network->datagram_loss_mask : &network->stream_loss_mask;
}

static int picowt_wire_network_should_drop(uint64_t* loss_mask)
{
    uint64_t loss_bit = *loss_mask & 1ull;

    *loss_mask >>= 1;
    *loss_mask |= loss_bit << 63;

    return (int)loss_bit;
}

static int picowt_wire_network_submit(picowt_wire_network_t* network,
    picowt_wire_packet_type_t packet_type, const uint8_t* bytes, size_t length,
    uint64_t current_time, uint64_t extra_delay)
{
    int ret = 0;

    if (length > PICOWT_WIRE_NETWORK_PACKET_SIZE) {
        ret = -1;
    }
    else if (picowt_wire_network_should_drop(
        picowt_wire_network_loss_mask(network, packet_type))) {
        network->packets_dropped++;
    }
    else if (network->queue_size >= PICOWT_WIRE_NETWORK_QUEUE_SIZE) {
        ret = -1;
    }
    else {
        uint64_t sequence = network->next_sequence++;
        uint64_t arrival_time = current_time + network->config.base_delay + extra_delay;
        size_t insert_index = network->queue_size;

        while (insert_index > 0 &&
            (network->queue[insert_index - 1].arrival_time > arrival_time ||
                (network->queue[insert_index - 1].arrival_time == arrival_time &&
                    network->queue[insert_index - 1].sequence > sequence))) {
            network->queue[insert_index] = network->queue[insert_index - 1];
            insert_index--;
        }

        network->queue[insert_index].packet_type = packet_type;
        network->queue[insert_index].sequence = sequence;
        network->queue[insert_index].arrival_time = arrival_time;
        network->queue[insert_index].length = length;
        if (length > 0) {
            memcpy(network->queue[insert_index].bytes, bytes, length);
        }
        network->queue_size++;
        network->packets_sent++;
    }

    return ret;
}

static int picowt_wire_network_pop(picowt_wire_network_t* network,
    uint64_t current_time, picowt_wire_network_packet_t* packet)
{
    int ret = 0;

    if (network->queue_size > 0 && network->queue[0].arrival_time <= current_time) {
        *packet = network->queue[0];
        memmove(network->queue, network->queue + 1,
            (network->queue_size - 1) * sizeof(picowt_wire_network_packet_t));
        network->queue_size--;
        ret = 1;
    }

    return ret;
}

static int picowt_wire_network_expect_packet(
    const picowt_wire_network_packet_t* packet, picowt_wire_packet_type_t packet_type,
    const uint8_t* bytes, size_t length)
{
    int ret = 0;

    if (packet->packet_type != packet_type ||
        packet->length != length ||
        memcmp(packet->bytes, bytes, length) != 0) {
        ret = -1;
    }

    return ret;
}

static void picowt_wire_malformed_reset(picowt_wire_malformed_sample_t* sample,
    picowt_wire_fragment_target_t target)
{
    memset(sample, 0, sizeof(picowt_wire_malformed_sample_t));
    sample->target = target;
}

static int picowt_wire_malformed_append_varint(picowt_wire_malformed_sample_t* sample,
    uint64_t value)
{
    uint8_t* bytes = sample->bytes + sample->length;
    uint8_t* bytes_max = sample->bytes + sizeof(sample->bytes);

    bytes = picoquic_frames_varint_encode(bytes, bytes_max, value);
    if (bytes == NULL) {
        return -1;
    }

    sample->length = bytes - sample->bytes;
    return 0;
}

static int picowt_wire_malformed_append_bytes(picowt_wire_malformed_sample_t* sample,
    const uint8_t* bytes, size_t length)
{
    int ret = 0;

    if (sample->length + length > sizeof(sample->bytes)) {
        ret = -1;
    }
    else {
        memcpy(sample->bytes + sample->length, bytes, length);
        sample->length += length;
    }

    return ret;
}

static int picowt_wire_malformed_build_tlv(picowt_wire_malformed_sample_t* sample,
    picowt_wire_fragment_target_t target, uint64_t type, size_t declared_length,
    const uint8_t* payload, size_t payload_length)
{
    int ret;

    picowt_wire_malformed_reset(sample, target);
    sample->type = type;
    sample->declared_length = declared_length;
    sample->payload_length = payload_length;

    ret = picowt_wire_malformed_append_varint(sample, type);
    if (ret == 0) {
        ret = picowt_wire_malformed_append_varint(sample, declared_length);
    }
    if (ret == 0) {
        sample->payload_offset = sample->length;
        ret = picowt_wire_malformed_append_bytes(sample, payload, payload_length);
    }

    return ret;
}

static int picowt_wire_malformed_build_h3_frame(picowt_wire_malformed_sample_t* sample)
{
    const uint8_t payload[] = { 0x68, 0x33 };

    return picowt_wire_malformed_build_tlv(sample, picowt_wire_fragment_h3_frame,
        h3zero_frame_data, sizeof(payload) + 2, payload, sizeof(payload));
}

static int picowt_wire_malformed_build_settings(picowt_wire_malformed_sample_t* sample)
{
    uint8_t payload[8];
    uint8_t* bytes = payload;
    uint8_t* bytes_max = payload + sizeof(payload);

    bytes = picoquic_frames_varint_encode(bytes, bytes_max, h3zero_setting_h3_datagram);
    if (bytes == NULL) {
        return -1;
    }

    return picowt_wire_malformed_build_tlv(sample, picowt_wire_fragment_settings,
        h3zero_frame_settings, bytes - payload, payload, bytes - payload);
}

static int picowt_wire_malformed_build_capsule(picowt_wire_malformed_sample_t* sample)
{
    const uint8_t payload[] = { 0xca, 0xfe };

    return picowt_wire_malformed_build_tlv(sample, picowt_wire_fragment_capsule,
        H3ZERO_CAPSULE_CLOSE_WEBTRANSPORT_SESSION, sizeof(payload) + 2,
        payload, sizeof(payload));
}

static int picowt_wire_malformed_build_stream_prefix(picowt_wire_malformed_sample_t* sample)
{
    const uint8_t prefix[] = { 0x40 };

    picowt_wire_malformed_reset(sample, picowt_wire_fragment_stream_prefix);
    sample->type = h3zero_frame_webtransport_stream;
    sample->declared_length = 2;
    sample->payload_length = sizeof(prefix);
    return picowt_wire_malformed_append_bytes(sample, prefix, sizeof(prefix));
}

static int picowt_wire_malformed_build_datagram(picowt_wire_malformed_sample_t* sample)
{
    const uint8_t quarter_stream_id[] = { 0x40 };

    picowt_wire_malformed_reset(sample, picowt_wire_fragment_datagram);
    sample->payload_length = sizeof(quarter_stream_id);
    return picowt_wire_malformed_append_bytes(sample, quarter_stream_id,
        sizeof(quarter_stream_id));
}

static int picowt_wire_malformed_build_qpack_header(picowt_wire_malformed_sample_t* sample)
{
    const uint8_t qpack_prefix[] = { 0x00 };

    picowt_wire_malformed_reset(sample, picowt_wire_fragment_qpack_header);
    sample->payload_length = sizeof(qpack_prefix);
    return picowt_wire_malformed_append_bytes(sample, qpack_prefix, sizeof(qpack_prefix));
}

static int picowt_wire_malformed_expect_tlv(
    const picowt_wire_malformed_sample_t* sample)
{
    const uint8_t* bytes = sample->bytes;
    const uint8_t* bytes_max = sample->bytes + sample->length;
    uint64_t decoded_type = UINT64_MAX;
    uint64_t decoded_length = UINT64_MAX;
    int ret = 0;

    if ((bytes = picoquic_frames_varint_decode(bytes, bytes_max, &decoded_type)) == NULL ||
        (bytes = picoquic_frames_varint_decode(bytes, bytes_max, &decoded_length)) == NULL ||
        decoded_type != sample->type ||
        decoded_length != sample->declared_length ||
        (size_t)(bytes - sample->bytes) != sample->payload_offset ||
        (size_t)(bytes_max - bytes) != sample->payload_length) {
        ret = -1;
    }

    return ret;
}

static int picowt_wire_fragment_all_boundaries(picowt_wire_fragment_target_t target,
    const uint8_t* bytes, size_t length, picowt_wire_fragment_cb_fn callback, void* callback_ctx)
{
    int ret = 0;

    for (size_t boundary = 0; ret == 0 && boundary <= length; boundary++) {
        picowt_wire_fragment_chunk_t chunk = {
            target,
            bytes,
            boundary,
            boundary,
            0,
            boundary == length
        };

        ret = callback(&chunk, callback_ctx);

        if (ret == 0 && boundary < length) {
            chunk.bytes = bytes + boundary;
            chunk.length = length - boundary;
            chunk.chunk_index = 1;
            chunk.is_last = 1;

            ret = callback(&chunk, callback_ctx);
        }
    }

    return ret;
}

static int picowt_wire_fragment_varint_cb(
    const picowt_wire_fragment_chunk_t* chunk, void* callback_ctx)
{
    picowt_wire_fragment_varint_ctx_t* ctx = (picowt_wire_fragment_varint_ctx_t*)callback_ctx;
    const uint8_t* bytes = chunk->bytes;
    const uint8_t* bytes_max = bytes + chunk->length;
    int ret = 0;

    if (chunk->chunk_index == 0) {
        ctx->decoded = UINT64_MAX;
        ctx->buffer_length = 0;
    }

    while (ret == 0 && bytes != NULL && bytes < bytes_max && ctx->decoded == UINT64_MAX) {
        bytes = h3zero_varint_from_stream((uint8_t*)bytes, (uint8_t*)bytes_max,
            &ctx->decoded, ctx->buffer, &ctx->buffer_length);
    }

    if (bytes == NULL) {
        ret = -1;
    }
    else if (!chunk->is_last && ctx->decoded != UINT64_MAX) {
        ret = -1;
    }
    else if (chunk->is_last && ctx->decoded != ctx->expected) {
        ret = -1;
    }

    return ret;
}

static void picowt_wire_fragment_frame_ctx_reset(picowt_wire_fragment_frame_ctx_t* ctx)
{
    ctx->frame_type = UINT64_MAX;
    ctx->frame_length = UINT64_MAX;
    ctx->buffer_length = 0;
    ctx->payload_seen = 0;
}

static int picowt_wire_fragment_frame_cb(
    const picowt_wire_fragment_chunk_t* chunk, void* callback_ctx)
{
    picowt_wire_fragment_frame_ctx_t* ctx = (picowt_wire_fragment_frame_ctx_t*)callback_ctx;
    const uint8_t* bytes = chunk->bytes;
    const uint8_t* bytes_max = bytes + chunk->length;
    int ret = 0;

    if (chunk->chunk_index == 0) {
        picowt_wire_fragment_frame_ctx_reset(ctx);
    }

    while (ret == 0 && bytes != NULL && bytes < bytes_max) {
        if (ctx->frame_type == UINT64_MAX) {
            bytes = h3zero_varint_from_stream((uint8_t*)bytes, (uint8_t*)bytes_max,
                &ctx->frame_type, ctx->buffer, &ctx->buffer_length);
        }
        else if (ctx->frame_length == UINT64_MAX) {
            bytes = h3zero_varint_from_stream((uint8_t*)bytes, (uint8_t*)bytes_max,
                &ctx->frame_length, ctx->buffer, &ctx->buffer_length);
        }
        else {
            size_t available = bytes_max - bytes;
            size_t needed = (size_t)ctx->frame_length - ctx->payload_seen;
            size_t copied = (available < needed) ? available : needed;

            if (needed == 0 ||
                ctx->payload_seen + copied > ctx->expected_payload_length ||
                memcmp(bytes, ctx->expected_payload + ctx->payload_seen, copied) != 0) {
                ret = -1;
            }
            else {
                ctx->payload_seen += copied;
                bytes += copied;
            }
        }
    }

    if (bytes == NULL) {
        ret = -1;
    }
    else if (chunk->is_last && (ctx->frame_type != ctx->expected_type ||
        ctx->frame_length != ctx->expected_payload_length ||
        ctx->payload_seen != ctx->expected_payload_length)) {
        ret = -1;
    }

    return ret;
}

static int picowt_wire_malformed_expect_truncated_varint(
    const picowt_wire_malformed_sample_t* sample, uint64_t expected)
{
    picowt_wire_fragment_varint_ctx_t ctx = { expected, UINT64_MAX, { 0 }, 0 };
    picowt_wire_fragment_chunk_t chunk = {
        sample->target,
        sample->bytes,
        sample->length,
        sample->length,
        0,
        1
    };

    return (picowt_wire_fragment_varint_cb(&chunk, &ctx) == 0) ? -1 : 0;
}

static int picowt_wire_malformed_h3_frame_test(
    const picowt_wire_malformed_sample_t* sample)
{
    picowt_wire_fragment_frame_ctx_t ctx = {
        sample->type,
        sample->bytes + sample->payload_offset,
        sample->payload_length,
        UINT64_MAX,
        UINT64_MAX,
        { 0 },
        0,
        0
    };
    picowt_wire_fragment_chunk_t chunk = {
        sample->target,
        sample->bytes,
        sample->length,
        sample->length,
        0,
        1
    };
    int ret = picowt_wire_malformed_expect_tlv(sample);

    if (ret == 0 && picowt_wire_fragment_frame_cb(&chunk, &ctx) == 0) {
        ret = -1;
    }

    return ret;
}

static int picowt_wire_malformed_settings_test(
    const picowt_wire_malformed_sample_t* sample)
{
    h3zero_settings_t settings;
    int ret = picowt_wire_malformed_expect_tlv(sample);

    if (ret == 0 && h3zero_settings_decode(sample->bytes,
        sample->bytes + sample->length, &settings) != NULL) {
        ret = -1;
    }

    return ret;
}

static int picowt_wire_malformed_capsule_test(
    const picowt_wire_malformed_sample_t* sample)
{
    h3zero_capsule_t capsule = { 0 };
    const uint8_t* parsed;
    int ret = picowt_wire_malformed_expect_tlv(sample);

    if (ret == 0) {
        parsed = h3zero_accumulate_capsule(sample->bytes,
            sample->bytes + sample->length, &capsule);
        if (parsed != sample->bytes + sample->length ||
            !capsule.is_length_known ||
            capsule.is_stored ||
            capsule.capsule_type != sample->type ||
            capsule.capsule_length != sample->declared_length ||
            capsule.value_read != sample->payload_length) {
            ret = -1;
        }
    }

    h3zero_release_capsule(&capsule);
    return ret;
}

static int picowt_wire_malformed_qpack_test(
    const picowt_wire_malformed_sample_t* sample)
{
    h3zero_header_parts_t parts;
    uint8_t* parsed = h3zero_parse_qpack_header_frame(
        (uint8_t*)sample->bytes, (uint8_t*)sample->bytes + sample->length, &parts);
    int ret = (parsed == NULL) ? 0 : -1;

    h3zero_release_header_parts(&parts);
    return ret;
}

static int picowt_wire_expect_no_connection_error(picowt_wire_harness_t* harness)
{
    int ret = picowt_wire_expect_uint64("client local error", 0,
        harness->test_ctx->cnx_client->local_error);

    if (ret == 0) {
        ret = picowt_wire_expect_uint64("client remote error", 0,
            harness->test_ctx->cnx_client->remote_error);
    }
    if (ret == 0) {
        ret = picowt_wire_expect_uint64("server local error", 0,
            harness->test_ctx->cnx_server->local_error);
    }
    if (ret == 0) {
        ret = picowt_wire_expect_uint64("server remote error", 0,
            harness->test_ctx->cnx_server->remote_error);
    }

    return ret;
}

static int picowt_wire_expect_wt_settings(char const* what,
    const h3zero_settings_t* settings, int expect_received)
{
    int ret = 0;

    if (settings == NULL ||
        (expect_received && !settings->settings_received) ||
        settings->enable_connect_protocol == 0 ||
        settings->h3_datagram == 0 ||
        settings->webtransport_enabled == 0) {
        DBG_PRINTF("%s: missing required WebTransport H3 settings", what);
        ret = -1;
    }

    return ret;
}

static int picowt_wire_expect_wt_transport_parameters(char const* what,
    picoquic_cnx_t* cnx)
{
    const picoquic_tp_t* local_tp = picoquic_get_transport_parameters(cnx, 1);
    const picoquic_tp_t* remote_tp = picoquic_get_transport_parameters(cnx, 0);
    int ret = 0;

    if (local_tp == NULL || remote_tp == NULL ||
        local_tp->max_datagram_frame_size == 0 ||
        remote_tp->max_datagram_frame_size == 0 ||
        local_tp->is_reset_stream_at_enabled == 0 ||
        remote_tp->is_reset_stream_at_enabled == 0) {
        DBG_PRINTF("%s: missing required WebTransport transport parameters", what);
        ret = -1;
    }

    return ret;
}

int picowt_wire_malformed_builder_test(void)
{
    picowt_wire_malformed_sample_t sample;
    int ret = picowt_wire_malformed_build_h3_frame(&sample);

    if (ret == 0) {
        ret = picowt_wire_malformed_h3_frame_test(&sample);
    }
    if (ret == 0) {
        ret = picowt_wire_malformed_build_settings(&sample);
    }
    if (ret == 0) {
        ret = picowt_wire_malformed_settings_test(&sample);
    }
    if (ret == 0) {
        ret = picowt_wire_malformed_build_capsule(&sample);
    }
    if (ret == 0) {
        ret = picowt_wire_malformed_capsule_test(&sample);
    }
    if (ret == 0) {
        ret = picowt_wire_malformed_build_stream_prefix(&sample);
    }
    if (ret == 0) {
        ret = picowt_wire_malformed_expect_truncated_varint(&sample,
            h3zero_frame_webtransport_stream);
    }
    if (ret == 0) {
        ret = picowt_wire_malformed_build_datagram(&sample);
    }
    if (ret == 0) {
        ret = picowt_wire_malformed_expect_truncated_varint(&sample, 0);
    }
    if (ret == 0) {
        ret = picowt_wire_malformed_build_qpack_header(&sample);
    }
    if (ret == 0) {
        ret = picowt_wire_malformed_qpack_test(&sample);
    }

    return ret;
}

int picowt_wire_network_controls_test(void)
{
    picowt_wire_network_config_t config = { 1, 1, 100 };
    picowt_wire_network_t network;
    picowt_wire_network_packet_t packet;
    uint8_t stream_drop[] = { 0x41 };
    uint8_t stream_late[] = { 0x41, 0x00, 0xa1 };
    uint8_t stream_early[] = { 0x41, 0x00, 0xa0 };
    uint8_t datagram_drop[] = { 0x00, 0xd0 };
    uint8_t datagram_deliver[] = { 0x00, 0xd1 };
    int ret;

    picowt_wire_network_init(&network, &config);

    ret = picowt_wire_network_submit(&network, picowt_wire_packet_stream,
        stream_drop, sizeof(stream_drop), 100, 0);
    if (ret == 0) {
        ret = picowt_wire_expect_uint64("stream drops", 1, network.packets_dropped);
    }
    if (ret == 0) {
        ret = picowt_wire_network_submit(&network, picowt_wire_packet_stream,
            stream_late, sizeof(stream_late), 100, 400);
    }
    if (ret == 0) {
        ret = picowt_wire_network_submit(&network, picowt_wire_packet_stream,
            stream_early, sizeof(stream_early), 100, 0);
    }
    if (ret == 0 && picowt_wire_network_pop(&network, 200, &packet) != 1) {
        ret = -1;
    }
    if (ret == 0) {
        ret = picowt_wire_network_expect_packet(&packet, picowt_wire_packet_stream,
            stream_early, sizeof(stream_early));
    }
    if (ret == 0 && picowt_wire_network_pop(&network, 200, &packet) != 0) {
        ret = -1;
    }
    if (ret == 0) {
        ret = picowt_wire_network_submit(&network, picowt_wire_packet_datagram,
            datagram_drop, sizeof(datagram_drop), 100, 0);
    }
    if (ret == 0) {
        ret = picowt_wire_expect_uint64("datagram drops", 2, network.packets_dropped);
    }
    if (ret == 0) {
        ret = picowt_wire_network_submit(&network, picowt_wire_packet_datagram,
            datagram_deliver, sizeof(datagram_deliver), 100, 20);
    }
    if (ret == 0 && picowt_wire_network_pop(&network, 220, &packet) != 1) {
        ret = -1;
    }
    if (ret == 0) {
        ret = picowt_wire_network_expect_packet(&packet, picowt_wire_packet_datagram,
            datagram_deliver, sizeof(datagram_deliver));
    }
    if (ret == 0 && picowt_wire_network_pop(&network, 600, &packet) != 1) {
        ret = -1;
    }
    if (ret == 0) {
        ret = picowt_wire_network_expect_packet(&packet, picowt_wire_packet_stream,
            stream_late, sizeof(stream_late));
    }
    if (ret == 0) {
        ret = picowt_wire_expect_uint64("packets sent", 3, network.packets_sent);
    }
    if (ret == 0) {
        ret = picowt_wire_expect_uint64("packets queued", 0, network.queue_size);
    }

    return ret;
}

static void picowt_wire_harness_dispose(picowt_wire_harness_t* harness)
{
    if (harness->client_h3_ctx != NULL) {
        h3zero_callback_delete_context(
            (harness->test_ctx == NULL) ? NULL : harness->test_ctx->cnx_client,
            harness->client_h3_ctx);
        if (harness->test_ctx != NULL &&
            harness->test_ctx->cnx_client != NULL) {
            picoquic_set_callback(harness->test_ctx->cnx_client, NULL, NULL);
        }
        harness->client_h3_ctx = NULL;
        harness->client_control_stream = NULL;
    }

    if (harness->test_ctx != NULL) {
        tls_api_delete_ctx(harness->test_ctx);
        harness->test_ctx = NULL;
    }
}

static int picowt_wire_harness_init(picowt_wire_harness_t* harness)
{
    picoquic_connection_id_t initial_cid = { { 0x77, 0x74, 0xc0, 0x02, 0, 0, 0, 0 }, 8 };
    int ret = 0;

    memset(harness, 0, sizeof(picowt_wire_harness_t));

    ret = tls_api_init_ctx_ex(&harness->test_ctx,
        PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, "h3", &harness->simulated_time, NULL, NULL, 0, 1, 0, &initial_cid);

    if (ret == 0) {
        picowt_set_default_transport_parameters(harness->test_ctx->qserver);
        picowt_set_transport_parameters(harness->test_ctx->cnx_client);

        picoquic_set_alpn_select_fn_v2(harness->test_ctx->qserver,
            picoquic_demo_server_callback_select_alpn);
        picoquic_set_default_callback(harness->test_ctx->qserver,
            h3zero_callback, &harness->server_param);

        ret = picowt_prepare_client_cnx(harness->test_ctx->qclient, NULL,
            &harness->test_ctx->cnx_client, &harness->client_h3_ctx,
            &harness->client_control_stream, harness->simulated_time, PICOQUIC_TEST_SNI);
    }

    return ret;
}

static int picowt_wire_harness_connect(picowt_wire_harness_t* harness)
{
    int ret = picoquic_start_client_cnx(harness->test_ctx->cnx_client);

    if (ret == 0) {
        ret = tls_api_connection_loop(harness->test_ctx,
            &harness->loss_mask, 0, &harness->simulated_time);
    }

    return ret;
}

static int picowt_wire_harness_wait_for_server_settings(picowt_wire_harness_t* harness,
    h3zero_callback_ctx_t** server_h3_ctx)
{
    uint64_t time_out = harness->simulated_time + 1000000;
    int was_active = 0;
    int nb_rounds = 0;
    int ret = 0;

    do {
        *server_h3_ctx = (h3zero_callback_ctx_t*)picoquic_get_callback_context(
            harness->test_ctx->cnx_server);
        if (*server_h3_ctx != NULL && (*server_h3_ctx)->settings.settings_received) {
            break;
        }
        ret = tls_api_one_sim_round(harness->test_ctx, &harness->simulated_time,
            time_out, &was_active);
        nb_rounds++;
    } while (ret == 0 && nb_rounds < 1000 &&
        picoquic_get_cnx_state(harness->test_ctx->cnx_client) != picoquic_state_disconnected &&
        harness->simulated_time < time_out);

    if (ret == 0 &&
        (*server_h3_ctx == NULL || !(*server_h3_ctx)->settings.settings_received)) {
        ret = -1;
    }

    return ret;
}

int picowt_wire_harness_test(void)
{
    picowt_wire_harness_t harness;
    int ret = picowt_wire_harness_init(&harness);

    if (ret == 0) {
        ret = picowt_wire_harness_connect(&harness);
    }

    if (ret == 0 && (harness.test_ctx->cnx_client == NULL ||
        harness.test_ctx->cnx_server == NULL ||
        harness.client_h3_ctx == NULL ||
        harness.client_control_stream == NULL ||
        !harness.client_h3_ctx->settings.settings_received)) {
        ret = -1;
    }

    if (ret == 0) {
        ret = picowt_wire_expect_no_connection_error(&harness);
    }

    picowt_wire_harness_dispose(&harness);
    return ret;
}

int picowt_wire_capability_negotiation_test(void)
{
    picowt_wire_harness_t harness;
    h3zero_callback_ctx_t* server_h3_ctx = NULL;
    int ret = picowt_wire_harness_init(&harness);

    if (ret == 0) {
        ret = picowt_wire_harness_connect(&harness);
    }
    if (ret == 0) {
        ret = picowt_wire_harness_wait_for_server_settings(&harness, &server_h3_ctx);
        if (ret == 0 && harness.client_h3_ctx == NULL) {
            ret = -1;
        }
    }
    if (ret == 0) {
        ret = picowt_wire_expect_wt_settings("client local settings",
            &harness.client_h3_ctx->local_settings, 0);
    }
    if (ret == 0) {
        ret = picowt_wire_expect_wt_settings("client remote settings",
            &harness.client_h3_ctx->settings, 1);
    }
    if (ret == 0) {
        ret = picowt_wire_expect_wt_settings("server local settings",
            &server_h3_ctx->local_settings, 0);
    }
    if (ret == 0) {
        ret = picowt_wire_expect_wt_settings("server remote settings",
            &server_h3_ctx->settings, 1);
    }
    if (ret == 0) {
        ret = picowt_wire_expect_wt_transport_parameters("client transport parameters",
            harness.test_ctx->cnx_client);
    }
    if (ret == 0) {
        ret = picowt_wire_expect_wt_transport_parameters("server transport parameters",
            harness.test_ctx->cnx_server);
    }
    if (ret == 0 && (!h3zero_webtransport_is_ready(harness.test_ctx->cnx_client,
        &harness.client_h3_ctx->settings) ||
        !h3zero_webtransport_is_ready(harness.test_ctx->cnx_server,
            &server_h3_ctx->settings))) {
        ret = -1;
    }
    if (ret == 0) {
        ret = picowt_wire_expect_no_connection_error(&harness);
    }

    picowt_wire_harness_dispose(&harness);
    return ret;
}

int picowt_wire_fragment_test(void)
{
    uint8_t buffer[64];
    uint8_t* bytes = buffer;
    uint8_t* bytes_max = buffer + sizeof(buffer);
    uint8_t frame_payload[] = { 0xa0, 0xa1, 0xa2 };
    picowt_wire_fragment_varint_ctx_t varint_ctx = { 0 };
    picowt_wire_fragment_frame_ctx_t frame_ctx = { 0 };
    int ret;

    varint_ctx.expected = h3zero_frame_webtransport_stream;
    bytes = picoquic_frames_varint_encode(bytes, bytes_max, varint_ctx.expected);
    if (bytes == NULL) {
        ret = -1;
    }
    else {
        ret = picowt_wire_fragment_all_boundaries(
            picowt_wire_fragment_stream_prefix, buffer, bytes - buffer,
            picowt_wire_fragment_varint_cb, &varint_ctx);
    }

    if (ret == 0) {
        bytes = buffer;
        frame_ctx.expected_type = h3zero_frame_settings;
        frame_ctx.expected_payload = frame_payload;
        frame_ctx.expected_payload_length = sizeof(frame_payload);
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, frame_ctx.expected_type);
        if (bytes != NULL) {
            bytes = picoquic_frames_varint_encode(bytes, bytes_max, sizeof(frame_payload));
        }
        if (bytes == NULL || bytes + sizeof(frame_payload) > bytes_max) {
            ret = -1;
        }
        else {
            memcpy(bytes, frame_payload, sizeof(frame_payload));
            bytes += sizeof(frame_payload);
            ret = picowt_wire_fragment_all_boundaries(
                picowt_wire_fragment_h3_frame, buffer, bytes - buffer,
                picowt_wire_fragment_frame_cb, &frame_ctx);
        }
    }

    return ret;
}
