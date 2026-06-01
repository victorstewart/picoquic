/*
* Author: Christian Huitema
* Copyright (c) 2024, Private Octopus, Inc.
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
#include <stdlib.h>
#include <stdio.h>
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "picoquictest_internal.h"
#include "tls_api.h"
#include "h3zero.h"
#include "h3zero_common.h"
#include "democlient.h"
#include "demoserver.h"
#ifdef _WINDOWS
#include "wincompat.h"
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif
#if 0
/* Include picotls.h in order to support tests of ESNI */
#include "picotls.h"
#include "tls_api.h"
#endif
#include "autoqlog.h"
#include "picoquic_binlog.h"
#include "pico_webtransport.h"

/* testing:
 * uint8_t * h3zero_varint_from_stream(uint8_t* bytes, uint8_t* bytes_max, uint64_t * result, uint8_t * buffer, size_t* buffer_length)
 *
 * start with a stream encoding, made of a set of bytes, encoding a number of varint puls some extra bytes.
 * the state is captured in a decoded varint vector of size N.
 * the vector is initialized to UINT64_MAX.
 * the logic:
 *   get the encoded buffer that contains the encoded value of the varints, as a string of bytes
 *   feed that buffer to the decoder in multiple ways:
 *    - all bytes at once,
 *    - one byte at a time,
 *    - two bytes at a time.
 *   The decoder itself will try to decode the next varint in the record, and consume bytes.
 *   if the varint value is not UINT64_MAX, go to the next one, etc.
 */

typedef struct st_h3zero_varint_stream_test_t {
    uint64_t v_int[4];
    uint64_t targets[4];
    size_t nb_targets;
    uint8_t buffer[16];
    size_t buffer_length;
    uint8_t bytes[64];
    size_t nb_bytes;
    size_t nb_processed;
} h3zero_varint_stream_test_t;

static int h3zero_varint_stream_test_init(h3zero_varint_stream_test_t * hvst, uint64_t * targets, size_t nb_targets)
{
    int ret = 0;
    uint8_t * bytes = hvst->bytes;
    uint8_t * bytes_max = bytes + sizeof(hvst->bytes);

    memset(hvst, 0, sizeof(h3zero_varint_stream_test_t));
    if (nb_targets > 4) {
        ret = -1;
    }
    else {
        hvst->nb_targets = nb_targets;
        for (size_t i = 0; i < nb_targets && i < 4; i++) {
            hvst->targets[i] = targets[i];
            hvst->v_int[i] = UINT64_MAX;
            bytes = picoquic_frames_varint_encode(bytes, bytes_max, targets[i]);
            if (bytes == NULL) {
                ret = -1;
                break;
            }
        }
        if (ret == 0) {
            hvst->nb_bytes = bytes - hvst->bytes;
        }
    }
    return ret;
}

int h3zero_varint_stream_chunk_test(uint64_t * targets, size_t nb_targets, size_t chunk_bytes)
{
    h3zero_varint_stream_test_t hvst;
    int ret = h3zero_varint_stream_test_init(&hvst, targets, nb_targets);
    size_t nb_not_64max = 0;
    size_t nb_chunks = 0;
    uint8_t* bytes = hvst.bytes;
    uint8_t* bytes_max = hvst.bytes + hvst.nb_bytes;
    uint8_t* chunk_start;
    uint8_t* chunk_end;

    while (ret == 0) {
        chunk_start = hvst.bytes + chunk_bytes * nb_chunks;
        chunk_end = chunk_start + chunk_bytes;
        if (chunk_start >= bytes_max) {
            /* nothing more to feed */
            break;
        }
        else if (chunk_end >= bytes_max) {
            chunk_end = bytes_max;
        }
        nb_chunks++;
        bytes = chunk_start;
        while (bytes != NULL && bytes < chunk_end) {
            bytes = h3zero_varint_from_stream(bytes, chunk_end, &hvst.v_int[nb_not_64max], hvst.buffer, &hvst.buffer_length);
            if (hvst.v_int[nb_not_64max] != UINT64_MAX) {
                nb_not_64max++;
                if (nb_not_64max >= nb_targets) {
                    break;
                }
                continue;
            }
        }
        if (nb_not_64max >= nb_targets) {
            break;
        }
    }
    if (nb_not_64max < nb_targets) {
        ret = -1;
    }
    else {
        for (size_t i = 0; ret == 0 && i < nb_targets; i++) {
            if (hvst.v_int[i] != targets[i]) {
                ret = -1;
                break;
            }
        }
    }
    return ret;
}

int h3zero_varint_stream_test(void)
{
    int ret = 0;
    uint64_t targets[4] = { 132, 4, 0x10001, 0x10000001 };

    for (size_t nb_targets = 1; ret == 0 && nb_targets <= 4; nb_targets++) {
        for (size_t j = 0; ret == 0 && j < 4; j++) {
            size_t chunk_bytes = (size_t)(1 << j);
            ret = h3zero_varint_stream_chunk_test(targets, nb_targets, chunk_bytes);
            if (ret == -1) {
                DBG_PRINTF("varint_stream test fails for chunks size= %zu, nb_target=%zu", chunk_bytes, nb_targets);
            }
        }
    }
    return ret;
}

/*
 * Test of
 *  uint8_t* h3zero_parse_remote_unidir_stream(
 *     uint8_t* bytes, uint8_t* bytes_max,
 *     h3zero_stream_ctx_t* stream_ctx,
 *     h3zero_callback_ctx_t* ctx,
 *     uint64_t * error_found)
 * 
 * uint8_t* h3zero_parse_incoming_remote_stream(
 *    uint8_t* bytes, uint8_t* bytes_max,
 *    h3zero_stream_ctx_t* stream_ctx,
 *    h3zero_callback_ctx_t* ctx)
 * 
 * The test requires that a valid context is defined:
 * 
 * h3zero_stream_ctx_t: incoming stream context.
 */

int incoming_unidir_test_fn(picoquic_cnx_t* UNUSED(cnx),
    uint8_t* UNUSED(bytes), size_t UNUSED(length),
    picohttp_call_back_event_t UNUSED(fin_or_event),
    struct st_h3zero_stream_ctx_t* UNUSED(stream_ctx),
    void* UNUSED(path_app_ctx))
{
    return 0;
}

int h3zero_set_test_context(picoquic_quic_t** quic, picoquic_cnx_t** cnx, h3zero_callback_ctx_t** h3_ctx, uint64_t * simulated_time)
{
    int ret = picoquic_test_set_minimal_cnx_with_time(quic, cnx, simulated_time);
    
    if (ret == 0) {
        *h3_ctx = h3zero_callback_create_context(NULL);
        if (*h3_ctx == NULL) {
            ret = -1;
        }
        else {
            picoquic_set_callback(*cnx, h3zero_callback, *h3_ctx);
        }
    }

    return ret;
}

int h3zero_incoming_unidir_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);
    uint64_t stream_id = 3;
    h3zero_stream_ctx_t* control_stream_ctx;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    uint8_t unidir_input[] = { 0x40, 0x54, 0x04, 0xf0 };

    if (ret == 0) {
        control_stream_ctx  = picowt_set_control_stream(cnx, h3_ctx);
        if (control_stream_ctx == NULL) {
            ret = -1;
        }
        else {
            unidir_input[2] = (uint8_t)control_stream_ctx->stream_id;
            control_stream_ctx->is_upgraded = 1;
            /* Need to program a stream prefix that matches the connection */
            ret = h3zero_declare_stream_prefix(h3_ctx, control_stream_ctx->stream_id, incoming_unidir_test_fn, NULL);
        }
    }

    if (ret == 0) {
        stream_ctx = h3zero_find_or_create_stream(cnx, stream_id, h3_ctx, 1, 1);
        if (stream_ctx == NULL) {
            ret = -1;
        }
    }
    
    picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx);

    if (ret == 0) {
        int success = 0;

        for (size_t i = 0; ret == 0 && i < 4; i++) {
            uint8_t * bytes = &unidir_input[i];
            uint8_t * bytes_max = bytes + 1;
            bytes = h3zero_parse_incoming_remote_stream(bytes, bytes_max, stream_ctx, h3_ctx, NULL);
            if (bytes == bytes_max) {
                continue;
            }
            else if (bytes == NULL) {
                ret = -1;
            }
            else if (bytes != &unidir_input[3]) {
                ret = -1;
            }
            else {
                success = 1;
            }
        }
        if (!success) {
            ret = -1;
        }
    }
    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int h3zero_process_remote_stream(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event,
    h3zero_stream_ctx_t* stream_ctx,
    h3zero_callback_ctx_t* ctx);

int h3zero_process_h3_server_data(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, h3zero_callback_ctx_t* ctx,
    h3zero_stream_ctx_t* stream_ctx);

int h3zero_callback_datagram(picoquic_cnx_t* cnx, uint8_t* bytes, size_t length,
    h3zero_callback_ctx_t* h3_ctx);

