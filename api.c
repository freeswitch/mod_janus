/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 * Richard Screene <richard.screene@thisisdrum.com>
 *
 *
 * api.c -- API functions for janus endpoint module
 *
 */

// need latest version of cJSON to handle size of Janus identifiers (~64-bit)
#include  "cJSON.h"
#include  "switch.h"
// use switch_stun_random_string() to get a transactionId
#include  "switch_stun.h"
#include  "globals.h"
#include  "servers.h"
#include  "http.h"
#include  "auth.h"
#include  "api.h"
#if defined(HAVE_MOD_JANUS_WS)
#include  "janus_ws.h"
#endif

#define TRANSACTION_ID_LENGTH 16
#define JANUS_STRING  "janus"
#define JANUS_PLUGIN "janus.plugin.audiobridge"
#define	MAX_POLL_EVENTS 10
// The long-poll request has a 30 seconds timeout. If it has no event to report, a simple keep-alive message will be triggered
#define HTTP_GET_TIMEOUT 60000
#define HTTP_POST_TIMEOUT 3000

/*
 * Maximum number of descriptors we ever embed in a signed token. The plugin
 * package itself plus one scope descriptor (e.g. "room=<id>") is enough for
 * today; bump this if we ever need more.
 */
#define MAX_TOKEN_DESCRIPTORS 4

/*
 * URL flavour for HTTP transport. WS transport ignores this and adds the
 * session_id / handle_id into the JSON body instead.
 */
typedef enum {
	URL_ROOT,    /* pUrl                            (create)              */
	URL_SESSION, /* pUrl/<serverId>                 (claim, attach)       */
	URL_HANDLE   /* pUrl/<serverId>/<senderId>      (everything handle)   */
} api_url_kind_t;

#if defined(HAVE_MOD_JANUS_WS)
/*
 * Add a uint64 field as a raw JSON integer. cJSON stores numbers as `double`
 * and its printer may fall back to `%1.15g` (e.g. "8.31461053888952e+15"),
 * which Janus/Jansson parses as a JSON real and rejects for integer-only
 * fields like session_id / handle_id. Emit the value verbatim instead.
 */
static void api_ws_add_u64(cJSON *root, const char *name, uint64_t value)
{
	char buf[32];
	(void) snprintf(buf, sizeof(buf), "%" SWITCH_UINT64_T_FMT, value);
	cJSON_AddRawToObject(root, name, buf);
}
#endif

typedef struct {
	const char *pType;
	janus_id_t serverId;
	const char *pTransactionId;
	const char *opaqueId;
	janus_id_t senderId;
	switch_bool_t isPlugin;
	const char *pSecret;
	/*
	 * When pHmacSecret is non-NULL, encode() will generate a fresh HMAC-SHA1
	 * signed token (TTL = hmacTokenTtl seconds, fallback to 300) that embeds
	 * the plugin package plus any additional descriptors listed below, and
	 * attach it to the top-level request as the `token` field. The same
	 * token is also returned via pSignedTokenOut so the caller can reuse it
	 * in nested locations such as the audiobridge join body.
	 */
	const char *pHmacSecret;
	int hmacTokenTtl;
	const char *pExtraDescriptors[MAX_TOKEN_DESCRIPTORS];
	int nExtraDescriptors;
	char **pSignedTokenOut; /* optional: receives malloc'd token; caller frees */
	cJSON *pJsonBody;
	cJSON *pJsonJsep;
	const char *pCandidate;
} message_t;

// calling process must delete the returned value
static char *generateTransactionId() {
	char *pTransactionId;


	switch_malloc(pTransactionId, TRANSACTION_ID_LENGTH);

	switch_stun_random_string(pTransactionId, TRANSACTION_ID_LENGTH - 1, NULL);
	// add terminating null
	pTransactionId[TRANSACTION_ID_LENGTH - 1] = '\0';

	return pTransactionId;
}

// calling process is responsible for freeing the returned JSON object
static cJSON *encode(const message_t message) {
	cJSON *pJsonRequest = cJSON_CreateObject();

	if (pJsonRequest == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		goto error;
	}

	if (cJSON_AddStringToObject(pJsonRequest, JANUS_STRING, message.pType) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (create)\n");
		goto error;
	}

	if (message.pTransactionId) {
		if (cJSON_AddStringToObject(pJsonRequest, "transaction", message.pTransactionId) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (transaction)\n");
			goto error;
		}
	}

	if (message.pSecret) {
		if (cJSON_AddStringToObject(pJsonRequest, "apisecret", message.pSecret) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (apisecret)\n");
			goto error;
		}
	}

	if (message.pHmacSecret) {
		/*
		 * Always include the plugin package as the first descriptor so that
		 * Janus core's per-plugin access check passes. Extra descriptors
		 * (e.g. "room=<id>") are appended to scope the token further; the
		 * audiobridge/videoroom signed_tokens enforcement will look those up
		 * via auth_signature_contains().
		 */
		const char *pDescriptors[MAX_TOKEN_DESCRIPTORS + 1];
		int ndesc = 0;
		int i;
		pDescriptors[ndesc++] = JANUS_PLUGIN;
		for (i = 0; i < message.nExtraDescriptors && ndesc <= MAX_TOKEN_DESCRIPTORS; i++) {
			if (message.pExtraDescriptors[i] && *message.pExtraDescriptors[i]) {
				pDescriptors[ndesc++] = message.pExtraDescriptors[i];
			}
		}

		{
			int ttl = message.hmacTokenTtl > 0 ? message.hmacTokenTtl : API_HMAC_DEFAULT_LIFECYCLE_TTL;
			char *pToken = authSignToken(message.pHmacSecret, ttl, pDescriptors, ndesc);
			if (!pToken) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot sign token\n");
				goto error;
			}

			if (cJSON_AddStringToObject(pJsonRequest, "token", pToken) == NULL) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (token)\n");
				free(pToken);
				goto error;
			}

			if (message.pSignedTokenOut) {
				/* Hand ownership to the caller; they must free(). */
				*message.pSignedTokenOut = pToken;
			} else {
				free(pToken);
			}
		}
	}

	if (message.opaqueId) {
		if (cJSON_AddStringToObject(pJsonRequest, "opaque_id", message.opaqueId) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (opaque_id)\n");
			goto error;
		}
	}

	if (message.isPlugin) {
		if (cJSON_AddStringToObject(pJsonRequest, "plugin", JANUS_PLUGIN) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (plugin)\n");
			goto error;
		}
	}

	if (message.pJsonBody) {
		cJSON_AddItemToObject(pJsonRequest, "body", message.pJsonBody);
	}

	if (message.pJsonJsep) {
		cJSON_AddItemToObject(pJsonRequest, "jsep", message.pJsonJsep);
	}

	return pJsonRequest;

	error:
	cJSON_Delete(pJsonRequest);
	return NULL;
}

