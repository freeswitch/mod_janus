/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * janus_ws.c -- Janus WebSocket transport via libks.
 *
 * Design:
 *   - One persistent WebSocket per server_t (server->janus_ws_handle).
 *   - All socket I/O (write + poll + read) is serialized under ctx->io_mutex.
 *   - Because io_mutex makes every RPC strictly serial, the ctx only tracks
 *     ONE in-flight RPC transaction (ctx->rpc_txn / ctx->rpc_result) instead
 *     of a hash/cond/per-request subpool.
 *   - Non-RPC frames arriving during a sync RPC are stashed on ctx->deferred
 *     and dispatched by the next pump iteration, so Janus events are never
 *     silently dropped.
 */
#if defined(HAVE_MOD_JANUS_WS)

#include "libks/ks.h"
#include "janus_ws.h"
#include "api.h"
#include "cJSON.h"
#include "globals.h"
#include "switch_stun.h"

#define JANUS_WS_DEFAULT_RPC_US (15 * 1000000)

/* Max time the pump holds io_mutex across kws_wait_sock before releasing it
 * so RPC callers can interleave. 200 ms gives decent responsiveness while
 * keeping syscall overhead negligible. */
#define JANUS_WS_PUMP_SLICE_MS  200

typedef struct janus_ws_deferred_s {
	cJSON *root;
	struct janus_ws_deferred_s *next;
} janus_ws_deferred_t;

typedef struct {
	server_t *server;
	kws_t *kws;
	ks_pool_t *kpool;
	switch_mutex_t *io_mutex; /* serializes all kws_* I/O */
	switch_memory_pool_t *pool;

	/* Current synchronous RPC, protected by io_mutex. */
	const char *rpc_txn;
	cJSON *rpc_result;

	/* Non-RPC frames captured during an RPC; dispatched by the pump. */
	janus_ws_deferred_t *deferred_head;
	janus_ws_deferred_t *deferred_tail;
} janus_ws_ctx_t;

/* -------------------------------------------------------------------------- */
/* libks global init refcount                                                 */
/* -------------------------------------------------------------------------- */

static switch_mutex_t *g_libks_ref_mutex = NULL;
static int g_libks_refs = 0;

static void janus_ws_libks_acquire(switch_memory_pool_t *pool)
{
	if (!g_libks_ref_mutex) {
		switch_mutex_init(&g_libks_ref_mutex, SWITCH_MUTEX_NESTED, pool);
	}
	switch_mutex_lock(g_libks_ref_mutex);
	if (g_libks_refs++ == 0) {
		ks_ssl_init_skip(KS_TRUE);
		ks_init();
	}
	switch_mutex_unlock(g_libks_ref_mutex);
}

static void janus_ws_libks_release(void)
{
	if (!g_libks_ref_mutex) {
		return;
	}
	switch_mutex_lock(g_libks_ref_mutex);
	if (g_libks_refs > 0 && --g_libks_refs == 0) {
		ks_shutdown();
	}
	switch_mutex_unlock(g_libks_ref_mutex);
}

void janus_ws_mod_global_shutdown(void)
{
	if (!g_libks_ref_mutex) {
		return;
	}
	switch_mutex_lock(g_libks_ref_mutex);
	if (g_libks_refs > 0) {
		g_libks_refs = 0;
		ks_shutdown();
	}
	switch_mutex_unlock(g_libks_ref_mutex);
}

/* -------------------------------------------------------------------------- */
/* helpers                                                                    */
/* -------------------------------------------------------------------------- */

static janus_ws_ctx_t *janus_ws_ctx_get(server_t *server)
{
	return server ? (janus_ws_ctx_t *) server->janus_ws_handle : NULL;
}

static void janus_ws_dbg_json(const char *dir, const char *json)
{
	if (json && *json) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "janus_ws %s %s\n", dir, json);
	}
}

/* Add `root` to the deferred list. Takes ownership on success; caller must
 * cJSON_Delete(root) if this returns SWITCH_FALSE. */