int h3zero_wt_id_error_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    uint64_t stream_id = 3;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    uint8_t unidir_input[] = { 0x40, 0x54, 0x02 };
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        stream_ctx = h3zero_find_or_create_stream(cnx, stream_id, h3_ctx, 1, 1);
        if (stream_ctx == NULL) {
            ret = -1;
        }
    }

    if (ret == 0) {
        cnx->cnx_state = picoquic_state_ready;
        ret = h3zero_process_remote_stream(cnx, stream_id, unidir_input, sizeof(unidir_input),
            picoquic_callback_stream_data, stream_ctx, h3_ctx);
        if (ret != 0 || cnx->application_error != H3ZERO_ID_ERROR) {
            DBG_PRINTF("Invalid WT stream ID error: ret=%d, app_error=%" PRIu64,
                ret, cnx->application_error);
            ret = -1;
        }
    }

    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

typedef struct st_h3zero_wt_zero_buffer_test_ctx_t {
    int nb_data;
    int nb_datagrams;
} h3zero_wt_zero_buffer_test_ctx_t;

static int h3zero_wt_zero_buffer_callback(picoquic_cnx_t* UNUSED(cnx),
    uint8_t* bytes, size_t length, picohttp_call_back_event_t fin_or_event,
    struct st_h3zero_stream_ctx_t* UNUSED(stream_ctx), void* path_app_ctx)
{
    h3zero_wt_zero_buffer_test_ctx_t* ctx = (h3zero_wt_zero_buffer_test_ctx_t*)path_app_ctx;

    if (fin_or_event == picohttp_callback_post_data &&
        length == 1 && bytes != NULL && bytes[0] == 0xf0) {
        ctx->nb_data++;
    }
    else if (fin_or_event == picohttp_callback_post_datagram &&
        length == 1 && bytes != NULL && bytes[0] == 0xd0) {
        ctx->nb_datagrams++;
    }
    return 0;
}

static int h3zero_wt_create_stream_pair(picoquic_cnx_t* cnx, h3zero_callback_ctx_t* h3_ctx,
    uint64_t stream_id, h3zero_stream_ctx_t** stream_ctx)
{
    int ret = 0;

    if (picoquic_create_stream(cnx, stream_id) == NULL) {
        ret = -1;
    }
    else if ((*stream_ctx = h3zero_find_or_create_stream(cnx, stream_id, h3_ctx, 1, 1)) == NULL) {
        ret = -1;
    }
    else {
        picoquic_set_app_stream_ctx(cnx, stream_id, *stream_ctx);
    }
    return ret;
}

int h3zero_wt_zero_buffer_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    const uint64_t session_id = 4;
    h3zero_stream_ctx_t* session_ctx = NULL;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    h3zero_wt_zero_buffer_test_ctx_t test_ctx = { 0, 0 };
    uint8_t unidir_input[] = { 0x40, 0x54, 0x04, 0xf0 };
    uint8_t bidi_input[] = { 0x40, 0x41, 0x04, 0xf0 };
    uint8_t datagram_input[] = { 0x01, 0xd0 };
    uint8_t capsule_payload[] = { 0xd0 };
    h3zero_capsule_t capsule = { 0 };
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    capsule.capsule_buffer = capsule_payload;
    capsule.capsule_length = sizeof(capsule_payload);

    if (ret == 0) {
        cnx->client_mode = 0;
        cnx->cnx_state = picoquic_state_ready;
        session_ctx = h3zero_find_or_create_stream(cnx, session_id, h3_ctx, 1, 1);
        if (session_ctx == NULL ||
            h3zero_declare_stream_prefix(h3_ctx, session_id, h3zero_wt_zero_buffer_callback, &test_ctx) != 0) {
            ret = -1;
        }
    }

    if (ret == 0) {
        ret = h3zero_callback_datagram(cnx, datagram_input, sizeof(datagram_input), h3_ctx);
        h3zero_receive_datagram_capsule(cnx, session_ctx, &capsule, h3_ctx);
        if (ret != 0 || test_ctx.nb_datagrams != 0 || test_ctx.nb_data != 0) {
            DBG_PRINTF("Pre-response datagram was not dropped, ret=%d, data=%d, dg=%d",
                ret, test_ctx.nb_data, test_ctx.nb_datagrams);
            ret = -1;
        }
    }

    if (ret == 0) {
        const uint64_t stream_id = 2;
        picoquic_stream_head_t* stream;

        ret = h3zero_wt_create_stream_pair(cnx, h3_ctx, stream_id, &stream_ctx);
        if (ret == 0) {
            ret = h3zero_process_remote_stream(cnx, stream_id, unidir_input, sizeof(unidir_input),
                picoquic_callback_stream_data, stream_ctx, h3_ctx);
        }
        stream = picoquic_find_stream(cnx, stream_id);
        if (ret != 0 || stream == NULL || !stream->stop_sending_requested ||
            stream->local_stop_error != H3ZERO_WEBTRANSPORT_BUFFERED_STREAM_REJECTED ||
            cnx->application_error != 0 || test_ctx.nb_data != 0) {
            DBG_PRINTF("Pre-response unidir reject failed, ret=%d, app_error=%" PRIu64,
                ret, cnx->application_error);
            ret = -1;
        }
    }

    if (ret == 0) {
        const uint64_t stream_id = 8;
        picoquic_stream_head_t* stream;

        ret = h3zero_wt_create_stream_pair(cnx, h3_ctx, stream_id, &stream_ctx);
        if (ret == 0) {
            ret = h3zero_process_h3_server_data(cnx, stream_id, bidi_input, sizeof(bidi_input),
                picoquic_callback_stream_data, h3_ctx, stream_ctx);
        }
        stream = picoquic_find_stream(cnx, stream_id);
        if (ret != 0 || stream == NULL || !stream->stop_sending_requested ||
            stream->local_stop_error != H3ZERO_WEBTRANSPORT_BUFFERED_STREAM_REJECTED ||
            !stream->reset_requested ||
            stream->local_error != H3ZERO_WEBTRANSPORT_BUFFERED_STREAM_REJECTED ||
            cnx->application_error != 0 || test_ctx.nb_data != 0) {
            DBG_PRINTF("Pre-response bidi reject failed, ret=%d, app_error=%" PRIu64,
                ret, cnx->application_error);
            ret = -1;
        }
    }

    if (ret == 0) {
        session_ctx->is_upgraded = 1;
        ret = h3zero_callback_datagram(cnx, datagram_input, sizeof(datagram_input), h3_ctx);
        h3zero_receive_datagram_capsule(cnx, session_ctx, &capsule, h3_ctx);
        if (ret != 0 || test_ctx.nb_datagrams != 2 || test_ctx.nb_data != 0) {
            DBG_PRINTF("Established datagram delivery failed, ret=%d, data=%d, dg=%d",
                ret, test_ctx.nb_data, test_ctx.nb_datagrams);
            ret = -1;
        }
    }

    if (ret == 0) {
        const uint64_t stream_id = 6;

        ret = h3zero_wt_create_stream_pair(cnx, h3_ctx, stream_id, &stream_ctx);
        if (ret == 0) {
            ret = h3zero_process_remote_stream(cnx, stream_id, unidir_input, sizeof(unidir_input),
                picoquic_callback_stream_data, stream_ctx, h3_ctx);
        }
        if (ret != 0 || test_ctx.nb_datagrams != 2 || test_ctx.nb_data != 1) {
            DBG_PRINTF("Established unidir delivery failed, ret=%d, data=%d, dg=%d",
                ret, test_ctx.nb_data, test_ctx.nb_datagrams);
            ret = -1;
        }
    }
    if (ret == 0) {
        const uint64_t stream_id = 12;
        uint8_t bidi_first[] = { 0x40 };
        uint8_t bidi_second[] = { 0x41, 0x04, 0xf0 };

        ret = h3zero_wt_create_stream_pair(cnx, h3_ctx, stream_id, &stream_ctx);
        if (ret == 0) {
            ret = h3zero_process_remote_stream(cnx, stream_id, bidi_first,
                sizeof(bidi_first), picoquic_callback_stream_data,
                stream_ctx, h3_ctx);
        }
        if (ret != 0 || stream_ctx->ps.stream_state.stream_type != UINT64_MAX ||
            cnx->application_error != 0 || test_ctx.nb_data != 1) {
            DBG_PRINTF("Split bidi WT stream prefix first byte failed, ret=%d, app_error=%" PRIu64 ", data=%d",
                ret, cnx->application_error, test_ctx.nb_data);
            ret = -1;
        }
        if (ret == 0) {
            ret = h3zero_process_remote_stream(cnx, stream_id, bidi_second,
                sizeof(bidi_second), picoquic_callback_stream_data,
                stream_ctx, h3_ctx);
        }
        if (ret != 0 ||
            stream_ctx->ps.stream_state.stream_type != h3zero_frame_webtransport_stream ||
            stream_ctx->ps.stream_state.control_stream_id != session_id ||
            cnx->application_error != 0 || test_ctx.nb_data != 2) {
            DBG_PRINTF("Split bidi WT stream prefix completion failed, ret=%d, app_error=%" PRIu64 ", data=%d",
                ret, cnx->application_error, test_ctx.nb_data);
            ret = -1;
        }
    }

    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

