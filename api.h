/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * api.h -- API header for janus endpoint module
 */
#ifndef _API_H_
#define _API_H_

#include "globals.h"
#include "servers.h"
#include "cJSON.h"

/* Dispatch one Janus event object (HTTP long-poll element or WebSocket text frame). */
switch_status_t api_dispatch_poll_event(cJSON *pEvent,
	switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
	switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
	switch_bool_t (*pAnswerOnWebrtcupFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason));

janus_id_t apiGetServerId(server_t *pServer);
switch_status_t apiClaimServerId(server_t *pServer, janus_id_t serverId);
janus_id_t apiGetSenderId(server_t *pServer, const janus_id_t serverId, const char *callId);
janus_id_t apiCreateRoom(server_t *pServer, const janus_id_t serverId,
	const janus_id_t senderId, const janus_id_t roomId, const char *pDescription,
	switch_bool_t record, const char *pRecordingFile, const char *pPin, const char *pRoomIdStr);
switch_status_t apiJoin(server_t *pServer,
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
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason));

#endif /* _API_H_ */
