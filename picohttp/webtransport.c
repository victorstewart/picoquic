/*
* Author: Christian Huitema
* Copyright (c) 2023, Private Octopus, Inc.
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

/*
 * Web Transport API implementation for Picoquic.
 * 
 * Expected usage:
 *  - quic server is multipurpose, serves H3 pages, posts, etc., in addition to web socket.
 *  - WT acting as client learns of a connection to the intended server. TBD: generic
 *    connection also used for something else, or specialized connection?
 *  - WT client issues CONNECT on connection, which creates a WT context.
 *  - Server side, WT responder is notified of connect, which creates a WT context.
 *  - Both client and server could open streams
 * 
 * Architecture:
 * 
 *    -- quic events generate picoquic callbacks.
 *    -- web transport state established when processing CONNECT
 *    -- web transport intercepts related callbacks:
 *        -- incoming unidir streams starting with specified frame
 *        -- incoming bidir streams starting with specified frame
 *        -- datagrams starting with specified ID
 *    -- mapping of picoquic callbacks to WT callbacks
 * 
 * 
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <picoquic.h>
#include "picoquic_utils.h"
#include "picoquic_internal.h"
#include "h3zero_common.h"
#include "pico_webtransport.h"

/* web transport set parameters
* Set the parameters adequate for web transport, including:
* - initial number of bidir and unidir streams to 63
* - initial max data per stream to 0x3FFF (16K -1)
* - datagram length to PICOQUIC_MAX_PACKET_SIZE
*/
static void picowt_set_transport_parameters_values(const picoquic_tp_t* tp_current, picoquic_tp_t* tp_new)
{
    if (tp_current != NULL) {
        memcpy(tp_new, tp_current, sizeof(picoquic_tp_t));
    }
    else {
        memset(tp_new, 0, sizeof(picoquic_tp_t));
    }
    if (tp_new->initial_max_data < 0x3FFF) {
        tp_new->initial_max_data = 0x3FFF;
    }
    if (tp_new->initial_max_stream_data_bidi_local < 0x3FFF) {
        tp_new->initial_max_stream_data_bidi_local = 0x3FFF;
    }
    if (tp_new->initial_max_stream_data_bidi_remote < 0x3FFF) {
        tp_new->initial_max_stream_data_bidi_remote = 0x3FFF;
    }
    if (tp_new->initial_max_stream_data_uni < 0x3FFF) {
        tp_new->initial_max_stream_data_uni = 0x3FFF;
    }
    if (tp_new->initial_max_stream_id_bidir < 0x3F) {
        tp_new->initial_max_stream_id_bidir = 0x3F;
    }
    if (tp_new->initial_max_stream_id_unidir < 0x3F) {
        tp_new->initial_max_stream_id_unidir = 0x3F;
    }
    if (tp_new->max_datagram_frame_size == 0) {
        tp_new->max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
    }
    tp_new->is_reset_stream_at_enabled = 1;
}

void picowt_set_transport_parameters(picoquic_cnx_t* cnx)
{
    const picoquic_tp_t* tp_current = picoquic_get_transport_parameters(cnx, 1);
    picoquic_tp_t tp_new;
    picowt_set_transport_parameters_values(tp_current, &tp_new);
    picoquic_set_transport_parameters(cnx, &tp_new);
}

void picowt_set_default_transport_parameters(picoquic_quic_t* quic)
{
    quic->default_tp.is_reset_stream_at_enabled = 1;
    if (quic->default_tp.max_datagram_frame_size == 0) {
        quic->default_tp.max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
    }
}

/* Web transport commands */

static int picowt_settings_enable_flow_control(const h3zero_settings_t* settings)
{
    return (settings != NULL &&
        (settings->wt_initial_max_data != 0 ||
            settings->wt_initial_max_streams_uni != 0 ||
            settings->wt_initial_max_streams_bidi != 0));
}

static int picowt_session_flow_control_is_enabled(h3zero_callback_ctx_t* h3_ctx)
{
    return (h3_ctx != NULL &&
        picowt_settings_enable_flow_control(&h3_ctx->local_settings) &&
        picowt_settings_enable_flow_control(&h3_ctx->settings));
}

static void picowt_signal_local_stream_blocked(picoquic_cnx_t* cnx,
    h3zero_stream_ctx_t* control_stream_ctx, int is_bidir)
{
    uint64_t capsule_type = is_bidir ?
        picowt_capsule_wt_streams_blocked_bidi :
        picowt_capsule_wt_streams_blocked_uni;
    uint64_t blocked_value = is_bidir ?
        control_stream_ctx->wt_max_streams_bidi_remote :
        control_stream_ctx->wt_max_streams_uni_remote;

    if (is_bidir &&
        (!control_stream_ctx->wt_streams_bidi_blocked_sent ||
            control_stream_ctx->wt_streams_bidi_blocked_sent_at !=
                blocked_value)) {
        if (picowt_send_flow_control_capsule(cnx, control_stream_ctx,
            capsule_type, blocked_value) == 0) {
            control_stream_ctx->wt_streams_bidi_blocked_sent = 1;
            control_stream_ctx->wt_streams_bidi_blocked_sent_at =
                blocked_value;
        }
    }
    else if (!is_bidir &&
        (!control_stream_ctx->wt_streams_uni_blocked_sent ||
            control_stream_ctx->wt_streams_uni_blocked_sent_at !=
                blocked_value)) {
        if (picowt_send_flow_control_capsule(cnx, control_stream_ctx,
            capsule_type, blocked_value) == 0) {
            control_stream_ctx->wt_streams_uni_blocked_sent = 1;
            control_stream_ctx->wt_streams_uni_blocked_sent_at =
                blocked_value;
        }
    }
}

