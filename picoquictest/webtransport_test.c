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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "picoquictest_internal.h"
#include "picoquic_binlog.h"
#include "picoquic_logger.h"
#include "picoquic_unified_log.h"
#include "autoqlog.h"
#include "h3zero.h"
#include "h3zero_common.h"
#include "demoserver.h"
#include "pico_webtransport.h"
#include "wt_baton.h"

#ifdef _WINDOWS
#include "wincompat.h"
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

/*
* The web transport unit tests are based on the "baton" protocol
* which is also used for interop testing. 
* TODO: the current protocol is limited. It does not test sending
* large volume of data, sending large number of streams, or
* sending datagrams. Consider extensions!
*/
int picowt_connect_ex(picoquic_cnx_t* cnx, h3zero_callback_ctx_t* ctx, h3zero_stream_ctx_t* stream_ctx,
    const char* authority, const char* path, picohttp_post_data_cb_fn wt_callback, void* wt_ctx,
    char const* wt_available_protocols, uint8_t* extra, size_t extra_length);
int h3zero_set_test_context(picoquic_quic_t** quic, picoquic_cnx_t** cnx, h3zero_callback_ctx_t** h3_ctx, uint64_t* simulated_time);
int h3zero_process_request_frame(picoquic_cnx_t* cnx, h3zero_stream_ctx_t* stream_ctx,
    h3zero_callback_ctx_t* app_ctx);
int h3zero_process_remote_stream(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event,
    h3zero_stream_ctx_t* stream_ctx,
    h3zero_callback_ctx_t* ctx);

wt_baton_app_ctx_t baton_test_ctx = {
    15
};

static int wt_baton_bad_alpn_callback(picoquic_cnx_t* cnx, uint8_t* bytes, size_t length,
    picohttp_call_back_event_t wt_event, h3zero_stream_ctx_t* stream_ctx, void* path_app_ctx)
{
    if (wt_event == picohttp_callback_connect && stream_ctx->ps.stream_state.wt_protocol == NULL) {
        stream_ctx->ps.stream_state.wt_protocol = picoquic_string_duplicate("wrong-baton-00");
        if (stream_ctx->ps.stream_state.wt_protocol == NULL) {
            return -1;
        }
    }
    return wt_baton_callback(cnx, bytes, length, wt_event, stream_ctx, path_app_ctx);
}

picohttp_server_path_item_t path_item_list[1] =
{
    {
        "/baton",
        6,
        wt_baton_callback,
        &baton_test_ctx,
        H3ZERO_WEBTRANSPORT_H3_PROTOCOL,
        sizeof(H3ZERO_WEBTRANSPORT_H3_PROTOCOL) - 1
    }
};

static int picowt_baton_test_reset(wt_baton_ctx_t * baton_ctx, int* reset_needed)
{
    int ret = 0;

    /* Check whether there is already a lane assigned to that stream */
    for (size_t i = 0; i < baton_ctx->nb_lanes; i++) {
        if (baton_ctx->lanes[i].baton_state == wt_baton_state_sending) {
            /* Found a reset target, look for stream context */
            h3zero_stream_ctx_t* stream_ctx = h3zero_find_stream(baton_ctx->h3_ctx,
                baton_ctx->lanes[i].sending_stream_id);
            if (stream_ctx == NULL) {
                ret = -1;
            } else {
                ret = picowt_reset_stream(baton_ctx->cnx, stream_ctx, 12345);
                *reset_needed = 0;
            }
            break;
        }
    }
    return ret;
}

static int picowt_connect_test_protocol(picoquic_cnx_t* cnx, h3zero_callback_ctx_t* ctx, h3zero_stream_ctx_t* stream_ctx,
    const char* authority, const char* path, const char* connect_scheme, const char* connect_protocol,
    const char* connect_origin, picohttp_post_data_cb_fn wt_callback, void* wt_ctx,
    char const* wt_available_protocols)
{
    int ret = h3zero_declare_stream_prefix(ctx, stream_ctx->stream_id, wt_callback, wt_ctx);
    if (ret == 0 && cnx != NULL) {
        picoquic_log_app_message(cnx, "Allocated prefix for control stream %" PRIu64, stream_ctx->stream_id);
    }

    if (ret == 0) {
        stream_ctx->is_open = 1;
        stream_ctx->path_callback = wt_callback;
        stream_ctx->path_callback_ctx = wt_ctx;
        ret = wt_callback(cnx, NULL, 0, picohttp_callback_connecting, stream_ctx, wt_ctx);
    }

    if (ret == 0) {
        uint8_t buffer[1024];
        uint8_t* bytes = buffer;
        uint8_t* bytes_max = bytes + 1024;

        *bytes++ = h3zero_frame_header;
        bytes += 2;

        *bytes++ = 0;
        *bytes++ = 0;
        bytes = h3zero_qpack_code_encode(bytes, bytes_max, 0xC0, 0x3F, H3ZERO_QPACK_CODE_CONNECT);
        if (connect_scheme != NULL && strcmp(connect_scheme, "http") == 0) {
            bytes = h3zero_qpack_code_encode(bytes, bytes_max, 0xC0, 0x3F, H3ZERO_QPACK_SCHEME_HTTP);
        }
        else if (connect_scheme != NULL) {
            bytes = h3zero_qpack_code_encode(bytes, bytes_max, 0xC0, 0x3F, H3ZERO_QPACK_SCHEME_HTTPS);
        }
        bytes = h3zero_qpack_literal_plus_ref_encode(bytes, bytes_max, H3ZERO_QPACK_CODE_PATH, (const uint8_t*)path, strlen(path));
        if (connect_protocol != NULL) {
            bytes = h3zero_qpack_literal_plus_name_encode(bytes, bytes_max, (uint8_t*)":protocol", 9,
                (uint8_t*)connect_protocol, strlen(connect_protocol));
        }
        if (authority != NULL && authority[0] != 0) {
            bytes = h3zero_qpack_literal_plus_ref_encode(bytes, bytes_max, H3ZERO_QPACK_AUTHORITY,
                (const uint8_t*)authority, strlen(authority));
        }
        if (connect_origin != NULL && connect_origin[0] != 0) {
            bytes = h3zero_qpack_literal_plus_ref_encode(bytes, bytes_max, H3ZERO_QPACK_ORIGIN,
                (const uint8_t*)connect_origin, strlen(connect_origin));
        }
        if (wt_available_protocols != NULL) {
            bytes = h3zero_encode_wt_available_protocols_header(bytes, bytes_max, wt_available_protocols);
        }
        if (bytes == NULL) {
            ret = -1;
        }
        else {
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
            stream_ctx->ps.stream_state.is_upgrade_requested = 1;
            stream_ctx->ps.stream_state.is_webtransport_requested = 1;
            ret = picoquic_add_to_stream_with_ctx(cnx, stream_ctx->stream_id, buffer, bytes - buffer, 0, stream_ctx);
        }
    }

    return ret;
}

static int picowt_baton_queue_connect(picoquic_cnx_t* cnx, h3zero_callback_ctx_t* h3zero_cb,
    h3zero_stream_ctx_t* control_stream_ctx, wt_baton_ctx_t* baton_ctx, uint8_t test_id,
    const char* connect_scheme, const char* connect_protocol, const char* connect_authority,
    const char* connect_origin, int connect_before_settings)
{
    int ret;

    if (!connect_before_settings && test_id == 8 && connect_authority == NULL && connect_origin == NULL &&
        connect_scheme != NULL && strcmp(connect_scheme, "https") == 0 &&
        connect_protocol != NULL &&
        strcmp(connect_protocol, H3ZERO_WEBTRANSPORT_H3_PROTOCOL) == 0) {
        uint8_t grease_capsule[12] = { 0x00,0x0a,0xc0,0xe9,0x89,0x05,0x97,0xf9,0x46,0xe4,0x01,0x1d };
        ret = picowt_connect_ex(cnx, h3zero_cb, control_stream_ctx,
            baton_ctx->authority, baton_ctx->server_path,
            wt_baton_callback, baton_ctx, PICOWT_BATON_ALPN, grease_capsule, 12);
    }
    else if (!connect_before_settings && connect_authority == NULL && connect_origin == NULL &&
        connect_scheme != NULL && strcmp(connect_scheme, "https") == 0 &&
        connect_protocol != NULL && strcmp(connect_protocol, H3ZERO_WEBTRANSPORT_H3_PROTOCOL) == 0) {
        ret = picowt_connect(cnx, h3zero_cb, control_stream_ctx,
            baton_ctx->authority, baton_ctx->server_path,
            wt_baton_callback, baton_ctx, PICOWT_BATON_ALPN_AVAILABLE);
    }
    else {
        char origin[512];
        char const* origin_arg = connect_origin;
        char const* authority_arg = (connect_authority == NULL) ? baton_ctx->authority : connect_authority;

        if (origin_arg == NULL && authority_arg != NULL && authority_arg[0] != 0 &&
            picoquic_sprintf(origin, sizeof(origin), NULL, "https://%s", authority_arg) == 0) {
            origin_arg = origin;
        }
        ret = picowt_connect_test_protocol(cnx, h3zero_cb, control_stream_ctx,
            authority_arg, baton_ctx->server_path, connect_scheme, connect_protocol, origin_arg,
            wt_baton_callback, baton_ctx, PICOWT_BATON_ALPN_AVAILABLE);
    }
    return ret;
}

