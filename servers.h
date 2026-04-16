/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * servers.h -- Server list headers for janus endpoint module
 */
#ifndef _SERVERS_H_
#define _SERVERS_H_

#include "switch.h"
#include "hash.h"

typedef enum {
	SFLAG_ENABLED        = (1 << 0),
	SFLAG_TERMINATING    = (1 << 1),
	SFLAG_AUTO_NAT       = (1 << 2)
} SFLAGS;

typedef enum {
	JANUS_TP_HTTP = 0,
	JANUS_TP_WS
} janus_transport_t;

typedef struct server_s {
	char *name;
	char *pUrl;
	char *pSecret;
	char *pAuthToken;
	switch_thread_t *pThread;

	char *local_network;
	char *extrtpip;

	char *cand_acl[SWITCH_MAX_CAND_ACL];
	uint32_t cand_acl_count;

	char *rtpip;
	char *rtpip6;
	char *codec_string;

	switch_mutex_t *flag_mutex;
	unsigned int flags;

	switch_mutex_t *mutex;

	hash_t senderIdLookup;

	janus_id_t serverId;
	switch_time_t started;
	unsigned int totalCalls;
	unsigned int callsInProgress;

	janus_transport_t transport;
	void *janus_ws_handle; /* janus_ws_ctx_t when transport == JANUS_TP_WS */
	switch_time_t ws_last_poll; /* WebSocket keepalive / activity timestamp */
} server_t;

switch_status_t serversList(const char *pLine, const char *pCursor, switch_console_callback_match_t **matches);
switch_status_t serversAdd(switch_xml_t xmlint);
switch_status_t serversSummary(switch_stream_handle_t *pStream);
server_t *serversFind(const char * const pName);
server_t *serversIterate(switch_hash_index_t **pIndex);
switch_status_t serversDestroy(void);

#endif /* _SERVERS_H_ */
