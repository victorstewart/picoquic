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
        H3ZERO_WEBTRANSPORT_H3_PROTOCOL
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
        { "*", 1, wt_baton_callback, NULL, H3ZERO_WEBTRANSPORT_H3_PROTOCOL }
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
        ret = picowt_baton_protocol_refusal_test_one(11, NULL);
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
    int ret = picowt_baton_scheme_refusal_test_one(12, "http");

    if (ret == 0) {
        ret = picowt_baton_scheme_refusal_test_one(13, NULL);
    }

    return ret;
}

int picowt_baton_authority_test(void)
{
    return picowt_baton_test_one_ex(14, "/baton?baton=240", 0, 2000000, NULL, NULL,
        path_item_list, 1, "https", H3ZERO_WEBTRANSPORT_H3_PROTOCOL, "", NULL, 0, 0, 1);
}

int picowt_baton_origin_test(void)
{
    return picowt_baton_test_one_ex(15, "/baton?baton=240", 0, 2000000, NULL, NULL,
        path_item_list, 1, "https", H3ZERO_WEBTRANSPORT_H3_PROTOCOL, NULL, "", 0, 0, 1);
}

int picowt_baton_settings_test(void)
{
    return picowt_baton_test_one_ex(16, "/baton?baton=240", 0, 2000000, NULL, NULL,
        path_item_list, 1, "https", H3ZERO_WEBTRANSPORT_H3_PROTOCOL, NULL, NULL, 1, 0, 0);
}

int picowt_baton_alpn_test(void)
{
    picohttp_server_path_item_t bad_alpn_table[1] = {
        { "/baton", 6, wt_baton_bad_alpn_callback, &baton_test_ctx, H3ZERO_WEBTRANSPORT_H3_PROTOCOL }
    };
    return picowt_baton_test_one_ex(17, "/baton?baton=240", 0, 0, NULL, NULL,
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

int h3zero_set_test_context(picoquic_quic_t** quic, picoquic_cnx_t** cnx, h3zero_callback_ctx_t** h3_ctx, uint64_t* simulated_time);

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