static switch_bool_t janus_ws_defer_event(janus_ws_ctx_t *ctx, cJSON *root)
{
	janus_ws_deferred_t *node = malloc(sizeof(*node));
	if (!node) {
		return SWITCH_FALSE;
	}
	node->root = root;
	node->next = NULL;
	if (ctx->deferred_tail) {
		ctx->deferred_tail->next = node;
	} else {
		ctx->deferred_head = node;
	}
	ctx->deferred_tail = node;
	return SWITCH_TRUE;
}

/* Test whether `root` is the reply for the currently in-flight RPC (ctx->rpc_txn).
 * Takes ownership of `root` and stashes it on ctx->rpc_result on match. */
static switch_bool_t janus_ws_match_rpc_reply(janus_ws_ctx_t *ctx, cJSON *root)
{
	cJSON *txn;
	cJSON *jt;

	if (!ctx->rpc_txn) {
		return SWITCH_FALSE;
	}
	txn = cJSON_GetObjectItemCaseSensitive(root, "transaction");
	jt  = cJSON_GetObjectItemCaseSensitive(root, "janus");
	if (!cJSON_IsString(txn) || !cJSON_IsString(jt)) {
		return SWITCH_FALSE;
	}
	if (strcmp(txn->valuestring, ctx->rpc_txn) != 0) {
		return SWITCH_FALSE;
	}
	if (strcmp(jt->valuestring, "success") &&
		strcmp(jt->valuestring, "ack") &&
		strcmp(jt->valuestring, "error")) {
		return SWITCH_FALSE;
	}
	ctx->rpc_result = root;
	return SWITCH_TRUE;
}

/* -------------------------------------------------------------------------- */
/* drain                                                                      */
/* -------------------------------------------------------------------------- */

/* Read every frame currently available on the socket.
 *
 * For each TEXT frame:
 *   - If it matches the in-flight RPC transaction: store on ctx->rpc_result.
 *   - Else if `dispatch` is non-NULL: dispatch via api_dispatch_poll_event.
 *   - Else: append to ctx->deferred for the pump to process later.
 *
 * Caller MUST hold ctx->io_mutex.
 */
typedef struct {
	switch_status_t (*joined)(const janus_id_t, const janus_id_t, const janus_id_t, const janus_id_t);
	switch_status_t (*accepted)(const janus_id_t, const janus_id_t, const char *);
	switch_status_t (*trickle)(const janus_id_t, const janus_id_t, const char *);
	switch_bool_t   (*answer_on_webrtcup)(const janus_id_t, const janus_id_t);
	switch_status_t (*answered)(const janus_id_t, const janus_id_t);
	switch_status_t (*hungup)(const janus_id_t, const janus_id_t, const char *);
} janus_ws_dispatch_t;

static void janus_ws_dispatch_event(cJSON *root, const janus_ws_dispatch_t *d)
{
	(void) api_dispatch_poll_event(root,
		d->joined, d->accepted, d->trickle, d->answer_on_webrtcup, d->answered, d->hungup);
	cJSON_Delete(root);
}

static switch_status_t janus_ws_drain(janus_ws_ctx_t *ctx, const janus_ws_dispatch_t *dispatch)
{
	for (;;) {
		kws_opcode_t oc = WSOC_INVALID;
		uint8_t *data = NULL;
		ks_ssize_t bytes;
		char *text;
		cJSON *root;

		/*
		 * libks kws_read_frame blocks for WS_BLOCK (10 s) when the socket is
		 * idle, and on timeout it returns -1 AND closes the underlying socket
		 * (via kws_close). To stay non-blocking, peek with a 0 ms poll first
		 * and only call kws_read_frame when we know data is ready. This also
		 * picks up kws->unprocessed_buffer_len / SSL_pending via kws_wait_sock.
		 */
		{
			int pr = kws_wait_sock(ctx->kws, 0, KS_POLL_READ);
			if (pr < 0 || (pr & KS_POLL_INVALID) || ((pr & KS_POLL_ERROR) && !(pr & KS_POLL_READ))) {
				return SWITCH_STATUS_FALSE;
			}
			if (!(pr & KS_POLL_READ)) {
				return SWITCH_STATUS_SUCCESS;
			}
		}

		bytes = kws_read_frame(ctx->kws, &oc, &data);
		if (bytes < 0) {
			return SWITCH_STATUS_FALSE;
		}
		if (bytes == 0) {
			return SWITCH_STATUS_SUCCESS;
		}
		if (oc != WSOC_TEXT) {
			continue;
		}

		text = malloc((size_t) bytes + 1);
		if (!text) {
			return SWITCH_STATUS_FALSE;
		}
		memcpy(text, data, (size_t) bytes);
		text[bytes] = '\0';
		janus_ws_dbg_json("recv", text);

		root = cJSON_Parse(text);
		free(text);
		if (!root) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "janus_ws: invalid JSON\n");
			continue;
		}

		if (janus_ws_match_rpc_reply(ctx, root)) {
			continue; /* owned by ctx->rpc_result */
		}
		if (dispatch) {
			janus_ws_dispatch_event(root, dispatch);
		} else if (!janus_ws_defer_event(ctx, root)) {
			cJSON_Delete(root);
		}
	}
}

