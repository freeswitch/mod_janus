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

#include  "libks/ks.h"
#include  "libks/ks_json.h"
#include  "switch.h"
// use switch_stun_random_string() to get a transactionId
#include  "switch_stun.h"
#include  "globals.h"
#include  "http.h"

#define TRANSACTION_ID_LENGTH 16
#define JANUS_STRING  "janus"
#define JANUS_PLUGIN "janus.plugin.audiobridge"
#define	MAX_POLL_EVENTS 10
#define HTTP_GET_TIMEOUT 0
#define HTTP_POST_TIMEOUT 3000

typedef struct {
	const char *pType;
	janus_id_t serverId;
	const char *pTransactionId;
	janus_id_t senderId;
	switch_bool_t isPlugin;
	const char *pSecret;
	ks_json_t *pJsonBody;
	ks_json_t *pJsonJsep;
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
static ks_json_t *encode(const message_t message) {
	ks_json_t *pJsonRequest = ks_json_create_object();

	if (pJsonRequest == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
    goto error;
  }

  if (ks_json_add_string_to_object(pJsonRequest, JANUS_STRING, message.pType) == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (create)\n");
    goto error;
  }

	if (message.pTransactionId) {
	  if (ks_json_add_string_to_object(pJsonRequest, "transaction", message.pTransactionId) == NULL) {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (transaction)\n");
	    goto error;
	  }
	}

	if (message.pSecret) {
	  if (ks_json_add_string_to_object(pJsonRequest, "apisecret", message.pSecret) == NULL) {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (apisecret)\n");
	    goto error;
	  }
	}

	if (message.isPlugin) {
		if (ks_json_add_string_to_object(pJsonRequest, "plugin", JANUS_PLUGIN) == NULL) {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (plugin)\n");
	    goto error;
	  }
	}

	if (message.pJsonBody) {
		ks_json_add_item_to_object(pJsonRequest, "body", message.pJsonBody);
	}

	if (message.pJsonJsep) {
		ks_json_add_item_to_object(pJsonRequest, "jsep", message.pJsonJsep);
	}

	return pJsonRequest;

	error:
	ks_json_delete(pJsonRequest);
	return NULL;
}

// calling process should delete return value
static message_t *decode(ks_json_t *pJsonResponse) {
	message_t *pMessage;
	ks_json_t *pJsonRspPluginData;
	ks_json_t *pJsonRspPlugin;
	ks_json_t *pJsonRspCandidate;
	ks_json_t *pJsonRspCandidateData;
	ks_json_t *pJsonRspCandidateCompleted;
	ks_json_t *pJsonRspJanus;
	ks_json_t *pJsonRspTransaction;
	ks_json_t *pJsonRspServerId;
	ks_json_t *pJsonRspSender;

  if (pJsonResponse == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No response to decode\n");
		return NULL;
  }

	switch_zmalloc(pMessage, sizeof(*pMessage));

	pMessage->pJsonBody = ks_json_get_object_item(pJsonResponse, "data");
	if (pMessage->pJsonBody && !ks_json_type_is_object(pMessage->pJsonBody)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid type (data)\n");
		return NULL;
	} else if (!pMessage->pJsonBody) {
		if ((pJsonRspPluginData = ks_json_get_object_item(pJsonResponse, "plugindata")) != NULL) {
			if (ks_json_type_is_object(pJsonRspPluginData)) {
				pJsonRspPlugin = ks_json_get_object_item(pJsonRspPluginData, "plugin");
			  if (!ks_json_type_is_string(pJsonRspPlugin) || strcmp(JANUS_PLUGIN, pJsonRspPlugin->valuestring)) {
			    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.plugin)\n");
					return NULL;
			  }

			  pMessage->pJsonBody = ks_json_get_object_item(pJsonRspPluginData, "data");
			  if (!ks_json_type_is_object(pMessage->pJsonBody)) {
			    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid type (plugindata.data)\n");
					return NULL;
			  }
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid type (plugindata)\n");
				return NULL;
			}
		} else if ((pJsonRspCandidate = ks_json_get_object_item(pJsonResponse, "candidate")) != NULL) {
			if (ks_json_type_is_object(pJsonRspCandidate)) {
				//NB. sdpMLineIndex is ignored - we're only doing audio

				if ((pJsonRspCandidateCompleted = ks_json_get_object_item(pJsonRspCandidate, "completed")) != NULL) {
				  if (!ks_json_type_is_bool(pJsonRspCandidateCompleted) || ks_json_type_is_false(pJsonRspCandidateCompleted)) {
						// assumes that completed is always true value
				    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (candidate.completed)\n");
						return NULL;
				  }
					pMessage->pCandidate = "";
				} else if ((pJsonRspCandidateData = ks_json_get_object_item(pJsonRspCandidate, "candidate")) != NULL) {
					if (!ks_json_type_is_string(pJsonRspCandidateData)) {
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

	pMessage->pJsonJsep = ks_json_get_object_item(pJsonResponse, "jsep");
	if (pMessage->pJsonJsep && !ks_json_type_is_object(pMessage->pJsonJsep)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid type (jsep)\n");
		return NULL;
	}

	pJsonRspJanus = ks_json_get_object_item(pJsonResponse, JANUS_STRING);
	if (pJsonRspJanus) {
		if (!ks_json_type_is_string(pJsonRspJanus)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (janus)\n");
			return NULL;
		} else {
			pMessage->pType = pJsonRspJanus->valuestring;
			DEBUG(SWITCH_CHANNEL_LOG, "janus=%s\n", pMessage->pType);
		}
	}

	pJsonRspTransaction = ks_json_get_object_item(pJsonResponse, "transaction");
	if (pJsonRspTransaction) {
		if (!ks_json_type_is_string(pJsonRspTransaction)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (transaction)\n");
			return NULL;
		} else {
			pMessage->pTransactionId = pJsonRspTransaction->valuestring;
			DEBUG(SWITCH_CHANNEL_LOG, "transaction=%s\n", pMessage->pTransactionId);
		}
	}

	pJsonRspServerId = ks_json_get_object_item(pJsonResponse, "session_id");
	if (pJsonRspServerId) {
		if (!ks_json_type_is_number(pJsonRspServerId)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (session_id)\n");
			return NULL;
		} else {
			pMessage->serverId = (janus_id_t) pJsonRspServerId->valuedouble;
			DEBUG(SWITCH_CHANNEL_LOG, "serverId=%" SWITCH_UINT64_T_FMT "\n", pMessage->serverId);
		}
	}

	pJsonRspSender = ks_json_get_object_item(pJsonResponse, "sender");
	if (pJsonRspSender) {
		if (!ks_json_type_is_number(pJsonRspSender)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (transaction)\n");
			return NULL;
		} else {
			pMessage->senderId = (janus_id_t) pJsonRspSender->valuedouble;
			DEBUG(SWITCH_CHANNEL_LOG, "sender=%" SWITCH_UINT64_T_FMT "\n", pMessage->senderId);
		}
	}

	return pMessage;
}


janus_id_t apiGetServerId(const char *pUrl, const char *pSecret) {
  janus_id_t serverId = 0;
	message_t request, *pResponse = NULL;

  ks_json_t *pJsonRequest = NULL;
  ks_json_t *pJsonResponse = NULL;
	ks_json_t *pJsonRspId;
  char *pTransactionId = generateTransactionId();

	switch_assert(pUrl);

  //"{\"janus\":\"create\",\"transaction\":\"5Y1VuEbeNf7U\",\"apisecret\":\"API-SECRET\"}";

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "create";
	request.pTransactionId = pTransactionId;
	request.pSecret = pSecret;

  if (!(pJsonRequest = encode(request))) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
    goto done;
  }

  DEBUG(SWITCH_CHANNEL_LOG, "Sending HTTP request\n");
  pJsonResponse = httpPost(pUrl, HTTP_POST_TIMEOUT, pJsonRequest);

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

  pJsonRspId = ks_json_get_object_item(pResponse->pJsonBody, "id");
  if (!ks_json_type_is_number(pJsonRspId)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (id)\n");
    goto done;
  }
  serverId = (janus_id_t) pJsonRspId->valuedouble;

  done:
  ks_json_delete(pJsonRequest);
  ks_json_delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

  return serverId;
}

switch_status_t apiClaimServerId(const char *pUrl, const char *pSecret, janus_id_t serverId) {
	message_t request, *pResponse = NULL;
	switch_status_t result = SWITCH_STATUS_SUCCESS;

  ks_json_t *pJsonRequest = NULL;
  ks_json_t *pJsonResponse = NULL;
	ks_json_t *pJsonRspError;
	ks_json_t *pJsonRspErrorCode;
	ks_json_t *pJsonRspErrorReason;
	char *pTransactionId = generateTransactionId();
	char url[1024];

	switch_assert(pUrl);

  //"{\"janus\":\"claim\",\"transaction\":\"5Y1VuEbeNf7U\",\"apisecret\":\"API-SECRET\",\"session_id\":999999}";

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "claim";
	request.pTransactionId = pTransactionId;
	request.pSecret = pSecret;

  if (!(pJsonRequest = encode(request))) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	if (snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT, pUrl, serverId) < 0) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate URL\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	DEBUG(SWITCH_CHANNEL_LOG, "Sending HTTP request\n");
  pJsonResponse = httpPost(url, HTTP_POST_TIMEOUT, pJsonRequest);

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
		DEBUG(SWITCH_CHANNEL_LOG, "Successful claim\n");
		result = SWITCH_STATUS_SUCCESS;
		goto done;
	} else 	if (!strcmp(pResponse->pType, "error")) {
		pJsonRspError = ks_json_get_object_item(pJsonResponse, "error");
		if (ks_json_type_is_object(pJsonRspError)) {
			pJsonRspErrorCode = ks_json_get_object_item(pJsonRspError, "code");
			if (!ks_json_type_is_number(pJsonRspErrorCode)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No error code (error.code)\n");
				result = SWITCH_STATUS_FALSE;
				goto done;
			}
			DEBUG(SWITCH_CHANNEL_LOG, "error.code=%d\n", pJsonRspErrorCode->valueint);

			pJsonRspErrorReason = ks_json_get_object_item(pJsonRspError, "reason");
			if (!ks_json_type_is_string(pJsonRspErrorReason)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No reason (error.reason)\n");
				result = SWITCH_STATUS_FALSE;
				goto done;
			}
			DEBUG(SWITCH_CHANNEL_LOG, "error.reason=%s\n", pJsonRspErrorReason->valuestring);
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
  ks_json_delete(pJsonRequest);
  ks_json_delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

  return result;
}

janus_id_t apiGetSenderId(const char *pUrl, const char *pSecret, const janus_id_t serverId) {
	message_t request, *pResponse = NULL;
  janus_id_t senderId = 0;

  ks_json_t *pJsonRequest = NULL;
  ks_json_t *pJsonResponse = NULL;
	ks_json_t *pJsonRspId;
	char *pTransactionId = generateTransactionId();
  char url[1024];

	switch_assert(pUrl);

  // {"janus":"attach","plugin":"janus.plugin.audiobridge","opaque_id":"audiobridgetest-QsFKsttqnbOx","transaction":"ScRdYl6r0qoX"}

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "attach";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pSecret;
	request.isPlugin = SWITCH_TRUE;

	if (!(pJsonRequest = encode(request))) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
    goto done;
  }

  if (snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT, pUrl, serverId) < 0) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate URL\n");
    goto done;
  }

	DEBUG(SWITCH_CHANNEL_LOG, "Sending HTTP request\n");
  //	http_get("https://curl.haxx.se/libcurl/c/CURLOPT_HEADERFUNCTION.html", session);
  pJsonResponse = httpPost(url, HTTP_POST_TIMEOUT, pJsonRequest);

	if (!(pResponse = decode(pJsonResponse))) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
    goto done;
  }

	if (!pResponse->pType || strcmp("success", pResponse->pType) ||
			!pResponse->pTransactionId || strcmp(pTransactionId, pResponse->pTransactionId)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Value mismatch\n");
    goto done;
	}

  pJsonRspId = ks_json_get_object_item(pResponse->pJsonBody, "id");
  if (!ks_json_type_is_number(pJsonRspId)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (id)\n");
    goto done;
  }
  senderId = (janus_id_t) pJsonRspId->valuedouble;

  done:
  ks_json_delete(pJsonRequest);
  ks_json_delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

  return senderId;
}