static int picowt_local_stream_credit_available(picoquic_cnx_t* cnx,
    h3zero_callback_ctx_t* h3_ctx, uint64_t control_stream_id, int is_bidir)
{
    int ret = 1;

    if (picowt_session_flow_control_is_enabled(h3_ctx)) {
        h3zero_stream_ctx_t* control_stream_ctx =
            h3zero_find_stream(h3_ctx, control_stream_id);

        if (control_stream_ctx == NULL) {
            ret = 0;
        }
        else if (is_bidir) {
            ret = (control_stream_ctx->wt_streams_bidi_sent <
                control_stream_ctx->wt_max_streams_bidi_remote);
        }
        else {
            ret = (control_stream_ctx->wt_streams_uni_sent <
                control_stream_ctx->wt_max_streams_uni_remote);
        }
        if (!ret && control_stream_ctx != NULL) {
            picowt_signal_local_stream_blocked(cnx, control_stream_ctx,
                is_bidir);
        }
    }

    return ret;
}

static void picowt_local_stream_credit_used(h3zero_callback_ctx_t* h3_ctx,
    uint64_t control_stream_id, int is_bidir)
{
    if (picowt_session_flow_control_is_enabled(h3_ctx)) {
        h3zero_stream_ctx_t* control_stream_ctx =
            h3zero_find_stream(h3_ctx, control_stream_id);

        if (control_stream_ctx != NULL) {
            if (is_bidir) {
                control_stream_ctx->wt_streams_bidi_sent++;
            }
            else {
                control_stream_ctx->wt_streams_uni_sent++;
            }
        }
    }
}

/**
* Create stream: when a stream is created locally. 
* Send the stream header. Associate the stream with a per_stream
* app context. mark the stream as active, per batn protocol.
*/
static h3zero_stream_ctx_t* picowt_create_stream_ctx(picoquic_cnx_t* cnx, int is_bidir, h3zero_callback_ctx_t* h3_ctx, 
    uint64_t control_stream_id)
{
    uint64_t stream_id = picoquic_get_next_local_stream_id(cnx, !is_bidir);
    h3zero_stream_ctx_t* stream_ctx = h3zero_find_or_create_stream(
        cnx, stream_id, h3_ctx, 1, 1);
    if (stream_ctx != NULL) {
        /* Associate the stream with a per_stream context */
        stream_ctx->ps.stream_state.stream_type = (is_bidir) ? h3zero_frame_webtransport_stream : h3zero_stream_type_webtransport;
        stream_ctx->ps.stream_state.control_stream_id = control_stream_id;
        if (picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx) != 0) {
            DBG_PRINTF("Could not set context for stream %"PRIu64, stream_id);
        }
    }
    return stream_ctx;
}

h3zero_stream_ctx_t* picowt_create_local_stream(picoquic_cnx_t* cnx, int is_bidir, h3zero_callback_ctx_t* h3_ctx,
    uint64_t control_stream_id)
{
    h3zero_stream_ctx_t* stream_ctx = NULL;
    h3zero_stream_ctx_t* control_stream_ctx =
        h3zero_find_stream(h3_ctx, control_stream_id);

    if (control_stream_ctx == NULL ||
        control_stream_ctx->ps.stream_state.is_fin_sent ||
        control_stream_ctx->ps.stream_state.is_fin_received ||
        !picowt_local_stream_credit_available(cnx, h3_ctx, control_stream_id,
        is_bidir)) {
        return NULL;
    }

    stream_ctx = picowt_create_stream_ctx(cnx, is_bidir, h3_ctx,
        control_stream_id);
    if (stream_ctx != NULL) {
        /* Write the first required bytes for sending the context ID */
        uint8_t stream_header[16];
        int ret;

        uint8_t* bytes = stream_header;
        bytes = picoquic_frames_varint_encode(bytes, stream_header + 16, 
            (is_bidir)?h3zero_frame_webtransport_stream:h3zero_stream_type_webtransport);
        bytes = picoquic_frames_varint_encode(bytes, stream_header + 16, control_stream_id);
        if ((ret = picoquic_add_to_stream_with_ctx(cnx, stream_ctx->stream_id, stream_header, bytes - stream_header, 0, stream_ctx)) != 0) {
            /* something went wrong */
            DBG_PRINTF("Could not add data for stream %"PRIu64 ", ret = %d", stream_ctx->stream_id, ret);
            h3zero_delete_stream(cnx, h3_ctx, stream_ctx);
            stream_ctx = NULL;
        }
        else {
            picowt_local_stream_credit_used(h3_ctx, control_stream_id,
                is_bidir);
        }
    }
    return(stream_ctx);
}


int picowt_reset_stream(picoquic_cnx_t* cnx, h3zero_stream_ctx_t * stream_ctx, uint64_t local_stream_error)
{
    /* Compute the length of the preamble:
    * if is local:
    *    varint(h3zero_frame_webtransport_stream or h3zero_stream_type_webtransport): 2 bytes
    *    + varint (control stream_id)
    * else if is bidir:
    *    resetting a remotely created half of a bidir stream. Just reset.
    * else: can't do that.
    * 
    * if both sides of the stream are closed, delete the H3 stream context.
     */
    int ret = 0;
    int is_bidir = IS_BIDIR_STREAM_ID(stream_ctx->stream_id);
    int is_local = IS_LOCAL_STREAM_ID(stream_ctx->stream_id, cnx->client_mode);

    if (local_stream_error > UINT32_MAX) {
        ret = -1;
    }
    else if (!is_local && !is_bidir) {
        ret = -1;
    }
    else {
        size_t reliable_size = 0;
        uint64_t h3_stream_error = H3ZERO_WEBTRANSPORT_APPLICATION_ERROR(local_stream_error);
        if (is_local) {
            reliable_size = 2 + picoquic_frames_varint_encode_length(stream_ctx->ps.stream_state.control_stream_id);
        }
        ret = picoquic_reset_stream_at(cnx, stream_ctx->stream_id, h3_stream_error, reliable_size);
        stream_ctx->ps.stream_state.is_fin_sent = 1;
    }

    return ret;
}