static int picowt_baton_test_one_ex(
    uint8_t test_id, const char* baton_path,
    uint64_t do_losses, uint64_t completion_target, const char* client_qlog_dir,
    const char* server_qlog_dir, picohttp_server_path_item_t* table, size_t table_nb,
    const char* connect_scheme, const char* connect_protocol, const char* connect_authority,
    const char* connect_origin, int connect_before_settings, uint64_t expected_client_error,
    int expect_refused)
{
    char const* alpn = "h3";
    uint64_t simulated_time = 0;
    uint64_t loss_mask = do_losses;
    uint64_t time_out;
    int nb_trials = 0;
    int was_active = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    wt_baton_ctx_t baton_ctx = { 0 };
    int ret = 0;
    picohttp_server_parameters_t server_param = { 0 };
    picoquic_connection_id_t initial_cid = { {0x77, 0x74, 0xba, 0, 0, 0, 0, 0}, 8 };
    h3zero_callback_ctx_t* h3zero_cb = NULL;
    int reset_needed = (test_id == 9);
    h3zero_stream_ctx_t* control_stream_ctx = NULL;

    initial_cid.id[3] = test_id;

    if (ret == 0) {
        ret = tls_api_init_ctx_ex(&test_ctx,
            PICOQUIC_INTERNAL_TEST_VERSION_1,
            PICOQUIC_TEST_SNI, alpn, &simulated_time, NULL, NULL, 0, 1, 0, &initial_cid);

        if (ret == 0 && server_qlog_dir != NULL) {
            picoquic_set_qlog(test_ctx->qserver, server_qlog_dir);
            test_ctx->qserver->use_long_log = 1;
        }

        if (ret == 0 && client_qlog_dir != NULL) {
            picoquic_set_qlog(test_ctx->qclient, client_qlog_dir);
        }

        if (ret == 0) {
            picowt_set_default_transport_parameters(test_ctx->qserver);
            picowt_set_transport_parameters(test_ctx->cnx_client);
        }
    }

    if (ret != 0) {
        DBG_PRINTF("Could not create the QUIC test contexts for V=%x\n", PICOQUIC_INTERNAL_TEST_VERSION_1);
    }
    else if (test_ctx == NULL || test_ctx->cnx_client == NULL) {
        DBG_PRINTF("%s", "Connections where not properly created!\n");
        ret = -1;
    }

    /* The default procedure creates connections using the test callback.
    * We want to replace that by the demo client callback */

    if (ret == 0) {
        /* Set the client callback context using as much as possible
        * the generic picowt calls. */
        ret = picowt_prepare_client_cnx(test_ctx->qclient, (struct sockaddr*)NULL,
            &test_ctx->cnx_client, &h3zero_cb, &control_stream_ctx, simulated_time, PICOQUIC_TEST_SNI);
    }

    if (ret == 0) {
        /* Initialize the server -- should include the path setup for connect action */
        memset(&server_param, 0, sizeof(picohttp_server_parameters_t));
        server_param.web_folder = NULL;
        server_param.path_table = table;
        server_param.path_table_nb = table_nb;

        picoquic_set_alpn_select_fn_v2(test_ctx->qserver, picoquic_demo_server_callback_select_alpn);
        picoquic_set_default_callback(test_ctx->qserver, h3zero_callback, &server_param);
    }

    if (ret == 0 && connect_before_settings) {
        ret = wt_baton_prepare_context(test_ctx->cnx_client, &baton_ctx, h3zero_cb,
            control_stream_ctx, PICOQUIC_TEST_SNI, baton_path);
    }

    if (ret == 0 && connect_before_settings) {
        ret = picowt_baton_queue_connect(test_ctx->cnx_client, h3zero_cb, control_stream_ctx,
            &baton_ctx, test_id, connect_scheme, connect_protocol, connect_authority,
            connect_origin, 1);
    }

    if (ret == 0) {
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }

    /* Establish the connection from client to server. At this stage,
    * this is merely an H3 connection.
    */

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0 && !connect_before_settings && !h3zero_cb->settings.settings_received) {
        DBG_PRINTF("Settings not received before WebTransport CONNECT at t: %llu", simulated_time);
        ret = -1;
    }

    if (ret == 0 && !connect_before_settings) {
        ret = wt_baton_prepare_context(test_ctx->cnx_client, &baton_ctx, h3zero_cb,
            control_stream_ctx, PICOQUIC_TEST_SNI, baton_path);
    }

    if (ret == 0 && !connect_before_settings) {
        ret = picowt_baton_queue_connect(test_ctx->cnx_client, h3zero_cb, control_stream_ctx,
            &baton_ctx, test_id, connect_scheme, connect_protocol, connect_authority,
            connect_origin, 0);
    }

    /* Simulate the connection from the client side. */
    time_out = simulated_time + 30000000;
    while (ret == 0 && picoquic_get_cnx_state(test_ctx->cnx_client) != picoquic_state_disconnected) {
        ret = tls_api_one_sim_round(test_ctx, &simulated_time, time_out, &was_active);

        if (ret != 0) {
            DBG_PRINTF("Simulation error detected after %d trials\n", nb_trials);
            break;
        }

        /* logic of web transport scenarios. */
        if (ret == 0 && baton_ctx.nb_turns > 2 && reset_needed) {
            ret = picowt_baton_test_reset(&baton_ctx, &reset_needed);
        }

        if (ret == 0 && ++nb_trials > 100000) {
            DBG_PRINTF("Simulation not concluded after %d trials\n", nb_trials);
            ret = -1;
            break;
        }
    }

    /* Verify that the web transport scenarios were properly executed  */
    if (ret == 0) {
        if (expected_client_error != 0) {
            if (test_ctx->cnx_client->application_error != expected_client_error) {
                DBG_PRINTF("Expected client application error %" PRIu64 ", got %" PRIu64,
                    expected_client_error, test_ctx->cnx_client->application_error);
                ret = -1;
            }
        }
        else if (expect_refused) {
            int response_status = (control_stream_ctx == NULL) ? 0 : control_stream_ctx->ps.stream_state.header.status;
            if (response_status != 400 || baton_ctx.nb_turns != 0 ||
                baton_ctx.nb_datagrams_sent != 0 || baton_ctx.nb_datagrams_received != 0) {
                DBG_PRINTF("Baton protocol refusal failed, status %d, turns %d",
                    response_status, baton_ctx.nb_turns);
                ret = -1;
            }
        }
        else if (test_id == 3 || test_id == 4 ||
            ((baton_ctx.baton_state == wt_baton_state_done || baton_ctx.baton_state == wt_baton_state_closed) &&
                baton_ctx.nb_turns >= 8 &&
                baton_ctx.lanes_completed == baton_ctx.nb_lanes &&
                baton_ctx.nb_datagrams_sent > 0 && baton_ctx.nb_datagrams_received > 0)) {
            DBG_PRINTF("Baton test succeeds after %d turns, %d datagrams sent, %d received",
                baton_ctx.nb_turns, baton_ctx.nb_datagrams_sent, baton_ctx.nb_datagrams_received);
        }
        else if (test_id == 9 && baton_ctx.baton_state == wt_baton_state_closed) {
            DBG_PRINTF("Baton reset test succeeds after %d turns, %d datagrams sent, %d received",
                baton_ctx.nb_turns, baton_ctx.nb_datagrams_sent, baton_ctx.nb_datagrams_received);
        }
        else {
            DBG_PRINTF("Baton test fails after %d turns, state %d",
                baton_ctx.nb_turns, baton_ctx.baton_state);
            ret = -1;
        }
        if (ret == 0 && test_id == 5 && baton_ctx.lanes[0].first_baton != 33) {
            DBG_PRINTF("On URI test, first baton was %d instead of 33",
                baton_ctx.lanes[0].first_baton);
            ret = -1;
        }
        if (ret == 0 && test_id == 1 && strcmp(baton_ctx.wt_protocol, PICOWT_BATON_ALPN) != 0) {
            DBG_PRINTF("Negotiated WT protocol was %s instead of %s",
                baton_ctx.wt_protocol, PICOWT_BATON_ALPN);
            ret = -1;
        }
    }
    /* Verify that settings were correctly received */
    if (ret == 0 && !h3zero_cb->settings.settings_received) {
        DBG_PRINTF("Settings not received at t: %llu", simulated_time);
        ret = -1;
    }
    /* verify that the execution time is as expected */
    if (ret == 0 && completion_target != 0) {
        if (simulated_time > completion_target) {
            DBG_PRINTF("Test uses %llu microsec instead of %llu", simulated_time, completion_target);
            ret = -1;
        }
    }
    /* verify that the connection was disconnected without error */
    if (ret == 0 && expected_client_error == 0 &&
        (test_ctx->cnx_client->remote_error != 0 ||
            test_ctx->cnx_client->local_error != 0)) {
        DBG_PRINTF("Connection close error: remote %llu, local %llu",
            test_ctx->cnx_client->remote_error, test_ctx->cnx_client->local_error);
        ret = -1;

    }

    if (h3zero_cb != NULL)
    {
        h3zero_callback_delete_context(test_ctx->cnx_client, h3zero_cb);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

static int picowt_baton_test_one(
    uint8_t test_id, const char* baton_path,
    uint64_t do_losses, uint64_t completion_target, const char* client_qlog_dir,
    const char* server_qlog_dir)
{
    return picowt_baton_test_one_ex(test_id, baton_path, do_losses, completion_target,
        client_qlog_dir, server_qlog_dir, path_item_list, 1, "https", H3ZERO_WEBTRANSPORT_H3_PROTOCOL, NULL, NULL, 0, 0, 0);
}

int picowt_baton_basic_test(void)
{
    int ret = picowt_baton_test_one(1, "/baton?baton=240", 0, 2000000, ".", ".");

    return ret;
}

int picowt_baton_error_test(void)
{
    int ret = picowt_baton_test_one(4, "/baton?inject=1", 0, 2000000, ".", ".");

    return ret;
}

int picowt_baton_long_test(void)
{
    int ret = picowt_baton_test_one(2, "/baton", 0, 5000000, ".", ".");

    return ret;
}

int picowt_baton_wrong_test(void)
{
    int ret = picowt_baton_test_one(3, "/wrong_baton", 0, 2000000, ".", ".");

    return ret;
}

int picowt_baton_uri_test(void)
{
    int ret = picowt_baton_test_one(5, "/baton?baton=33", 0, 5000000, ".", ".");

    return ret;
}

int picowt_baton_multi_test(void)
{
    int ret = picowt_baton_test_one(6, "/baton?baton=240&count=4", 0, 5000000, ".", ".");

    return ret;
}

int picowt_baton_random_test(void)
{
    int ret = picowt_baton_test_one(7, "/baton?count=4", 0, 5000000, ".", ".");

    return ret;
}

int picowt_baton_krome_test(void)
{
    int ret = picowt_baton_test_one(8, "/baton?baton=240", 0, 2000000, ".", ".");

    return ret;
}

int picowt_baton_reset_test(void)
{
    int ret = picowt_baton_test_one(9, "/baton?count=8", 0, 5000000, ".", ".");

    return ret;
}

int picowt_baton_wildcard_test(void)
{
    picohttp_server_path_item_t wildcard_table[1] = {
        { "*", 1, wt_baton_callback, NULL, H3ZERO_WEBTRANSPORT_H3_PROTOCOL,
            sizeof(H3ZERO_WEBTRANSPORT_H3_PROTOCOL) - 1 }
    };
    /* /baton is not a specific entry in wildcard_table; the '*' handler must catch it */
    return picowt_baton_test_one_ex(1, "/baton?baton=240", 0, 2000000, ".", ".",
        wildcard_table, 1, "https", H3ZERO_WEBTRANSPORT_H3_PROTOCOL, NULL, NULL, 0, 0, 0);
}

static int picowt_baton_protocol_refusal_test_one(uint8_t test_id, const char* connect_protocol)
{
    return picowt_baton_test_one_ex(test_id, "/baton?baton=240", 0, 2000000, NULL, NULL,
        path_item_list, 1, "https", connect_protocol, NULL, NULL, 0, 0, 1);
}

int picowt_baton_protocol_test(void)
{
    int ret = picowt_baton_protocol_refusal_test_one(10, "webtransport");

    if (ret == 0) {
        ret = picowt_baton_protocol_refusal_test_one(11, "not-webtransport");
    }
    if (ret == 0) {
        ret = picowt_baton_protocol_refusal_test_one(12, NULL);
    }

    return ret;
}

static int picowt_baton_scheme_refusal_test_one(uint8_t test_id, const char* connect_scheme)
{
    return picowt_baton_test_one_ex(test_id, "/baton?baton=240", 0, 2000000, NULL, NULL,
        path_item_list, 1, connect_scheme, H3ZERO_WEBTRANSPORT_H3_PROTOCOL, NULL, NULL, 0, 0, 1);
}

int picowt_baton_scheme_test(void)
{
    int ret = picowt_baton_scheme_refusal_test_one(13, "http");

    if (ret == 0) {
        ret = picowt_baton_scheme_refusal_test_one(14, NULL);
    }

    return ret;
}

int picowt_baton_authority_test(void)
{
    return picowt_baton_test_one_ex(15, "/baton?baton=240", 0, 2000000, NULL, NULL,
        path_item_list, 1, "https", H3ZERO_WEBTRANSPORT_H3_PROTOCOL, "", NULL, 0, 0, 1);
}

int picowt_baton_origin_test(void)
{
    return picowt_baton_test_one_ex(16, "/baton?baton=240", 0, 2000000, NULL, NULL,
        path_item_list, 1, "https", H3ZERO_WEBTRANSPORT_H3_PROTOCOL, NULL, "", 0, 0, 1);
}

int picowt_baton_settings_test(void)
{
    return picowt_baton_test_one_ex(17, "/baton?baton=240", 0, 2000000, NULL, NULL,
        path_item_list, 1, "https", H3ZERO_WEBTRANSPORT_H3_PROTOCOL, NULL, NULL, 1, 0, 0);
}

int picowt_baton_alpn_test(void)
{
    picohttp_server_path_item_t bad_alpn_table[1] = {
        { "/baton", 6, wt_baton_bad_alpn_callback, &baton_test_ctx,
            H3ZERO_WEBTRANSPORT_H3_PROTOCOL, sizeof(H3ZERO_WEBTRANSPORT_H3_PROTOCOL) - 1 }
    };
    return picowt_baton_test_one_ex(18, "/baton?baton=240", 0, 0, NULL, NULL,
        bad_alpn_table, 1, "https", H3ZERO_WEBTRANSPORT_H3_PROTOCOL, NULL, NULL,
        0, H3ZERO_WEBTRANSPORT_ALPN_ERROR, 0);
}

static int picowt_protocol_select_test_one(char const* wt_available_protocols, int expect_success)
{
    h3zero_stream_ctx_t stream_ctx;
    int ret = 0;

    memset(&stream_ctx, 0, sizeof(stream_ctx));
    stream_ctx.ps.stream_state.header.wt_available_protocols = (uint8_t const*)wt_available_protocols;
    stream_ctx.ps.stream_state.header.wt_available_protocols_length = strlen(wt_available_protocols);

    if (picowt_select_wt_protocol(&stream_ctx, PICOWT_BATON_ALPN_FILTER) == 0) {
        if (!expect_success ||
            stream_ctx.ps.stream_state.wt_protocol == NULL ||
            strcmp(stream_ctx.ps.stream_state.wt_protocol, PICOWT_BATON_ALPN) != 0) {
            ret = -1;
        }
    }
    else if (expect_success) {
        ret = -1;
    }

    if (stream_ctx.ps.stream_state.wt_protocol != NULL) {
        free((char*)stream_ctx.ps.stream_state.wt_protocol);
    }
    return ret;
}

int picowt_protocol_select_test(void)
{
    int ret = picowt_protocol_select_test_one("\"wrong-end-baton\", \"devious-baton-00\"", 1);

    if (ret == 0) {
        ret = picowt_protocol_select_test_one("\"wrong-end-baton\";v=\"x,y\", \"devious-baton-00\";v=1", 1);
    }
    if (ret == 0) {
        ret = picowt_protocol_select_test_one("wrong-end-baton, devious-baton-00", 0);
    }
    if (ret == 0) {
        ret = picowt_protocol_select_test_one("\"devious-baton-00\", wrong-end-baton", 0);
    }
    return ret;
}

int picowt_tp_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    uint64_t simulated_time = 0;
    int ret = picoquic_test_set_minimal_cnx_with_time(&quic, &cnx, &simulated_time);

    if (ret == 0) {
        /* Reset the client TP to low values in order to test the picowt function */

        if (cnx->local_parameters.initial_max_data >= 0x3FFF) {
            cnx->local_parameters.initial_max_data = 0x1000;
        }
        if (cnx->local_parameters.initial_max_stream_data_bidi_local >= 0x3FFF) {
            cnx->local_parameters.initial_max_stream_data_bidi_local = 0x1000;
        }
        if (cnx->local_parameters.initial_max_stream_data_bidi_remote >= 0x3FFF) {
            cnx->local_parameters.initial_max_stream_data_bidi_remote = 0x1000;
        }
        if (cnx->local_parameters.initial_max_stream_data_uni >= 0x3FFF) {
            cnx->local_parameters.initial_max_stream_data_uni = 0x1000;
        }
        if (cnx->local_parameters.initial_max_stream_id_bidir >= 0x3F) {
            cnx->local_parameters.initial_max_stream_id_bidir = 0;
        }
        if (cnx->local_parameters.initial_max_stream_id_unidir >= 0x3F) {
            cnx->local_parameters.initial_max_stream_id_unidir = 0;
        }
        if (cnx->local_parameters.max_datagram_frame_size > 0) {
            cnx->local_parameters.max_datagram_frame_size = 0;
        }
        /* Call the setup function */
        picowt_set_transport_parameters(cnx);

        /* verify*/
        if (cnx->local_parameters.initial_max_data < 0x3FFF ||
            cnx->local_parameters.initial_max_stream_data_bidi_local < 0x3FFF ||
            cnx->local_parameters.initial_max_stream_data_bidi_remote < 0x3FFF ||
            cnx->local_parameters.initial_max_stream_data_uni < 0x3FFF ||
            cnx->local_parameters.initial_max_stream_id_bidir < 0x3F ||
            cnx->local_parameters.initial_max_stream_id_unidir < 0x3F ||
            cnx->local_parameters.max_datagram_frame_size == 0) {
            ret = -1;
        }
    }

    picoquic_set_callback(cnx, NULL, NULL);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int picowt_requirements_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    h3zero_settings_t settings = { 0 };
    int ret = picoquic_test_set_minimal_cnx_with_time(&quic, &cnx, &simulated_time);

    if (ret == 0) {
        settings.enable_connect_protocol = 1;
        settings.h3_datagram = 1;
        settings.webtransport_enabled = 1;
        cnx->local_parameters.max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
        cnx->remote_parameters.max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
        cnx->local_parameters.is_reset_stream_at_enabled = 1;
        if (h3zero_webtransport_is_ready(cnx, &settings)) {
            ret = -1;
        }
        cnx->remote_parameters.is_reset_stream_at_enabled = 1;
        if (ret == 0 && !h3zero_webtransport_is_ready(cnx, &settings)) {
            ret = -1;
        }
        settings.webtransport_enabled = 0;
        if (ret == 0 && h3zero_webtransport_is_ready(cnx, &settings)) {
            ret = -1;
        }
    }

    if (ret == 0 && (h3_ctx = h3zero_callback_create_context(NULL)) == NULL) {
        ret = -1;
    }
    else if (ret == 0) {
        h3zero_stream_ctx_t* control_stream_ctx = picowt_set_control_stream(cnx, h3_ctx);
        if (control_stream_ctx == NULL) {
            ret = -1;
        }
        else if (picowt_connect(cnx, h3_ctx, control_stream_ctx, PICOQUIC_TEST_SNI, "/baton",
            wt_baton_callback, NULL, PICOWT_BATON_ALPN_AVAILABLE) != H3ZERO_MISSING_SETTINGS) {
            ret = -1;
        }
    }

    picoquic_set_callback(cnx, NULL, NULL);
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

static int picowt_reset_error_case(uint64_t app_error, uint64_t h3_error, int expect_success)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    h3zero_stream_ctx_t* control_stream_ctx = NULL;
    h3zero_stream_ctx_t* stream_ctx = NULL;
    picoquic_stream_head_t* stream = NULL;
    uint64_t simulated_time = 0;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);
    int reset_ret = 0;

    if (ret == 0) {
        cnx->is_reset_stream_at_enabled = 1;
    }

    if (ret == 0 && (control_stream_ctx = picowt_set_control_stream(cnx, h3_ctx)) == NULL) {
        ret = -1;
    }

    if (ret == 0 && (stream_ctx = picowt_create_local_stream(cnx, 1, h3_ctx, control_stream_ctx->stream_id)) == NULL) {
        ret = -1;
    }

    if (ret == 0) {
        reset_ret = picowt_reset_stream(cnx, stream_ctx, app_error);
        stream = picoquic_find_stream(cnx, stream_ctx->stream_id);
        if (expect_success) {
            if (reset_ret != 0 || stream == NULL || !stream->reset_requested || stream->local_error != h3_error) {
                ret = -1;
            }
        }
        else if (reset_ret == 0 || stream == NULL || stream->reset_requested) {
            ret = -1;
        }
    }

    picoquic_set_callback(cnx, NULL, NULL);
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int picowt_reset_error_test(void)
{
    int ret = picowt_reset_error_case(0, H3ZERO_WEBTRANSPORT_APPLICATION_ERROR_FIRST, 1);

    if (ret == 0) {
        ret = picowt_reset_error_case(0x1d, H3ZERO_WEBTRANSPORT_APPLICATION_ERROR_FIRST + 0x1d, 1);
    }
    if (ret == 0) {
        ret = picowt_reset_error_case(0x1e, H3ZERO_WEBTRANSPORT_APPLICATION_ERROR_FIRST + 0x1f, 1);
    }
    if (ret == 0) {
        ret = picowt_reset_error_case(UINT32_MAX, H3ZERO_WEBTRANSPORT_APPLICATION_ERROR_LAST, 1);
    }
    if (ret == 0) {
        ret = picowt_reset_error_case(((uint64_t)UINT32_MAX) + 1, 0, 0);
    }

    return ret;
}

