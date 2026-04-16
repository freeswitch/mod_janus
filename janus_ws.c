/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * janus_ws.c -- Janus WebSocket API via libks (optional)
 */
#if defined(HAVE_MOD_JANUS_WS)

#include "libks/ks.h"
#include "janus_ws.h"
#include "api.h"
#include "cJSON.h"
#include "globals.h"
#include "switch_stun.h"

#define JANUS_WS_TXN_LEN 32
#define JANUS_WS_DEFAULT_RPC_US (15 * 1000000)

typedef struct janus_ws_pending_rpc {
	char transaction[JANUS_WS_TXN_LEN];
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;
	int done;
	cJSON *result;
	switch_memory_pool_t *pool;
} janus_ws_pending_rpc_t;

typedef struct {
	server_t *server;
	kws_t *kws;
	ks_pool_t *kpool;
	switch_mutex_t *write_mutex;
	switch_mutex_t *pending_mutex;
	switch_hash_t *pending;
	switch_memory_pool_t *pool;
} janus_ws_ctx_t;

static switch_mutex_t *g_libks_ref_mutex = NULL;
static int g_libks_refs = 0;

static void janus_ws_libks_acquire(switch_memory_pool_t *pool)
{
	if (!g_libks_ref_mutex) {
		switch_mutex_init(&g_libks_ref_mutex, SWITCH_MUTEX_NESTED, pool);
	}
	switch_mutex_lock(g_libks_ref_mutex);
	if (g_libks_refs == 0) {
		ks_ssl_init_skip(KS_TRUE);
		ks_init();
	}
	g_libks_refs++;
	switch_mutex_unlock(g_libks_ref_mutex);
}

static void janus_ws_libks_release(void)
{
	if (!g_libks_ref_mutex) {
		return;
	}
	switch_mutex_lock(g_libks_ref_mutex);
	if (g_libks_refs > 0) {
		g_libks_refs--;
	}
	if (g_libks_refs == 0) {
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

static janus_ws_ctx_t *janus_ws_ctx_get(server_t *server)
{
	return (janus_ws_ctx_t *) server->janus_ws_handle;
}

static switch_status_t janus_ws_process_json_text(janus_ws_ctx_t *ctx, server_t *server, const char *text,
	switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
	switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
	switch_bool_t (*pAnswerOnWebrtcupFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason))
{
	cJSON *root = cJSON_Parse(text);
	cJSON *janus_type;
	cJSON *txn;
	const char *jt;
	const char *tx = NULL;

	(void) server;

	if (!root) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "janus_ws: invalid JSON\n");
		return SWITCH_STATUS_FALSE;
	}

	janus_type = cJSON_GetObjectItemCaseSensitive(root, "janus");
	txn = cJSON_GetObjectItemCaseSensitive(root, "transaction");
	if (txn && cJSON_IsString(txn)) {
		tx = txn->valuestring;
	}

	if (tx && janus_type && cJSON_IsString(janus_type)) {
		jt = janus_type->valuestring;
		if (!strcmp(jt, "success") || !strcmp(jt, "ack") || !strcmp(jt, "error")) {
			janus_ws_pending_rpc_t *pending = NULL;
			switch_mutex_lock(ctx->pending_mutex);
			pending = (janus_ws_pending_rpc_t *) switch_core_hash_find(ctx->pending, tx);
			if (pending) {
				switch_core_hash_delete(ctx->pending, tx);
				switch_mutex_unlock(ctx->pending_mutex);

				switch_mutex_lock(pending->mutex);
				pending->result = root;
				pending->done = 1;
				switch_thread_cond_signal(pending->cond);
				switch_mutex_unlock(pending->mutex);
				return SWITCH_STATUS_SUCCESS;
			}
			switch_mutex_unlock(ctx->pending_mutex);
		}
	}

	(void) api_dispatch_poll_event(root, pJoinedFunc, pAcceptedFunc, pTrickleFunc, pAnswerOnWebrtcupFunc, pAnsweredFunc, pHungupFunc);
	cJSON_Delete(root);
	return SWITCH_STATUS_SUCCESS;
}

