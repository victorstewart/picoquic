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

static int picowt_wire_expect_uint64(char const* what, uint64_t expected, uint64_t actual)
{
    int ret = 0;

    if (expected != actual) {
        DBG_PRINTF("%s: expected 0x%" PRIx64 ", got 0x%" PRIx64, what, expected, actual);
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

static void picowt_wire_harness_dispose(picowt_wire_harness_t* harness)
{
    if (harness->client_h3_ctx != NULL) {
        h3zero_callback_delete_context(
            (harness->test_ctx == NULL) ? NULL : harness->test_ctx->cnx_client,
            harness->client_h3_ctx);
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
