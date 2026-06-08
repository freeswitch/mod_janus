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
 * api.h -- API header for janus endpoint module
 *
 */
#ifndef	_API_H_
#define	_API_H_

#include  "globals.h"
#include  "servers.h"
#include  "cJSON.h"

/*
 * Default TTLs (seconds) used for internally generated HMAC-signed tokens
 * when the caller does not supply an explicit value. Call-scoped functions
 * (join, configure, leave, detach) accept a per-call TTL so operators can
 * override via the `janus-hmac-token-ttl` channel variable; lifecycle
 * helpers (create/claim session, poll) use a short fixed default since the
 * token only needs to cover one round-trip (or one long-poll cycle).
 */
#define API_HMAC_DEFAULT_CALL_TTL     7200 /* 2 hours */
#define API_HMAC_DEFAULT_LIFECYCLE_TTL 300 /* 5 minutes */

/* Reports each audiobridge participant; isSelf marks the local leg's own id, setup is TRUE once the peer's PeerConnection is up. */
typedef switch_status_t (*api_participant_func_t)(const janus_id_t serverId, const janus_id_t senderId,
	const char *pParticipantIdStr, const switch_bool_t isSelf, const switch_bool_t setup);

/* Dispatch one Janus event object (HTTP long-poll element or WebSocket text frame). */
switch_status_t api_dispatch_poll_event(cJSON *pEvent,
	switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
	switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
	switch_bool_t (*pAnswerOnWebrtcupFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason),
	api_participant_func_t pParticipantFunc);

janus_id_t apiGetServerId(server_t *pServer);
switch_status_t apiClaimServerId(server_t *pServer, janus_id_t serverId);
janus_id_t apiGetSenderId(server_t *pServer, const janus_id_t serverId, const char *callId);
janus_id_t apiCreateRoom(server_t *pServer, const janus_id_t serverId,
	const janus_id_t senderId, const janus_id_t roomId, const char *pDescription,
	switch_bool_t record, const char *pRecordingFile, const char *pPin, const char *pRoomIdStr);
switch_status_t apiJoin(server_t *pServer, int hmacTokenTtl,
	const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId,
	const char *pDisplay, const char *pPin, const char *pToken, const char *callId, const char *pRoomIdStr);
switch_status_t apiConfigure(server_t *pServer,
	const janus_id_t serverId, const janus_id_t senderId, const switch_bool_t muted,
	switch_bool_t record, const char *pRecordingFile,
	const char *pType, const char *pSdp, const char *callId);
switch_status_t apiLeave(server_t *pServer, const janus_id_t serverId, const janus_id_t senderId, const char *callId);
switch_status_t apiDetach(server_t *pServer, const janus_id_t serverId, const janus_id_t senderId);
switch_status_t apiPoll(server_t *pServer, const janus_id_t serverId,
	switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
	switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
	switch_bool_t (*pAnswerOnWebrtcupFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason),
	api_participant_func_t pParticipantFunc);

#endif //_API_H_
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