cJSON *janus_ws_rpc_json(server_t *server, cJSON *request, const char *transaction, switch_interval_time_t timeout_us)
{
	janus_ws_ctx_t *ctx = janus_ws_ctx_get(server);
	janus_ws_pending_rpc_t *pending = NULL;
	char *payload = NULL;
	ks_ssize_t wsz;
	cJSON *out = NULL;
	switch_interval_time_t wait_step = 100000;
	switch_interval_time_t waited = 0;

	if (!ctx || !ctx->kws || !request || !transaction) {
		return NULL;
	}

	pending = switch_core_alloc(ctx->pool, sizeof(*pending));
	switch_mutex_init(&pending->mutex, SWITCH_MUTEX_NESTED, ctx->pool);
	switch_thread_cond_create(&pending->cond, ctx->pool);
	pending->pool = ctx->pool;
	pending->done = 0;
	pending->result = NULL;
	switch_copy_string(pending->transaction, transaction, sizeof(pending->transaction));

	switch_mutex_lock(ctx->pending_mutex);
	switch_core_hash_insert(ctx->pending, pending->transaction, pending);
	switch_mutex_unlock(ctx->pending_mutex);

	payload = cJSON_PrintUnformatted(request);
	if (!payload) {
		goto fail_waiter;
	}

	switch_mutex_lock(ctx->write_mutex);
	wsz = kws_write_frame(ctx->kws, WSOC_TEXT, payload, (ks_size_t)strlen(payload));
	switch_mutex_unlock(ctx->write_mutex);
	cJSON_free(payload);
	payload = NULL;

	if (wsz < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "janus_ws: write failed\n");
		goto fail_waiter;
	}

	switch_mutex_lock(pending->mutex);
	while (!pending->done && waited < timeout_us) {
		switch_status_t wst = switch_thread_cond_timedwait(pending->cond, pending->mutex, wait_step);
		if (wst == SWITCH_STATUS_TIMEOUT) {
			waited += wait_step;
		}
	}
	if (!pending->done) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "janus_ws: RPC timeout transaction=%s\n", transaction);
		switch_mutex_unlock(pending->mutex);
		goto fail_waiter;
	}
	out = pending->result;
	pending->result = NULL;
	switch_mutex_unlock(pending->mutex);

	switch_mutex_destroy(pending->mutex);
	switch_thread_cond_destroy(pending->cond);
	switch_core_free(ctx->pool, pending);
	return out;

fail_waiter:
	switch_mutex_lock(ctx->pending_mutex);
	switch_core_hash_delete(ctx->pending, pending->transaction);
	switch_mutex_unlock(ctx->pending_mutex);

	switch_mutex_lock(pending->mutex);
	if (pending->result) {
		cJSON_Delete(pending->result);
	}
	switch_mutex_unlock(pending->mutex);
	switch_mutex_destroy(pending->mutex);
	switch_thread_cond_destroy(pending->cond);
	switch_core_free(ctx->pool, pending);
	return NULL;
}

static switch_status_t janus_ws_drain_incoming_frames(janus_ws_ctx_t *ctx, server_t *server,
	switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
	switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
	switch_bool_t (*pAnswerOnWebrtcupFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason))
{
	for (;;) {
		kws_opcode_t oc = WSOC_INVALID;
		uint8_t *data = NULL;
		ks_ssize_t bytes = kws_read_frame(ctx->kws, &oc, &data);
		char *text = NULL;

		if (bytes < 0) {
			return SWITCH_STATUS_FALSE;
		}
		if (bytes == 0) {
			return SWITCH_STATUS_SUCCESS;
		}
		if (oc != WSOC_TEXT) {
			continue;
		}
		text = (char *) malloc((size_t)bytes + 1);
		if (!text) {
			return SWITCH_STATUS_FALSE;
		}
		memcpy(text, data, (size_t)bytes);
		text[bytes] = '\0';
		janus_ws_process_json_text(ctx, server, text, pJoinedFunc, pAcceptedFunc, pTrickleFunc, pAnswerOnWebrtcupFunc, pAnsweredFunc, pHungupFunc);
		free(text);
	}
}