// calling process should delete return value
static message_t *decode(cJSON *pJsonResponse) {
	message_t *pMessage;
	cJSON *pJsonRspPluginData;
	cJSON *pJsonRspPlugin;
	cJSON *pJsonRspCandidate;
	cJSON *pJsonRspCandidateData;
	cJSON *pJsonRspCandidateCompleted;
	cJSON *pJsonRspJanus;
	cJSON *pJsonRspTransaction;
	cJSON *pJsonRspServerId;
	cJSON *pJsonRspSender;

	if (pJsonResponse == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No response to decode\n");
		return NULL;
	}

	switch_zmalloc(pMessage, sizeof(*pMessage));

	pMessage->pJsonBody = cJSON_GetObjectItemCaseSensitive(pJsonResponse, "data");
	if (pMessage->pJsonBody && !cJSON_IsObject(pMessage->pJsonBody)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid type (data)\n");
		return NULL;
	} else if (!pMessage->pJsonBody) {
		if ((pJsonRspPluginData = cJSON_GetObjectItemCaseSensitive(pJsonResponse, "plugindata")) != NULL) {
			if (cJSON_IsObject(pJsonRspPluginData)) {
				pJsonRspPlugin = cJSON_GetObjectItemCaseSensitive(pJsonRspPluginData, "plugin");
			  	if (!cJSON_IsString(pJsonRspPlugin) || strcmp(JANUS_PLUGIN, pJsonRspPlugin->valuestring)) {
			    	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.plugin)\n");
					return NULL;
			  	}

			  	pMessage->pJsonBody = cJSON_GetObjectItemCaseSensitive(pJsonRspPluginData, "data");
			  	if (!cJSON_IsObject(pMessage->pJsonBody)) {
			    	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid type (plugindata.data)\n");
					return NULL;
			  	}
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid type (plugindata)\n");
				return NULL;
			}
		} else if ((pJsonRspCandidate = cJSON_GetObjectItemCaseSensitive(pJsonResponse, "candidate")) != NULL) {
			if (cJSON_IsObject(pJsonRspCandidate)) {
				//NB. sdpMLineIndex is ignored - we're only doing audio

				if ((pJsonRspCandidateCompleted = cJSON_GetObjectItemCaseSensitive(pJsonRspCandidate, "completed")) != NULL) {
				  	if (!cJSON_IsBool(pJsonRspCandidateCompleted) || cJSON_IsFalse(pJsonRspCandidateCompleted)) {
						// assumes that completed is always true value
				    	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (candidate.completed)\n");
						return NULL;
				  	}
					pMessage->pCandidate = "";
				} else if ((pJsonRspCandidateData = cJSON_GetObjectItemCaseSensitive(pJsonRspCandidate, "candidate")) != NULL) {
					if (!cJSON_IsString(pJsonRspCandidateData)) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (candidate.candidate)\n");
						return NULL;
					}
					pMessage->pCandidate = pJsonRspCandidateData->valuestring;
				}
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid type (candidate)\n");
				return NULL;
			}
		}
	}

	pMessage->pJsonJsep = cJSON_GetObjectItemCaseSensitive(pJsonResponse, "jsep");
	if (pMessage->pJsonJsep && !cJSON_IsObject(pMessage->pJsonJsep)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid type (jsep)\n");
		return NULL;
	}

	pJsonRspJanus = cJSON_GetObjectItemCaseSensitive(pJsonResponse, JANUS_STRING);
	if (pJsonRspJanus) {
		if (!cJSON_IsString(pJsonRspJanus)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (janus)\n");
			return NULL;
		} else {
			pMessage->pType = pJsonRspJanus->valuestring;
			MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "janus=%s\n", pMessage->pType);
		}
	}

	pJsonRspTransaction = cJSON_GetObjectItemCaseSensitive(pJsonResponse, "transaction");
	if (pJsonRspTransaction) {
		if (!cJSON_IsString(pJsonRspTransaction)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (transaction)\n");
			return NULL;
		} else {
			pMessage->pTransactionId = pJsonRspTransaction->valuestring;
			MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "transaction=%s\n", pMessage->pTransactionId);
		}
	}

	pJsonRspServerId = cJSON_GetObjectItemCaseSensitive(pJsonResponse, "session_id");
	if (pJsonRspServerId) {
		if (!cJSON_IsNumber(pJsonRspServerId)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (session_id)\n");
			return NULL;
		} else {
			pMessage->serverId = (janus_id_t) pJsonRspServerId->valuedouble;
			MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "serverId=%" SWITCH_UINT64_T_FMT "\n", pMessage->serverId);
		}
	}

	pJsonRspSender = cJSON_GetObjectItemCaseSensitive(pJsonResponse, "sender");
	if (pJsonRspSender) {
		if (!cJSON_IsNumber(pJsonRspSender)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (transaction)\n");
			return NULL;
		} else {
			pMessage->senderId = (janus_id_t) pJsonRspSender->valuedouble;
			MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "sender=%" SWITCH_UINT64_T_FMT "\n", pMessage->senderId);
		}
	}

	return pMessage;
}