int picowt_error_code_test(void)
{
    return (H3ZERO_WEBTRANSPORT_BUFFERED_STREAM_REJECTED != 0x3994bd84 ||
        H3ZERO_WEBTRANSPORT_SESSION_GONE != 0x170d7b68 ||
        H3ZERO_WEBTRANSPORT_FLOW_CONTROL_ERROR != 0x045d4487 ||
        H3ZERO_WEBTRANSPORT_ALPN_ERROR != 0x0817b3dd ||
        H3ZERO_WEBTRANSPORT_REQUIREMENTS_NOT_MET != 0x212c0d48 ||
        H3ZERO_WEBTRANSPORT_APPLICATION_ERROR_FIRST != 0x52e4a40fa8dbull ||
        H3ZERO_WEBTRANSPORT_APPLICATION_ERROR_LAST != 0x52e5ac983162ull ||
        H3ZERO_WEBTRANSPORT_APPLICATION_ERROR(0x1e) != 0x52e4a40fa8faull) ? -1 : 0;
}

static int picowt_session_gone_add_stream(picoquic_cnx_t* cnx,
    h3zero_callback_ctx_t* h3_ctx, uint64_t stream_id, uint64_t control_stream_id)
{
    h3zero_stream_ctx_t* stream_ctx = NULL;
    int ret = 0;

    if (picoquic_create_stream(cnx, stream_id) == NULL ||
        (stream_ctx = h3zero_find_or_create_stream(cnx, stream_id, h3_ctx, 1, 1)) == NULL ||
        picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx) != 0) {
        ret = -1;
    }
    else {
        stream_ctx->ps.stream_state.control_stream_id = control_stream_id;
    }

    return ret;
}

