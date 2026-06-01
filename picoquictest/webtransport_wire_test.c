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

static int picowt_wire_expect_uint64(char const* what, uint64_t expected, uint64_t actual)
{
    int ret = 0;

    if (expected != actual) {
        DBG_PRINTF("%s: expected 0x%" PRIx64 ", got 0x%" PRIx64, what, expected, actual);
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
