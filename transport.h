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
#ifndef	_TRANSPORT_H_
#define	_TRANSPORT_H_

#include  "libks/ks.h"
#include  "libks/ks_json.h"
#include  "globals.h"
//#include  "http.h"
#include  "http_struct.h"

typedef enum {
	TRANSPORT_TYPE_NONE,
	TRANSPORT_TYPE_HTTP,
	TRANSPORT_TYPE_WS
} transport_types_t;

//TODO - can we do this better???
//struct server;
typedef struct server server_t;

typedef struct {
	api_states_t state;
	char *pUrl;
	char *pSecret;
	transport_types_t type;

	// requests
	ks_json_t *(*pSend)(server_t *pServer, const char *url, ks_json_t *pJsonRequest);
	ks_json_t *(*pPoll)(server_t *pServer, const char *url);	// HTTP-only

	// callbacks
	switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId);
	switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp);
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate);
	switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId);
	switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason);

	//TODO - union with ws
	http_t http;

} transport_t;


#endif //_TRANSPORT_H_
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