/* Web transport initiate, client side. Start with two parameters:
* cnx: an established QUIC connection, set to ALPN=H3.
* h3_ctx: the http3 connection context.
* 
* The web transport connection is set in four phases:
* 
* 1- Create an h3zero stream context for the control stream, using
*    the API picowt_set_control_stream.
* 
* 2- Prepare the application state before the connection. This may
*    include documenting the control stream context.
* 
* 3- Start the H3 connection and wait until peer SETTINGS have been received.
*
* 4- Call the picowt_connect API to prepare and queue the web transport
*    connect message. The API takes the following parameters:
* 
*      - cnx: QUIC connection context
*      - stream_ctx: the stream context returned by `picowt_set_control_stream`
*      - path: the path parameter for the connect request
*      - wt_callback: the path callback used for the application
*      - wt_ctx: the web transport application context associated with the path callback
* 
* 5- Make sure that the application is ready to process incoming streams.
*/

h3zero_stream_ctx_t* picowt_set_control_stream(picoquic_cnx_t* cnx, h3zero_callback_ctx_t* h3_ctx)
{
    uint64_t stream_id = picoquic_get_next_local_stream_id(cnx, 0);
    h3zero_stream_ctx_t* stream_ctx = h3zero_find_or_create_stream(
        cnx, stream_id, h3_ctx, 1, 1);
    if (stream_ctx != NULL) {
        /* Associate the stream with a per_stream context */
        if (picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx) != 0) {
            DBG_PRINTF("Could not set context for stream %"PRIu64, stream_id);
            h3zero_delete_stream(cnx, h3_ctx, stream_ctx);
            stream_ctx = NULL;
        }
    }
    return stream_ctx;
}

int picowt_prepare_client_cnx(picoquic_quic_t* quic, struct sockaddr* server_address,
    picoquic_cnx_t** p_cnx, h3zero_callback_ctx_t** p_h3_ctx,
    h3zero_stream_ctx_t** p_stream_ctx,
    uint64_t current_time, const char* sni)
{
    int ret = 0;


    /* use the generic H3 callback */
    /* Set the client callback context */
    if ((*p_h3_ctx == NULL && (*p_h3_ctx = h3zero_callback_create_context(NULL)) == NULL) ||
        (*p_cnx == NULL && ((*p_cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)server_address, current_time, 0, sni, "h3", 1)) == NULL)) ||
        ((*p_stream_ctx = picowt_set_control_stream(*p_cnx, *p_h3_ctx)) == NULL)) {
        ret = 1;
    }
    else
    {
        picowt_set_transport_parameters(*p_cnx);
        picoquic_set_callback(*p_cnx, h3zero_callback, *p_h3_ctx);
    }
    return ret;
}

static int picowt_set_wt_protocol_n(h3zero_stream_ctx_t* stream_ctx,
    const char* selected_protocol, size_t selected_protocol_length)
{
    int ret = 0;
    if (stream_ctx->ps.stream_state.wt_protocol != NULL) {
        ret = -1;
    }
    else if ((stream_ctx->ps.stream_state.wt_protocol =
        picoquic_string_create(selected_protocol, selected_protocol_length)) == NULL) {
        ret = -1; /* memory allocation failed */
    }
    return ret;
}

/* set web transport protocol to selected value.
*/
int picowt_set_wt_protocol(h3zero_stream_ctx_t* stream_ctx, const char* selected_protocol)
{
    return (selected_protocol == NULL) ? -1 :
        picowt_set_wt_protocol_n(stream_ctx, selected_protocol, strlen(selected_protocol));
}

/*
* Set selected web transport protocol
* - Compare the incoming 'wt_available_protocol" to the server list.
* - If there is a match, set the selected protocol in the context, and return 0.
* - If there is no match, return -1.
*/
static int picowt_protocol_is_supported(char const* supported, char const* candidate, size_t candidate_length)
{
    int ret = 0;

    while (supported != NULL && *supported != 0) {
        size_t supported_length = 0;
        while (*supported == ' ' || *supported == '\t' || *supported == ',') {
            supported++;
        }
        while (supported[supported_length] != 0 &&
            supported[supported_length] != ' ' &&
            supported[supported_length] != '\t' &&
            supported[supported_length] != ',') {
            supported_length++;
        }
        if (supported_length == candidate_length &&
            memcmp(supported, candidate, candidate_length) == 0) {
            ret = 1;
            break;
        }
        supported += supported_length;
    }
    return ret;
}