static int h3zero_wt_prefix_fragment_case(int is_bidir, size_t split)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    const uint64_t session_id = 4;
    const uint64_t stream_id = is_bidir ? 12 : 6;
    h3zero_stream_ctx_t* session_ctx = NULL;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    h3zero_wt_zero_buffer_test_ctx_t test_ctx = { 0, 0 };
    uint8_t bidi_input[] = { 0x40, 0x41, 0x04, 0xf0 };
    uint8_t unidir_input[] = { 0x40, 0x54, 0x04, 0xf0 };
    uint8_t* input = is_bidir ? bidi_input : unidir_input;
    size_t input_length = is_bidir ? sizeof(bidi_input) : sizeof(unidir_input);
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        cnx->client_mode = 0;
        cnx->cnx_state = picoquic_state_ready;
        session_ctx = h3zero_find_or_create_stream(cnx, session_id, h3_ctx, 1, 1);
        if (session_ctx == NULL ||
            h3zero_declare_stream_prefix(h3_ctx, session_id,
                h3zero_wt_zero_buffer_callback, &test_ctx) != 0) {
            ret = -1;
        }
        else {
            session_ctx->is_upgraded = 1;
        }
    }
    if (ret == 0) {
        ret = h3zero_wt_create_stream_pair(cnx, h3_ctx, stream_id, &stream_ctx);
    }
    if (ret == 0) {
        ret = h3zero_process_remote_stream(cnx, stream_id, input, split,
            picoquic_callback_stream_data, stream_ctx, h3_ctx);
    }
    if (ret != 0 || cnx->application_error != 0 || test_ctx.nb_data != 0) {
        DBG_PRINTF("WT %s prefix split %zu first chunk failed, ret=%d, app_error=%" PRIu64 ", data=%d",
            is_bidir ? "bidi" : "uni", split, ret, cnx->application_error,
            test_ctx.nb_data);
        ret = -1;
    }
    if (ret == 0) {
        ret = h3zero_process_remote_stream(cnx, stream_id, input + split,
            input_length - split, picoquic_callback_stream_data, stream_ctx,
            h3_ctx);
    }
    if (ret != 0 ||
        stream_ctx->ps.stream_state.stream_type !=
            (is_bidir ? h3zero_frame_webtransport_stream : h3zero_stream_type_webtransport) ||
        stream_ctx->ps.stream_state.control_stream_id != session_id ||
        cnx->application_error != 0 || test_ctx.nb_data != 1) {
        DBG_PRINTF("WT %s prefix split %zu completion failed, ret=%d, app_error=%" PRIu64 ", data=%d",
            is_bidir ? "bidi" : "uni", split, ret, cnx->application_error,
            test_ctx.nb_data);
        ret = -1;
    }

    if (cnx != NULL) {
        picoquic_set_callback(cnx, NULL, NULL);
    }
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int h3zero_wt_prefix_fragment_test(void)
{
    int ret = 0;

    for (int is_bidir = 0; ret == 0 && is_bidir <= 1; is_bidir++) {
        for (size_t split = 1; ret == 0 && split < 4; split++) {
            ret = h3zero_wt_prefix_fragment_case(is_bidir, split);
        }
    }

    return ret;
}

static int h3zero_wt_prefix_fin_case(int is_bidir, uint8_t* input, size_t input_length)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    const uint64_t stream_id = is_bidir ? 12 : 6;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    picoquic_stream_head_t* stream = NULL;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        cnx->client_mode = 0;
        cnx->cnx_state = picoquic_state_ready;
        ret = h3zero_wt_create_stream_pair(cnx, h3_ctx, stream_id, &stream_ctx);
    }
    if (ret == 0) {
        ret = h3zero_process_remote_stream(cnx, stream_id, input, input_length,
            picoquic_callback_stream_fin, stream_ctx, h3_ctx);
    }
    stream = (cnx == NULL) ? NULL : picoquic_find_stream(cnx, stream_id);
    if (ret != 0 || stream == NULL || !stream->stop_sending_requested ||
        stream->local_stop_error != H3ZERO_WEBTRANSPORT_BUFFERED_STREAM_REJECTED ||
        (is_bidir && (!stream->reset_requested ||
            stream->local_error != H3ZERO_WEBTRANSPORT_BUFFERED_STREAM_REJECTED)) ||
        (!is_bidir && stream->reset_requested) ||
        cnx->application_error != 0) {
        DBG_PRINTF("WT %s prefix FIN reject failed, ret=%d, app_error=%" PRIu64,
            is_bidir ? "bidi" : "uni", ret,
            (cnx == NULL) ? UINT64_MAX : cnx->application_error);
        ret = -1;
    }

    if (cnx != NULL) {
        picoquic_set_callback(cnx, NULL, NULL);
    }
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int h3zero_wt_prefix_fin_test(void)
{
    uint8_t bidi_partial_type[] = { 0x40 };
    uint8_t bidi_no_session[] = { 0x40, 0x41 };
    uint8_t uni_no_session[] = { 0x40, 0x54 };
    int ret = h3zero_wt_prefix_fin_case(1, bidi_partial_type,
        sizeof(bidi_partial_type));

    if (ret == 0) {
        ret = h3zero_wt_prefix_fin_case(1, bidi_no_session,
            sizeof(bidi_no_session));
    }
    if (ret == 0) {
        ret = h3zero_wt_prefix_fin_case(0, uni_no_session,
            sizeof(uni_no_session));
    }

    return ret;
}

static int h3zero_wt_prefix_reset_case(int is_bidir,
    picoquic_call_back_event_t event, uint8_t* input, size_t input_length)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    const uint64_t stream_id = is_bidir ? 12 : 6;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    picoquic_stream_head_t* stream = NULL;
    int expect_stop = 1;
    int expect_reset = is_bidir;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        cnx->client_mode = 0;
        cnx->cnx_state = picoquic_state_ready;
        ret = h3zero_wt_create_stream_pair(cnx, h3_ctx, stream_id, &stream_ctx);
    }
    if (ret == 0) {
        ret = h3zero_process_remote_stream(cnx, stream_id, input, input_length,
            picoquic_callback_stream_data, stream_ctx, h3_ctx);
    }
    if (ret == 0) {
        ret = h3zero_callback(cnx, stream_id, NULL, 0, event, h3_ctx, stream_ctx);
    }
    stream = (cnx == NULL) ? NULL : picoquic_find_stream(cnx, stream_id);
    if (ret != 0 || stream == NULL ||
        stream->stop_sending_requested != expect_stop ||
        (expect_stop &&
            stream->local_stop_error != H3ZERO_WEBTRANSPORT_BUFFERED_STREAM_REJECTED) ||
        stream->reset_requested != expect_reset ||
        (expect_reset &&
            stream->local_error != H3ZERO_WEBTRANSPORT_BUFFERED_STREAM_REJECTED) ||
        cnx->application_error != 0) {
        DBG_PRINTF("WT %s prefix %s reject failed, ret=%d, app_error=%" PRIu64 ", stop=%d stop_error=%" PRIu64 ", reset=%d reset_error=%" PRIu64,
            is_bidir ? "bidi" : "uni",
            event == picoquic_callback_stream_reset ? "RESET" : "STOP",
            ret, (cnx == NULL) ? UINT64_MAX : cnx->application_error,
            stream == NULL ? -1 : stream->stop_sending_requested,
            stream == NULL ? UINT64_MAX : stream->local_stop_error,
            stream == NULL ? -1 : stream->reset_requested,
            stream == NULL ? UINT64_MAX : stream->local_error);
        ret = -1;
    }

    if (cnx != NULL) {
        picoquic_set_callback(cnx, NULL, NULL);
    }
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int h3zero_wt_prefix_reset_test(void)
{
    uint8_t bidi_partial_type[] = { 0x40 };
    uint8_t bidi_no_session[] = { 0x40, 0x41 };
    uint8_t uni_no_session[] = { 0x40, 0x54 };
    int ret = h3zero_wt_prefix_reset_case(1, picoquic_callback_stream_reset,
        bidi_partial_type, sizeof(bidi_partial_type));

    if (ret == 0) {
        ret = h3zero_wt_prefix_reset_case(1, picoquic_callback_stop_sending,
            bidi_no_session, sizeof(bidi_no_session));
    }
    if (ret == 0) {
        ret = h3zero_wt_prefix_reset_case(0, picoquic_callback_stream_reset,
            uni_no_session, sizeof(uni_no_session));
    }

    return ret;
}

