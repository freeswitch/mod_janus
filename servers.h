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
 * servers.h -- Server list headers for janus endpoint module
 *
 */
#ifndef _SERVERS_H_
#define _SERVERS_H_

#include	"switch.h"
#include	"hash.h"

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
	/* When set, Janus core is expected to run with token_auth=true and
	 * token_auth_secret=<pHmacSecret>. mod_janus will HMAC-sign short-lived
	 * tokens with this key and attach them to every Janus API request, as
	 * well as to the audiobridge join body so that per-room signed_tokens
	 * enforcement (PR #3635) accepts them. */
	char *pHmacSecret;
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

	// values that may be changed go under here - the static values
	// above are only modified during initialisation and so we
	// not mutexed

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
switch_status_t serversDestroy();

#endif //_SERVERS_H_
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
