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
 * mod_janus.c -- janus Endpoint Module
 *
 * Implements interface between FreeSWITCH and Janus audiobridge
 * (https://github.com/meetecho/janus-gateway)
 */

#include	"switch.h"
#include  "switch_curl.h"

#include	"globals.h"
#include	"http.h"
#include	"servers.h"
#include	"api.h"
#include	"hash.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_janus_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_janus_shutdown);
//SWITCH_MODULE_RUNTIME_FUNCTION(mod_janus_runtime);
SWITCH_MODULE_DEFINITION(mod_janus, mod_janus_load, mod_janus_shutdown, NULL);	//mod_janus_runtime);


switch_endpoint_interface_t *janus_endpoint_interface;

typedef enum {
	TFLAG_IO = (1 << 0),
	TFLAG_INBOUND = (1 << 1),
	TFLAG_OUTBOUND = (1 << 2),
	TFLAG_DTMF = (1 << 3),
	TFLAG_VOICE = (1 << 4),
	TFLAG_HANGUP = (1 << 5),
	TFLAG_LINEAR = (1 << 6),
	TFLAG_CODEC = (1 << 7),
	TFLAG_BREAK = (1 << 8)
} TFLAGS;

struct private_object {
	unsigned int flags;
	switch_codec_t read_codec;
	switch_codec_t write_codec;
	switch_frame_t read_frame;
	unsigned char databuf[SWITCH_RECOMMENDED_BUFFER_SIZE];
	switch_caller_profile_t *caller_profile;
	switch_mutex_t *mutex;
	switch_mutex_t *flag_mutex;
	//switch_thread_cond_t *cond;
	switch_media_handle_t *smh;
	switch_core_media_params_t mparams;

	janus_id_t serverId;
	janus_id_t roomId;
	char *pDisplay;
	janus_id_t senderId;

	char *pSdpBody;
	char *pSdpCandidates;
	char isTrickleComplete;
};
typedef struct private_object private_t;

#define JANUS_SYNTAX "janus [debug|status|listgw]"
#define JANUS_DEBUG_SYNTAX "janus debug [true|false]"
#define	JANUS_GATEWAY_SYNTAX "janus server <name> [enable|disable]"

SWITCH_STANDARD_API(janus_api_commands);


static switch_status_t channel_on_init(switch_core_session_t *session);
static switch_status_t channel_on_hangup(switch_core_session_t *session);
static switch_status_t channel_on_destroy(switch_core_session_t *session);
static switch_status_t channel_on_routing(switch_core_session_t *session);
static switch_status_t channel_on_exchange_media(switch_core_session_t *session);
static switch_status_t channel_on_soft_execute(switch_core_session_t *session);
static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session, switch_memory_pool_t **pool, switch_originate_flag_t flags,
													switch_call_cause_t *cancel_cause);
static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id);
static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id);
static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig);


