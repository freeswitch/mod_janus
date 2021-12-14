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

janus_id_t apiGetServerId(const char *pUrl, const char *pSecret);
switch_status_t apiClaimServerId(const char *pUrl, const char *pSecret, const janus_id_t serverId);
janus_id_t apiGetSenderId(const char *pUrl, const char *pSecret, const janus_id_t serverId);
janus_id_t apiCreateRoom(const char *pUrl, const char *pSecret, const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const char *pDescription, switch_bool_t record, const char *pRecordingFile, const char *pPin, switch_bool_t pAudioLevelEvent);
switch_status_t apiJoin(const char *pUrl, const char *pSecret, const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const char *pDisplay, const char *pPin, const char *pToken);
switch_status_t apiConfigure(const char *pUrl, const char *pSecret,
		const janus_id_t serverId, const janus_id_t senderId, const switch_bool_t muted,
		switch_bool_t record, const char *pRecordingFile,
		const char *pType, const char *pSdp);
switch_status_t apiLeave(const char *pUrl, const char *pSecret, const janus_id_t serverId, const janus_id_t senderId);
switch_status_t apiDetach(const char *pUrl, const char *pSecret, const janus_id_t serverId, const janus_id_t senderId);
switch_status_t apiPoll(const char *pUrl, const char *pSecret, const janus_id_t serverId, const char *pAuthToken,
  switch_status_t (*pJoinedFunc)(const janus_id_t serverId, const janus_id_t senderId, const janus_id_t roomId, const janus_id_t participantId),
  switch_status_t (*pAcceptedFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pSdp),
	switch_status_t (*pTrickleFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pCandidate),
  switch_status_t (*pAnsweredFunc)(const janus_id_t serverId, const janus_id_t senderId),
  switch_status_t (*pHungupFunc)(const janus_id_t serverId, const janus_id_t senderId, const char *pReason));

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