int picowt_session_gone_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    h3zero_stream_ctx_t* control_stream_ctx = NULL;
    uint64_t simulated_time = 0;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0 && (control_stream_ctx = picowt_set_control_stream(cnx, h3_ctx)) == NULL) {
        ret = -1;
    }
    if (ret == 0) {
        uint64_t control_stream_id = control_stream_ctx->stream_id;
        ret = picowt_session_gone_add_stream(cnx, h3_ctx, 4, control_stream_id);
        if (ret == 0) {
            ret = picowt_session_gone_add_stream(cnx, h3_ctx, 2, control_stream_id);
        }
        if (ret == 0) {
            ret = picowt_session_gone_add_stream(cnx, h3_ctx, 1, control_stream_id);
        }
        if (ret == 0) {
            ret = picowt_session_gone_add_stream(cnx, h3_ctx, 3, control_stream_id);
        }
        if (ret == 0) {
            picowt_deregister(cnx, h3_ctx, control_stream_ctx);
        }
    }
    if (ret == 0) {
        picoquic_stream_head_t* local_bidi = picoquic_find_stream(cnx, 4);
        picoquic_stream_head_t* local_uni = picoquic_find_stream(cnx, 2);
        picoquic_stream_head_t* remote_bidi = picoquic_find_stream(cnx, 1);
        picoquic_stream_head_t* remote_uni = picoquic_find_stream(cnx, 3);

        if (local_bidi == NULL || !local_bidi->reset_requested ||
            local_bidi->local_error != H3ZERO_WEBTRANSPORT_SESSION_GONE ||
            !local_bidi->stop_sending_requested ||
            local_bidi->local_stop_error != H3ZERO_WEBTRANSPORT_SESSION_GONE ||
            local_uni == NULL || !local_uni->reset_requested ||
            local_uni->local_error != H3ZERO_WEBTRANSPORT_SESSION_GONE ||
            local_uni->stop_sending_requested ||
            remote_bidi == NULL || !remote_bidi->reset_requested ||
            remote_bidi->local_error != H3ZERO_WEBTRANSPORT_SESSION_GONE ||
            !remote_bidi->stop_sending_requested ||
            remote_bidi->local_stop_error != H3ZERO_WEBTRANSPORT_SESSION_GONE ||
            remote_uni == NULL || remote_uni->reset_requested ||
            !remote_uni->stop_sending_requested ||
            remote_uni->local_stop_error != H3ZERO_WEBTRANSPORT_SESSION_GONE ||
            h3zero_find_stream(h3_ctx, 4) != NULL ||
            h3zero_find_stream(h3_ctx, 2) != NULL ||
            h3zero_find_stream(h3_ctx, 1) != NULL ||
            h3zero_find_stream(h3_ctx, 3) != NULL) {
            ret = -1;
        }
    }

    picoquic_set_callback(cnx, NULL, NULL);
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