/*
 * Send one Janus request through the transport configured on pServer.
 *   - WS: adds session_id/handle_id and (if set) the stored auth-token to the
 *         request body, then calls janus_ws_rpc_json. Note: when pHmacSecret
 *         is in use, encode() has already added the signed `token` field at
 *         the top level, so we only need to add the stored token in non-HMAC
 *         mode.
 *   - HTTP: builds URL per `kind` and calls httpPost(HTTP_POST_TIMEOUT).
 *
 * Caller retains ownership of `pJsonRequest` and must cJSON_Delete() the
 * returned response (or NULL on error).
 */
static cJSON *api_send_request(server_t *pServer, cJSON *pJsonRequest, const char *pTransactionId,
	janus_id_t serverId, janus_id_t senderId, api_url_kind_t kind, const char *label)
{
#if defined(HAVE_MOD_JANUS_WS)
	if (pServer->transport == JANUS_TP_WS) {
		if (serverId) api_ws_add_u64(pJsonRequest, "session_id", (uint64_t) serverId);
		if (senderId) api_ws_add_u64(pJsonRequest, "handle_id", (uint64_t) senderId);
		/* Stored-mode token only; HMAC-signed token already added by encode(). */
		if (!pServer->pHmacSecret && !zstr(pServer->pAuthToken)) {
			cJSON_AddStringToObject(pJsonRequest, "token", pServer->pAuthToken);
		}
		MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Sending WebSocket %s\n", label);
		return janus_ws_rpc_json(pServer, pJsonRequest, pTransactionId,
			(switch_interval_time_t) HTTP_POST_TIMEOUT * 1000);
	}
#else
	(void) pTransactionId;
#endif
	{
		char url[1024];
		int n;
		switch (kind) {
		case URL_ROOT:
			n = snprintf(url, sizeof(url), "%s", pServer->pUrl);
			break;
		case URL_SESSION:
			n = snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT, pServer->pUrl, serverId);
			break;
		case URL_HANDLE:
		default:
			n = snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT "/%" SWITCH_UINT64_T_FMT,
				pServer->pUrl, serverId, senderId);
			break;
		}
		if (n < 0 || (size_t) n >= sizeof(url)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate URL\n");
			return NULL;
		}
		MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Sending HTTP %s - url=%s\n", label, url);
		return httpPost(url, HTTP_POST_TIMEOUT, pJsonRequest);
	}
}

/* Reports each remote participant in an audiobridge "participants" array; ids are normalised to a string (numeric or string_ids rooms). */
static void api_dispatch_participants(message_t *pResponse, cJSON *pParticipants,
	api_participant_func_t pParticipantFunc)
{
	cJSON *pItem = NULL;

	if (!pParticipantFunc || !cJSON_IsArray(pParticipants)) {
		return;
	}

	cJSON_ArrayForEach(pItem, pParticipants) {
		cJSON *pId = cJSON_GetObjectItemCaseSensitive(pItem, "id");
		cJSON *pSetup = cJSON_GetObjectItemCaseSensitive(pItem, "setup");
		char idBuf[64];
		const char *pIdStr = NULL;
		switch_bool_t setup;

		if (cJSON_IsString(pId)) {
			pIdStr = pId->valuestring;
		} else if (cJSON_IsNumber(pId)) {
			(void) switch_snprintf(idBuf, sizeof(idBuf), "%" SWITCH_UINT64_T_FMT, (janus_id_t) pId->valuedouble);
			pIdStr = idBuf;
		} else {
			continue;
		}

		setup = cJSON_IsTrue(pSetup) ? SWITCH_TRUE : SWITCH_FALSE;

		(void) pParticipantFunc(pResponse->serverId, pResponse->senderId, pIdStr, SWITCH_FALSE, setup);
	}
}