typedef struct st_h3zero_wt_stream_fin_test_ctx_t {
    int nb_data;
    int nb_fin;
} h3zero_wt_stream_fin_test_ctx_t;

static int h3zero_wt_stream_fin_callback(picoquic_cnx_t* UNUSED(cnx),
    uint8_t* UNUSED(bytes), size_t length, picohttp_call_back_event_t fin_or_event,
    struct st_h3zero_stream_ctx_t* UNUSED(stream_ctx), void* path_app_ctx)
{
    h3zero_wt_stream_fin_test_ctx_t* ctx =
        (h3zero_wt_stream_fin_test_ctx_t*)path_app_ctx;

    if (fin_or_event == picohttp_callback_post_data) {
        ctx->nb_data++;
    }
    else if (fin_or_event == picohttp_callback_post_fin && length == 0) {
        ctx->nb_fin++;
    }
    return 0;
}

static int h3zero_wt_stream_empty_fin_case(int is_bidir)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    const uint64_t session_id = 4;
    const uint64_t stream_id = is_bidir ? 12 : 6;
    h3zero_stream_ctx_t* session_ctx = NULL;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    h3zero_wt_stream_fin_test_ctx_t test_ctx = { 0, 0 };
    uint8_t bidi_input[] = { 0x40, 0x41, 0x04 };
    uint8_t unidir_input[] = { 0x40, 0x54, 0x04 };
    uint8_t* input = is_bidir ? bidi_input : unidir_input;
    size_t input_length = is_bidir ? sizeof(bidi_input) : sizeof(unidir_input);
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        cnx->client_mode = 0;
        cnx->cnx_state = picoquic_state_ready;
        session_ctx = h3zero_find_or_create_stream(cnx, session_id, h3_ctx, 1, 1);
        if (session_ctx == NULL ||
            h3zero_declare_stream_prefix(h3_ctx, session_id,
                h3zero_wt_stream_fin_callback, &test_ctx) != 0) {
            ret = -1;
        }
        else {
            session_ctx->is_upgraded = 1;
        }
    }
    if (ret == 0) {
        ret = h3zero_wt_create_stream_pair(cnx, h3_ctx, stream_id, &stream_ctx);
    }
    if (ret == 0) {
        ret = h3zero_process_remote_stream(cnx, stream_id, input, input_length,
            picoquic_callback_stream_fin, stream_ctx, h3_ctx);
    }
    if (ret != 0 || test_ctx.nb_data != 0 || test_ctx.nb_fin != 1 ||
        stream_ctx->ps.stream_state.control_stream_id != session_id ||
        cnx->application_error != 0) {
        DBG_PRINTF("WT %s empty FIN failed, ret=%d, app_error=%" PRIu64 ", data=%d, fin=%d",
            is_bidir ? "bidi" : "uni", ret,
            (cnx == NULL) ? UINT64_MAX : cnx->application_error,
            test_ctx.nb_data, test_ctx.nb_fin);
        ret = -1;
    }

    if (cnx != NULL) {
        picoquic_set_callback(cnx, NULL, NULL);
    }
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int h3zero_wt_stream_empty_fin_test(void)
{
    int ret = h3zero_wt_stream_empty_fin_case(0);

    if (ret == 0) {
        ret = h3zero_wt_stream_empty_fin_case(1);
    }

    return ret;
}

#define H3ZERO_WT_PAYLOAD_ORDER_LEN 4096

typedef struct st_h3zero_wt_payload_order_test_ctx_t {
    size_t nb_data;
    int nb_fin;
    int error_seen;
} h3zero_wt_payload_order_test_ctx_t;

static uint8_t h3zero_wt_payload_order_byte(size_t offset)
{
    return (uint8_t)((offset * 17 + 43) & 0xff);
}

static int h3zero_wt_payload_order_callback(picoquic_cnx_t* UNUSED(cnx),
    uint8_t* bytes, size_t length, picohttp_call_back_event_t fin_or_event,
    struct st_h3zero_stream_ctx_t* UNUSED(stream_ctx), void* path_app_ctx)
{
    h3zero_wt_payload_order_test_ctx_t* ctx =
        (h3zero_wt_payload_order_test_ctx_t*)path_app_ctx;

    if (fin_or_event == picohttp_callback_post_data) {
        for (size_t i = 0; i < length; i++) {
            if (ctx->nb_data >= H3ZERO_WT_PAYLOAD_ORDER_LEN ||
                bytes == NULL ||
                bytes[i] != h3zero_wt_payload_order_byte(ctx->nb_data)) {
                ctx->error_seen = 1;
                break;
            }
            ctx->nb_data++;
        }
    }
    else if (fin_or_event == picohttp_callback_post_fin) {
        ctx->nb_fin++;
    }

    return 0;
}

static int h3zero_wt_payload_order_send(
    picoquic_cnx_t* cnx, h3zero_callback_ctx_t* h3_ctx,
    h3zero_stream_ctx_t* stream_ctx, uint64_t stream_id,
    uint8_t* prefix, size_t prefix_length)
{
    uint8_t buffer[512];
    size_t sent = 0;
    size_t first_payload = sizeof(buffer) - prefix_length;
    int ret = 0;

    if (first_payload > H3ZERO_WT_PAYLOAD_ORDER_LEN) {
        first_payload = H3ZERO_WT_PAYLOAD_ORDER_LEN;
    }
    memcpy(buffer, prefix, prefix_length);
    for (size_t i = 0; i < first_payload; i++) {
        buffer[prefix_length + i] = h3zero_wt_payload_order_byte(i);
    }
    ret = h3zero_process_remote_stream(cnx, stream_id, buffer,
        prefix_length + first_payload, picoquic_callback_stream_data,
        stream_ctx, h3_ctx);
    sent = first_payload;

    while (ret == 0 && sent < H3ZERO_WT_PAYLOAD_ORDER_LEN) {
        size_t chunk = H3ZERO_WT_PAYLOAD_ORDER_LEN - sent;

        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        for (size_t i = 0; i < chunk; i++) {
            buffer[i] = h3zero_wt_payload_order_byte(sent + i);
        }
        ret = h3zero_process_remote_stream(cnx, stream_id, buffer, chunk,
            picoquic_callback_stream_data, stream_ctx, h3_ctx);
        sent += chunk;
    }

    return ret;
}

static int h3zero_wt_payload_order_case(int is_bidir)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    const uint64_t session_id = 4;
    const uint64_t stream_id = is_bidir ? 12 : 6;
    h3zero_stream_ctx_t* session_ctx = NULL;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    h3zero_wt_payload_order_test_ctx_t test_ctx = { 0, 0, 0 };
    uint8_t bidi_prefix[] = { 0x40, 0x41, 0x04 };
    uint8_t unidir_prefix[] = { 0x40, 0x54, 0x04 };
    uint8_t* prefix = is_bidir ? bidi_prefix : unidir_prefix;
    size_t prefix_length = is_bidir ? sizeof(bidi_prefix) : sizeof(unidir_prefix);
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        cnx->client_mode = 0;
        cnx->cnx_state = picoquic_state_ready;
        session_ctx = h3zero_find_or_create_stream(cnx, session_id, h3_ctx, 1, 1);
        if (session_ctx == NULL ||
            h3zero_declare_stream_prefix(h3_ctx, session_id,
                h3zero_wt_payload_order_callback, &test_ctx) != 0) {
            ret = -1;
        }
        else {
            session_ctx->is_upgraded = 1;
        }
    }
    if (ret == 0) {
        ret = h3zero_wt_create_stream_pair(cnx, h3_ctx, stream_id, &stream_ctx);
    }
    if (ret == 0) {
        ret = h3zero_wt_payload_order_send(cnx, h3_ctx, stream_ctx,
            stream_id, prefix, prefix_length);
    }
    if (ret != 0 || test_ctx.error_seen ||
        test_ctx.nb_data != H3ZERO_WT_PAYLOAD_ORDER_LEN ||
        test_ctx.nb_fin != 0 || cnx->application_error != 0) {
        DBG_PRINTF("WT %s payload order failed, ret=%d, app_error=%" PRIu64 ", data=%zu, fin=%d, error=%d",
            is_bidir ? "bidi" : "uni", ret,
            (cnx == NULL) ? UINT64_MAX : cnx->application_error,
            test_ctx.nb_data, test_ctx.nb_fin, test_ctx.error_seen);
        ret = -1;
    }

    if (cnx != NULL) {
        picoquic_set_callback(cnx, NULL, NULL);
    }
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int h3zero_wt_payload_order_test(void)
{
    int ret = h3zero_wt_payload_order_case(0);

    if (ret == 0) {
        ret = h3zero_wt_payload_order_case(1);
    }

    return ret;
}

#define H3ZERO_WT_INTERLEAVE_STREAMS 4
#define H3ZERO_WT_INTERLEAVE_LEN 96