static void picowt_set_close_test_message(char* msg, size_t msg_len)
{
    memset(msg, 'w', msg_len);
    msg[msg_len] = 0;
}

static size_t picowt_format_test_capsule(uint8_t* buffer, size_t buffer_size,
    uint64_t capsule_type, size_t capsule_length, const uint8_t* payload);

static size_t picowt_format_close_test_capsule(uint8_t* buffer, size_t buffer_size,
    uint32_t error_code, size_t msg_len)
{
    uint8_t payload[4 + picowt_close_message_max + 1];
    uint8_t* bytes = picoquic_frames_uint32_encode(payload, payload + sizeof(payload), error_code);

    if (bytes != NULL && msg_len <= sizeof(payload) - 4) {
        memset(bytes, 'm', msg_len);
        bytes += msg_len;
    }

    return (bytes == NULL) ? 0 : picowt_format_test_capsule(buffer, buffer_size,
        picowt_capsule_close_webtransport_session, bytes - payload, payload);
}

int picowt_close_message_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    h3zero_stream_ctx_t* control_stream_ctx = NULL;
    uint64_t simulated_time = 0;
    char close_msg[picowt_close_message_max + 2];
    uint8_t capsule_buffer[4 + picowt_close_message_max + 16];
    picowt_capsule_t capsule = { 0 };
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0 && (control_stream_ctx = picowt_set_control_stream(cnx, h3_ctx)) == NULL) {
        ret = -1;
    }
    if (ret == 0) {
        picowt_set_close_test_message(close_msg, picowt_close_message_max + 1);
        if (picowt_send_close_session_message(cnx, control_stream_ctx, 0x12345678, close_msg) == 0 ||
            control_stream_ctx->ps.stream_state.is_fin_sent) {
            ret = -1;
        }
    }
    if (ret == 0) {
        picoquic_stream_head_t* stream = picoquic_find_stream(cnx, control_stream_ctx->stream_id);
        picowt_set_close_test_message(close_msg, picowt_close_message_max);
        if (picowt_send_close_session_message(cnx, control_stream_ctx, 0x12345678, close_msg) != 0 ||
            !control_stream_ctx->ps.stream_state.is_fin_sent ||
            stream == NULL || !stream->fin_requested) {
            ret = -1;
        }
    }
    if (ret == 0) {
        size_t capsule_length = picowt_format_close_test_capsule(capsule_buffer, sizeof(capsule_buffer),
            0x12345678, picowt_close_message_max);

        if (capsule_length == 0 ||
            picowt_receive_capsule(cnx, capsule_buffer, capsule_buffer + capsule_length, &capsule) != 0 ||
            capsule.error_code != 0x12345678 ||
            capsule.error_msg_len != picowt_close_message_max) {
            ret = -1;
        }
        picowt_release_capsule(&capsule);
    }
    if (ret == 0) {
        size_t capsule_length = picowt_format_close_test_capsule(capsule_buffer, sizeof(capsule_buffer),
            0x12345678, picowt_close_message_max + 1);

        if (capsule_length == 0 ||
            picowt_receive_capsule(cnx, capsule_buffer, capsule_buffer + capsule_length, &capsule) == 0) {
            ret = -1;
        }
        picowt_release_capsule(&capsule);
    }

    picoquic_set_callback(cnx, NULL, NULL);
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

typedef struct st_picowt_goaway_test_ctx_t {
    int nb_drains;
} picowt_goaway_test_ctx_t;

static int picowt_goaway_callback(picoquic_cnx_t* UNUSED(cnx),
    uint8_t* UNUSED(bytes), size_t UNUSED(length), picohttp_call_back_event_t wt_event,
    h3zero_stream_ctx_t* UNUSED(stream_ctx), void* path_app_ctx)
{
    picowt_goaway_test_ctx_t* test_ctx = (picowt_goaway_test_ctx_t*)path_app_ctx;

    if (wt_event == picohttp_callback_drain) {
        test_ctx->nb_drains++;
    }

    return 0;
}

static size_t picowt_format_goaway_test_input(uint8_t* bytes, uint8_t* bytes_max,
    int include_stream_type, uint64_t goaway_stream_id)
{
    uint8_t payload[8];
    uint8_t* payload_end = picoquic_frames_varint_encode(payload, payload + sizeof(payload), goaway_stream_id);
    uint8_t* start = bytes;

    if (payload_end == NULL) {
        bytes = NULL;
    }
    if (bytes != NULL && include_stream_type) {
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, h3zero_stream_type_control);
    }
    if (bytes != NULL &&
        (bytes = picoquic_frames_varint_encode(bytes, bytes_max, h3zero_frame_goaway)) != NULL &&
        (bytes = picoquic_frames_varint_encode(bytes, bytes_max, payload_end - payload)) != NULL) {
        if ((size_t)(bytes_max - bytes) < (size_t)(payload_end - payload)) {
            bytes = NULL;
        }
        else {
            memcpy(bytes, payload, payload_end - payload);
            bytes += payload_end - payload;
        }
    }

    return (bytes == NULL) ? 0 : (size_t)(bytes - start);
}