switch_status_t api_dispatch_poll_event(cJSON *pEvent,
	switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
	switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
	switch_bool_t (*pAnswerOnWebrtcupFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason),
	api_participant_func_t pParticipantFunc)
{
	message_t *pResponse = NULL;
	cJSON *pJsonRspReason;
	cJSON *pJsonRspType;
	cJSON *pJsonRspRoomId;
	cJSON *pJsonRspParticipantId;
	cJSON *pJsonRspJsepType;
	cJSON *pJsonRspJsepSdp;
	cJSON *pJsonRspError;
	cJSON *pJsonRspErrorCode;

	if (!(pResponse = decode(pEvent))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
		goto end_dispatch;
	}

	if (!strcmp(pResponse->pType, "keepalive")) {
		MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Its a keepalive - do nothing\n");
	} else if (!strcmp(pResponse->pType, "ack")) {
		MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Janus ack (no-op)\n");
	} else if (!strcmp(pResponse->pType, "hangup")) {
		MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Its an hangup\n");

		pJsonRspReason = cJSON_GetObjectItemCaseSensitive(pEvent, "reason");
		if (!cJSON_IsString(pJsonRspReason)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (reason)\n");
			goto end_dispatch;
		}

		if ((*pHungupFunc)(pResponse->serverId, pResponse->senderId, pJsonRspReason->valuestring)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't hangup\n");
		}
	} else if (!strcmp(pResponse->pType, "detached")) {
		if ((*pHungupFunc)(pResponse->serverId, pResponse->senderId, NULL)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't detach\n");
		}
	} else if (!strcmp(pResponse->pType, "webrtcup")) {
		MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "WebRTC has been setup\n");
		if (pAnswerOnWebrtcupFunc && pAnswerOnWebrtcupFunc(pResponse->serverId, pResponse->senderId)) {
			if ((*pAnsweredFunc)(pResponse->serverId, pResponse->senderId)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't answer (on webrtcup)\n");
			}
		}
	} else if (!strcmp(pResponse->pType, "media")) {
		MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Media is flowing\n");
	} else if (!strcmp(pResponse->pType, "trickle")) {
		MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Receieved a candidate\n");

		if ((*pTrickleFunc)(pResponse->serverId, pResponse->senderId, pResponse->pCandidate)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't add candidate\n");
		}
	} else if (!strcmp(pResponse->pType, "event")) {
		pJsonRspType = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "audiobridge");
		if (!cJSON_IsString(pJsonRspType)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No response (plugindata.data.audiobridge)\n");
			goto end_dispatch;
		}

		if (!strcmp("joined", pJsonRspType->valuestring)) {
			janus_id_t roomId;
			janus_id_t participantId;

			pJsonRspRoomId = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "room");
			if (!pJsonRspRoomId) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Missing response (plugindata.data.room)\n");
				goto end_dispatch;
			}
			if (cJSON_IsNumber(pJsonRspRoomId)) {
				roomId = (janus_id_t) pJsonRspRoomId->valuedouble;
			} else if (cJSON_IsString(pJsonRspRoomId)) {
				roomId = 0;
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.room)\n");
				goto end_dispatch;
			}
			MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "roomId=%" SWITCH_UINT64_T_FMT "\n", (janus_id_t) roomId);

			pJsonRspParticipantId = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "id");
			if (pJsonRspParticipantId) {
				if (cJSON_IsNumber(pJsonRspParticipantId)) {
					participantId = (janus_id_t) pJsonRspParticipantId->valuedouble;
					MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "participantId=%" SWITCH_UINT64_T_FMT "\n", (janus_id_t) participantId);
				} else if (cJSON_IsString(pJsonRspParticipantId)) {
					participantId = 0;
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.id)\n");
					goto end_dispatch;
				}
				if ((*pJoinedFunc)(pResponse->serverId, pResponse->senderId, roomId, participantId)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't join\n");
				}

				/* Record our own participant id so we never treat ourselves as the remote peer. */
				if (pParticipantFunc) {
					char selfBuf[64];
					const char *pSelfIdStr = NULL;
					if (cJSON_IsString(pJsonRspParticipantId)) {
						pSelfIdStr = pJsonRspParticipantId->valuestring;
					} else if (cJSON_IsNumber(pJsonRspParticipantId)) {
						(void) switch_snprintf(selfBuf, sizeof(selfBuf), "%" SWITCH_UINT64_T_FMT, (janus_id_t) participantId);
						pSelfIdStr = selfBuf;
					}
					if (pSelfIdStr) {
						(void) pParticipantFunc(pResponse->serverId, pResponse->senderId, pSelfIdStr, SWITCH_TRUE, SWITCH_TRUE);
					}
				}

				api_dispatch_participants(pResponse,
					cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "participants"), pParticipantFunc);
			} else {
				MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Someone else has joined\n");
				api_dispatch_participants(pResponse,
					cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "participants"), pParticipantFunc);
			}
		} else if (!strcmp("event", pJsonRspType->valuestring)) {
			pJsonRspType = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "result");
			if (pJsonRspType) {
				if (!cJSON_IsString(pJsonRspType) || strcmp("ok", pJsonRspType->valuestring)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.result)\n");
					goto end_dispatch;
				}

				pJsonRspJsepType = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonJsep, "type");
				if (!cJSON_IsString(pJsonRspJsepType) || strcmp("answer", pJsonRspJsepType->valuestring)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (jsep.type)\n");
					goto end_dispatch;
				}

				pJsonRspJsepSdp = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonJsep, "sdp");
				if (!cJSON_IsString(pJsonRspJsepSdp)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (jsep.sdp)\n");
					goto end_dispatch;
				}

				if ((*pAcceptedFunc)(pResponse->serverId, pResponse->senderId, pJsonRspJsepSdp->valuestring)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't accept\n");
				}

				if (!pAnswerOnWebrtcupFunc || !pAnswerOnWebrtcupFunc(pResponse->serverId, pResponse->senderId)) {
					if ((*pAnsweredFunc)(pResponse->serverId, pResponse->senderId)) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't answer\n");
					}
				}
			} else if ((pJsonRspType = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "leaving")) != NULL) {
				if (cJSON_IsNumber(pJsonRspType)) {
					MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "leaving=%" SWITCH_UINT64_T_FMT "\n", (janus_id_t) pJsonRspType->valuedouble);
				} else if (cJSON_IsString(pJsonRspType)) {
					MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "leaving=%s (string id)\n", pJsonRspType->valuestring);
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.leaving)\n");
					goto end_dispatch;
				}
			} else if ((pJsonRspErrorCode = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "error_code")) != NULL
						&& (pJsonRspError = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "error")) != NULL) {
				if (!cJSON_IsNumber(pJsonRspErrorCode) || !cJSON_IsString(pJsonRspError)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No error code or reason on join request\n");
					goto end_dispatch;
				}
				MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "error_code=%d\n", pJsonRspErrorCode->valueint);
				MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "error=%s\n", pJsonRspError->valuestring);

				if ((*pHungupFunc)(pResponse->serverId, pResponse->senderId, pJsonRspError->valuestring)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't hangup\n");
				}
			} else if (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "participants"))) {
				/* Participant state change (e.g. a peer reaching setup:true) - how we learn the browser can hear audio. */
				api_dispatch_participants(pResponse,
					cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "participants"), pParticipantFunc);
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Unknown audiobridge event\n");
				goto end_dispatch;
			}
		} else if (!strcmp("left", pJsonRspType->valuestring)) {
			MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Caller has left the room\n");
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Unknown result - audiobridge=%s\n", pJsonRspType->valuestring);
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Unknown event - janus=%s\n", pResponse->pType);
	}

end_dispatch:
	switch_safe_free(pResponse);
	return SWITCH_STATUS_SUCCESS;
}