static void janus_ws_flush_deferred(janus_ws_ctx_t *ctx, const janus_ws_dispatch_t *dispatch)
{
	janus_ws_deferred_t *node = ctx->deferred_head;
	ctx->deferred_head = ctx->deferred_tail = NULL;
	while (node) {
		janus_ws_deferred_t *next = node->next;
		if (dispatch) {
			janus_ws_dispatch_event(node->root, dispatch);
		} else {
			cJSON_Delete(node->root);
		}
		free(node);
		node = next;
	}
}

/* -------------------------------------------------------------------------- */
/* public: synchronous RPC                                                    */
/* -------------------------------------------------------------------------- */

cJSON *janus_ws_rpc_json(server_t *server, cJSON *request, const char *transaction, switch_interval_time_t timeout_us)
{
	janus_ws_ctx_t *ctx = janus_ws_ctx_get(server);
	char *payload = NULL;
	cJSON *result = NULL;
	switch_time_t deadline;

	if (!ctx || !ctx->kws || !request || !transaction) {
		return NULL;
	}

	switch_mutex_lock(ctx->io_mutex);

	payload = cJSON_PrintUnformatted(request);
	if (!payload) {
		switch_mutex_unlock(ctx->io_mutex);
		return NULL;
	}
	janus_ws_dbg_json("send", payload);

	if (kws_write_frame(ctx->kws, WSOC_TEXT, payload, strlen(payload)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "janus_ws: write failed\n");
		cJSON_free(payload);
		switch_mutex_unlock(ctx->io_mutex);
		return NULL;
	}
	cJSON_free(payload);

	ctx->rpc_txn    = transaction;
	ctx->rpc_result = NULL;

	deadline = switch_time_now() + timeout_us;
	while (!ctx->rpc_result && switch_time_now() < deadline) {
		switch_interval_time_t remaining = deadline - switch_time_now();
		uint32_t slice_ms = (uint32_t) ((remaining < 100000 ? remaining : 100000) / 1000);
		int pr = kws_wait_sock(ctx->kws, slice_ms, KS_POLL_READ);

		if (pr < 0 || (pr & KS_POLL_INVALID) || ((pr & KS_POLL_ERROR) && !(pr & KS_POLL_READ))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"janus_ws: RPC wait poll error pr=%d\n", pr);
			break;
		}
		if ((pr & KS_POLL_READ) && janus_ws_drain(ctx, NULL) != SWITCH_STATUS_SUCCESS) {
			break;
		}
	}

	if (!ctx->rpc_result) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"janus_ws: RPC timeout transaction=%s\n", transaction);
	}

	result = ctx->rpc_result;
	ctx->rpc_txn    = NULL;
	ctx->rpc_result = NULL;

	switch_mutex_unlock(ctx->io_mutex);
	return result;
}

/* -------------------------------------------------------------------------- */
/* public: event pump                                                         */
/* -------------------------------------------------------------------------- */