switch_status_t joined(janus_id_t serverId, janus_id_t senderId, janus_id_t roomId, janus_id_t participantId) {
	switch_core_session_t *session;
	switch_channel_t *channel;
	private_t *tech_pvt;
	server_t *pServer;

	if (!(pServer = (server_t *) hashFind(&globals.serverIdLookup, serverId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No server for serverId=%" SWITCH_UINT64_T_FMT "\n", serverId);
		return SWITCH_STATUS_NOTFOUND;
	}

	if (!(session = (switch_core_session_t *) hashFind(&pServer->senderIdLookup, senderId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No session for senderId=%" SWITCH_UINT64_T_FMT "\n", senderId);
		return SWITCH_STATUS_NOTFOUND;
	}

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	switch_channel_set_variable(channel, "media_webrtc", "true");
	switch_channel_set_flag(channel, CF_AUDIO);

	switch_channel_set_variable(channel, "absolute_codec_string", pServer->codec_string);

	switch_core_media_prepare_codecs(session, SWITCH_TRUE);

	switch_core_session_set_ice(session);

	for (unsigned int i = 0; i < pServer->cand_acl_count; i ++) {
		switch_core_media_add_ice_acl(session, SWITCH_MEDIA_TYPE_AUDIO, pServer->cand_acl[i]);
	}

	if (switch_core_media_choose_ports(session, SWITCH_TRUE, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot choose ports\n");
		switch_channel_hangup(channel, SWITCH_CAUSE_NETWORK_OUT_OF_ORDER);
		return SWITCH_STATUS_FALSE;
	}

	switch_core_media_gen_local_sdp(session, SDP_TYPE_REQUEST, NULL, 0, NULL, 0);

	//switch_channel_set_flag(channel, CF_REQ_MEDIA);
	//switch_channel_set_flag(channel, CF_MEDIA_ACK);
	//switch_channel_set_flag(channel, CF_MEDIA_SET);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Generated SDP=%s\n", tech_pvt->mparams.local_sdp_str);

	if (apiConfigure(pServer->pUrl, pServer->pSecret, tech_pvt->serverId, tech_pvt->senderId,
					switch_channel_var_true(channel, "janus-start-muted"),
					switch_channel_var_true(channel, "janus-user-record"),
					switch_channel_get_variable(channel, "janus-user-record-file"),
					"offer", tech_pvt->mparams.local_sdp_str) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to configure\n");
		switch_channel_hangup(channel, SWITCH_CAUSE_NETWORK_OUT_OF_ORDER);
	}

	return SWITCH_STATUS_SUCCESS;
}

// called when we have received the body of the SDP and all of the candidates
switch_status_t proceed(switch_core_session_t *session) {
	char sdp[4096] = "";

	switch_channel_t *channel;
	private_t *tech_pvt;

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	if (tech_pvt->pSdpBody) {
		(void) strncat(sdp, tech_pvt->pSdpBody, sizeof(sdp) - 1);
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "No SDP received\n");
		switch_channel_hangup(channel, SWITCH_CAUSE_NETWORK_OUT_OF_ORDER);
		return SWITCH_STATUS_FALSE;
	}
	if (tech_pvt->pSdpCandidates) {
		(void) strncat(sdp, tech_pvt->pSdpCandidates, sizeof(sdp) - 1);
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "ACCEPTED sdp=%s\n", sdp);

	if (!switch_core_media_negotiate_sdp(session, sdp, NULL, SDP_TYPE_RESPONSE)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Cannot negotiate SDP\n");
		switch_channel_hangup(channel, SWITCH_CAUSE_NETWORK_OUT_OF_ORDER);
		return SWITCH_STATUS_FALSE;
	}

	if (switch_core_media_activate_rtp(session) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "switch_core_media_activate_rtp success\n");
		switch_channel_hangup(channel, SWITCH_CAUSE_NETWORK_OUT_OF_ORDER);
		return SWITCH_STATUS_FALSE;
	}

	switch_yield(1000000);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "doing pre-answer\n");
	switch_channel_pre_answer(channel);

	switch_set_flag_locked(tech_pvt, TFLAG_VOICE);

	tech_pvt->read_frame.codec = switch_core_session_get_read_codec(session);


	return SWITCH_STATUS_SUCCESS;
}

switch_status_t accepted(janus_id_t serverId, janus_id_t senderId, const char *pSdp) {
	switch_core_session_t *session;
	private_t *tech_pvt;
	server_t *pServer;
	char isTrickling;

	if (!(pServer = (server_t *) hashFind(&globals.serverIdLookup, serverId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No server for serverId=%" SWITCH_UINT64_T_FMT "\n", serverId);
		return SWITCH_STATUS_NOTFOUND;
	}

	if (!(session = (switch_core_session_t *) hashFind(&pServer->senderIdLookup, senderId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No session for senderId=%" SWITCH_UINT64_T_FMT "\n", senderId);
		return SWITCH_STATUS_NOTFOUND;
	}

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	tech_pvt->pSdpBody = switch_core_session_strdup(session, pSdp);

	isTrickling = strstr(pSdp, "a=candidate:") == NULL;
	DEBUG(SWITCH_CHANNEL_LOG, "isTrickling=%d\n", isTrickling);

	if (!isTrickling || tech_pvt->isTrickleComplete) {
		return proceed(session);
	} else {
		return SWITCH_STATUS_SUCCESS;
	}
}

switch_status_t trickle(janus_id_t serverId, janus_id_t senderId, const char *pCandidate) {
	switch_core_session_t *session;
	server_t *pServer;
	private_t *tech_pvt;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "serverId=%" SWITCH_UINT64_T_FMT " candidate=%s\n",
		serverId, pCandidate);

	if (!(pServer = (server_t *) hashFind(&globals.serverIdLookup, serverId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No server for serverId=%" SWITCH_UINT64_T_FMT "\n", serverId);
		return SWITCH_STATUS_NOTFOUND;
	}

	if (!(session = (switch_core_session_t *) hashFind(&pServer->senderIdLookup, senderId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No session for senderId=%" SWITCH_UINT64_T_FMT "\n", senderId);
		return SWITCH_STATUS_NOTFOUND;
	}

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	if (switch_strlen_zero(pCandidate)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "serverId=%" SWITCH_UINT64_T_FMT " got all candidates\n", serverId);
		tech_pvt->isTrickleComplete = SWITCH_TRUE;
		tech_pvt->pSdpCandidates = switch_core_session_sprintf(session, "%sa=end-of-candidates\r\n",
			tech_pvt->pSdpCandidates ? tech_pvt->pSdpCandidates : "");
		if (tech_pvt->pSdpBody) {
			// we've already for the body - proceed
			return proceed(session);
		} else {
			// wait for the SDP body and then continue
			return SWITCH_STATUS_SUCCESS;
		}
	} else {
		tech_pvt->pSdpCandidates = switch_core_session_sprintf(session, "%sa=%s\r\n",
			tech_pvt->pSdpCandidates ? tech_pvt->pSdpCandidates : "", pCandidate);

		DEBUG(SWITCH_CHANNEL_LOG, "candidates=%s\n", tech_pvt->pSdpCandidates);
		return SWITCH_STATUS_SUCCESS;
	}
}

switch_status_t answered(janus_id_t serverId, janus_id_t senderId) {
	// called when we get a webrtcup event from Janus
	switch_core_session_t *session;
	switch_channel_t *channel;
	server_t *pServer;

	if (!(pServer = (server_t *) hashFind(&globals.serverIdLookup, serverId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No server for serverId=%" SWITCH_UINT64_T_FMT "\n", serverId);
		return SWITCH_STATUS_NOTFOUND;
	}

	if (!(session = (switch_core_session_t *) hashFind(&pServer->senderIdLookup, senderId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No session for senderId=%" SWITCH_UINT64_T_FMT "\n", senderId);
		return SWITCH_STATUS_NOTFOUND;
	}

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	switch_channel_answer(channel);

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t hungup(janus_id_t serverId, janus_id_t senderId, const char *pReason) {
	switch_core_session_t *session;
	switch_channel_t *channel;
	server_t *pServer;

	if (!(pServer = (server_t *) hashFind(&globals.serverIdLookup, serverId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No server for serverId=%" SWITCH_UINT64_T_FMT "\n", serverId);
		return SWITCH_STATUS_NOTFOUND;
	}

	if (!(session = (switch_core_session_t *) hashFind(&pServer->senderIdLookup, senderId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No session for senderId=%" SWITCH_UINT64_T_FMT "\n", senderId);
		return SWITCH_STATUS_NOTFOUND;
	}

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Hungup reason=%s\n", pReason);

	return SWITCH_STATUS_SUCCESS;
}

static void *SWITCH_THREAD_FUNC server_thread_run(switch_thread_t *pThread, void *pObj) {
	server_t *pServer = (server_t *) pObj;
	janus_id_t serverId = 0;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Thread started - server=%s\n", pServer->name);

	(void) hashCreate(&pServer->senderIdLookup, globals.pModulePool);

	while (!switch_test_flag(pServer, SFLAG_TERMINATING)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Server=%s invoking\n", pServer->name);

		// wait for a few seconds before re-connection
		switch_yield(5000000);

		if (serverId) {
			// the connection or Janus has restarted - try to re-use the same serverId
			switch_status_t status =  apiClaimServerId(pServer->pUrl, pServer->pSecret, serverId);
			if (status == SWITCH_STATUS_SOCKERR) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Server=%s claim socket error - retry later\n", pServer->name);
			} else if (status == SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Server=%s claim success - serverId=%" SWITCH_UINT64_T_FMT "\n", pServer->name, serverId);
				if (hashInsert(&globals.serverIdLookup, serverId, (void *) pServer) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't insert server into hash table\n");
				}
			} else {
				// terminate all calls in progress on this server
				switch_core_session_t *session;
				private_t *tech_pvt;
				switch_channel_t *channel;
				switch_hash_index_t *pIndex = NULL;

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Server=%s claim failed - status=%d\n", pServer->name, status);

				while ((session = (switch_core_session_t *) hashIterate(&pServer->senderIdLookup, &pIndex)) != NULL) {
					tech_pvt = switch_core_session_get_private(session);
 					switch_assert(tech_pvt);
					channel = switch_core_session_get_channel(session);
					switch_assert(channel);
					//NB. the server is likely to have been removed by the time the hangup has completed
					switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
					(void) hashDelete(&pServer->senderIdLookup, tech_pvt->senderId);
				}
				// reset serverId so we get a new one the next time around
				serverId = 0;
			}
		} else {
			// first time
			serverId = apiGetServerId(pServer->pUrl, pServer->pSecret);
			if (!serverId) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Error getting serverId\n");
			} else if (hashInsert(&globals.serverIdLookup, serverId, (void *) pServer) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't insert server into hash table\n");
			} else {
				switch_mutex_lock(pServer->mutex);
				pServer->started = switch_time_now();
				pServer->serverId = serverId;
				pServer->callsInProgress = 0;
				switch_mutex_unlock(pServer->mutex);
			}
		}

		while (!switch_test_flag(pServer, SFLAG_TERMINATING) && hashFind(&globals.serverIdLookup, serverId)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Poll started\n");

			if (apiPoll(pServer->pUrl, pServer->pSecret, serverId, pServer->pAuthToken, joined, accepted, trickle, answered, hungup) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Poll failed\n");
				if (hashDelete(&globals.serverIdLookup, serverId) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't remove server from hash table\n");
				}
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Poll completed\n");
			}
		}
	}
	(void) hashDestroy(&pServer->senderIdLookup);

	(void) hashDelete(&globals.serverIdLookup, serverId);

	return NULL;
}


static void startServerThread(server_t *pServer, switch_bool_t force) {
	switch_threadattr_t *pThreadAttr = NULL;

	switch_assert(pServer);

	switch_mutex_lock(pServer->flag_mutex);
	if (force || !switch_test_flag(pServer, SFLAG_ENABLED)) {
		switch_set_flag(pServer, SFLAG_ENABLED);
		switch_mutex_unlock(pServer->flag_mutex);

		DEBUG(SWITCH_CHANNEL_LOG, "Starting server=%s\n", pServer->name);

		switch_threadattr_create(&pThreadAttr, globals.pModulePool);
		//switch_threadattr_detach_set(pThreadAttr, 1);
	  switch_threadattr_stacksize_set(pThreadAttr, SWITCH_THREAD_STACKSIZE);
	  switch_thread_create(&pServer->pThread, pThreadAttr, server_thread_run, pServer, globals.pModulePool);
	} else {
		switch_mutex_unlock(pServer->flag_mutex);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Server=%s is already enabled\n", pServer->name);
	}
}

static void stopServerThread(server_t *pServer) {
	switch_status_t status, returnValue;

	switch_assert(pServer);

	switch_mutex_lock(pServer->flag_mutex);
	if (switch_test_flag(pServer, SFLAG_ENABLED) &&
			!switch_test_flag(pServer, SFLAG_TERMINATING) && pServer->pThread) {
		switch_set_flag(pServer, SFLAG_TERMINATING);
		switch_mutex_unlock(pServer->flag_mutex);

		DEBUG(SWITCH_CHANNEL_LOG, "Stopping server=%s\n", pServer->name);

		// wait for the thread to terminate.  If we don't do this the thread is
		// forcefully terminated and valgrind complains about memory being leaked.
		status = switch_thread_join(&returnValue, pServer->pThread);
		pServer->pThread = NULL;

		if ((status != SWITCH_STATUS_SUCCESS) || (returnValue != SWITCH_STATUS_SUCCESS)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Server=%s result from thread=%d %d\n",
				pServer->name, status, returnValue);
		}

		switch_clear_flag_locked(pServer, SFLAG_TERMINATING | SFLAG_ENABLED);
	} else {
		switch_mutex_unlock(pServer->flag_mutex);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Server=%s is already disabled\n", pServer->name);
	}
}

static switch_bool_t isVideoCall(switch_core_session_t *session) {
	switch_channel_t *channel;

	switch_assert(session);

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	return switch_channel_test_flag(channel, CF_VIDEO);
}

/*
   State methods they get called when the state changes to the specific state
   returning SWITCH_STATUS_SUCCESS tells the core to execute the standard state method next
   so if you fully implement the state you can return SWITCH_STATUS_FALSE to skip it.
*/
static switch_status_t channel_on_init(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;
	server_t *pServer = NULL;

	switch_assert(session);

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	if (!switch_test_flag(tech_pvt, TFLAG_OUTBOUND)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Module supports only outgoing calls\n");
		switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
		return SWITCH_STATUS_FALSE;
	}

	if (!(pServer = (server_t *) hashFind(&globals.serverIdLookup, tech_pvt->serverId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No server for serverId=%" SWITCH_UINT64_T_FMT "\n", tech_pvt->serverId);
		switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
		return SWITCH_STATUS_NOTFOUND;
	}

	tech_pvt->senderId = apiGetSenderId(pServer->pUrl, pServer->pSecret, tech_pvt->serverId);
	if (!tech_pvt->senderId) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error getting senderId\n");
		switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
		return SWITCH_STATUS_FALSE;
	}

	if (hashInsert(&pServer->senderIdLookup, tech_pvt->senderId, (void *) session) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to insert senderId=%" SWITCH_UINT64_T_FMT " in hash\n", tech_pvt->senderId);
		switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_var_false(channel, "janus-use-existing-room")) {
		if (!apiCreateRoom(pServer->pUrl, pServer->pSecret, tech_pvt->serverId, tech_pvt->senderId, tech_pvt->roomId,
						switch_channel_get_variable(channel, "janus-room-description"),
						switch_channel_var_true(channel, "janus-room-record"),
						switch_channel_get_variable(channel, "janus-room-record-file"),
						switch_channel_get_variable(channel, "janus-room-pin"),
						switch_channel_var_true(channel, "janus-audio-level-event"))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to create room\n");
			switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
			return SWITCH_STATUS_FALSE;
		}
	}

	switch_set_flag_locked(tech_pvt, TFLAG_IO);

	switch_mutex_lock(pServer->mutex);
	pServer->callsInProgress ++;
	pServer->totalCalls ++;
	switch_mutex_unlock(pServer->mutex);

	switch_mutex_lock(globals.mutex);
	globals.callsInProgress ++;
	globals.totalCalls ++;
	switch_mutex_unlock(globals.mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_routing(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;
	server_t *pServer = NULL;

	switch_assert(session);

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	DEBUG(SWITCH_CHANNEL_SESSION_LOG(session), "%s CHANNEL ROUTING\n", switch_channel_get_name(channel));

	if (!(pServer = (server_t *) hashFind(&globals.serverIdLookup, tech_pvt->serverId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No server for serverId=%" SWITCH_UINT64_T_FMT "\n", tech_pvt->serverId);
		switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
		return SWITCH_STATUS_NOTFOUND;
	}

	if (apiJoin(pServer->pUrl, pServer->pSecret, tech_pvt->serverId,
			tech_pvt->senderId, tech_pvt->roomId, tech_pvt->pDisplay,
			switch_channel_get_variable(channel, "janus-room-pin"),
			switch_channel_get_variable(channel, "janus-user-token")) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to join room\n");
		switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_execute(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	switch_assert(session);

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	DEBUG(SWITCH_CHANNEL_SESSION_LOG(session), "%s CHANNEL EXECUTE\n", switch_channel_get_name(channel));

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_destroy(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	switch_assert(session);

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	DEBUG(SWITCH_CHANNEL_SESSION_LOG(session), "%s CHANNEL DESTROY\n", switch_channel_get_name(channel));

	tech_pvt = switch_core_session_get_private(session);

	if (tech_pvt) {
		if (switch_core_codec_ready(&tech_pvt->read_codec)) {
			switch_core_codec_destroy(&tech_pvt->read_codec);
		}

		if (switch_core_codec_ready(&tech_pvt->write_codec)) {
			switch_core_codec_destroy(&tech_pvt->write_codec);
		}
	}

	switch_core_media_deactivate_rtp(session);

	switch_media_handle_destroy(session);


	return SWITCH_STATUS_SUCCESS;
}


static switch_status_t channel_on_hangup(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;
	server_t *pServer = NULL;

	switch_assert(session);

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	switch_core_media_kill_socket(session, SWITCH_MEDIA_TYPE_AUDIO);

	switch_clear_flag_locked(tech_pvt, TFLAG_IO);
	switch_clear_flag_locked(tech_pvt, TFLAG_VOICE);

	DEBUG(SWITCH_CHANNEL_SESSION_LOG(session), "%s CHANNEL HANGUP\n", switch_channel_get_name(channel));

	if (!(pServer = (server_t *) hashFind(&globals.serverIdLookup, tech_pvt->serverId))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No server for serverId=%" SWITCH_UINT64_T_FMT "\n", tech_pvt->serverId);
		// we can get here if the calls are being terminated because the server has failed
		return SWITCH_STATUS_NOTFOUND;
	}

	if (apiLeave(pServer->pUrl, pServer->pSecret, tech_pvt->serverId, tech_pvt->senderId) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Failed to leave room\n");
		// carry on regardless
	}

	if (apiDetach(pServer->pUrl, pServer->pSecret, tech_pvt->serverId, tech_pvt->senderId) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Failed to detach\n");
		// carry on regardless
	}

	(void) hashDelete(&pServer->senderIdLookup, tech_pvt->senderId);

	switch_mutex_lock(pServer->mutex);
	if (pServer->callsInProgress > 0) {
		pServer->callsInProgress --;
	}
	switch_mutex_unlock(pServer->mutex);

	switch_mutex_lock(globals.mutex);
	if (globals.callsInProgress > 0) {
		globals.callsInProgress --;
	}
	switch_mutex_unlock(globals.mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	switch_assert(session);

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	DEBUG(SWITCH_CHANNEL_SESSION_LOG(session), "%s CHANNEL KILLED sig=%d\n", switch_channel_get_name(channel), sig);

	switch (sig) {
	case SWITCH_SIG_KILL:
		switch_set_flag_locked(tech_pvt, TFLAG_IO);

		switch_core_media_kill_socket(session, SWITCH_MEDIA_TYPE_AUDIO);
		break;
	case SWITCH_SIG_BREAK:
		switch_set_flag_locked(tech_pvt, TFLAG_BREAK);

		switch_core_media_break(session, SWITCH_MEDIA_TYPE_AUDIO);
		break;
	default:
		break;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_exchange_media(switch_core_session_t *session)
{
	DEBUG(SWITCH_CHANNEL_SESSION_LOG(session), "CHANNEL LOOPBACK\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_soft_execute(switch_core_session_t *session)
{
	DEBUG(SWITCH_CHANNEL_SESSION_LOG(session), "CHANNEL TRANSMIT\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_send_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf)
{
	private_t *tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;
	//switch_time_t started = switch_time_now();
	//unsigned int elapsed;
	switch_byte_t *data;

	//DEBUG(SWITCH_CHANNEL_SESSION_LOG(session), "Read frame called\n");

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	tech_pvt->read_frame.flags = SFF_NONE;
	*frame = NULL;

	switch_set_flag(tech_pvt, TFLAG_VOICE);

	while (switch_test_flag(tech_pvt, TFLAG_IO)) {

		if (switch_test_flag(tech_pvt, TFLAG_BREAK)) {
			switch_clear_flag(tech_pvt, TFLAG_BREAK);
			goto cng;
		}

		if (!switch_test_flag(tech_pvt, TFLAG_IO)) {
			return SWITCH_STATUS_FALSE;
		}

		if (switch_test_flag(tech_pvt, TFLAG_IO) && switch_test_flag(tech_pvt, TFLAG_VOICE)) {
			switch_clear_flag_locked(tech_pvt, TFLAG_VOICE);
			if (!tech_pvt->read_frame.datalen) {
				continue;
			}
			*frame = &tech_pvt->read_frame;
#if SWITCH_BYTE_ORDER == __BIG_ENDIAN
			if (switch_test_flag(tech_pvt, TFLAG_LINEAR)) {
				switch_swap_linear((*frame)->data, (int) (*frame)->datalen / 2);
			}
#endif
			return switch_core_media_read_frame(session, frame, flags, stream_id, SWITCH_MEDIA_TYPE_AUDIO);
		}

		switch_cond_next();
	}


	return SWITCH_STATUS_FALSE;

  cng:
	data = (switch_byte_t *) tech_pvt->read_frame.data;
	data[0] = 65;
	data[1] = 0;
	tech_pvt->read_frame.datalen = 2;
	tech_pvt->read_frame.flags = SFF_CNG;
	*frame = &tech_pvt->read_frame;
	return SWITCH_STATUS_SUCCESS;

}

static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id)
{
	//switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;
	//switch_frame_t *pframe;

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	if (!switch_test_flag(tech_pvt, TFLAG_IO)) {
		return SWITCH_STATUS_FALSE;
	}
#if SWITCH_BYTE_ORDER == __BIG_ENDIAN
	if (switch_test_flag(tech_pvt, TFLAG_LINEAR)) {
		switch_swap_linear(frame->data, (int) frame->datalen / 2);
	}
#endif

	return switch_core_media_write_frame(session, frame, flags, stream_id, SWITCH_MEDIA_TYPE_AUDIO);
}

static switch_status_t channel_answer_channel(switch_core_session_t *session)
{
	private_t *tech_pvt;
	switch_channel_t *channel = NULL;

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg)
{
	switch_channel_t *channel;
	private_t *tech_pvt;

	channel = switch_core_session_get_channel(session);
	switch_assert(channel);

	tech_pvt = (private_t *) switch_core_session_get_private(session);
	switch_assert(tech_pvt);

	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_ANSWER:
		{
			channel_answer_channel(session);
		}
		break;
	default:
		break;
	}

	return SWITCH_STATUS_SUCCESS;
}

/* Make sure when you have 2 sessions in the same scope that you pass the appropriate one to the routines
   that allocate memory or you will have 1 channel with memory allocated from another channel's pool!
*/
static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session, switch_memory_pool_t **pool, switch_originate_flag_t flags,
													switch_call_cause_t *cancel_cause)
{
	char channelName[2048] = "janus/";
	char *pServerName = NULL;
	char *pCurr = NULL;
	char *pNext = NULL;
	char *pDialStr = NULL;
	private_t *tech_pvt;
	switch_channel_t *channel;
	switch_caller_profile_t *caller_profile;
	switch_call_cause_t status = SWITCH_CAUSE_SUCCESS;
	server_t *pTmpServer, *pServer;

	if (isVideoCall(session)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Only implemented for audio calls\n");
		return SWITCH_CAUSE_BEARERCAPABILITY_NOTIMPL;
	}

	*new_session = switch_core_session_request(janus_endpoint_interface, SWITCH_CALL_DIRECTION_OUTBOUND, flags, pool);

	if (!*new_session) {
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}

	if (!outbound_profile) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Doh! no caller profile\n");
		status = SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		goto error;
	}

	channel = switch_core_session_get_channel(*new_session);
	switch_assert(channel);

	(void) strncat(channelName, outbound_profile->destination_number, sizeof(channelName) - 7);
	switch_channel_set_name(channel, channelName);

	pDialStr = switch_safe_strdup(outbound_profile->destination_number);

	if (pDialStr == NULL) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot allocate memory for dialstring\n");
		status = SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		goto error;
	}

	pCurr = pDialStr;
	pNext = strchr(pCurr, '/');
	if (pNext == NULL) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid dialstring format=%s\n", pDialStr);
		status = SWITCH_CAUSE_PROTOCOL_ERROR;
		goto error;
	}
	*pNext ++ = '\0';
	pServerName = pDialStr;

	if (!(pTmpServer = serversFind(pServerName))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Unknown server=%s\n", pServerName);
		status = SWITCH_CAUSE_NO_ROUTE_DESTINATION;
		goto error;
	}
	DEBUG(SWITCH_CHANNEL_SESSION_LOG(session), "Found serverId=%" SWITCH_UINT64_T_FMT "\n", pTmpServer->serverId);

	// check that the server is available via the serverIdLookup map - ie. that it is active
	if (!(pServer = (server_t *) hashFind(&globals.serverIdLookup, pTmpServer->serverId))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "No server for serverId=%" SWITCH_UINT64_T_FMT "\n", pTmpServer->serverId);
		status = SWITCH_CAUSE_NO_ROUTE_DESTINATION;
		goto error;
	}

	if (!switch_test_flag(pServer, SFLAG_ENABLED)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Server=%s has been disabled\n", pServer->name);
		return SWITCH_CAUSE_OUTGOING_CALL_BARRED;
	}

	switch_core_session_add_stream(*new_session, NULL);
	if (!(tech_pvt = (private_t *) switch_core_session_alloc(*new_session, sizeof(private_t)))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Hey where is my memory pool?\n");
		status = SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		goto error;
	}

	tech_pvt->read_frame.data = tech_pvt->databuf;
	tech_pvt->read_frame.buflen = sizeof(tech_pvt->databuf);
	switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(*new_session));
	switch_mutex_init(&tech_pvt->flag_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(*new_session));
	switch_core_session_set_private(*new_session, tech_pvt);

	switch_media_handle_create(&tech_pvt->smh, *new_session, &tech_pvt->mparams);

	//tech_pvt->mparams.codec_string = switch_core_session_strdup(*new_session, pServer->codec_string);

	if (!zstr(pServer->rtpip)) {
		tech_pvt->mparams.rtpip4 = switch_core_session_strdup(*new_session, pServer->rtpip);
    tech_pvt->mparams.rtpip = tech_pvt->mparams.rtpip4;
	}

  if (!zstr(pServer->rtpip6)) {
		tech_pvt->mparams.rtpip6 = switch_core_session_strdup(*new_session, pServer->rtpip6);
	}

	tech_pvt->mparams.local_network = pServer->local_network;
	tech_pvt->mparams.extrtpip = pServer->extrtpip;

	tech_pvt->serverId = pServer->serverId;

	pCurr = pNext;
	pNext = strchr(pCurr, '@');
	if (pNext == NULL) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid dialstring format=%s\n", pDialStr);
		status = SWITCH_CAUSE_PROTOCOL_ERROR;
		goto error;
	}
	*pNext ++ = '\0';

	tech_pvt->pDisplay = switch_core_session_strdup(*new_session, pCurr);
	tech_pvt->roomId = strtoull(pNext, NULL, 10);

	caller_profile = switch_caller_profile_clone(*new_session, outbound_profile);
	switch_channel_set_caller_profile(channel, caller_profile);
	tech_pvt->caller_profile = caller_profile;

	switch_channel_ring_ready(channel);

	switch_set_flag_locked(tech_pvt, TFLAG_OUTBOUND);

	switch_channel_set_state(channel, CS_INIT);

	switch_safe_free(pDialStr);
	return status;

	error:
	switch_safe_free(pDialStr);
	switch_core_session_destroy(new_session);
	return status;
}

static switch_status_t channel_receive_event(switch_core_session_t *session, switch_event_t *event)
{
	struct private_object *tech_pvt = switch_core_session_get_private(session);
	char *body = switch_event_get_body(event);
	switch_assert(tech_pvt);

	if (!body) {
		body = "";
	}

	return SWITCH_STATUS_SUCCESS;
}

switch_state_handler_table_t janus_state_handlers = {
	/*.on_init */ channel_on_init,
	/*.on_routing */ channel_on_routing,
	/*.on_execute */ channel_on_execute,
	/*.on_hangup */ channel_on_hangup,
	/*.on_exchange_media */ channel_on_exchange_media,
	/*.on_soft_execute */ channel_on_soft_execute,
	/*.on_consume_media */ NULL,
	/*.on_hibernate */ NULL,
	/*.on_reset */ NULL,
	/*.on_park */ NULL,
	/*.on_reporting */ NULL,
	/*.on_destroy */ channel_on_destroy
};

switch_io_routines_t janus_io_routines = {
	/*.outgoing_channel */ channel_outgoing_channel,
	/*.read_frame */ channel_read_frame,
	/*.write_frame */ channel_write_frame,
	/*.kill_channel */ channel_kill_channel,
	/*.send_dtmf */ channel_send_dtmf,
	/*.receive_message */ channel_receive_message,
	/*.receive_event */ channel_receive_event
};

static switch_status_t load_config(void)
{
	char *cf = "janus.conf";
	switch_xml_t cfg, xml, settings, param, xmlint;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Janus - loading config\n");

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		return SWITCH_STATUS_TERM;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *pVarStr = (char *) switch_xml_attr_soft(param, "name");
			char *pValStr = (char *) switch_xml_attr_soft(param, "value");
			DEBUG(SWITCH_CHANNEL_LOG, "Config  %s->%s\n", pVarStr, pValStr);

			if (!strcmp(pVarStr, "debug")) {
				globals.debug = switch_true(pValStr);
			}
		}
	}

	xmlint = switch_xml_child(cfg, "server");
	if (!xmlint) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No servers defined\n");
		return SWITCH_STATUS_TERM;
	}	else {
		for (; xmlint; xmlint = xmlint->next) {
			(void) serversAdd(xmlint);
		}
	}

	switch_xml_free(xml);
	switch_xml_free(xmlint);

	return SWITCH_STATUS_SUCCESS;
}



SWITCH_MODULE_LOAD_FUNCTION(mod_janus_load)
{
	switch_api_interface_t *api_interface;
	switch_hash_index_t *pIndex = NULL;
	server_t *pServer;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Janus - Loading\n");

	switch_curl_init();

	memset(&globals, 0, sizeof(globals));
	globals.pModulePool = pool;
	globals.auto_nat = (switch_nat_get_type() ? SWITCH_TRUE : SWITCH_FALSE);
	switch_core_hash_init(&globals.pServerNameLookup);

	switch_find_local_ip(globals.guess_ip, sizeof(globals.guess_ip), NULL, AF_INET);

	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, globals.pModulePool);
	// default values
	globals.debug = SWITCH_FALSE;


	load_config();

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	janus_endpoint_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE);
	janus_endpoint_interface->interface_name = "janus";
	janus_endpoint_interface->io_routines = &janus_io_routines;
	janus_endpoint_interface->state_handler = &janus_state_handlers;


	SWITCH_ADD_API(api_interface, "janus", "Janus Menu", janus_api_commands, JANUS_SYNTAX);

	switch_console_set_complete("add janus debug ::[true:false");
	switch_console_set_complete("add janus status");
	switch_console_set_complete("add janus list");
	switch_console_set_complete("add janus server ::janus::listServers enable");
	switch_console_set_complete("add janus server ::janus::listServers disable");
	switch_console_add_complete_func("::janus::listServers", serversList);

	globals.started = switch_time_now();

	if (hashCreate(&globals.serverIdLookup, globals.pModulePool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Cannot create serverId lookup\n");
		return SWITCH_STATUS_FALSE;
	}

	while ((pServer = serversIterate(&pIndex))) {
		if (switch_test_flag(pServer, SFLAG_ENABLED)) {
			startServerThread(pServer, SWITCH_TRUE);
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Janus - Loaded\n");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/*
SWITCH_MODULE_RUNTIME_FUNCTION(mod_janus_runtime)
{
	return SWITCH_STATUS_TERM;
}
*/

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_janus_shutdown)
{
	switch_hash_index_t *pIndex = NULL;
  server_t *pServer;

  while ((pServer = serversIterate(&pIndex)) != NULL) {
		stopServerThread(pServer);
  }

	(void) hashDestroy(&globals.serverIdLookup);

	(void) serversDestroy();

	switch_curl_destroy();

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(janus_api_commands)
{
	char *argv[10] = { 0 };
	int argc = 0;
	char *myarg = NULL;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (session) {
		// do nothing
	} else if (zstr(cmd) || !(myarg = strdup(cmd))) {
		stream->write_function(stream, "USAGE %s\n", JANUS_SYNTAX);
		status = SWITCH_STATUS_FALSE;
	} else if ((argc = switch_separate_string(myarg, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) < 1) {
		stream->write_function(stream, "USAGE %s\n", JANUS_SYNTAX);
		status = SWITCH_STATUS_FALSE;
	} else if (argv[0] && !strncasecmp(argv[0], "status", 6)) {
		stream->write_function(stream, "totalCalls|callsInProgress|started\n");
		stream->write_function(stream, "%u|%u|%lld\n", globals.totalCalls, globals.callsInProgress, globals.started);
	} else if (argv[0] && !strncasecmp(argv[0], "debug", 5)) {
		if ((argc >= 2) && argv[1]) {
			globals.debug = switch_true(argv[1]);
			stream->write_function(stream, "OK\n");
		} else {
			stream->write_function(stream, "USAGE %s\n", JANUS_DEBUG_SYNTAX);
		}
	} else if (argv[0] && !strncasecmp(argv[0], "list", 6)) {
		serversSummary(stream);
	} else if (argv[0] && !strncasecmp(argv[0], "server", 7)) {
		if (argc >= 3 && argv[1] && argv[2]) {
			server_t *pServer = serversFind(argv[1]);
			if (pServer == NULL) {
				stream->write_function(stream, "ERR Unknown server [%s]\n", argv[1]);
			} else if (!strncasecmp(argv[2], "enable", 6)) {
				startServerThread(pServer, FALSE);
				stream->write_function(stream, "OK\n");
			} else if (!strncasecmp(argv[2], "disable", 7)) {
				stopServerThread(pServer);
				stream->write_function(stream, "OK\n");
			} else {
				stream->write_function(stream, "USAGE %s\n", JANUS_GATEWAY_SYNTAX);
			}
		} else {
			stream->write_function(stream, "USAGE %s\n", JANUS_GATEWAY_SYNTAX);
		}
	} else {
		stream->write_function(stream, "USAGE %s\n", JANUS_SYNTAX);
	}

	switch_safe_free(myarg);
	return status;
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