janus_id_t apiGetServerId(server_t *pServer) {
  	janus_id_t serverId = 0;
	message_t request, *pResponse = NULL;

  	cJSON *pJsonRequest = NULL;
 	cJSON *pJsonResponse = NULL;
  	cJSON *pJsonRspId;
  	char *pTransactionId = generateTransactionId();

	switch_assert(pServer);
	switch_assert(pServer->pUrl);

  	//"{\"janus\":\"create\",\"transaction\":\"5Y1VuEbeNf7U\",\"apisecret\":\"API-SECRET\"}";

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "create";
	request.pTransactionId = pTransactionId;
	request.pSecret = pServer->pSecret;
	request.pHmacSecret = pServer->pHmacSecret;

	if (!(pJsonRequest = encode(request))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		goto done;
	}

	pJsonResponse = api_send_request(pServer, pJsonRequest, pTransactionId, 0, 0, URL_ROOT, "create");

	if (!(pResponse = decode(pJsonResponse))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
		goto done;
	}

	if (!pResponse->pType || strcmp("success", pResponse->pType) ||
			!pResponse->pTransactionId || strcmp(pTransactionId, pResponse->pTransactionId) ||
			!pResponse->pJsonBody) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Value mismatch\n");
    	goto done;
	}

	pJsonRspId = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "id");
	if (!cJSON_IsNumber(pJsonRspId)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (id)\n");
		goto done;
	}
  	serverId = (janus_id_t) pJsonRspId->valuedouble;

  	done:
  	cJSON_Delete(pJsonRequest);
  	cJSON_Delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

  	return serverId;
}

switch_status_t apiClaimServerId(server_t *pServer, janus_id_t serverId) {
	message_t request, *pResponse = NULL;
	switch_status_t result = SWITCH_STATUS_SUCCESS;

    cJSON *pJsonRequest = NULL;
    cJSON *pJsonResponse = NULL;
	cJSON *pJsonRspError;
	cJSON *pJsonRspErrorCode;
	cJSON *pJsonRspErrorReason;
	char *pTransactionId = generateTransactionId();

	switch_assert(pServer);
	switch_assert(pServer->pUrl);

  	//"{\"janus\":\"claim\",\"transaction\":\"5Y1VuEbeNf7U\",\"apisecret\":\"API-SECRET\",\"session_id\":999999}";

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "claim";
	request.pTransactionId = pTransactionId;
	request.pSecret = pServer->pSecret;
	request.pHmacSecret = pServer->pHmacSecret;

	if (!(pJsonRequest = encode(request))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
			result = SWITCH_STATUS_FALSE;
		goto done;
	}

	pJsonResponse = api_send_request(pServer, pJsonRequest, pTransactionId, serverId, 0, URL_SESSION, "claim");

	if (!(pResponse = decode(pJsonResponse))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
			result = SWITCH_STATUS_SOCKERR;
		goto done;
	}

	if (!pResponse->pType	|| !pResponse->pTransactionId || strcmp(pTransactionId, pResponse->pTransactionId)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Value mismatch\n");
		result = SWITCH_STATUS_FALSE;
    	goto done;
	}

	if (!strcmp(pResponse->pType, "success")) {
		MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Successful claim\n");
		result = SWITCH_STATUS_SUCCESS;
		goto done;
	} else if (!strcmp(pResponse->pType, "error")) {
		pJsonRspError = cJSON_GetObjectItemCaseSensitive(pJsonResponse, "error");
		if (cJSON_IsObject(pJsonRspError)) {
			pJsonRspErrorCode = cJSON_GetObjectItemCaseSensitive(pJsonRspError, "code");
			if (!cJSON_IsNumber(pJsonRspErrorCode)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No error code (error.code)\n");
				result = SWITCH_STATUS_FALSE;
				goto done;
			}
			MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "error.code=%d\n", pJsonRspErrorCode->valueint);

			pJsonRspErrorReason = cJSON_GetObjectItemCaseSensitive(pJsonRspError, "reason");
			if (!cJSON_IsString(pJsonRspErrorReason)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No reason (error.reason)\n");
				result = SWITCH_STATUS_FALSE;
				goto done;
			}
			MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "error.reason=%s\n", pJsonRspErrorReason->valuestring);
			result = SWITCH_STATUS_NOT_INITALIZED;
			goto done;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No error block\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Unknown response=%s\n", pResponse->pType);
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

  	done:
		cJSON_Delete(pJsonRequest);
		cJSON_Delete(pJsonResponse);
		switch_safe_free(pResponse);
		switch_safe_free(pTransactionId);

  	return result;
}

janus_id_t apiGetSenderId(server_t *pServer, const janus_id_t serverId, const char *callId) {
	message_t request, *pResponse = NULL;
	janus_id_t senderId = 0;

	cJSON *pJsonRequest = NULL;
	cJSON *pJsonResponse = NULL;
	cJSON *pJsonRspId;
	char *pTransactionId = generateTransactionId();

	switch_assert(pServer);
	switch_assert(pServer->pUrl);

	// {"janus":"attach","plugin":"janus.plugin.audiobridge","opaque_id":"audiobridgetest-QsFKsttqnbOx","transaction":"ScRdYl6r0qoX"}

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "attach";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.opaqueId = callId;
	request.pSecret = pServer->pSecret;
	request.pHmacSecret = pServer->pHmacSecret;
	request.isPlugin = SWITCH_TRUE;

	if (!(pJsonRequest = encode(request))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		goto done;
	}

	pJsonResponse = api_send_request(pServer, pJsonRequest, pTransactionId, serverId, 0, URL_SESSION, "attach");

	if (!(pResponse = decode(pJsonResponse))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
		goto done;
	}

	if (!pResponse->pType || strcmp("success", pResponse->pType) ||
			!pResponse->pTransactionId || strcmp(pTransactionId, pResponse->pTransactionId)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Value mismatch\n");
		goto done;
	}

	pJsonRspId = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "id");
	if (!cJSON_IsNumber(pJsonRspId)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (id)\n");
		goto done;
	}
	senderId = (janus_id_t) pJsonRspId->valuedouble;

	done:
	cJSON_Delete(pJsonRequest);
	cJSON_Delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

	return senderId;
}