int picowt_goaway_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    h3zero_stream_ctx_t* control_stream_ctx = NULL;
    h3zero_stream_ctx_t* goaway_stream_ctx = NULL;
    uint8_t goaway_input[16];
    size_t goaway_input_length = 0;
    uint64_t simulated_time = 0;
    picowt_goaway_test_ctx_t test_ctx = { 0 };
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        h3_ctx->settings.settings_received = 1;
        h3_ctx->settings.enable_connect_protocol = 1;
        h3_ctx->settings.h3_datagram = 1;
        h3_ctx->settings.webtransport_enabled = 1;
        cnx->local_parameters.max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
        cnx->remote_parameters.max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
        cnx->local_parameters.is_reset_stream_at_enabled = 1;
        cnx->remote_parameters.is_reset_stream_at_enabled = 1;
    }
    if (ret == 0 && (control_stream_ctx = picowt_set_control_stream(cnx, h3_ctx)) == NULL) {
        ret = -1;
    }
    if (ret == 0) {
        control_stream_ctx->is_upgraded = 1;
        control_stream_ctx->ps.stream_state.is_webtransport_requested = 1;
        ret = h3zero_declare_stream_prefix(h3_ctx, control_stream_ctx->stream_id,
            picowt_goaway_callback, &test_ctx);
    }
    if (ret == 0 &&
        (goaway_stream_ctx = h3zero_find_or_create_stream(cnx, 3, h3_ctx, 1, 1)) == NULL) {
        ret = -1;
    }
    if (ret == 0) {
        goaway_input_length = picowt_format_goaway_test_input(goaway_input,
            goaway_input + sizeof(goaway_input), 1, 4);
        if (goaway_input_length == 0 ||
            h3zero_process_remote_stream(cnx, 3, goaway_input, goaway_input_length,
                picoquic_callback_stream_data, goaway_stream_ctx, h3_ctx) != 0 ||
            !h3_ctx->goaway_received ||
            h3_ctx->goaway_stream_id != 4 ||
            test_ctx.nb_drains != 1) {
            ret = -1;
        }
    }
    if (ret == 0) {
        h3zero_stream_ctx_t* blocked_control_stream_ctx = picowt_set_control_stream(cnx, h3_ctx);
        if (blocked_control_stream_ctx == NULL ||
            picowt_connect(cnx, h3_ctx, blocked_control_stream_ctx, PICOQUIC_TEST_SNI, "/baton",
                picowt_goaway_callback, &test_ctx, NULL) != H3ZERO_REQUEST_REJECTED) {
            ret = -1;
        }
    }

    picoquic_set_callback(cnx, NULL, NULL);
    if (h3_ctx != NULL) {
        h3zero_callback_delete_context(cnx, h3_ctx);
    }
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int picowt_exporter_test(void)
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    h3zero_stream_ctx_t session_ctx = { 0 };
    const uint8_t exporter_label[] = { 'b', 'a', 't', 'o', 'n' };
    const uint8_t exporter_context[] = { 1, 2, 3, 4 };
    const uint8_t serialized_context[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        5, 'b', 'a', 't', 'o', 'n',
        4, 1, 2, 3, 4
    };
    const uint8_t embedded_label[] = { 'b', 0, 'n' };
    const uint8_t serialized_embedded_label[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        3, 'b', 0, 'n',
        4, 1, 2, 3, 4
    };
    const uint8_t serialized_empty_context[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        5, 'b', 'a', 't', 'o', 'n',
        0
    };
    const size_t export_key_len = 16;
    uint8_t client_export_key[16] = { 0 };
    uint8_t server_export_key[16] = { 0 };
    uint8_t direct_export_key[16] = { 0 };
    uint8_t other_session_key[16] = { 0 };
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        if (test_ctx->qclient != NULL) {
            picoquic_free(test_ctx->qclient);
        }
        test_ctx->qclient = picoquic_create(8, NULL, NULL, NULL, NULL, test_api_callback,
            (void*)&test_ctx->client_callback, NULL, NULL, NULL,
            simulated_time, &simulated_time, NULL, NULL, 0);
        test_ctx->cnx_client = NULL;
        if (test_ctx->qclient == NULL) {
            ret = -1;
        }
    }
    if (ret == 0) {
        picoquic_set_use_exporter(test_ctx->qclient, 1);
        picoquic_set_use_exporter(test_ctx->qserver, 1);
    }
    if (ret == 0) {
        test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient,
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&test_ctx->server_addr, 0, 0,
            PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 1);
        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        }
    }
    if (ret == 0) {
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }
    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0 && (test_ctx->cnx_client == NULL || test_ctx->cnx_server == NULL)) {
        ret = -1;
    }

    if (ret == 0) {
        session_ctx.stream_id = 0;
        ret = picowt_export_secret(test_ctx->cnx_client, &session_ctx,
            exporter_label, sizeof(exporter_label),
            exporter_context, sizeof(exporter_context), client_export_key, export_key_len);
    }
    if (ret == 0) {
        ret = picowt_export_secret(test_ctx->cnx_server, &session_ctx,
            exporter_label, sizeof(exporter_label),
            exporter_context, sizeof(exporter_context), server_export_key, export_key_len);
    }
    if (ret == 0 && memcmp(client_export_key, server_export_key, export_key_len) != 0) {
        ret = -1;
    }
    if (ret == 0) {
        ret = picoquic_export_secret_with_context(test_ctx->cnx_client,
            "EXPORTER-WebTransport", serialized_context, sizeof(serialized_context),
            direct_export_key, export_key_len);
    }
    if (ret == 0 && memcmp(client_export_key, direct_export_key, export_key_len) != 0) {
        ret = -1;
    }
    if (ret == 0) {
        session_ctx.stream_id = 4;
        ret = picowt_export_secret(test_ctx->cnx_client, &session_ctx,
            exporter_label, sizeof(exporter_label),
            exporter_context, sizeof(exporter_context), other_session_key, export_key_len);
    }
    if (ret == 0 && memcmp(client_export_key, other_session_key, export_key_len) == 0) {
        ret = -1;
    }
    if (ret == 0) {
        session_ctx.stream_id = 0;
        ret = picowt_export_secret(test_ctx->cnx_client, &session_ctx,
            embedded_label, sizeof(embedded_label),
            exporter_context, sizeof(exporter_context), other_session_key, export_key_len);
    }
    if (ret == 0) {
        ret = picoquic_export_secret_with_context(test_ctx->cnx_client,
            "EXPORTER-WebTransport", serialized_embedded_label, sizeof(serialized_embedded_label),
            direct_export_key, export_key_len);
    }
    if (ret == 0 && memcmp(other_session_key, direct_export_key, export_key_len) != 0) {
        ret = -1;
    }
    if (ret == 0) {
        ret = picowt_export_secret(test_ctx->cnx_client, &session_ctx,
            exporter_label, sizeof(exporter_label),
            NULL, 0, other_session_key, export_key_len);
    }
    if (ret == 0) {
        ret = picoquic_export_secret_with_context(test_ctx->cnx_client,
            "EXPORTER-WebTransport", serialized_empty_context, sizeof(serialized_empty_context),
            direct_export_key, export_key_len);
    }
    if (ret == 0 && memcmp(other_session_key, direct_export_key, export_key_len) != 0) {
        ret = -1;
    }
    if (ret == 0) {
        uint8_t max_label[UINT8_MAX];
        memset(max_label, 'x', sizeof(max_label));
        if (picowt_export_secret(test_ctx->cnx_client, &session_ctx,
            max_label, sizeof(max_label), NULL, 0, other_session_key, export_key_len) != 0) {
            ret = -1;
        }
    }
    if (ret == 0) {
        uint8_t long_label[UINT8_MAX + 1];
        memset(long_label, 'x', sizeof(long_label));
        if (picowt_export_secret(test_ctx->cnx_client, &session_ctx,
            long_label, sizeof(long_label),
            exporter_context, sizeof(exporter_context), other_session_key, export_key_len) == 0) {
            ret = -1;
        }
    }
    if (ret == 0) {
        uint8_t long_context[UINT8_MAX + 1] = { 0 };
        if (picowt_export_secret(test_ctx->cnx_client, &session_ctx,
            exporter_label, sizeof(exporter_label),
            long_context, sizeof(long_context), other_session_key, export_key_len) == 0) {
            ret = -1;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }

    return ret;
}

