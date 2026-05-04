/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * janus_ws.h -- Janus WebSocket transport (libks) for mod_janus
 */
#ifndef MOD_JANUS_JANUS_WS_H
#define MOD_JANUS_JANUS_WS_H

#include "servers.h"

#if defined(HAVE_MOD_JANUS_WS)

void janus_ws_mod_global_shutdown(void);

switch_status_t janus_ws_server_open(server_t *server);
void janus_ws_server_close(server_t *server);

/* Synchronous request/response over the shared WebSocket (blocking). */
cJSON *janus_ws_rpc_json(server_t *server, cJSON *request, const char *transaction, switch_interval_time_t timeout_us);

/*
 * Block until one WS text frame is received and processed, or timeout.
 * Dispatches async Janus messages to the same callbacks as apiPoll.
 * Sends keepalive when idle longer than keepalive_interval_us.
 */
switch_status_t janus_ws_pump_once(server_t *server, janus_id_t session_id,
	switch_interval_time_t wait_us,
	switch_interval_time_t keepalive_interval_us,
	switch_time_t *last_activity_ref,
	switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
	switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
	switch_bool_t (*pAnswerOnWebrtcupFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason));

#endif /* HAVE_MOD_JANUS_WS */

#endif /* MOD_JANUS_JANUS_WS_H */