janus_id_t apiCreateRoom(server_t *pServer, const janus_id_t serverId,
		const janus_id_t senderId, const janus_id_t roomId, const char *pDescription,
		switch_bool_t record, const char *pRecordingFile, const char *pPin, const char *pRoomIdStr) {
	message_t request, *pResponse = NULL;
	janus_id_t result = 0;

	cJSON *pJsonRequest = NULL;
	cJSON *pJsonResponse = NULL;
	cJSON *pJsonRspResult;
	cJSON *pJsonRspErrorCode;
	cJSON *pJsonRspRoomId;
	char *pTransactionId = generateTransactionId();

	switch_assert(pServer);
	switch_assert(pServer->pUrl);

	//"{\"janus\":\"message\",\"transaction\":\"%s\",\"apisecret\":\"%s\",\"body\":{\"request\":\"create\",\"room\":%s}}",

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "message";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pServer->pSecret;
	request.pHmacSecret = pServer->pHmacSecret;

	request.pJsonBody = cJSON_CreateObject();
	if (request.pJsonBody == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create body\n");
		goto done;
	}

	if (cJSON_AddStringToObject(request.pJsonBody, "request", "create") == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.request)\n");
		goto done;
	}

	if (pRoomIdStr && *pRoomIdStr) {
		if (cJSON_AddStringToObject(request.pJsonBody, "room", pRoomIdStr) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.room)\n");
			goto done;
		}
	} else {
		if (cJSON_AddNumberToObject(request.pJsonBody, "room", roomId) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create number (body.room)\n");
			goto done;
		}
	}

	if (pDescription) {
		if (cJSON_AddStringToObject(request.pJsonBody, "description", pDescription) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.description)\n");
		goto done;
		}
	}

	if (pPin) {
    	if (cJSON_AddStringToObject(request.pJsonBody, "pin", pPin) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.pin)\n");
			goto done;
		}
	}

	if (cJSON_AddBoolToObject(request.pJsonBody, "record", (cJSON_bool) record) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create boolean (body.record)\n");
		goto done;
	}

	if (pRecordingFile) {
		if (cJSON_AddStringToObject(request.pJsonBody, "record_file", pRecordingFile) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.record_file)\n");
			goto done;
		}
	}

	if (!(pJsonRequest = encode(request))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		goto done;
	}

	pJsonResponse = api_send_request(pServer, pJsonRequest, pTransactionId, serverId, senderId, URL_HANDLE, "create room");

	if (!(pResponse = decode(pJsonResponse))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
		goto done;
	}

	if (!pResponse->pType || strcmp("success", pResponse->pType) ||
			!pResponse->pTransactionId || strcmp(pTransactionId, pResponse->pTransactionId) ||
			(pResponse->senderId != senderId) || !pResponse->pJsonBody) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Value mismatch\n");
		goto done;
	}

	pJsonRspResult = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "audiobridge");
	if (!cJSON_IsString(pJsonRspResult)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No response (plugindata.data.audiobridge)\n");
		goto done;
	}

	if (!strcmp("event", pJsonRspResult->valuestring)) {
		pJsonRspErrorCode = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "error_code");
		if (!cJSON_IsNumber(pJsonRspErrorCode)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No error code (error_code)\n");
			goto done;
		}

		if (pJsonRspErrorCode->valueint == 486) {
			MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Room already exists\n");
			result = (pRoomIdStr && *pRoomIdStr) ? 1 : (roomId != 0 ? roomId : 1); /* never 0: caller uses !apiCreateRoom() */
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (error_code) - error=%d\n", pJsonRspErrorCode->valueint);
			goto done;
		}
	} else if (!strcmp("created", pJsonRspResult->valuestring)) {
	  pJsonRspRoomId = cJSON_GetObjectItemCaseSensitive(pResponse->pJsonBody, "room");
	  if (!pJsonRspRoomId) {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Missing response (plugindata.data.room)\n");
	    goto done;
	  }
	  if (cJSON_IsNumber(pJsonRspRoomId)) {
	    result = (janus_id_t) pJsonRspRoomId->valuedouble;
		MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Create room success (integer room)\n");
	  } else if (cJSON_IsString(pJsonRspRoomId)) {
	    result = 1; /* string room: success; caller uses (apiCreateRoom() == 0) so must be non-zero */
	    MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Create room success (string room)\n");
	  } else {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.room)\n");
	    goto done;
	  }
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.audiobridge)\n");
		goto done;
	}

  done:
	cJSON_Delete(pJsonRequest);
	cJSON_Delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

	return result;
}