typedef struct st_h3zero_wt_interleave_stream_t {
    uint64_t stream_id;
    size_t nb_data;
    int nb_fin;
    int error_seen;
} h3zero_wt_interleave_stream_t;

typedef struct st_h3zero_wt_interleave_test_ctx_t {
    h3zero_wt_interleave_stream_t streams[H3ZERO_WT_INTERLEAVE_STREAMS];
    int unknown_stream;
} h3zero_wt_interleave_test_ctx_t;

static uint8_t h3zero_wt_interleave_byte(size_t stream_index, size_t offset)
{
    return (uint8_t)((stream_index * 53 + offset * 29 + 7) & 0xff);
}

static size_t h3zero_wt_interleave_find_stream(
    h3zero_wt_interleave_test_ctx_t* ctx, uint64_t stream_id)
{
    for (size_t i = 0; i < H3ZERO_WT_INTERLEAVE_STREAMS; i++) {
        if (ctx->streams[i].stream_id == stream_id) {
            return i;
        }
    }

    return H3ZERO_WT_INTERLEAVE_STREAMS;
}

static int h3zero_wt_interleave_callback(picoquic_cnx_t* UNUSED(cnx),
    uint8_t* bytes, size_t length, picohttp_call_back_event_t fin_or_event,
    struct st_h3zero_stream_ctx_t* stream_ctx, void* path_app_ctx)
{
    h3zero_wt_interleave_test_ctx_t* ctx =
        (h3zero_wt_interleave_test_ctx_t*)path_app_ctx;
    size_t stream_index = h3zero_wt_interleave_find_stream(ctx,
        (stream_ctx == NULL) ? UINT64_MAX : stream_ctx->stream_id);

    if (stream_index >= H3ZERO_WT_INTERLEAVE_STREAMS) {
        ctx->unknown_stream++;
    }
    else if (fin_or_event == picohttp_callback_post_data) {
        h3zero_wt_interleave_stream_t* stream = &ctx->streams[stream_index];

        for (size_t i = 0; i < length; i++) {
            if (stream->nb_data >= H3ZERO_WT_INTERLEAVE_LEN ||
                bytes == NULL ||
                bytes[i] != h3zero_wt_interleave_byte(stream_index,
                    stream->nb_data)) {
                stream->error_seen = 1;
                break;
            }
            stream->nb_data++;
        }
    }
    else if (fin_or_event == picohttp_callback_post_fin) {
        ctx->streams[stream_index].nb_fin++;
    }

    return 0;
}

static int h3zero_wt_interleave_send_chunk(picoquic_cnx_t* cnx,
    h3zero_callback_ctx_t* h3_ctx, h3zero_stream_ctx_t* stream_ctx,
    size_t stream_index, size_t* sent, uint8_t* prefix, size_t prefix_length,
    size_t chunk_length, int include_prefix)
{
    uint8_t buffer[32];
    size_t byte_index = 0;
    int ret = 0;

    if (*sent + chunk_length > H3ZERO_WT_INTERLEAVE_LEN ||
        chunk_length + (include_prefix ? prefix_length : 0) > sizeof(buffer)) {
        ret = -1;
    }
    else {
        if (include_prefix) {
            memcpy(buffer, prefix, prefix_length);
            byte_index = prefix_length;
        }
        for (size_t i = 0; i < chunk_length; i++) {
            buffer[byte_index + i] = h3zero_wt_interleave_byte(
                stream_index, *sent + i);
        }
        ret = h3zero_process_remote_stream(cnx, stream_ctx->stream_id, buffer,
            byte_index + chunk_length, picoquic_callback_stream_data,
            stream_ctx, h3_ctx);
        if (ret == 0) {
            *sent += chunk_length;
        }
    }

    return ret;
}

int h3zero_wt_stream_interleave_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    const uint64_t session_id = 4;
    const uint64_t stream_ids[H3ZERO_WT_INTERLEAVE_STREAMS] = {
        6, 8, 10, 12
    };
    const int is_bidir[H3ZERO_WT_INTERLEAVE_STREAMS] = {
        0, 1, 0, 1
    };
    const size_t initial_order[H3ZERO_WT_INTERLEAVE_STREAMS] = {
        0, 2, 1, 3
    };
    const size_t round_order[H3ZERO_WT_INTERLEAVE_STREAMS] = {
        2, 0, 3, 1
    };
    const size_t fin_order[H3ZERO_WT_INTERLEAVE_STREAMS] = {
        3, 1, 2, 0
    };
    h3zero_stream_ctx_t* session_ctx = NULL;
    h3zero_stream_ctx_t* stream_ctx[H3ZERO_WT_INTERLEAVE_STREAMS] = { 0 };
    h3zero_wt_interleave_test_ctx_t test_ctx = { 0 };
    size_t sent[H3ZERO_WT_INTERLEAVE_STREAMS] = { 0 };
    uint8_t bidi_prefix[] = { 0x40, 0x41, 0x04 };
    uint8_t unidir_prefix[] = { 0x40, 0x54, 0x04 };
    uint8_t fin_byte = 0;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    for (size_t i = 0; i < H3ZERO_WT_INTERLEAVE_STREAMS; i++) {
        test_ctx.streams[i].stream_id = stream_ids[i];
    }

    if (ret == 0) {
        cnx->client_mode = 0;
        cnx->cnx_state = picoquic_state_ready;
        session_ctx = h3zero_find_or_create_stream(cnx, session_id, h3_ctx, 1, 1);
        if (session_ctx == NULL ||
            h3zero_declare_stream_prefix(h3_ctx, session_id,
                h3zero_wt_interleave_callback, &test_ctx) != 0) {
            ret = -1;
        }
        else {
            session_ctx->is_upgraded = 1;
        }
    }
    for (size_t i = 0; ret == 0 && i < H3ZERO_WT_INTERLEAVE_STREAMS; i++) {
        ret = h3zero_wt_create_stream_pair(cnx, h3_ctx, stream_ids[i],
            &stream_ctx[i]);
    }
    for (size_t j = 0; ret == 0 && j < H3ZERO_WT_INTERLEAVE_STREAMS; j++) {
        size_t i = initial_order[j];
        uint8_t* prefix = is_bidir[i] ? bidi_prefix : unidir_prefix;
        size_t prefix_length = is_bidir[i] ? sizeof(bidi_prefix) : sizeof(unidir_prefix);

        ret = h3zero_wt_interleave_send_chunk(cnx, h3_ctx, stream_ctx[i], i,
            &sent[i], prefix, prefix_length, 7, 1);
    }
    while (ret == 0) {
        int complete = 1;

        for (size_t j = 0; ret == 0 && j < H3ZERO_WT_INTERLEAVE_STREAMS; j++) {
            size_t i = round_order[j];

            if (sent[i] < H3ZERO_WT_INTERLEAVE_LEN) {
                size_t chunk = 11 + i;

                if (chunk > H3ZERO_WT_INTERLEAVE_LEN - sent[i]) {
                    chunk = H3ZERO_WT_INTERLEAVE_LEN - sent[i];
                }
                ret = h3zero_wt_interleave_send_chunk(cnx, h3_ctx,
                    stream_ctx[i], i, &sent[i], NULL, 0, chunk, 0);
            }
        }
        for (size_t i = 0; i < H3ZERO_WT_INTERLEAVE_STREAMS; i++) {
            if (sent[i] < H3ZERO_WT_INTERLEAVE_LEN) {
                complete = 0;
                break;
            }
        }
        if (complete) {
            break;
        }
    }
    for (size_t j = 0; ret == 0 && j < H3ZERO_WT_INTERLEAVE_STREAMS; j++) {
        size_t i = fin_order[j];

        ret = h3zero_process_remote_stream(cnx, stream_ids[i], &fin_byte, 0,
            picoquic_callback_stream_fin, stream_ctx[i], h3_ctx);
    }
    if (ret == 0 && (test_ctx.unknown_stream != 0 ||
        cnx->application_error != 0)) {
        ret = -1;
    }
    for (size_t i = 0; ret == 0 && i < H3ZERO_WT_INTERLEAVE_STREAMS; i++) {
        if (test_ctx.streams[i].error_seen ||
            test_ctx.streams[i].nb_data != H3ZERO_WT_INTERLEAVE_LEN ||
            test_ctx.streams[i].nb_fin != 1 ||
            stream_ctx[i]->ps.stream_state.control_stream_id != session_id) {
            DBG_PRINTF("WT interleave stream %zu failed, data=%zu, fin=%d, error=%d",
                i, test_ctx.streams[i].nb_data,
                test_ctx.streams[i].nb_fin,
                test_ctx.streams[i].error_seen);
            ret = -1;
        }
    }
    if (ret != 0) {
        DBG_PRINTF("WT interleave failed, ret=%d, app_error=%" PRIu64 ", unknown=%d",
            ret, (cnx == NULL) ? UINT64_MAX : cnx->application_error,
            test_ctx.unknown_stream);
    }

    if (cnx != NULL) {
        picoquic_set_callback(cnx, NULL, NULL);
    }
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