int picowt_drain_test_one(int expect_error)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        h3zero_stream_ctx_t* control_stream_ctx = picowt_set_control_stream(cnx, h3_ctx);

        if (control_stream_ctx == NULL) {
            ret = -1;
        }
        else if (expect_error) {
            control_stream_ctx->ps.stream_state.is_fin_sent = 1;
            if (picowt_send_drain_session_message(cnx, control_stream_ctx) == 0) {
                ret = -1;
            }
        }
        else {
            ret = picowt_send_drain_session_message(cnx, control_stream_ctx);
            if (ret == 0) {
                picoquic_stream_head_t* stream = picoquic_find_stream(cnx, control_stream_ctx->stream_id);
                picoquic_stream_queue_node_t* queued = (stream == NULL) ? NULL : stream->send_queue;
                const uint8_t* bytes = (queued == NULL) ? NULL : queued->bytes;
                const uint8_t* bytes_max = (queued == NULL) ? NULL : queued->bytes + queued->length;
                const uint8_t* data_max = NULL;
                uint64_t frame_type = UINT64_MAX;
                uint64_t frame_length = UINT64_MAX;
                uint64_t capsule_type = UINT64_MAX;
                uint64_t capsule_length = UINT64_MAX;

                if (bytes == NULL ||
                    (bytes = picoquic_frames_varint_decode(bytes, bytes_max, &frame_type)) == NULL ||
                    (bytes = picoquic_frames_varint_decode(bytes, bytes_max, &frame_length)) == NULL ||
                    frame_length != (uint64_t)(bytes_max - bytes)) {
                    ret = -1;
                }
                else {
                    data_max = bytes_max;
                    if (frame_type != h3zero_frame_data ||
                        (bytes = picoquic_frames_varint_decode(bytes, data_max, &capsule_type)) == NULL ||
                        (bytes = picoquic_frames_varint_decode(bytes, data_max, &capsule_length)) == NULL ||
                        capsule_type != picowt_capsule_drain_webtransport_session ||
                        capsule_length != 0 || bytes != data_max) {
                        ret = -1;
                    }
                }
            }
        }
    }


    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

int picowt_drain_test(void)
{
    int ret = picowt_drain_test_one(0);

    if (ret == 0) {
        ret = picowt_drain_test_one(1);
    }

    return ret;
}

static size_t picowt_format_test_capsule(uint8_t* buffer, size_t buffer_size,
    uint64_t capsule_type, size_t capsule_length, const uint8_t* payload)
{
    uint8_t* bytes = buffer;
    uint8_t* bytes_max = buffer + buffer_size;

    if ((bytes = picoquic_frames_varint_encode(bytes, bytes_max, capsule_type)) != NULL &&
        (bytes = picoquic_frames_varint_encode(bytes, bytes_max, capsule_length)) != NULL &&
        capsule_length <= (size_t)(bytes_max - bytes)) {
        if (capsule_length > 0) {
            memcpy(bytes, payload, capsule_length);
        }
        bytes += capsule_length;
    }

    return (bytes == NULL) ? 0 : (size_t)(bytes - buffer);
}

int picowt_receive_drain_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    uint64_t simulated_time = 0;
    uint8_t buffer[16];
    uint8_t payload[4] = { 0, 0, 0, 0 };
    picowt_capsule_t capsule = { 0 };
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        size_t capsule_length = picowt_format_test_capsule(buffer, sizeof(buffer),
            picowt_capsule_drain_webtransport_session, 0, NULL);

        if (capsule_length == 0 ||
            picowt_receive_capsule(cnx, buffer, buffer + capsule_length, &capsule) != 0 ||
            !capsule.h3_capsule.is_stored ||
            capsule.h3_capsule.capsule_type != picowt_capsule_drain_webtransport_session ||
            capsule.h3_capsule.capsule_length != 0 ||
            capsule.error_code != 0 ||
            capsule.error_msg != NULL ||
            capsule.error_msg_len != 0) {
            ret = -1;
        }
        picowt_release_capsule(&capsule);
    }

    if (ret == 0) {
        size_t capsule_length = picowt_format_test_capsule(buffer, sizeof(buffer),
            picowt_capsule_drain_webtransport_session, 1, payload);

        if (capsule_length == 0 ||
            picowt_receive_capsule(cnx, buffer, buffer + capsule_length, &capsule) == 0) {
            ret = -1;
        }
        picowt_release_capsule(&capsule);
    }

    if (ret == 0) {
        size_t capsule_length = picowt_format_test_capsule(buffer, sizeof(buffer),
            picowt_capsule_close_webtransport_session, 0, NULL);

        if (capsule_length == 0 ||
            picowt_receive_capsule(cnx, buffer, buffer + capsule_length, &capsule) == 0) {
            ret = -1;
        }
        picowt_release_capsule(&capsule);
    }

    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

static int picowt_receive_flow_control_case(picoquic_cnx_t* cnx, uint64_t capsule_type,
    uint64_t flow_control_value, int expect_success)
{
    uint8_t buffer[24];
    uint8_t payload[8];
    uint8_t* payload_end = picoquic_frames_varint_encode(payload, payload + sizeof(payload), flow_control_value);
    size_t payload_length = (payload_end == NULL) ? 0 : (size_t)(payload_end - payload);
    size_t capsule_length = picowt_format_test_capsule(buffer, sizeof(buffer),
        capsule_type, payload_length, payload);
    picowt_capsule_t capsule = { 0 };
    int ret = 0;
    int capsule_ret = -1;

    if (capsule_length == 0) {
        ret = -1;
    }
    else {
        capsule_ret = picowt_receive_capsule(cnx, buffer, buffer + capsule_length, &capsule);
        if ((capsule_ret == 0) != expect_success) {
            ret = -1;
        }
        else if (expect_success &&
            (capsule.h3_capsule.capsule_type != capsule_type ||
                capsule.flow_control_value != flow_control_value)) {
            ret = -1;
        }
    }
    picowt_release_capsule(&capsule);
    return ret;
}

static int picowt_decode_queued_flow_control_capsule(picoquic_cnx_t* cnx,
    h3zero_stream_ctx_t* control_stream_ctx, uint64_t* capsule_type, uint64_t* flow_control_value)
{
    picoquic_stream_head_t* stream = picoquic_find_stream(cnx, control_stream_ctx->stream_id);
    picoquic_stream_queue_node_t* queued = (stream == NULL) ? NULL : stream->send_queue;
    const uint8_t* bytes = (queued == NULL) ? NULL : queued->bytes;
    const uint8_t* bytes_max = (queued == NULL) ? NULL : queued->bytes + queued->length;
    const uint8_t* data_max = NULL;
    uint64_t frame_type = UINT64_MAX;
    uint64_t frame_length = UINT64_MAX;
    uint64_t capsule_length = UINT64_MAX;

    if (bytes == NULL ||
        (bytes = picoquic_frames_varint_decode(bytes, bytes_max, &frame_type)) == NULL ||
        (bytes = picoquic_frames_varint_decode(bytes, bytes_max, &frame_length)) == NULL ||
        frame_type != h3zero_frame_data ||
        frame_length != (uint64_t)(bytes_max - bytes)) {
        return -1;
    }

    data_max = bytes_max;
    if ((bytes = picoquic_frames_varint_decode(bytes, data_max, capsule_type)) == NULL ||
        (bytes = picoquic_frames_varint_decode(bytes, data_max, &capsule_length)) == NULL ||
        capsule_length != (uint64_t)(data_max - bytes) ||
        (bytes = picoquic_frames_varint_decode(bytes, data_max, flow_control_value)) == NULL ||
        bytes != data_max) {
        return -1;
    }

    return 0;
}