static int picowt_parse_protocol_item(char const** pp, char const** decoded,
    char* escaped, size_t escaped_max, size_t* decoded_length, int* is_last)
{
    char const* p = *pp;
    char const* start;
    size_t length = 0;
    int closed = 0;
    int has_escape = 0;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p++ != '"') {
        return -1;
    }
    start = p;
    while (*p != 0) {
        uint8_t c = (uint8_t)*p++;

        if (c == '"') {
            closed = 1;
            break;
        }
        if (c == '\\') {
            if (!has_escape) {
                length = (size_t)(p - 1 - start);
                if (length >= escaped_max) {
                    return -1;
                }
                memcpy(escaped, start, length);
                has_escape = 1;
            }
            c = (uint8_t)*p++;
            if (c != '"' && c != '\\') {
                return -1;
            }
        }
        else if (c < 0x20 || c > 0x7e) {
            return -1;
        }
        if (length + 1 >= escaped_max) {
            return -1;
        }
        if (has_escape) {
            escaped[length] = (char)c;
        }
        length++;
    }
    if (!closed) {
        return -1;
    }
    if (has_escape) {
        escaped[length] = 0;
        *decoded = escaped;
    }
    else {
        *decoded = start;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    while (*p == ';') {
        p++;
        while (*p != 0 && *p != ',') {
            uint8_t c = (uint8_t)*p++;

            if (c < 0x20 || c > 0x7e) {
                return -1;
            }
            if (c == '"') {
                closed = 0;
                while (*p != 0) {
                    c = (uint8_t)*p++;
                    if (c == '\\') {
                        c = (uint8_t)*p++;
                        if (c != '"' && c != '\\') {
                            return -1;
                        }
                    }
                    else if (c == '"') {
                        closed = 1;
                        break;
                    }
                    else if (c < 0x20 || c > 0x7e) {
                        return -1;
                    }
                }
                if (!closed) {
                    return -1;
                }
            }
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
    }
    if (*p == ',') {
        p++;
        *is_last = 0;
    }
    else if (*p == 0) {
        *is_last = 1;
    }
    else {
        return -1;
    }
    *decoded_length = length;
    *pp = p;
    return 0;
}

int picowt_select_wt_protocol(h3zero_stream_ctx_t* stream_ctx, char const* supported)
{
    char decoded[256];
    char const* candidate;
    size_t candidate_length;
    char selected[256];
    size_t selected_length = 0;
    char const* a = (char const *)stream_ctx->ps.stream_state.header.wt_available_protocols;
    int is_last = 0;
    int ret = -1;

    selected[0] = 0;
    while (a != NULL && *a != 0 && !is_last) {
        if (picowt_parse_protocol_item(&a, &candidate, decoded, sizeof(decoded), &candidate_length, &is_last) != 0) {
            selected_length = 0;
            a = NULL;
            break;
        }
        if (candidate_length == 0) {
            selected_length = 0;
            a = NULL;
            break;
        }
        if (selected_length == 0 &&
            picowt_protocol_is_supported(supported, candidate, candidate_length)) {
            memcpy(selected, candidate, candidate_length);
            selected[candidate_length] = 0;
            selected_length = candidate_length;
        }
    }
    if (a != NULL && is_last && selected_length > 0) {
        ret = picowt_set_wt_protocol_n(stream_ctx, selected, selected_length);
    }
    return ret;
}

const char* picowt_get_authority(h3zero_stream_ctx_t* stream_ctx)
{
    return (const char*)stream_ctx->ps.stream_state.header.authority;
}

/*
* Connect
*/

int picowt_connect_ex(picoquic_cnx_t* cnx, h3zero_callback_ctx_t* ctx,  h3zero_stream_ctx_t* stream_ctx, 
    const char * authority, const char* path, picohttp_post_data_cb_fn wt_callback, void* wt_ctx,
    char const* wt_available_protocols, uint8_t * extra, size_t extra_length)
{
    /* register the stream ID as session ID */
    int ret = 0;
    int prefix_registered = 0;
    if (ctx == NULL || !ctx->settings.settings_received) {
        ret = H3ZERO_MISSING_SETTINGS;
    }
    else if (ctx->goaway_received) {
        ret = H3ZERO_REQUEST_REJECTED;
    }
    else if (cnx != NULL && picoquic_get_cnx_state(cnx) < picoquic_state_client_almost_ready) {
        /* Draft-15 forbids initiating WebTransport CONNECT in 0-RTT, even
         * when HTTP/3 settings were retained from the previous session. The
         * client-almost-ready state is past the early-data send window.
         */
        ret = H3ZERO_MISSING_SETTINGS;
    }
    else if (!h3zero_webtransport_is_ready(cnx, &ctx->settings)) {
        ret = H3ZERO_WEBTRANSPORT_REQUIREMENTS_NOT_MET;
    }
    else {
        ret = h3zero_declare_stream_prefix(ctx, stream_ctx->stream_id, wt_callback, wt_ctx);
        prefix_registered = (ret == 0);
    }
    if (ret == 0 && wt_available_protocols != NULL &&
        (stream_ctx->ps.stream_state.wt_available_protocols = picoquic_string_duplicate(wt_available_protocols)) == NULL) {
        ret = -1;
    }
    if (ret == 0 && cnx != NULL) {
        picoquic_log_app_message(cnx, "Allocated prefix for control stream %" PRIu64, stream_ctx->stream_id);
    }
    if (ret == 0) {
        /* set the required stream parameters for the state of the stream. */
        stream_ctx->is_open = 1;
        stream_ctx->path_callback = wt_callback;
        stream_ctx->path_callback_ctx = wt_ctx;
        stream_ctx->wt_data_received = 0;
        stream_ctx->wt_data_sent = 0;
        stream_ctx->wt_max_data_local = ctx->local_settings.wt_initial_max_data;
        stream_ctx->wt_max_data_remote = ctx->settings.wt_initial_max_data;
        stream_ctx->wt_streams_bidi_received = 0;
        stream_ctx->wt_streams_uni_received = 0;
        stream_ctx->wt_streams_bidi_sent = 0;
        stream_ctx->wt_streams_uni_sent = 0;
        stream_ctx->wt_max_streams_bidi_local =
            ctx->local_settings.wt_initial_max_streams_bidi;
        stream_ctx->wt_max_streams_uni_local =
            ctx->local_settings.wt_initial_max_streams_uni;
        stream_ctx->wt_max_streams_bidi_remote =
            ctx->settings.wt_initial_max_streams_bidi;
        stream_ctx->wt_max_streams_uni_remote =
            ctx->settings.wt_initial_max_streams_uni;
    }

    /* Declare the outgoing connection through the callback, so it can update its own state */
    if (ret == 0) {
        ret = wt_callback(cnx, NULL, 0, picohttp_callback_connecting, stream_ctx, wt_ctx);
    }

    if (ret == 0) {
        /* Format and send the connect frame. */
        uint8_t buffer[1024];
        char origin[512];
        char const* origin_arg = NULL;
        size_t authority_length = 0;
        size_t origin_length = 0;
        size_t path_length = strlen(path);
        uint8_t* bytes = buffer;
        uint8_t* bytes_max = bytes + 1024;

        *bytes++ = h3zero_frame_header;
        bytes += 2; /* reserve two bytes for frame length */

        if (authority != NULL) {
            static char const https_prefix[] = "https://";
            authority_length = strlen(authority);
            if (sizeof(https_prefix) + authority_length <= sizeof(origin)) {
                memcpy(origin, https_prefix, sizeof(https_prefix) - 1);
                memcpy(origin + sizeof(https_prefix) - 1, authority, authority_length + 1);
                origin_arg = origin;
                origin_length = sizeof(https_prefix) - 1 + authority_length;
            }
            else {
                ret = -1;
            }
        }

        if (ret == 0) {
            bytes = h3zero_create_connect_header_frame_ex(bytes, bytes_max,
                authority, authority_length, (const uint8_t*)path, path_length,
                H3ZERO_WEBTRANSPORT_H3_PROTOCOL, sizeof(H3ZERO_WEBTRANSPORT_H3_PROTOCOL) - 1,
                origin_arg, origin_length, H3ZERO_USER_AGENT_STRING,
                sizeof(H3ZERO_USER_AGENT_STRING) - 1, wt_available_protocols);
        }

        if (ret == 0 && bytes == NULL) {
            ret = -1;
        }
        else if (ret == 0) {
            /* Encode the header length */
            size_t header_length = bytes - &buffer[3];
            if (header_length < 64) {
                buffer[1] = (uint8_t)(header_length);
                memmove(&buffer[2], &buffer[3], header_length);
                bytes--;
            }
            else {
                buffer[1] = (uint8_t)((header_length >> 8) | 0x40);
                buffer[2] = (uint8_t)(header_length & 0xFF);
            }
            size_t connect_length = bytes - buffer;
            stream_ctx->ps.stream_state.is_upgrade_requested = 1;
            stream_ctx->ps.stream_state.is_webtransport_requested = 1;

            if (extra != NULL && extra_length > 0 && connect_length + extra_length <= sizeof(buffer)) {
                memcpy(buffer + connect_length, extra, extra_length);
                connect_length += extra_length;
            }


            ret = picoquic_add_to_stream_with_ctx(cnx, stream_ctx->stream_id, buffer, connect_length,
                    0, stream_ctx);
        }
    }
    if (ret != 0 && prefix_registered) {
        h3zero_delete_stream_prefix(cnx, ctx, stream_ctx->stream_id);
        if (stream_ctx->ps.stream_state.wt_available_protocols != NULL) {
            free((char*)stream_ctx->ps.stream_state.wt_available_protocols);
            stream_ctx->ps.stream_state.wt_available_protocols = NULL;
        }
    }
    return ret;
}

int picowt_connect(picoquic_cnx_t* cnx, h3zero_callback_ctx_t* ctx, h3zero_stream_ctx_t* stream_ctx,
    const char* authority, const char* path, picohttp_post_data_cb_fn wt_callback, void* wt_ctx, char const* wt_available_protocols)
{
    return picowt_connect_ex(cnx, ctx, stream_ctx, authority, path, wt_callback, wt_ctx, wt_available_protocols, NULL, 0);
}

/*
CLOSE_WEBTRANSPORT_SESSION Capsule {
    Type (i) = CLOSE_WEBTRANSPORT_SESSION,
    Length (i),
    Application Error Code (32),
    Application Error Message (..1024),
}
*/

static size_t picowt_close_message_length(const char* err_msg)
{
    size_t err_msg_len = 0;

    if (err_msg != NULL) {
        while (err_msg_len <= picowt_close_message_max && err_msg[err_msg_len] != 0) {
            err_msg_len++;
        }
    }

    return err_msg_len;
}

static int picowt_close_message_is_valid_utf8(const uint8_t* bytes, size_t length)
{
    size_t i = 0;

    if (bytes == NULL && length > 0) {
        return 0;
    }
    while (i < length) {
        if (bytes[i] < 0x80) {
            i++;
        }
        else if (bytes[i] >= 0xc2 && bytes[i] <= 0xdf) {
            if (i + 1 >= length || (bytes[i + 1] & 0xc0) != 0x80) {
                return 0;
            }
            i += 2;
        }
        else if (bytes[i] == 0xe0) {
            if (i + 2 >= length || bytes[i + 1] < 0xa0 ||
                bytes[i + 1] > 0xbf || (bytes[i + 2] & 0xc0) != 0x80) {
                return 0;
            }
            i += 3;
        }
        else if ((bytes[i] >= 0xe1 && bytes[i] <= 0xec) ||
            (bytes[i] >= 0xee && bytes[i] <= 0xef)) {
            if (i + 2 >= length || (bytes[i + 1] & 0xc0) != 0x80 ||
                (bytes[i + 2] & 0xc0) != 0x80) {
                return 0;
            }
            i += 3;
        }
        else if (bytes[i] == 0xed) {
            if (i + 2 >= length || bytes[i + 1] < 0x80 ||
                bytes[i + 1] > 0x9f || (bytes[i + 2] & 0xc0) != 0x80) {
                return 0;
            }
            i += 3;
        }
        else if (bytes[i] == 0xf0) {
            if (i + 3 >= length || bytes[i + 1] < 0x90 ||
                bytes[i + 1] > 0xbf || (bytes[i + 2] & 0xc0) != 0x80 ||
                (bytes[i + 3] & 0xc0) != 0x80) {
                return 0;
            }
            i += 4;
        }
        else if (bytes[i] >= 0xf1 && bytes[i] <= 0xf3) {
            if (i + 3 >= length || (bytes[i + 1] & 0xc0) != 0x80 ||
                (bytes[i + 2] & 0xc0) != 0x80 ||
                (bytes[i + 3] & 0xc0) != 0x80) {
                return 0;
            }
            i += 4;
        }
        else if (bytes[i] == 0xf4) {
            if (i + 3 >= length || bytes[i + 1] < 0x80 ||
                bytes[i + 1] > 0x8f || (bytes[i + 2] & 0xc0) != 0x80 ||
                (bytes[i + 3] & 0xc0) != 0x80) {
                return 0;
            }
            i += 4;
        }
        else {
            return 0;
        }
    }

    return 1;
}

int picowt_send_close_session_message(picoquic_cnx_t* cnx, 
    h3zero_stream_ctx_t* control_stream_ctx, 
    uint32_t picowt_err, const char* err_msg)
{
    uint8_t buffer[4 + picowt_close_message_max];
    int ret = 0;
    size_t err_msg_len = 0;
    uint8_t* bytes;
    uint8_t* bytes_max = buffer + sizeof(buffer);

    if (control_stream_ctx->ps.stream_state.is_fin_sent) {
        /* cannot send! */
        ret = -1;
    }
    else {
        err_msg_len = picowt_close_message_length(err_msg);
        if (err_msg_len > picowt_close_message_max ||
            !picowt_close_message_is_valid_utf8((const uint8_t*)err_msg, err_msg_len) ||
            (bytes = picoquic_frames_uint32_encode(buffer, bytes_max, picowt_err)) == NULL ||
            bytes + err_msg_len > bytes_max) {
            ret = -1;
        }
        else {
            if (err_msg_len > 0) {
                memcpy(bytes, err_msg, err_msg_len);
                bytes += err_msg_len;
            }
            ret = h3zero_send_capsule(cnx, control_stream_ctx, picowt_capsule_close_webtransport_session,
                bytes - buffer, buffer, 1 /* Set fin, because we are closing this stream */);
            if (ret == 0) {
                control_stream_ctx->ps.stream_state.is_fin_sent = 1;
            }
        }
    }
    return ret;
}

/*
DRAIN_WEBTRANSPORT_SESSION Capsule {
    Type (i) = DRAIN_WEBTRANSPORT_SESSION,
    Length (i) = 0
}
*/

int picowt_send_drain_session_message(picoquic_cnx_t* cnx, 
    h3zero_stream_ctx_t* control_stream_ctx)
{
    int ret = 0;
    uint8_t null_msg[] = { 0 };

    if (control_stream_ctx->ps.stream_state.is_fin_sent) {
        /* cannot send! */
        ret = -1;
    }
    else if (control_stream_ctx->wt_drain_sent) {
        ret = 0;
    }
    else {
        ret = h3zero_send_capsule(cnx, control_stream_ctx, picowt_capsule_drain_webtransport_session,
            0, null_msg, 0 /* Do not set fin, there could be other capsules */);
        if (ret == 0) {
            control_stream_ctx->wt_drain_sent = 1;
        }
    }

    return ret;
}

static int picowt_flow_control_capsule_is_valid(uint64_t capsule_type, uint64_t flow_control_value)
{
    int ret = 0;

    switch (capsule_type) {
    case picowt_capsule_wt_max_data:
    case picowt_capsule_wt_data_blocked:
        ret = 1;
        break;
    case picowt_capsule_wt_max_streams_bidi:
    case picowt_capsule_wt_max_streams_uni:
    case picowt_capsule_wt_streams_blocked_bidi:
    case picowt_capsule_wt_streams_blocked_uni:
        ret = (flow_control_value <= picowt_max_streams_limit);
        break;
    default:
        break;
    }

    return ret;
}

int picowt_send_flow_control_capsule(picoquic_cnx_t* cnx,
    h3zero_stream_ctx_t* control_stream_ctx, uint64_t capsule_type,
    uint64_t flow_control_value)
{
    uint8_t buffer[8];
    uint8_t* bytes;
    int ret = 0;

    if (cnx == NULL || control_stream_ctx == NULL ||
        control_stream_ctx->ps.stream_state.is_fin_sent ||
        !picowt_flow_control_capsule_is_valid(capsule_type, flow_control_value) ||
        (capsule_type == picowt_capsule_wt_max_data &&
            flow_control_value < control_stream_ctx->wt_max_data_local) ||
        (capsule_type == picowt_capsule_wt_max_streams_bidi &&
            flow_control_value < control_stream_ctx->wt_max_streams_bidi_local) ||
        (capsule_type == picowt_capsule_wt_max_streams_uni &&
            flow_control_value < control_stream_ctx->wt_max_streams_uni_local) ||
        (bytes = picoquic_frames_varint_encode(buffer, buffer + sizeof(buffer),
            flow_control_value)) == NULL) {
        ret = -1;
    }
    else {
        ret = h3zero_send_capsule(cnx, control_stream_ctx, capsule_type,
            bytes - buffer, buffer, 0);
        if (ret == 0 && capsule_type == picowt_capsule_wt_max_data) {
            control_stream_ctx->wt_max_data_local = flow_control_value;
        }
        else if (ret == 0 &&
            capsule_type == picowt_capsule_wt_max_streams_bidi) {
            control_stream_ctx->wt_max_streams_bidi_local =
                flow_control_value;
        }
        else if (ret == 0 &&
            capsule_type == picowt_capsule_wt_max_streams_uni) {
            control_stream_ctx->wt_max_streams_uni_local =
                flow_control_value;
        }
    }

    return ret;
}

int picowt_export_secret(picoquic_cnx_t* cnx, h3zero_stream_ctx_t* control_stream_ctx,
    const uint8_t* label, size_t label_len, const uint8_t* context, size_t context_len,
    uint8_t* out, size_t outlen)
{
    uint8_t exporter_context[8 + 1 + UINT8_MAX + 1 + UINT8_MAX];
    uint8_t* bytes = exporter_context;

    if (cnx == NULL || control_stream_ctx == NULL ||
        out == NULL || outlen == 0 ||
        !IS_CLIENT_STREAM_ID(control_stream_ctx->stream_id) ||
        !IS_BIDIR_STREAM_ID(control_stream_ctx->stream_id) ||
        (label == NULL && label_len > 0) ||
        (context == NULL && context_len > 0) ||
        label_len > UINT8_MAX || context_len > UINT8_MAX) {
        return -1;
    }

    picoformat_64(bytes, control_stream_ctx->stream_id);
    bytes += 8;
    *bytes++ = (uint8_t)label_len;
    if (label_len > 0) {
        memcpy(bytes, label, label_len);
        bytes += label_len;
    }
    *bytes++ = (uint8_t)context_len;
    if (context_len > 0) {
        memcpy(bytes, context, context_len);
        bytes += context_len;
    }

    return picoquic_export_secret_with_context(cnx, "EXPORTER-WebTransport",
        exporter_context, bytes - exporter_context, out, outlen);
}

static int picowt_decode_flow_control_capsule(picowt_capsule_t* capsule,
    uint64_t flow_control_value_max)
{
    const uint8_t* bytes = capsule->h3_capsule.capsule_buffer;
    uint64_t flow_control_value = 0;
    int ret = -1;

    if (bytes != NULL) {
        const uint8_t* bytes_max = bytes + capsule->h3_capsule.capsule_length;

        if ((bytes = picoquic_frames_varint_decode(bytes, bytes_max, &flow_control_value)) != NULL &&
            bytes == bytes_max &&
            flow_control_value <= flow_control_value_max) {
            capsule->flow_control_value = flow_control_value;
            ret = 0;
        }
    }

    return ret;
}

static int picowt_decode_max_flow_control_capsule(picowt_capsule_t* capsule,
    uint64_t flow_control_value_max, uint64_t* previous_value,
    unsigned int* previous_value_received)
{
    int ret = picowt_decode_flow_control_capsule(capsule,
        flow_control_value_max);

    if (ret == 0) {
        if (*previous_value_received &&
            capsule->flow_control_value < *previous_value) {
            capsule->h3_error_code = H3ZERO_WEBTRANSPORT_FLOW_CONTROL_ERROR;
            ret = -1;
        }
        else {
            *previous_value = capsule->flow_control_value;
            *previous_value_received = 1;
        }
    }

    return ret;
}


/* Receive a WT capsule.
* With web transport, we expect three types of capsule:
* - Datagram, if datagram was not negotiated at the QUIC level,
* - Drain session,
* - Close session.
* 
*/
int picowt_receive_capsule(picoquic_cnx_t* cnx, const uint8_t* bytes, const uint8_t* bytes_max, picowt_capsule_t * capsule)
{
    int ret = 0; 
    
    while (ret == 0 && bytes < bytes_max) {
        const uint8_t* bytes_first = bytes;

        bytes = h3zero_accumulate_capsule(bytes, bytes_max, &capsule->h3_capsule);

        if (bytes == NULL) {
            picoquic_log_app_message(cnx, "Cannot parse %zu capsule bytes", bytes_max - bytes_first);
            capsule->h3_error_code = H3ZERO_GENERAL_PROTOCOL_ERROR;
            ret = -1;
            break;
        }
        else{
            if (capsule->h3_capsule.is_stored) {
                capsule->h3_error_code = 0;
                switch (capsule->h3_capsule.capsule_type) {
                case picowt_capsule_drain_webtransport_session:
                    if (capsule->h3_capsule.capsule_length != 0) {
                        picoquic_log_app_message(cnx, "Web transport drain capsule length is %zu bytes", capsule->h3_capsule.capsule_length);
                        capsule->h3_error_code = H3ZERO_GENERAL_PROTOCOL_ERROR;
                        ret = -1;
                    }
                    else {
                        capsule->error_code = 0;
                        capsule->error_msg = NULL;
                        capsule->error_msg_len = 0;
                        picoquic_log_app_message(cnx,
                            "Received web transport session capsule, type: 0x%" PRIx64 " (drain session)",
                            capsule->h3_capsule.capsule_type);
                    }
                    break;
                case picowt_capsule_close_webtransport_session:
                    if (capsule->h3_capsule.capsule_length < 4 ||
                        capsule->h3_capsule.capsule_length > 4 + picowt_close_message_max) {
                        picoquic_log_app_message(cnx, "Invalid web transport close capsule length: %zu bytes", capsule->h3_capsule.capsule_length);
                        capsule->h3_error_code = H3ZERO_GENERAL_PROTOCOL_ERROR;
                        ret = -1;
                    }
                    else {
                        char text[256];
                        size_t text_len = 0;
                        capsule->error_msg = picoquic_frames_uint32_decode(
                            capsule->h3_capsule.capsule_buffer, capsule->h3_capsule.capsule_buffer + capsule->h3_capsule.capsule_length,
                            &capsule->error_code);
                        capsule->error_msg_len = capsule->h3_capsule.capsule_length - 4;
                        if (!picowt_close_message_is_valid_utf8(capsule->error_msg,
                            capsule->error_msg_len)) {
                            picoquic_log_app_message(cnx,
                                "Invalid web transport close capsule UTF-8 message");
                            capsule->h3_error_code = H3ZERO_GENERAL_PROTOCOL_ERROR;
                            ret = -1;
                        }
                        else {
                            text_len = (capsule->error_msg_len > 255) ? 255 : capsule->error_msg_len;
                            if (text_len > 0) {
                                memcpy(text, capsule->error_msg, text_len);
                            }
                            text[text_len] = 0;
                            picoquic_log_app_message(cnx,
                                "Received web transport session capsule, type: 0x%" PRIx64 " (%s), error: %" PRIx32 " (%s)",
                                capsule->h3_capsule.capsule_type,
                                "close session", capsule->error_code, text);
                        }
                    }
                    break;
                case picowt_capsule_wt_max_data:
                    ret = picowt_decode_max_flow_control_capsule(capsule,
                        UINT64_MAX, &capsule->wt_max_data,
                        &capsule->wt_max_data_received);
                    if (ret != 0) {
                        if (capsule->h3_error_code == 0) {
                            capsule->h3_error_code = H3ZERO_GENERAL_PROTOCOL_ERROR;
                        }
                        picoquic_log_app_message(cnx,
                            "Invalid web transport flow control capsule, type: 0x%" PRIx64,
                            capsule->h3_capsule.capsule_type);
                    }
                    break;
                case picowt_capsule_wt_data_blocked:
                    ret = picowt_decode_flow_control_capsule(capsule, UINT64_MAX);
                    if (ret != 0) {
                        capsule->h3_error_code = H3ZERO_GENERAL_PROTOCOL_ERROR;
                        picoquic_log_app_message(cnx,
                            "Invalid web transport flow control capsule, type: 0x%" PRIx64,
                            capsule->h3_capsule.capsule_type);
                    }
                    break;
                case picowt_capsule_wt_max_streams_bidi:
                    ret = picowt_decode_max_flow_control_capsule(capsule,
                        picowt_max_streams_limit, &capsule->wt_max_streams_bidi,
                        &capsule->wt_max_streams_bidi_received);
                    if (ret != 0) {
                        if (capsule->h3_error_code == 0) {
                            capsule->h3_error_code = H3ZERO_GENERAL_PROTOCOL_ERROR;
                        }
                        picoquic_log_app_message(cnx,
                            "Invalid web transport flow control capsule, type: 0x%" PRIx64,
                            capsule->h3_capsule.capsule_type);
                    }
                    break;
                case picowt_capsule_wt_max_streams_uni:
                    ret = picowt_decode_max_flow_control_capsule(capsule,
                        picowt_max_streams_limit, &capsule->wt_max_streams_uni,
                        &capsule->wt_max_streams_uni_received);
                    if (ret != 0) {
                        if (capsule->h3_error_code == 0) {
                            capsule->h3_error_code = H3ZERO_GENERAL_PROTOCOL_ERROR;
                        }
                        picoquic_log_app_message(cnx,
                            "Invalid web transport flow control capsule, type: 0x%" PRIx64,
                            capsule->h3_capsule.capsule_type);
                    }
                    break;
                case picowt_capsule_wt_streams_blocked_bidi:
                case picowt_capsule_wt_streams_blocked_uni:
                    ret = picowt_decode_flow_control_capsule(capsule, picowt_max_streams_limit);
                    if (ret != 0) {
                        capsule->h3_error_code = H3ZERO_GENERAL_PROTOCOL_ERROR;
                        picoquic_log_app_message(cnx,
                            "Invalid web transport flow control capsule, type: 0x%" PRIx64,
                            capsule->h3_capsule.capsule_type);
                    }
                    break;
                case picowt_capsule_wt_max_stream_data:
                case picowt_capsule_wt_stream_data_blocked:
                    picoquic_log_app_message(cnx,
                        "Prohibited web transport capsule type: 0x%" PRIx64,
                        capsule->h3_capsule.capsule_type);
                    capsule->h3_error_code = H3ZERO_GENERAL_PROTOCOL_ERROR;
                    ret = -1;
                    break;
                default:
                    picoquic_log_app_message(cnx, "Unexpected web transport capsule type: 0x%" PRIx64, capsule->h3_capsule.capsule_type);
                    break;
                }
            }
        }
    }

    return ret;
}

void picowt_release_capsule(picowt_capsule_t* capsule)
{
    h3zero_release_capsule(&capsule->h3_capsule);
    memset(capsule, 0, sizeof(picowt_capsule_t));
}

int picowt_apply_flow_control_capsule(h3zero_stream_ctx_t* control_stream_ctx,
    const picowt_capsule_t* capsule)
{
    int ret = 0;

    if (control_stream_ctx == NULL || capsule == NULL) {
        ret = -1;
    }
    else {
        if (capsule->wt_max_data_received) {
            control_stream_ctx->wt_max_data_remote = capsule->wt_max_data;
        }
        if (capsule->wt_max_streams_bidi_received) {
            control_stream_ctx->wt_max_streams_bidi_remote =
                capsule->wt_max_streams_bidi;
        }
        if (capsule->wt_max_streams_uni_received) {
            control_stream_ctx->wt_max_streams_uni_remote =
                capsule->wt_max_streams_uni;
        }
    }

    return ret;
}

int picowt_abort_session(picoquic_cnx_t* cnx,
    h3zero_callback_ctx_t* h3_ctx, h3zero_stream_ctx_t* control_stream_ctx,
    uint64_t h3_error_code)
{
    int ret = -1;

    if (cnx != NULL && h3_ctx != NULL && control_stream_ctx != NULL) {
        ret = picoquic_reset_stream(cnx, control_stream_ctx->stream_id,
            h3_error_code);
        if (ret == 0) {
            ret = picoquic_stop_sending(cnx, control_stream_ctx->stream_id,
                h3_error_code);
        }
        control_stream_ctx->ps.stream_state.is_fin_sent = 1;
        control_stream_ctx->ps.stream_state.is_fin_received = 1;
        h3zero_delete_stream_prefix(cnx, h3_ctx, control_stream_ctx->stream_id);
    }

    return ret;
}

static void picowt_abort_stream_on_session_close(picoquic_cnx_t* cnx, uint64_t stream_id)
{
    int is_bidir = IS_BIDIR_STREAM_ID(stream_id);
    int is_local = IS_LOCAL_STREAM_ID(stream_id, cnx->client_mode);

    if (is_bidir || !is_local) {
        (void)picoquic_stop_sending(cnx, stream_id, H3ZERO_WEBTRANSPORT_SESSION_GONE);
    }
    if (is_bidir || is_local) {
        (void)picoquic_reset_stream(cnx, stream_id, H3ZERO_WEBTRANSPORT_SESSION_GONE);
    }
}

void picowt_deregister(picoquic_cnx_t* cnx,
    h3zero_callback_ctx_t* h3_ctx,
    h3zero_stream_ctx_t* control_stream_ctx)
{
    picosplay_node_t* previous = NULL;
    uint64_t control_stream_id = control_stream_ctx->stream_id;
    /* Free the streams created for this session */
    while (1) {
        picosplay_node_t* next = (previous == NULL) ? picosplay_first(&h3_ctx->h3_stream_tree) : picosplay_next(previous);
        if (next == NULL) {
            break;
        }
        else {
            h3zero_stream_ctx_t* stream_ctx =
                (h3zero_stream_ctx_t*)picohttp_stream_node_value(next);

            if (control_stream_id == stream_ctx->ps.stream_state.control_stream_id &&
                control_stream_id != stream_ctx->stream_id) {
                picowt_abort_stream_on_session_close(cnx, stream_ctx->stream_id);
                stream_ctx->ps.stream_state.control_stream_id = UINT64_MAX;
                stream_ctx->path_callback = NULL;
                stream_ctx->path_callback_ctx = NULL;
                h3zero_forget_stream(cnx, stream_ctx);
                picosplay_delete_hint(&h3_ctx->h3_stream_tree, next);
            }
            else {
                previous = next;
            }
        }
    }
    /* Then deregister the control stream */
    h3zero_untrack_webtransport_session(h3_ctx, control_stream_ctx);
    if (!control_stream_ctx->ps.stream_state.is_fin_sent) {
        picoquic_add_to_stream(cnx, control_stream_ctx->stream_id, NULL, 0, 1);
        control_stream_ctx->ps.stream_state.is_fin_sent = 1;
    }
    picoquic_unlink_app_stream_ctx(cnx, control_stream_ctx->stream_id);
    picoquic_log_app_message(cnx, "Prefix for control stream %"PRIu64 " was unregistered", control_stream_id);
}