switch_status_t janus_ws_pump_once(server_t *server, janus_id_t session_id,
	switch_interval_time_t wait_us,
	switch_interval_time_t keepalive_interval_us,
	switch_time_t *last_activity_ref,
	switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
	switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
	switch_bool_t (*pAnswerOnWebrtcupFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason))
{
	janus_ws_ctx_t *ctx = janus_ws_ctx_get(server);
	int pr = 0;
	switch_time_t now;

	if (!ctx || !ctx->kws) {
		return SWITCH_STATUS_FALSE;
	}

	now = switch_time_now();
	if (keepalive_interval_us > 0 && session_id && last_activity_ref && (*last_activity_ref > 0) &&
		(now - *last_activity_ref) > keepalive_interval_us) {
		cJSON *ka = cJSON_CreateObject();
		char txn[17] = { 0 };
		cJSON *resp = NULL;

		if (ka) {
			switch_stun_random_string(txn, sizeof(txn) - 1, NULL);
			cJSON_AddStringToObject(ka, "janus", "keepalive");
			cJSON_AddNumberToObject(ka, "session_id", (double)session_id);
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

	pr = kws_wait_sock(ctx->kws, (uint32_t)(wait_us / 1000), KS_POLL_READ);
	if (pr == KS_POLL_ERROR || pr < 0) {
		return SWITCH_STATUS_FALSE;
	}
	if (pr & KS_POLL_READ) {
		if (janus_ws_drain_incoming_frames(ctx, server, pJoinedFunc, pAcceptedFunc, pTrickleFunc, pAnswerOnWebrtcupFunc, pAnsweredFunc, pHungupFunc) != SWITCH_STATUS_SUCCESS) {
			return SWITCH_STATUS_FALSE;
		}
		if (last_activity_ref) {
			*last_activity_ref = switch_time_now();
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t janus_ws_server_open(server_t *server)
{
	janus_ws_ctx_t *ctx = NULL;
	ks_json_t *params = NULL;
	ks_status_t kst;

	if (server->janus_ws_handle) {
		return SWITCH_STATUS_SUCCESS;
	}

	janus_ws_libks_acquire(globals.pModulePool);

	ctx = switch_core_alloc(globals.pModulePool, sizeof(*ctx));
	ctx->server = server;
	ctx->pool = globals.pModulePool;
	ctx->kpool = NULL;
	ctx->kws = NULL;
	switch_mutex_init(&ctx->write_mutex, SWITCH_MUTEX_NESTED, ctx->pool);
	switch_mutex_init(&ctx->pending_mutex, SWITCH_MUTEX_NESTED, ctx->pool);
	switch_core_hash_init(&ctx->pending);

	if (ks_pool_open(&ctx->kpool) != KS_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "janus_ws: ks_pool_open failed\n");
		goto fail;
	}

	params = ks_json_create_object();
	ks_json_add_string_to_object(params, "url", server->pUrl);
	ks_json_add_string_to_object(params, "protocol", "janus-protocol");
#if defined(KS_VERSION_NUM) && KS_VERSION_NUM >= 20000
	ks_json_add_number_to_object(params, "payload_size_max", 1000000);
#endif

	kst = kws_connect_ex(&ctx->kws, params, KWS_BLOCK, ctx->kpool, NULL, 30000);
	ks_json_delete(&params);
	params = NULL;

	if (kst != KS_STATUS_SUCCESS || !ctx->kws) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "janus_ws: connect failed for %s\n", server->pUrl);
		goto fail;
	}

	server->janus_ws_handle = ctx;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "janus_ws: connected server=%s url=%s\n", server->name, server->pUrl);
	return SWITCH_STATUS_SUCCESS;

fail:
	if (params) {
		ks_json_delete(&params);
	}
	if (ctx) {
		if (ctx->kws) {
			kws_destroy(&ctx->kws);
		}
		if (ctx->kpool) {
			ks_pool_close(&ctx->kpool);
		}
		switch_core_hash_destroy(&ctx->pending);
		switch_mutex_destroy(ctx->write_mutex);
		switch_mutex_destroy(ctx->pending_mutex);
		switch_core_free(ctx->pool, ctx);
	}
	janus_ws_libks_release();
	return SWITCH_STATUS_FALSE;
}

void janus_ws_server_close(server_t *server)
{
	janus_ws_ctx_t *ctx = janus_ws_ctx_get(server);

	if (!ctx) {
		return;
	}

	if (ctx->kws) {
		kws_close(ctx->kws, WS_NONE);
		kws_destroy(&ctx->kws);
	}
	if (ctx->kpool) {
		ks_pool_close(&ctx->kpool);
	}
	switch_mutex_destroy(ctx->write_mutex);
	switch_mutex_destroy(ctx->pending_mutex);
	switch_core_hash_destroy(&ctx->pending);
	server->janus_ws_handle = NULL;
	switch_core_free(ctx->pool, ctx);
	janus_ws_libks_release();
}

#endif /* HAVE_MOD_JANUS_WS */