janus_id_t apiCreateRoom(const char *pUrl, const char *pSecret, const janus_id_t serverId,
		const janus_id_t senderId, const janus_id_t roomId, const char *pDescription,
		switch_bool_t record, const char *pRecordingFile, const char *pPin) {
	message_t request, *pResponse = NULL;
  janus_id_t result = 0;

  ks_json_t *pJsonRequest = NULL;
  ks_json_t *pJsonResponse = NULL;
	ks_json_t *pJsonRspResult;
	ks_json_t *pJsonRspErrorCode;
	ks_json_t *pJsonRspRoomId;
	char *pTransactionId = generateTransactionId();
  char url[1024];

	switch_assert(pUrl);

  //"{\"janus\":\"message\",\"transaction\":\"%s\",\"apisecret\":\"%s\",\"body\":{\"request\":\"join\",\"room\":%lu,\"display\":\"%s\"}}",

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "message";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pSecret;

	request.pJsonBody = ks_json_create_object();
  if (request.pJsonBody == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create body\n");
    goto done;
  }

  if (ks_json_add_string_to_object(request.pJsonBody, "request", "create") == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.request)\n");
    goto done;
  }

  if (ks_json_add_number_to_object(request.pJsonBody, "room", roomId) == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.room)\n");
    goto done;
  }

  if (pDescription) {
    if (ks_json_add_string_to_object(request.pJsonBody, "description", pDescription) == NULL) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.description)\n");
      goto done;
    }
  }

	if (pPin) {
    if (ks_json_add_string_to_object(request.pJsonBody, "pin", pPin) == NULL) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.pin)\n");
      goto done;
    }
  }

  if (ks_json_add_bool_to_object(request.pJsonBody, "record", (ks_bool_t) record) == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create boolean (body.record)\n");
    goto done;
  }

	if (pRecordingFile) {
		if (ks_json_add_string_to_object(request.pJsonBody, "record_file", pRecordingFile) == NULL) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.record_file)\n");
      goto done;
    }
  }

	if (!(pJsonRequest = encode(request))) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
    goto done;
  }

  if (snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT "/%" SWITCH_UINT64_T_FMT, pUrl, serverId, senderId) < 0) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate URL\n");
    goto done;
  }

	DEBUG(SWITCH_CHANNEL_LOG, "Sending HTTP request\n");
  //	http_get("https://curl.haxx.se/libcurl/c/CURLOPT_HEADERFUNCTION.html", session);
  pJsonResponse = httpPost(url, HTTP_POST_TIMEOUT, pJsonRequest);

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

  pJsonRspResult = ks_json_get_object_item(pResponse->pJsonBody, "audiobridge");
  if (!ks_json_type_is_string(pJsonRspResult)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No response (plugindata.data.audiobridge)\n");
    goto done;
  }

	if (!strcmp("event", pJsonRspResult->valuestring)) {
		pJsonRspErrorCode = ks_json_get_object_item(pResponse->pJsonBody, "error_code");
		if (!ks_json_type_is_number(pJsonRspErrorCode)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No error code (error_code)\n");
			goto done;
		}

		if (pJsonRspErrorCode->valueint == 486) {
			// its not a proper error if the room already exists
			DEBUG(SWITCH_CHANNEL_LOG, "Room already exists\n");
			result = roomId;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (error_code) - error=%d\n", pJsonRspErrorCode->valueint);
			goto done;
		}
	} else if (!strcmp("created", pJsonRspResult->valuestring)) {
	  pJsonRspRoomId = ks_json_get_object_item(pResponse->pJsonBody, "room");
	  if (!ks_json_type_is_number(pJsonRspRoomId) && (roomId != (janus_id_t) pJsonRspRoomId->valuedouble)) {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.room)\n");
	    goto done;
	  }
	  result = roomId;
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.audiobridge)\n");
		goto done;
	}

  done:
  ks_json_delete(pJsonRequest);
  ks_json_delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

  return result;
}