switch_status_t janus_ws_pump_once(server_t *server, janus_id_t session_id,
	switch_interval_time_t wait_us,
	switch_interval_time_t keepalive_interval_us,
	switch_time_t *last_activity_ref,
	switch_status_t (*pJoinedFunc)(const janus_id_t, const janus_id_t, const janus_id_t, const janus_id_t),
	switch_status_t (*pAcceptedFunc)(const janus_id_t, const janus_id_t, const char *),
	switch_status_t (*pTrickleFunc)(const janus_id_t, const janus_id_t, const char *),
	switch_bool_t   (*pAnswerOnWebrtcupFunc)(const janus_id_t, const janus_id_t),
	switch_status_t (*pAnsweredFunc)(const janus_id_t, const janus_id_t),
	switch_status_t (*pHungupFunc)(const janus_id_t, const janus_id_t, const char *))
{
	janus_ws_ctx_t *ctx = janus_ws_ctx_get(server);
	janus_ws_dispatch_t dispatch;
	int pr;

	if (!ctx || !ctx->kws) {
		return SWITCH_STATUS_FALSE;
	}

	dispatch.joined             = pJoinedFunc;
	dispatch.accepted           = pAcceptedFunc;
	dispatch.trickle            = pTrickleFunc;
	dispatch.answer_on_webrtcup = pAnswerOnWebrtcupFunc;
	dispatch.answered           = pAnsweredFunc;
	dispatch.hungup             = pHungupFunc;

	/* Keepalive. Nested RPC re-enters io_mutex (NESTED), then drain happens below. */
	if (keepalive_interval_us > 0 && session_id && last_activity_ref && *last_activity_ref > 0 &&
		(switch_time_now() - *last_activity_ref) > keepalive_interval_us) {
		cJSON *ka = cJSON_CreateObject();
		if (ka) {
			char txn[17] = {0};
			cJSON *resp;
			switch_stun_random_string(txn, sizeof(txn) - 1, NULL);
			cJSON_AddStringToObject(ka, "janus", "keepalive");
			cJSON_AddNumberToObject(ka, "session_id", (double) session_id);
			cJSON_AddStringToObject(ka, "transaction", txn);
			if (server->pSecret) {
				cJSON_AddStringToObject(ka, "apisecret", server->pSecret);
			}
			resp = janus_ws_rpc_json(server, ka, txn, JANUS_WS_DEFAULT_RPC_US);
			cJSON_Delete(ka);
			if (resp) {
				cJSON_Delete(resp);
			}
			*last_activity_ref = switch_time_now();
		}
	}

	/*
	 * Slice the long pump wait into short windows so that concurrent RPC
	 * callers (attach, createroom, joinroom, hangup, ...) can acquire
	 * ctx->io_mutex between slices. Holding the mutex across a full
	 * wait_us (up to 60 s) would starve all outbound RPCs — exactly the
	 * "attach queued but not sent until next pump iteration" symptom.
	 *
	 * Slice length is capped at JANUS_WS_PUMP_SLICE_MS. Between slices we
	 * release the mutex, yield, and re-check. If data becomes available we
	 * drain it and return promptly so the outer loop can decide what to do.
	 */
	{
		const switch_time_t deadline = switch_time_now() + wait_us;
		for (;;) {
			const switch_interval_time_t remain_us = deadline - switch_time_now();
			uint32_t slice_ms;

			if (remain_us <= 0) {
				return SWITCH_STATUS_SUCCESS;
			}
			/* Clamp: never hold io_mutex longer than JANUS_WS_PUMP_SLICE_MS. */
			slice_ms = (remain_us < (JANUS_WS_PUMP_SLICE_MS * 1000))
				? (uint32_t) (remain_us / 1000)
				: JANUS_WS_PUMP_SLICE_MS;
			if (slice_ms == 0) {
				slice_ms = 1;
			}

			switch_mutex_lock(ctx->io_mutex);

			janus_ws_flush_deferred(ctx, &dispatch);

			pr = kws_wait_sock(ctx->kws, slice_ms, KS_POLL_READ);
			if (pr < 0 || (pr & KS_POLL_INVALID) ||
				((pr & (KS_POLL_ERROR | KS_POLL_HUP)) && !(pr & KS_POLL_READ))) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					"janus_ws: pump poll failed pr=%d (0x%x) slice_ms=%u\n",
					pr, (unsigned) pr, slice_ms);
				switch_mutex_unlock(ctx->io_mutex);
				return SWITCH_STATUS_FALSE;
			}
			if (pr & KS_POLL_READ) {
				if (janus_ws_drain(ctx, &dispatch) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "janus_ws: pump drain failed\n");
					switch_mutex_unlock(ctx->io_mutex);
					return SWITCH_STATUS_FALSE;
				}
				if (last_activity_ref) {
					*last_activity_ref = switch_time_now();
				}
				switch_mutex_unlock(ctx->io_mutex);
				return SWITCH_STATUS_SUCCESS;
			}

			switch_mutex_unlock(ctx->io_mutex);
			/* Brief yield so any thread blocked on io_mutex wins the next round. */
			switch_cond_next();
		}
	}
}