switch_status_t apiJoin(server_t *pServer, int hmacTokenTtl,
		const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId,
		const char *pDisplay, const char *pPin, const char *pToken, const char *callId, const char *pRoomIdStr) {
	message_t request, *pResponse = NULL;
 	switch_status_t result = SWITCH_STATUS_SUCCESS;

  	cJSON *pJsonRequest = NULL;
  	cJSON *pJsonResponse = NULL;
	char *pTransactionId = generateTransactionId();
	char *pSignedToken = NULL; /* owned here, freed in "done:" */
	char roomDesc[96];

	switch_assert(pServer);
	switch_assert(pServer->pUrl);

  //"{\"janus\":\"message\",\"transaction\":\"%s\",\"apisecret\":\"%s\",\"body\":{\"request\":\"join\",\"room\":%lu,\"display\":\"%s\"}}",

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "message";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pServer->pSecret;
	request.pHmacSecret = pServer->pHmacSecret;
	request.hmacTokenTtl = hmacTokenTtl > 0 ? hmacTokenTtl : API_HMAC_DEFAULT_CALL_TTL;

	/*
	 * When the server is in HMAC-signing mode, generate a token that also
	 * carries "room=<id>" so audiobridge's signed_tokens enforcement
	 * (PR #3635) — which calls auth_signature_contains(..., "room=<id>") —
	 * accepts the join. The signed token is emitted both at the top level
	 * (by encode()) and inside body.token, which is where the plugin reads
	 * it. The inbound `pToken` (stored-mode token from the
	 * `janus-user-token` channel variable) is ignored in signed mode.
	 */
	if (pServer->pHmacSecret) {
		if (pRoomIdStr && *pRoomIdStr) {
			(void) switch_snprintf(roomDesc, sizeof(roomDesc), "room=%s", pRoomIdStr);
		} else {
			(void) switch_snprintf(roomDesc, sizeof(roomDesc),
					"room=%" SWITCH_UINT64_T_FMT, roomId);
		}
		request.pExtraDescriptors[0] = roomDesc;
		request.nExtraDescriptors = 1;
		request.pSignedTokenOut = &pSignedToken;

		if (pToken) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
					"apiJoin: ignoring janus-user-token, HMAC-signed token will be used instead\n");
			pToken = NULL;
		}
	}

	request.pJsonBody = cJSON_CreateObject();
	if (request.pJsonBody == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create body\n");
			result = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (cJSON_AddStringToObject(request.pJsonBody, "request", "join") == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.request)\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (pRoomIdStr && *pRoomIdStr) {
		if (cJSON_AddStringToObject(request.pJsonBody, "room", pRoomIdStr) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.room)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	} else {
		if (cJSON_AddNumberToObject(request.pJsonBody, "room", roomId) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create number (body.room)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}

	if (pPin) {
		if (cJSON_AddStringToObject(request.pJsonBody, "pin", pPin) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.pin)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}

	if (pDisplay) {
		if (cJSON_AddStringToObject(request.pJsonBody, "display", pDisplay) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.display)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}

	if (pToken) {
		if (cJSON_AddStringToObject(request.pJsonBody, "token", pToken) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.token)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}

	if (callId) {
		if (cJSON_AddStringToObject(request.pJsonBody, "opaque_id", callId) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.opaque_id)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}

	if (!(pJsonRequest = encode(request))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	/*
	 * In HMAC-signing mode, mirror the top-level signed token into
	 * body.token so the audiobridge plugin's signed_tokens check (which
	 * reads from the plugin body, not the top-level) accepts the join.
	 */
	if (pSignedToken) {
		if (cJSON_AddStringToObject(request.pJsonBody, "token", pSignedToken) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.token)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}

	pJsonResponse = api_send_request(pServer, pJsonRequest, pTransactionId, serverId, senderId, URL_HANDLE, "join");

	if (!(pResponse = decode(pJsonResponse))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (!pResponse->pType || strcmp("ack", pResponse->pType) ||
			!pResponse->pTransactionId || strcmp(pTransactionId, pResponse->pTransactionId)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Value mismatch\n");
		result = SWITCH_STATUS_FALSE;
    	goto done;
	}

	done:
	cJSON_Delete(pJsonRequest);
	cJSON_Delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);
	switch_safe_free(pSignedToken);

  	return result;
}

switch_status_t apiConfigure(server_t *pServer,
		const janus_id_t serverId, const janus_id_t senderId, const switch_bool_t muted,
		switch_bool_t record, const char *pRecordingFile,
		const char *pType, const char *pSdp, const char *callId) {
	message_t request, *pResponse = NULL;
	switch_status_t result = SWITCH_STATUS_SUCCESS;

  	cJSON *pJsonRequest = NULL;
	cJSON *pJsonResponse = NULL;
	char *pTransactionId = generateTransactionId();

	switch_assert(pServer);
	switch_assert(pServer->pUrl);

	//{"janus":"message","body":{"request":"configure","muted":false},"transaction":"QPDt2vYOQmmd","jsep":{"type":"offer","sdp":"..."}}

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "message";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pServer->pSecret;
	request.pHmacSecret = pServer->pHmacSecret;

	request.pJsonBody = cJSON_CreateObject();

	if (request.pJsonBody == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create body\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (cJSON_AddStringToObject(request.pJsonBody, "request", "configure") == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.request)\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (cJSON_AddBoolToObject(request.pJsonBody, "muted", (cJSON_bool) muted) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create boolean (body.muted)\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (cJSON_AddBoolToObject(request.pJsonBody, "record", (cJSON_bool) record) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create boolean (body.record)\n");
		goto done;
	}

	if (pRecordingFile) {
		if (cJSON_AddStringToObject(request.pJsonBody, "filename", pRecordingFile) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.filename)\n");
			goto done;
		}
	}

	if (callId) {
		if (cJSON_AddStringToObject(request.pJsonBody, "opaque_id", callId) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.opaque_id)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "type=%s sdp=%s\n", pType, pSdp);

	if (pType && pSdp) {
		request.pJsonJsep = cJSON_CreateObject();
		if (request.pJsonJsep == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create jsep\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}

		if (cJSON_AddStringToObject(request.pJsonJsep, "type", pType) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (jsep.type)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}

		if (cJSON_AddFalseToObject(request.pJsonJsep, "trickle") == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (jsep.trickle)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}

		if (cJSON_AddStringToObject(request.pJsonJsep, "sdp", pSdp) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (jsep.sdp)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}

	if (!(pJsonRequest = encode(request))) {
    	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		result = SWITCH_STATUS_FALSE;
    	goto done;
  	}

	pJsonResponse = api_send_request(pServer, pJsonRequest, pTransactionId, serverId, senderId, URL_HANDLE, "configure");

	if (!(pResponse = decode(pJsonResponse))) {
    	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
		result = SWITCH_STATUS_FALSE;
    	goto done;
  	}

	if (!pResponse->pType || strcmp("ack", pResponse->pType) ||
			!pResponse->pTransactionId || strcmp(pTransactionId, pResponse->pTransactionId)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Value mismatch\n");
		result = SWITCH_STATUS_FALSE;
    	goto done;
	}

	done:
	cJSON_Delete(pJsonRequest);
	cJSON_Delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

  	return result;
}

switch_status_t apiLeave(server_t *pServer, const janus_id_t serverId, const janus_id_t senderId, const char *callId) {
	message_t request, *pResponse = NULL;
	switch_status_t result = SWITCH_STATUS_SUCCESS;

  	cJSON *pJsonRequest = NULL;
	cJSON *pJsonResponse = NULL;
	char *pTransactionId = generateTransactionId();

	switch_assert(pServer);
	switch_assert(pServer->pUrl);

	//{"janus":"message","body":{"request":"leave"},"transaction":"QPDt2vYOQmmd"}

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "message";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pServer->pSecret;
	request.pHmacSecret = pServer->pHmacSecret;

	request.pJsonBody = cJSON_CreateObject();
	if (request.pJsonBody == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create body\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (cJSON_AddStringToObject(request.pJsonBody, "request", "leave") == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.request)\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (callId) {
		if (cJSON_AddStringToObject(request.pJsonBody, "opaque_id", callId) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.opaque_id)\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}
	if (!(pJsonRequest = encode(request))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	pJsonResponse = api_send_request(pServer, pJsonRequest, pTransactionId, serverId, senderId, URL_HANDLE, "leave");

	if (!(pResponse = decode(pJsonResponse))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (!pResponse->pType || strcmp("ack", pResponse->pType) ||
			!pResponse->pTransactionId || strcmp(pTransactionId, pResponse->pTransactionId)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Value mismatch\n");
		result = SWITCH_STATUS_FALSE;
    	goto done;
	}

	done:
	cJSON_Delete(pJsonRequest);
  	cJSON_Delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

	return result;
}

switch_status_t apiDetach(server_t *pServer, const janus_id_t serverId, const janus_id_t senderId) {
	message_t request, *pResponse = NULL;
	switch_status_t result = SWITCH_STATUS_SUCCESS;

  	cJSON *pJsonRequest = NULL;
	cJSON *pJsonResponse = NULL;
	char *pTransactionId = generateTransactionId();

	switch_assert(pServer);
	switch_assert(pServer->pUrl);

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "detach";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pServer->pSecret;
	request.pHmacSecret = pServer->pHmacSecret;

	if (!(pJsonRequest = encode(request))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	pJsonResponse = api_send_request(pServer, pJsonRequest, pTransactionId, serverId, senderId, URL_HANDLE, "detach");

	if (!(pResponse = decode(pJsonResponse))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
		goto done;
	}

	if (!pResponse->pType || strcmp("success", pResponse->pType) ||
			!pResponse->pTransactionId || strcmp(pTransactionId, pResponse->pTransactionId)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Value mismatch\n");
		result = SWITCH_STATUS_FALSE;
    	goto done;
	}

	done:
	cJSON_Delete(pJsonRequest);
	cJSON_Delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

  	return result;
}

switch_status_t apiPoll(server_t *pServer, const janus_id_t serverId,
	switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
	switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
	switch_bool_t (*pAnswerOnWebrtcupFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason),
	api_participant_func_t pParticipantFunc)
{
	switch_status_t result = SWITCH_STATUS_SUCCESS;

	cJSON *pJsonResponse = NULL;
	cJSON *pEvent;

	char *pSignedToken = NULL;
	const char *pAuthToken;
	char url[1024];

	switch_assert(pServer);
	switch_assert(pServer->pUrl);

#if defined(HAVE_MOD_JANUS_WS)
	if (pServer->transport == JANUS_TP_WS) {
		return janus_ws_pump_once(pServer, serverId,
			(switch_interval_time_t)HTTP_GET_TIMEOUT * 1000,
			(switch_interval_time_t)(25 * 1000000),
			&pServer->ws_last_poll,
			pJoinedFunc, pAcceptedFunc, pTrickleFunc, pAnswerOnWebrtcupFunc, pAnsweredFunc, pHungupFunc, pParticipantFunc);
	}
#endif

	if (snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT "?maxev=%d", pServer->pUrl, serverId, MAX_POLL_EVENTS) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate URL\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (pServer->pSecret) {
		size_t len = strlen(url);
		if (snprintf(&url[len], sizeof(url) - len, "&apisecret=%s", pServer->pSecret) < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate secret\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}

	/*
	 * In signed-mode, every request (including long-poll GETs) needs a
	 * fresh signed token since old ones can expire mid-session. The
	 * lifecycle TTL is short on purpose — we regenerate per poll cycle, so
	 * leaking a URL to logs only exposes a 5-minute window.
	 */
	pAuthToken = pServer->pAuthToken;
	if (pServer->pHmacSecret) {
		const char *pDescriptors[1] = { JANUS_PLUGIN };
		pSignedToken = authSignToken(pServer->pHmacSecret, API_HMAC_DEFAULT_LIFECYCLE_TTL, pDescriptors, 1);
		if (!pSignedToken) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot sign poll token\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
		pAuthToken = pSignedToken; /* overrides any stored-mode token */
	}

	if (pAuthToken) {
		size_t len = strlen(url);
		if (snprintf(&url[len], sizeof(url) - len, "&token=%s", pAuthToken) < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate token\n");
			result = SWITCH_STATUS_FALSE;
			goto done;
		}
	}

	MOD_JANUS_DBG(SWITCH_CHANNEL_LOG, "Sending HTTP request - url=%s\n", url);
	pJsonResponse = httpGet(url, HTTP_GET_TIMEOUT);

	if (pJsonResponse == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
		result = SWITCH_STATUS_FALSE;
		goto done;
	}

	pEvent = pJsonResponse ? pJsonResponse->child : NULL;
	while (pEvent) {
		cJSON *next = pEvent->next;
		(void) api_dispatch_poll_event(pEvent, pJoinedFunc, pAcceptedFunc, pTrickleFunc, pAnswerOnWebrtcupFunc, pAnsweredFunc, pHungupFunc, pParticipantFunc);
		pEvent = next;
	}

done:
	cJSON_Delete(pJsonResponse);
	switch_safe_free(pSignedToken);

	return result;
}
/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