int picowt_flow_control_capsule_test(void)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    h3zero_stream_ctx_t* control_stream_ctx = NULL;
    uint64_t simulated_time = 0;
    uint64_t capsule_type = UINT64_MAX;
    uint64_t flow_control_value = UINT64_MAX;
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        ret = picowt_receive_flow_control_case(cnx, picowt_capsule_wt_max_data, 0x12345, 1);
    }
    if (ret == 0) {
        ret = picowt_receive_flow_control_case(cnx, picowt_capsule_wt_streams_blocked_uni,
            picowt_max_streams_limit, 1);
    }
    if (ret == 0) {
        ret = picowt_receive_flow_control_case(cnx, picowt_capsule_wt_max_streams_bidi,
            picowt_max_streams_limit + 1, 0);
    }
    if (ret == 0) {
        uint8_t buffer[16];
        picowt_capsule_t capsule = { 0 };
        size_t capsule_length = picowt_format_test_capsule(buffer, sizeof(buffer),
            picowt_capsule_wt_data_blocked, 0, NULL);

        if (capsule_length == 0 ||
            picowt_receive_capsule(cnx, buffer, buffer + capsule_length, &capsule) == 0) {
            ret = -1;
        }
        picowt_release_capsule(&capsule);
    }
    if (ret == 0 && (control_stream_ctx = picowt_set_control_stream(cnx, h3_ctx)) == NULL) {
        ret = -1;
    }
    if (ret == 0 &&
        picowt_send_flow_control_capsule(cnx, control_stream_ctx,
            picowt_capsule_wt_data_blocked, 0x12345) != 0) {
        ret = -1;
    }
    if (ret == 0 &&
        (picowt_decode_queued_flow_control_capsule(cnx, control_stream_ctx,
            &capsule_type, &flow_control_value) != 0 ||
            capsule_type != picowt_capsule_wt_data_blocked ||
            flow_control_value != 0x12345)) {
        ret = -1;
    }
    if (ret == 0 &&
        picowt_send_flow_control_capsule(cnx, control_stream_ctx,
            picowt_capsule_wt_max_streams_bidi, picowt_max_streams_limit + 1) == 0) {
        ret = -1;
    }

    picoquic_set_callback(cnx, NULL, NULL);
    h3zero_callback_delete_context(cnx, h3_ctx);
    picoquic_test_delete_minimal_cnx(&quic, &cnx);

    return ret;
}

typedef struct st_picowt_session_limit_test_ctx_t {
    h3zero_callback_ctx_t* h3_ctx;
    int nb_connects;
} picowt_session_limit_test_ctx_t;

static int picowt_session_limit_callback(picoquic_cnx_t* UNUSED(cnx),
    uint8_t* UNUSED(bytes), size_t UNUSED(length), picohttp_call_back_event_t wt_event,
    h3zero_stream_ctx_t* stream_ctx, void* path_app_ctx)
{
    int ret = 0;
    picowt_session_limit_test_ctx_t* test_ctx = (picowt_session_limit_test_ctx_t*)path_app_ctx;

    if (wt_event == picohttp_callback_connect) {
        test_ctx->nb_connects++;
        stream_ctx->ps.stream_state.control_stream_id = stream_ctx->stream_id;
        ret = h3zero_declare_stream_prefix(test_ctx->h3_ctx, stream_ctx->stream_id,
            picowt_session_limit_callback, test_ctx);
    }

    return ret;
}

static int picowt_session_limit_prepare_stream(picoquic_cnx_t* cnx,
    h3zero_callback_ctx_t* h3_ctx, uint64_t stream_id, h3zero_stream_ctx_t** stream_ctx)
{
    static const uint8_t path[] = "/baton";
    static const uint8_t scheme[] = "https";
    static const uint8_t origin[] = "https://" PICOQUIC_TEST_SNI;
    h3zero_data_stream_state_t* stream_state = NULL;
    int ret = 0;

    if (picoquic_create_stream(cnx, stream_id) == NULL ||
        (*stream_ctx = h3zero_find_or_create_stream(cnx, stream_id, h3_ctx, 1, 1)) == NULL) {
        ret = -1;
    }
    else {
        picoquic_set_app_stream_ctx(cnx, stream_id, *stream_ctx);
        stream_state = &(*stream_ctx)->ps.stream_state;
        stream_state->header_found = 1;
        stream_state->header.method = h3zero_method_connect;
        stream_state->header.path = path;
        stream_state->header.path_length = sizeof(path) - 1;
        stream_state->header.path_is_static = 1;
        stream_state->header.scheme = scheme;
        stream_state->header.scheme_length = sizeof(scheme) - 1;
        stream_state->header.scheme_is_static = 1;
        stream_state->header.origin = origin;
        stream_state->header.origin_length = sizeof(origin) - 1;
        stream_state->header.origin_is_static = 1;
        stream_state->header.protocol = (const uint8_t*)picoquic_string_duplicate(H3ZERO_WEBTRANSPORT_H3_PROTOCOL);
        stream_state->header.protocol_length = sizeof(H3ZERO_WEBTRANSPORT_H3_PROTOCOL) - 1;
        stream_state->header.authority = (const uint8_t*)picoquic_string_duplicate(PICOQUIC_TEST_SNI);
        stream_state->header.authority_length = strlen(PICOQUIC_TEST_SNI);
        if (stream_state->header.protocol == NULL || stream_state->header.authority == NULL) {
            ret = -1;
        }
    }

    return ret;
}

static int picowt_session_limit_case(uint64_t local_flow_control,
    uint64_t remote_flow_control, int expect_reject)
{
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    h3zero_callback_ctx_t* h3_ctx = NULL;
    h3zero_stream_ctx_t* first_stream_ctx = NULL;
    h3zero_stream_ctx_t* second_stream_ctx = NULL;
    uint64_t simulated_time = 0;
    picowt_session_limit_test_ctx_t test_ctx = { 0 };
    picohttp_server_path_item_t path_table[1] = {
        { "/baton", 6, picowt_session_limit_callback, &test_ctx,
            H3ZERO_WEBTRANSPORT_H3_PROTOCOL, sizeof(H3ZERO_WEBTRANSPORT_H3_PROTOCOL) - 1 }
    };
    int ret = h3zero_set_test_context(&quic, &cnx, &h3_ctx, &simulated_time);

    if (ret == 0) {
        cnx->client_mode = 0;
        cnx->cnx_state = picoquic_state_ready;
        cnx->local_parameters.max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
        cnx->remote_parameters.max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
        cnx->local_parameters.is_reset_stream_at_enabled = 1;
        cnx->remote_parameters.is_reset_stream_at_enabled = 1;
        h3_ctx->path_table = path_table;
        h3_ctx->path_table_nb = 1;
        h3_ctx->settings.settings_received = 1;
        h3_ctx->settings.enable_connect_protocol = 1;
        h3_ctx->settings.h3_datagram = 1;
        h3_ctx->settings.webtransport_enabled = 1;
        h3_ctx->settings.wt_initial_max_data = remote_flow_control;
        h3_ctx->local_settings.wt_initial_max_data = local_flow_control;
        test_ctx.h3_ctx = h3_ctx;
    }

    if (ret == 0) {
        ret = picowt_session_limit_prepare_stream(cnx, h3_ctx, 0, &first_stream_ctx);
    }
    if (ret == 0) {
        ret = h3zero_process_request_frame(cnx, first_stream_ctx, h3_ctx);
    }
    if (ret == 0 && (!first_stream_ctx->is_upgraded ||
        h3_ctx->nb_webtransport_sessions != 1 || test_ctx.nb_connects != 1)) {
        ret = -1;
    }
    if (ret == 0) {
        ret = picowt_session_limit_prepare_stream(cnx, h3_ctx, 4, &second_stream_ctx);
    }
    if (ret == 0) {
        ret = h3zero_process_request_frame(cnx, second_stream_ctx, h3_ctx);
    }
    if (ret == 0) {
        picoquic_stream_head_t* second_stream = picoquic_find_stream(cnx, 4);

        if (expect_reject) {
            if (second_stream == NULL || !second_stream->reset_requested ||
                second_stream->local_error != H3ZERO_REQUEST_REJECTED ||
                second_stream_ctx->is_upgraded ||
                h3_ctx->nb_webtransport_sessions != 1 ||
                test_ctx.nb_connects != 1) {
                ret = -1;
            }
        }
        else if (second_stream == NULL || second_stream->reset_requested ||
            !second_stream_ctx->is_upgraded ||
            h3_ctx->nb_webtransport_sessions != 2 ||
            test_ctx.nb_connects != 2) {
            ret = -1;
        }
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

int picowt_session_limit_test(void)
{
    int ret = picowt_session_limit_case(0, 0, 1);

    if (ret == 0) {
        ret = picowt_session_limit_case(1, 1, 0);
    }

    return ret;
}