/* -------------------------------------------------------------------------- */
/* public: connect / disconnect                                               */
/* -------------------------------------------------------------------------- */

switch_status_t janus_ws_server_open(server_t *server)
{
	janus_ws_ctx_t *ctx;
	switch_memory_pool_t *ctx_pool = NULL;
	ks_json_t *params;
	ks_status_t kst;

	if (!server) {
		return SWITCH_STATUS_FALSE;
	}
	if (server->janus_ws_handle) {
		return SWITCH_STATUS_SUCCESS;
	}

	janus_ws_libks_acquire(globals.pModulePool);

	if (switch_core_new_memory_pool(&ctx_pool) != SWITCH_STATUS_SUCCESS) {
		janus_ws_libks_release();
		return SWITCH_STATUS_FALSE;
	}
	ctx = switch_core_alloc(ctx_pool, sizeof(*ctx));
	memset(ctx, 0, sizeof(*ctx));
	ctx->server = server;
	ctx->pool   = ctx_pool;
	switch_mutex_init(&ctx->io_mutex, SWITCH_MUTEX_NESTED, ctx->pool);

	if (ks_pool_open(&ctx->kpool) != KS_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "janus_ws: ks_pool_open failed\n");
		goto fail;
	}

	params = ks_json_create_object();
	ks_json_add_string_to_object(params, "url",      server->pUrl);
	ks_json_add_string_to_object(params, "protocol", "janus-protocol");
#if defined(KS_VERSION_NUM) && KS_VERSION_NUM >= 20000
	ks_json_add_number_to_object(params, "payload_size_max", 1000000);
#endif
	kst = kws_connect_ex(&ctx->kws, params, KWS_BLOCK, ctx->kpool, NULL, 30000);
	ks_json_delete(&params);

	if (kst != KS_STATUS_SUCCESS || !ctx->kws) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "janus_ws: connect failed for %s\n", server->pUrl);
		goto fail;
	}

	server->janus_ws_handle = ctx;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"janus_ws: connected server=%s url=%s\n", server->name, server->pUrl);
	return SWITCH_STATUS_SUCCESS;

fail:
	if (ctx->kws) {
		kws_destroy(&ctx->kws);
	}
	if (ctx->kpool) {
		ks_pool_close(&ctx->kpool);
	}
	switch_mutex_destroy(ctx->io_mutex);
	switch_core_destroy_memory_pool(&ctx->pool);
	janus_ws_libks_release();
	return SWITCH_STATUS_FALSE;
}

void janus_ws_server_close(server_t *server)
{
	janus_ws_ctx_t *ctx = janus_ws_ctx_get(server);

	if (!ctx) {
		return;
	}
	server->janus_ws_handle = NULL;

	if (ctx->kws) {
		kws_close(ctx->kws, WS_NONE);
		kws_destroy(&ctx->kws);
	}
	if (ctx->kpool) {
		ks_pool_close(&ctx->kpool);
	}
	if (ctx->rpc_result) {
		cJSON_Delete(ctx->rpc_result);
	}
	janus_ws_flush_deferred(ctx, NULL);
	switch_mutex_destroy(ctx->io_mutex);
	switch_core_destroy_memory_pool(&ctx->pool);
	janus_ws_libks_release();
}

#endif /* HAVE_MOD_JANUS_WS */