/*
* A fraction of the control stream parsing is covered by normal usage :
* -receive h3 settings on control stream,
* -receive web transport control stream.
* This leaves testing gaps :
* -Data received on setting streams after the setting frame
* -Data received on streams that should be ignored.
* 
* The interesting stream types are:
* 
* h3zero_stream_type_control: settings stream
* h3zero_stream_type_push (ignored)
* h3zero_stream_type_qpack_encoder (ignored)
* h3zero_stream_type_qpack_decoder (ignored)
* some random type (ignored)
* 
* The test data on the streams is made of frames. Supported frame types
* are:
* - h3zero_frame_settings
* - h3zero_frame_data
* - h3zero_frame_header
* - h3zero_frame_push_promise
* - h3zero_frame_webtransport_stream
*/

uint8_t* h3zero_parse_remote_unidir_stream(
    uint8_t* bytes, uint8_t* bytes_max,
    h3zero_stream_ctx_t* stream_ctx,
    h3zero_callback_ctx_t* ctx,
    uint64_t* error_found, void* opt_cnx);

uint8_t* h3zero_test_get_setting_frame(uint8_t* bytes, uint8_t* bytes_max)
{
    h3zero_settings_t settings = { 0 };

    bytes = h3zero_settings_encode(bytes, bytes_max, &settings);

    return bytes;
}

uint8_t* h3zero_get_pretend_frame(uint8_t* bytes, uint8_t* bytes_max, uint64_t frame_type)
{
    if ((bytes = picoquic_frames_varint_encode(bytes, bytes_max, frame_type)) == NULL ||
        bytes + 2 >= bytes_max) {
        bytes = NULL;
    }
    else {
        size_t len = bytes_max - bytes - 2;
        if (len > 16) {
            len = 16;
        }
        *bytes++ = (uint8_t)len;
        memset(bytes, 0xaa, len);
        bytes += len;
    }

    return bytes;
}

uint8_t* h3zero_test_submit_frame(uint8_t* bytes, uint8_t* bytes_max, h3zero_stream_ctx_t* stream_ctx, h3zero_callback_ctx_t* h3_ctx, uint64_t* error_found)
{
    uint8_t* next_bytes = NULL;
    for (int i = 0; i < 16 && next_bytes < bytes_max; i++) {
        next_bytes = (i == 7) ? bytes_max : bytes + 1;
        if (next_bytes > bytes_max) {
            next_bytes = bytes_max;
        }
        if ((bytes = h3zero_parse_remote_unidir_stream(bytes, next_bytes, stream_ctx, h3_ctx, error_found, NULL)) != next_bytes) {
            bytes = NULL;
            break;
        }
    }
    return bytes;
}

int h3zero_unidir_error_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);
    const uint64_t stream_id[5] = { 3, 7, 11, 13, 17 };
    h3zero_stream_ctx_t * stream_ctx[5] = { NULL, NULL, NULL, NULL, NULL };
    uint64_t stream_type[5] = { h3zero_stream_type_control, h3zero_stream_type_push,
        h3zero_stream_type_qpack_encoder, h3zero_stream_type_qpack_decoder,
        123456789 };
    uint64_t frame_type[5] = { h3zero_frame_settings, h3zero_frame_data, 
        h3zero_frame_header, h3zero_frame_push_promise, 123456789 };
    uint8_t buffer[256];
    uint8_t* bytes = NULL;
    uint8_t* last_byte = NULL;
    uint8_t* bytes_max = buffer + sizeof(buffer);
    uint64_t error_found = 0;

    for (int i = 0; ret == 0 && i < 5; i++) {
        if ((stream_ctx[i] = h3zero_find_or_create_stream(cnx, stream_id[i], h3_ctx, 1, 1)) == NULL) {
            ret = -1;
        }
        else if ((bytes = picoquic_frames_varint_encode(buffer, bytes_max, stream_type[i])) != NULL) {
            if (i == 0) {
                bytes = h3zero_test_get_setting_frame(bytes, bytes_max);
            }
            else {
                bytes = h3zero_get_pretend_frame(bytes, bytes_max, frame_type[i]);
            }
        }
        if (bytes == NULL) {
            ret = -1;
        }
        else {
            last_byte = bytes;
            bytes = h3zero_test_submit_frame(buffer, last_byte, stream_ctx[i], h3_ctx, &error_found);
            if (bytes != last_byte ||
                error_found != 0 || !h3_ctx->settings.settings_received) {
                ret = -1;
            }
        }
    }
    /* add random frame to settings, after settings received */

    /* receive a settings frame again, after settings received. */

    /* clean up everything */
    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int h3zero_setting_submit(int is_after_settings, uint64_t frame_type, int expect_skip)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);
    uint8_t buffer[256];
    uint8_t* bytes = NULL;
    uint8_t* last_byte = NULL;
    uint8_t* bytes_max = buffer + sizeof(buffer);
    uint64_t error_found = 0;
    h3zero_stream_ctx_t* stream_ctx;

    if (ret != 0 ||
        (stream_ctx = h3zero_find_or_create_stream(cnx, 3, h3_ctx, 1, 1)) == NULL ||
        (bytes = picoquic_frames_varint_encode(buffer, bytes_max, h3zero_stream_type_control)) == NULL ||
        (is_after_settings &&
            (bytes = h3zero_test_get_setting_frame(bytes, bytes_max)) == NULL) ||
        (bytes = h3zero_get_pretend_frame(bytes, bytes_max, frame_type)) == NULL){
        ret = -1; /* format error */
    }

    else {
        last_byte = bytes;
        bytes = h3zero_test_submit_frame(buffer, last_byte, stream_ctx, h3_ctx, &error_found);
        if (expect_skip) {
            if (bytes == NULL || error_found != 0) {
                ret = -1;
            }
        }
        else {
            if (bytes != NULL || error_found == 0) {
                ret = -1;
            }
        }
    }

    /* clean up everything */
    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}


int h3zero_setting_error_test(void)
{
    uint64_t unexpected_frames[4] = { h3zero_frame_settings, h3zero_frame_data,
        h3zero_frame_header, h3zero_frame_push_promise };

    /* send a frame that is not a setting frames. This is an error */
    int ret = h3zero_setting_submit(0, 1234567, 0);
    /* Add unexpected frame after setting */
    for (int i = 0; ret == 0 && i < 4; i++) {
        ret = h3zero_setting_submit(1, unexpected_frames[i], 0);
    }
    /* add random frame to settings, after settings received */
    if (ret == 0) {
        ret = h3zero_setting_submit(1, 12345678, 1);
    }

    return ret;
}

static uint8_t* h3zero_settings_fragment_valid_stream(uint8_t* bytes,
    const uint8_t* bytes_max)
{
    h3zero_settings_t settings = {
        .webtransport_max_sessions = 1,
        .webtransport_enabled = 1,
        .wt_initial_max_data = 0x12345,
        .wt_initial_max_streams_uni = 7,
        .wt_initial_max_streams_bidi = 5,
        .enable_connect_protocol = 1,
        .h3_datagram = 1
    };

    if ((bytes = picoquic_frames_varint_encode(bytes, bytes_max,
        h3zero_stream_type_control)) != NULL) {
        bytes = h3zero_settings_encode(bytes, bytes_max, &settings);
    }

    return bytes;
}

static uint8_t* h3zero_settings_fragment_malformed_stream(uint8_t* bytes,
    const uint8_t* bytes_max, int truncated_varint)
{
    if ((bytes = picoquic_frames_varint_encode(bytes, bytes_max,
        h3zero_stream_type_control)) != NULL &&
        (bytes = picoquic_frames_varint_encode(bytes, bytes_max,
            h3zero_frame_settings)) != NULL &&
        (bytes = picoquic_frames_varint_encode(bytes, bytes_max, 1)) != NULL &&
        bytes < bytes_max) {
        *bytes++ = truncated_varint ? 0x40 : h3zero_settings_enable_connect_protocol;
    }
    else {
        bytes = NULL;
    }

    return bytes;
}