switch_status_t apiJoin(const char *pUrl, const char *pSecret,
		const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId,
		const char *pDisplay, const char *pPin, const char *pToken) {
	message_t request, *pResponse = NULL;
  switch_status_t result = SWITCH_STATUS_SUCCESS;

  ks_json_t *pJsonRequest = NULL;
  ks_json_t *pJsonResponse = NULL;
	char *pTransactionId = generateTransactionId();
  char url[1024];

	switch_assert(pUrl);

  //"{\"janus\":\"message\",\"transaction\":\"%s\",\"apisecret\":\"%s\",\"body\":{\"request\":\"join\",\"room\":%lu,\"display\":\"%s\"}}",

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "message";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pSecret;

	request.pJsonBody = ks_json_create_object();
  if (request.pJsonBody == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create body\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

  if (ks_json_add_string_to_object(request.pJsonBody, "request", "join") == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.request)\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

  if (ks_json_add_number_to_object(request.pJsonBody, "room", roomId) == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.room)\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	if (pPin) {
    if (ks_json_add_string_to_object(request.pJsonBody, "pin", pPin) == NULL) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.pin)\n");
			result = SWITCH_STATUS_FALSE;
      goto done;
    }
  }

  if (pDisplay) {
    if (ks_json_add_string_to_object(request.pJsonBody, "display", pDisplay) == NULL) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.display)\n");
			result = SWITCH_STATUS_FALSE;
      goto done;
    }
  }

	if (pToken) {
    if (ks_json_add_string_to_object(request.pJsonBody, "token", pToken) == NULL) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.token)\n");
			result = SWITCH_STATUS_FALSE;
      goto done;
    }
  }

	if (!(pJsonRequest = encode(request))) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

  if (snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT "/%" SWITCH_UINT64_T_FMT, pUrl, serverId, senderId) < 0) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate URL\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	DEBUG(SWITCH_CHANNEL_LOG, "Sending HTTP request\n");
  pJsonResponse = httpPost(url, HTTP_POST_TIMEOUT, pJsonRequest);

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
  ks_json_delete(pJsonRequest);
  ks_json_delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

  return result;
}