static int h3zero_settings_fragment_submit(const uint8_t* bytes, size_t length,
    size_t boundary, int expect_success)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    uint64_t error_found = 0;
    uint8_t* parsed = NULL;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0 &&
        (stream_ctx = h3zero_find_or_create_stream(cnx, 3, h3_ctx, 1, 1)) == NULL) {
        ret = -1;
    }

    if (ret == 0) {
        parsed = h3zero_parse_remote_unidir_stream((uint8_t*)bytes,
            (uint8_t*)bytes + boundary, stream_ctx, h3_ctx, &error_found, NULL);
        if (parsed == NULL) {
            if (expect_success || error_found == 0) {
                ret = -1;
            }
        }
        else if (parsed != (uint8_t*)bytes + boundary ||
            (expect_success && error_found != 0)) {
            ret = -1;
        }
    }
    if (ret == 0 && parsed != NULL && boundary < length) {
        parsed = h3zero_parse_remote_unidir_stream((uint8_t*)bytes + boundary,
            (uint8_t*)bytes + length, stream_ctx, h3_ctx, &error_found, NULL);
    }

    if (ret == 0 && expect_success) {
        if (parsed != (uint8_t*)bytes + length ||
            error_found != 0 ||
            !h3_ctx->settings.settings_received ||
            h3_ctx->settings.enable_connect_protocol != 1 ||
            h3_ctx->settings.h3_datagram != 1 ||
            h3_ctx->settings.webtransport_enabled != 1 ||
            h3_ctx->settings.webtransport_max_sessions != 1 ||
            h3_ctx->settings.wt_initial_max_data != 0x12345 ||
            h3_ctx->settings.wt_initial_max_streams_uni != 7 ||
            h3_ctx->settings.wt_initial_max_streams_bidi != 5) {
            ret = -1;
        }
    }
    else if (ret == 0 &&
        (parsed != NULL || error_found != H3ZERO_SETTINGS_ERROR ||
            h3_ctx->settings.settings_received)) {
        ret = -1;
    }

    if (cnx != NULL) {
        picoquic_set_callback(cnx, NULL, NULL);
    }
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

static int h3zero_settings_fragment_all_boundaries(const uint8_t* bytes,
    size_t length, int expect_success)
{
    int ret = 0;

    for (size_t boundary = 0; ret == 0 && boundary <= length; boundary++) {
        ret = h3zero_settings_fragment_submit(bytes, length, boundary, expect_success);
    }

    return ret;
}

int h3zero_settings_fragment_test(void)
{
    uint8_t buffer[256];
    uint8_t* bytes = h3zero_settings_fragment_valid_stream(buffer,
        buffer + sizeof(buffer));
    int ret = (bytes == NULL) ? -1 : h3zero_settings_fragment_all_boundaries(
        buffer, bytes - buffer, 1);

    if (ret == 0) {
        bytes = h3zero_settings_fragment_malformed_stream(buffer,
            buffer + sizeof(buffer), 0);
        ret = (bytes == NULL) ? -1 : h3zero_settings_fragment_all_boundaries(
            buffer, bytes - buffer, 0);
    }
    if (ret == 0) {
        bytes = h3zero_settings_fragment_malformed_stream(buffer,
            buffer + sizeof(buffer), 1);
        ret = (bytes == NULL) ? -1 : h3zero_settings_fragment_all_boundaries(
            buffer, bytes - buffer, 0);
    }

    return ret;
}

/* Unit test of data callback.
* 
* we want to exercise `h3zero_callback_data` without actually setting up connections.
* The client will have started a bidir stream context, properly initialized.
* The test program will simulate arrival of frames in this context, until
* FIN or Reset of the stream.

int h3zero_callback_data(picoquic_cnx_t* cnx,
	uint64_t stream_id, uint8_t* bytes, size_t length,
	picoquic_call_back_event_t fin_or_event, h3zero_callback_ctx_t* ctx,
	h3zero_stream_ctx_t* stream_ctx, uint64_t* fin_stream_id)
*
* The client when sending the command initialized the name of the file
* in stream_ctx->file_path.
* After that, the client will receive header frame and data frame,
* until the FIN.
 */
int h3zero_process_h3_client_data(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length, int is_fin,
    h3zero_callback_ctx_t* ctx, h3zero_stream_ctx_t* stream_ctx, uint64_t* fin_stream_id);

typedef struct st_client_data_test_spec {
    uint64_t stream_type;
    unsigned int expect_error : 1;
    unsigned int skip_header : 1;
    unsigned int trailer_after_header : 1;
    unsigned int add_trailer : 1;
    unsigned int data_after_trailer : 1;
    unsigned int split_data : 1;
    unsigned int split_submit : 1;
    unsigned int split_fin : 1;
    unsigned int short_length : 1;

} client_data_test_spec_t;

int h3zero_client_data_set_file_name(h3zero_stream_ctx_t* stream_ctx, char const* path_name)
{
    int ret = 0;
    if ((stream_ctx->file_path = picoquic_string_duplicate(path_name)) == NULL) {
        ret = -1;
    }
    else {
        /* ensure that no data is present */
        FILE* F = picoquic_file_open(stream_ctx->file_path, "w");
        if (F == NULL) {
            ret = -1;
        }
        else {
            (void)picoquic_file_close(F);
        }
    }
    return ret;
}

uint8_t* h3zero_client_data_get_response(uint8_t * bytes, uint8_t * bytes_max)
{
    uint8_t* length_byte = NULL;
    uint8_t* data_byte = NULL;
    if ((bytes = picoquic_frames_varint_encode(bytes, bytes_max, h3zero_frame_header)) != NULL) {
        if (bytes + 2 < bytes_max) {
            length_byte = bytes;
            bytes += 2;
            data_byte = bytes;
        }
        else {
            bytes = NULL;
        }
    }
    if (bytes != NULL) {
        bytes = h3zero_create_response_header_frame_ex(bytes, bytes_max,
            h3zero_content_type_text_html, "test client data", NULL);
    }
    if (bytes != NULL) {
        size_t sz = bytes - data_byte;
        length_byte[0] = 0x40 + (uint8_t)(sz >> 8);
        length_byte[1] = (uint8_t)(sz & 0xff);
    }
    return bytes;
}

uint8_t* h3zero_client_data_frame(uint8_t* bytes, uint8_t* bytes_max, size_t data_length)
{
    if ((bytes = picoquic_frames_varint_encode(bytes, bytes_max, h3zero_frame_data)) != NULL &&
        (bytes = picoquic_frames_varint_encode(bytes, bytes_max, data_length)) != NULL) {
        if (bytes + data_length > bytes_max) {
            bytes = NULL;
        }
        else {
            memset(bytes, 0xda, data_length);
            bytes += data_length;
        }
    }
    return bytes;
}

uint8_t* h3zero_client_data_frames(uint8_t* bytes, uint8_t* bytes_max, size_t data_length, unsigned int split_data)
{
    size_t l1 = (split_data) ? data_length / 2 : 0;

    if (l1 > 0 && (bytes = h3zero_client_data_frame(bytes, bytes_max, l1)) == NULL){
        bytes = NULL;
    }
    else {
        bytes = h3zero_client_data_frame(bytes, bytes_max, data_length - l1);
    }
    return bytes;
}

int h3zero_client_data_submit(picoquic_cnx_t * cnx, uint64_t  stream_id, uint8_t* bytes, size_t length, 
    h3zero_callback_ctx_t* h3_ctx, h3zero_stream_ctx_t* stream_ctx, uint64_t * finstream_id,
    client_data_test_spec_t* spec)
{
    int ret = 0;
    size_t chunk = (spec->split_submit) ? 7 : length;
    size_t submitted = 0;

    if (spec->short_length) {
        length--;
    }

    while (ret == 0 && submitted < length) {
        size_t next_chunk = chunk;
        int is_fin = 0;
        if (submitted + next_chunk >= length) {
            next_chunk = length - submitted;
            if (!spec->split_fin) {
                is_fin = 1;
            }
        }
        ret = h3zero_process_h3_client_data(cnx, stream_id, bytes + submitted, next_chunk, is_fin, h3_ctx,
            stream_ctx, finstream_id);
        submitted += next_chunk;
    }
    if (ret == 0 && spec->split_fin) {
        ret = h3zero_process_h3_client_data(cnx, stream_id, NULL, 0, 1, h3_ctx,
            stream_ctx, finstream_id);
    }
    if (cnx->cnx_state != picoquic_state_ready) {
        ret = -1;
    }
    return ret;
}