switch_status_t apiConfigure(const char *pUrl, const char *pSecret,
		const janus_id_t serverId, const janus_id_t senderId, const switch_bool_t muted,
		switch_bool_t record, const char *pRecordingFile,
		const char *pType, const char *pSdp) {
	message_t request, *pResponse = NULL;
	switch_status_t result = SWITCH_STATUS_SUCCESS;

  ks_json_t *pJsonRequest = NULL;
	ks_json_t *pJsonResponse = NULL;
	char *pTransactionId = generateTransactionId();
  char url[1024];

	switch_assert(pUrl);

	//{"janus":"message","body":{"request":"configure","muted":false},"transaction":"QPDt2vYOQmmd","jsep":{"type":"offer","sdp":"..."}}

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "message";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pSecret;

	request.pJsonBody = ks_json_create_object();
  if (request.pJsonBody == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create body\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

  if (ks_json_add_string_to_object(request.pJsonBody, "request", "configure") == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.request)\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

  if (ks_json_add_bool_to_object(request.pJsonBody, "muted", (ks_bool_t) muted) == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create boolean (body.muted)\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	if (ks_json_add_bool_to_object(request.pJsonBody, "record", (ks_bool_t) record) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create boolean (body.record)\n");
		goto done;
	}

	if (pRecordingFile) {
		if (ks_json_add_string_to_object(request.pJsonBody, "filename", pRecordingFile) == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.filename)\n");
			goto done;
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "type=%s sdp=%s\n", pType, pSdp);
	if (pType && pSdp) {
		request.pJsonJsep = ks_json_create_object();
		if (request.pJsonJsep == NULL) {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create jsep\n");
			result = SWITCH_STATUS_FALSE;
	    goto done;
	  }

		if (ks_json_add_string_to_object(request.pJsonJsep, "type", pType) == NULL) {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (jsep.type)\n");
			result = SWITCH_STATUS_FALSE;
	    goto done;
	  }

		if (ks_json_add_false_to_object(request.pJsonJsep, "trickle") == NULL) {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (jsep.trickle)\n");
			result = SWITCH_STATUS_FALSE;
	    goto done;
	  }

		if (ks_json_add_string_to_object(request.pJsonJsep, "sdp", pSdp) == NULL) {
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

  if (snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT "/%" SWITCH_UINT64_T_FMT, pUrl, serverId, senderId) < 0) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate URL\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	DEBUG(SWITCH_CHANNEL_LOG, "Sending HTTP request\n");
  pJsonResponse = httpPost(url, HTTP_POST_TIMEOUT, pJsonRequest);

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
  ks_json_delete(pJsonRequest);
  ks_json_delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

  return result;
}

switch_status_t apiLeave(const char *pUrl, const char *pSecret, const janus_id_t serverId, const janus_id_t senderId) {
	message_t request, *pResponse = NULL;
	switch_status_t result = SWITCH_STATUS_SUCCESS;

  ks_json_t *pJsonRequest = NULL;
	ks_json_t *pJsonResponse = NULL;
	char *pTransactionId = generateTransactionId();
  char url[1024];

	switch_assert(pUrl);

	//{"janus":"message","body":{"request":"leave"},"transaction":"QPDt2vYOQmmd"}

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "message";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pSecret;

	request.pJsonBody = ks_json_create_object();
  if (request.pJsonBody == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create body\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	if (ks_json_add_string_to_object(request.pJsonBody, "request", "leave") == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create string (body.request)\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	if (!(pJsonRequest = encode(request))) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

  if (snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT "/%" SWITCH_UINT64_T_FMT, pUrl, serverId, senderId) < 0) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate URL\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	DEBUG(SWITCH_CHANNEL_LOG, "Sending HTTP request\n");
  pJsonResponse = httpPost(url, HTTP_POST_TIMEOUT, pJsonRequest);

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
	ks_json_delete(pJsonRequest);
  ks_json_delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

	return result;
}

switch_status_t apiDetach(const char *pUrl, const char *pSecret, const janus_id_t serverId, const janus_id_t senderId) {
	message_t request, *pResponse = NULL;
	switch_status_t result = SWITCH_STATUS_SUCCESS;

  ks_json_t *pJsonRequest = NULL;
	ks_json_t *pJsonResponse = NULL;
	char *pTransactionId = generateTransactionId();
  char url[1024];

	switch_assert(pUrl);

	(void) memset((void *) &request, 0, sizeof(request));
	request.pType = "detach";
	request.serverId = serverId;
	request.pTransactionId = pTransactionId;
	request.pSecret = pSecret;

	if (!(pJsonRequest = encode(request))) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot create request\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

  if (snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT "/%" SWITCH_UINT64_T_FMT, pUrl, serverId, senderId) < 0) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate URL\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	DEBUG(SWITCH_CHANNEL_LOG, "Sending HTTP request\n");
  pJsonResponse = httpPost(url, HTTP_POST_TIMEOUT, pJsonRequest);

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
  ks_json_delete(pJsonRequest);
  ks_json_delete(pJsonResponse);
	switch_safe_free(pResponse);
	switch_safe_free(pTransactionId);

  return result;
}

switch_status_t apiPoll(const char *pUrl, const char *pSecret, const janus_id_t serverId, const char *pAuthToken,
  switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
  switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
  switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
  switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason)) {
	switch_status_t result = SWITCH_STATUS_SUCCESS;

  ks_json_t *pJsonResponse = NULL;
	ks_json_t *pEvent;
	ks_json_t *pJsonRspReason;
	ks_json_t *pJsonRspType;
	ks_json_t *pJsonRspRoomId;
	ks_json_t *pJsonRspParticipantId;
	ks_json_t *pJsonRspJsepType;
	ks_json_t *pJsonRspJsepSdp;

  char url[1024];

	switch_assert(pUrl);

	if (snprintf(url, sizeof(url), "%s/%" SWITCH_UINT64_T_FMT "?maxev=%d", pUrl, serverId, MAX_POLL_EVENTS) < 0) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate URL\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	if (pSecret) {
		size_t len = strlen(url);
		if (snprintf(&url[len], sizeof(url) - len, "&apisecret=%s", pSecret) < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate secret\n");
			result = SWITCH_STATUS_FALSE;
	    goto done;
		}
	}

	if (pAuthToken) {
		size_t len = strlen(url);
		if (snprintf(&url[len], sizeof(url) - len, "&token=%s", pAuthToken) < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not generate token\n");
			result = SWITCH_STATUS_FALSE;
	    goto done;
		}
	}

  DEBUG(SWITCH_CHANNEL_LOG, "Sending HTTP request - url=%s\n", url);
  //	http_get("https://curl.haxx.se/libcurl/c/CURLOPT_HEADERFUNCTION.html", session);
  pJsonResponse = httpGet(url, HTTP_GET_TIMEOUT);

  if (pJsonResponse == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
		result = SWITCH_STATUS_FALSE;
    goto done;
  }

	pEvent = pJsonResponse ? pJsonResponse->child : NULL;
  while (pEvent) {
		message_t *pResponse = NULL;

		if (!(pResponse = decode(pEvent))) {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response\n");
	    goto endloop;
	  }

		if (!strcmp(pResponse->pType, "keepalive")) {
			DEBUG(SWITCH_CHANNEL_LOG, "Its a keepalive - do nothing\n");
		} else if (!strcmp(pResponse->pType, "hangup")) {
			DEBUG(SWITCH_CHANNEL_LOG, "Its an hangup\n");

			pJsonRspReason = ks_json_get_object_item(pEvent, "reason");
			if (!ks_json_type_is_string(pJsonRspReason)) {
			 switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (reason)\n");
			 goto endloop;
			}

			if ((*pHungupFunc)(pResponse->serverId, pResponse->senderId, pJsonRspReason->valuestring)) {
			 switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't hangup\n");
			}
		} else if (!strcmp(pResponse->pType, "detached")) {
			// treat detached the same as hangup in case we missed the first message -
			// this might mean we call the hungup function when the call has been terminated
			if ((*pHungupFunc)(pResponse->serverId, pResponse->senderId, NULL)) {
			 switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't detach\n");
			}
		} else if (!strcmp(pResponse->pType, "webrtcup")) {
			DEBUG(SWITCH_CHANNEL_LOG, "WebRTC has been setup\n");

			if ((*pAnsweredFunc)(pResponse->serverId, pResponse->senderId)) {
			 switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't answer\n");
			}
		} else if (!strcmp(pResponse->pType, "media")) {
			DEBUG(SWITCH_CHANNEL_LOG, "Media is flowing\n");
		} else if (!strcmp(pResponse->pType, "trickle")) {
			DEBUG(SWITCH_CHANNEL_LOG, "Receieved a candidate\n");

			if ((*pTrickleFunc)(pResponse->serverId, pResponse->senderId, pResponse->pCandidate)) {
			 switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't add candidate\n");
			}
		} else if (!strcmp(pResponse->pType, "event")) {
			  pJsonRspType = ks_json_get_object_item(pResponse->pJsonBody, "audiobridge");
			  if (!ks_json_type_is_string(pJsonRspType)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No response (plugindata.data.audiobridge)\n");
					goto endloop;
			  }

				if (!strcmp("joined", pJsonRspType->valuestring)) {
					janus_id_t roomId;
					janus_id_t participantId;

					pJsonRspRoomId = ks_json_get_object_item(pResponse->pJsonBody, "room");
					if (!ks_json_type_is_number(pJsonRspRoomId)) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.room)\n");
						goto endloop;
					}
					roomId = (janus_id_t) pJsonRspRoomId->valuedouble;
					DEBUG(SWITCH_CHANNEL_LOG, "roomId=%" SWITCH_UINT64_T_FMT "\n", (janus_id_t) roomId);

					pJsonRspParticipantId = ks_json_get_object_item(pResponse->pJsonBody, "id");
					if (pJsonRspParticipantId) {
						if (!ks_json_type_is_number(pJsonRspParticipantId)) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.id)\n");
							goto endloop;
						}
						participantId = (janus_id_t) pJsonRspParticipantId->valuedouble;
						DEBUG(SWITCH_CHANNEL_LOG, "participantId=%" SWITCH_UINT64_T_FMT "\n", (janus_id_t) participantId);


						// call the relevant handler in mod_janus

						if ((*pJoinedFunc)(pResponse->serverId, pResponse->senderId, roomId, participantId)) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't join\n");
						}
					} else {
						DEBUG(SWITCH_CHANNEL_LOG, "Someone else has joined\n");
					}
				} else if (!strcmp("event", pJsonRspType->valuestring)) {
					pJsonRspType = ks_json_get_object_item(pResponse->pJsonBody, "result");
					if (pJsonRspType) {
						if (!ks_json_type_is_string(pJsonRspType) || strcmp("ok", pJsonRspType->valuestring)) {
					    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.result)\n");
							goto endloop;
					  }

						pJsonRspJsepType = ks_json_get_object_item(pResponse->pJsonJsep, "type");
						if (!ks_json_type_is_string(pJsonRspJsepType) || strcmp("answer", pJsonRspJsepType->valuestring)) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (jsep.type)\n");
							goto endloop;
						}

						pJsonRspJsepSdp = ks_json_get_object_item(pResponse->pJsonJsep, "sdp");
						if (!ks_json_type_is_string(pJsonRspJsepSdp)) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (jsep.sdp)\n");
							goto endloop;
						}

						if ((*pAcceptedFunc)(pResponse->serverId, pResponse->senderId, pJsonRspJsepSdp->valuestring)) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't accept\n");
						}
					} else if ((pJsonRspType = ks_json_get_object_item(pResponse->pJsonBody, "leaving")) != NULL) {
						if (!ks_json_type_is_number(pJsonRspType)) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid response (plugindata.data.leaving)\n");
							goto endloop;
						}
						DEBUG(SWITCH_CHANNEL_LOG, "leaving=%" SWITCH_UINT64_T_FMT "\n", (janus_id_t) pJsonRspType->valuedouble);
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Unknown audiobridge event\n");
						goto endloop;
					}
				} else if (!strcmp("left", pJsonRspType->valuestring)) {
					DEBUG(SWITCH_CHANNEL_LOG, "Caller has left the room\n");
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Unknown result - audiobridge=%s\n", pJsonRspType->valuestring);
				}
		 } else {
			 switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Unknown event - janus=%s\n", pResponse->pType);
		 }

		 endloop:
		 switch_safe_free(pResponse);
     pEvent = pEvent->next;
  }

	done:
	ks_json_delete(pJsonResponse);

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