int h3zero_client_data_test_one(client_data_test_spec_t * spec)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);
    uint8_t buffer[1024];
    uint8_t* bytes = NULL;
    uint8_t* bytes_max = buffer + sizeof(buffer);
    uint64_t stream_id = 4;
    uint64_t fin_stream_id = UINT64_MAX;
    size_t data_length = 128;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    char const* path_name = "h3zero_test_client_data.html";

    if (ret == 0 && (stream_ctx = h3zero_find_or_create_stream(cnx, 4, h3_ctx, 1, 1)) == NULL) {
        ret = -1;
    }
    else {
        cnx->cnx_state = picoquic_state_ready;
        ret = h3zero_client_data_set_file_name(stream_ctx, path_name);
        if (ret == 0) {
            stream_ctx->is_open = 1;
        }
    }
    bytes = buffer;

    /* Encode a stream header */
    if (ret == 0 && !spec->skip_header && 
        (bytes = h3zero_client_data_get_response(bytes, bytes_max)) == NULL){
        ret = -1;
    }
    /* encode a stray trailer */
    if (ret == 0 && spec->trailer_after_header &&
        (bytes = h3zero_client_data_get_response(bytes, bytes_max)) == NULL) {
        ret = -1;
    }
    /* Encode a data frame (or 2?)*/
    if (ret == 0 &&
        (bytes = h3zero_client_data_frames(bytes, bytes_max, data_length, spec->split_data)) == NULL) {
        ret = -1;
    }
    /* Encode a stream trailer */
    if (ret == 0 && spec->add_trailer &&
        (bytes = h3zero_client_data_get_response(bytes, bytes_max)) == NULL) {
        ret = -1;
    }

    /* Encode a data frame after the trailer, causing an error */
    if (ret == 0 && spec->data_after_trailer &&
        (bytes = h3zero_client_data_frames(bytes, bytes_max, 15, 0)) == NULL) {
        ret = -1;
    }

    /* submit as incoming data */
    if (ret == 0) {
        int data_ret = h3zero_client_data_submit(cnx, stream_id, buffer, bytes - buffer, h3_ctx, stream_ctx, &fin_stream_id, spec);
        /* verify that the result is as expected */
        if (spec->expect_error) {
            if (data_ret == 0) {
                ret = -1;
            }
        }
        else {
            if (data_ret != 0) {
                ret = -1;
            }
            else {
                /* verify that the stream is properly removed */
                FILE* Fbis = picoquic_file_open(path_name, "r");
                if (Fbis == NULL) {
                    /* error -- the file remained open! */
                    ret = -1;
                }
                else {
                    long sz;
                    fseek(Fbis, 0, SEEK_END);
                    sz = ftell(Fbis);
                    (void)picoquic_file_close(Fbis);
                    if ((size_t)sz != data_length) {
                        ret = -1;
                    }
                }
            }
        }
    }

    /* clean up everything */
    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}


int h3zero_client_open_stream_file(picoquic_cnx_t* cnx, h3zero_callback_ctx_t* ctx, h3zero_stream_ctx_t* stream_ctx);

int h3zero_error_client_stream_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);
    h3zero_stream_ctx_t* stream_ctx = NULL;
    char const* path_name = "no_such_path/bad_path\\h3zero_test_client_data.html";

    if (ret == 0 && (stream_ctx = h3zero_find_or_create_stream(cnx, 4, h3_ctx, 1, 1)) == NULL) {
        ret = -1;
    }
    else {
        cnx->cnx_state = picoquic_state_ready;
        if ((stream_ctx->file_path = picoquic_string_duplicate(path_name)) == NULL) {
            ret = -1;
        } else {
            stream_ctx->is_open = 1;

            if (h3zero_client_open_stream_file(cnx, h3_ctx, stream_ctx) == 0) {
                ret = -1;
            }
        }
    }

    /* clean up everything */
    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}


int h3zero_client_data_test(void)
{
    client_data_test_spec_t spec = { 0 };
    int ret = h3zero_client_data_test_one(&spec);

    if (ret == 0) {
        memset(&spec, 0, sizeof(spec));
        spec.split_data = 1;
        ret = h3zero_client_data_test_one(&spec);
    }

    if (ret == 0) {
        memset(&spec, 0, sizeof(spec));
        spec.split_fin = 1;
        ret = h3zero_client_data_test_one(&spec);
    }

    if (ret == 0) {
        memset(&spec, 0, sizeof(spec));
        spec.split_submit = 1;
        ret = h3zero_client_data_test_one(&spec);
    }

    if (ret == 0) {
        memset(&spec, 0, sizeof(spec));
        spec.add_trailer = 1;
        ret = h3zero_client_data_test_one(&spec);
    }

    if (ret == 0) {
        memset(&spec, 0, sizeof(spec));
        spec.expect_error = 1;
        spec.short_length = 1;
        ret = h3zero_client_data_test_one(&spec);
    }

    if (ret == 0) {
        memset(&spec, 0, sizeof(spec));
        spec.expect_error = 1;
        spec.skip_header = 1;
        ret = h3zero_client_data_test_one(&spec);
    }

    if (ret == 0) {
        memset(&spec, 0, sizeof(spec));
        spec.expect_error = 1;
        spec.trailer_after_header = 1;
        ret = h3zero_client_data_test_one(&spec);
    }

    if (ret == 0) {
        memset(&spec, 0, sizeof(spec));
        spec.expect_error = 1;
        spec.add_trailer = 1;
        spec.data_after_trailer = 1;
        ret = h3zero_client_data_test_one(&spec);
    }

    if (ret == 0) {
        ret = h3zero_error_client_stream_test();
    }

    return ret;
}




/* Tests of the datagram and capsule protocol */

typedef struct st_test_datagram_ctx_t {
    int nb_datagrams_received;
} test_datagram_ctx_t;


int h3zero_test_datagram_cb(picoquic_cnx_t* UNUSED(cnx),
    uint8_t* UNUSED(bytes), size_t UNUSED(length),
    picohttp_call_back_event_t wt_event,
    struct st_h3zero_stream_ctx_t* UNUSED(stream_ctx),
    void* path_app_ctx)
{
    int ret = 0;
    switch (wt_event) {
    case picohttp_callback_connecting:
        break;
    case picohttp_callback_connect:
        break;
    case picohttp_callback_connect_refused:
        break;
    case picohttp_callback_connect_accepted:
        break;
    case picohttp_callback_post_fin:
    case picohttp_callback_post_data:
        break;
    case picohttp_callback_provide_data: /* Stack is ready to send chunk of response */
        /* We assume that the required stream headers have already been pushed,
        * and that the stream context is already set. Just send the data.
        */
        break;
    case picohttp_callback_post_datagram:
    {
        test_datagram_ctx_t* dg_ctx = (test_datagram_ctx_t*)path_app_ctx;
        if (dg_ctx != NULL) {
            dg_ctx->nb_datagrams_received += 1;
        }
        break;
    }
    case picohttp_callback_provide_datagram: /* Stack is ready to send a datagram */
        break;
    case picohttp_callback_reset: /* Stream has been abandoned. */
        break;
    case picohttp_callback_free: /* Used during clean up the stream. Only cause the freeing of memory. */
        /* Free the memory attached to the stream */
        break;
    case picohttp_callback_deregister:
        break;
    default:
        /* protocol error */
        ret = -1;
        break;
    }
    return ret;
}

uint8_t capsule_datagram[] = {
    0, /* Datagram capsule type = 0 */
    5, /* length = 5 */
    1, 2, 3, 4, 5
};

int h3zero_capsule_receive_chunks(const uint8_t * capsule_bytes, size_t capsule_size, size_t chunk_size, int is_stored)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    h3zero_capsule_t capsule = { 0 };
    test_datagram_ctx_t dg_ctx = { 0 };
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0 && chunk_size > PICOQUIC_MAX_PACKET_SIZE) {
        ret = -1;
    }

    if (ret == 0) {
        ret = h3zero_declare_stream_prefix(h3_ctx, 4, h3zero_test_datagram_cb, &dg_ctx);
    }

    if (ret == 0) {
        /* simulate arrival of a capsule */
        size_t bytes_received = 0;

        capsule.is_stored = is_stored;

        while (ret == 0 && bytes_received < capsule_size) {
            size_t this_chunk = (bytes_received + chunk_size > capsule_size) ? capsule_size - bytes_received : chunk_size;
            uint8_t buffer[PICOQUIC_MAX_PACKET_SIZE];
            const uint8_t* next_bytes;
            memset(buffer, 0xff, sizeof(buffer));
            memcpy(buffer, capsule_bytes + bytes_received, this_chunk);
            if ((next_bytes = h3zero_accumulate_capsule(buffer, buffer + chunk_size, &capsule)) == NULL) {
                ret = -1;
            }
            else {
                size_t consumed = next_bytes - buffer;
                bytes_received += consumed;
                if ((consumed < chunk_size && bytes_received < capsule_size) ||
                    bytes_received > capsule_size) {
                    ret = -1;
                }
            }
        }

        if (ret == 0 && (!capsule.is_length_known || !capsule.is_stored)){
            ret = -1;
        }
    }

    if (capsule.capsule_buffer != NULL) {
        free(capsule.capsule_buffer);
        capsule.capsule_buffer = NULL;
    }

    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int h3zero_capsule_test(void)
{
    int ret = 0;
    size_t test_chunk[3] = { sizeof(capsule_datagram), sizeof(capsule_datagram) - 1, 1 };

    for (int i = 0; ret == 0 && i < 3; i++) {
        ret = h3zero_capsule_receive_chunks(capsule_datagram, sizeof(capsule_datagram), test_chunk[i], i == 0);
        if (ret != 0) {
            DBG_PRINTF("Capsule receive chunk=%zu/%zu fails", test_chunk[i], sizeof(capsule_datagram));
        }
    }

    return ret;
}
