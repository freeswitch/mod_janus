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
 * servers.c -- Server list functions for janus endpoint module
 *
 */
#include  "globals.h"
#include  "servers.h"

switch_status_t serversList(const char *pLine, const char *pCursor, switch_console_callback_match_t **matches) {
	switch_hash_index_t *pIndex = NULL;
	server_t *pServer;
	switch_console_callback_match_t *myMatches = NULL;

  while ((pServer = serversIterate(&pIndex)) != NULL) {
		switch_console_push_match(&myMatches, pServer->name);
	}

	if (myMatches) {
		*matches = myMatches;
	}

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t serversAdd(switch_xml_t xmlint) {
	switch_xml_t param;
	char *pName = (char *) switch_xml_attr_soft(xmlint, "name");
	server_t *pServer;

  switch_assert(globals.pServerNameLookup);

	if (!pName) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Server has no name\n");
		return SWITCH_STATUS_FALSE;
	}

  if (serversFind(pName) != NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Server=%s already defined\n", pName);
		return SWITCH_STATUS_FALSE;
	}

	DEBUG(SWITCH_CHANNEL_LOG, "Defining server=%s\n", pName);

	pServer = switch_core_alloc(globals.pModulePool, sizeof(*pServer));

  switch_mutex_init(&pServer->mutex, SWITCH_MUTEX_NESTED, globals.pModulePool);
	switch_mutex_init(&pServer->flag_mutex, SWITCH_MUTEX_NESTED, globals.pModulePool);

  pServer->serverId = 0;
  pServer->totalCalls = 0;
  pServer->callsInProgress = 0;
  pServer->pThread = NULL;
  pServer->transport = JANUS_TP_HTTP;
  pServer->janus_ws_handle = NULL;
  pServer->ws_last_poll = 0;

	// set default values
	pServer->name = switch_core_strdup(globals.pModulePool, pName);
  pServer->codec_string = "opus";
  pServer->local_network = "localnet.auto";

	for (param = switch_xml_child(xmlint, "param"); param; param = param->next) {
		char *pVarStr = (char *) switch_xml_attr_soft(param, "name");
		char *pValStr = (char *) switch_xml_attr_soft(param, "value");
		DEBUG(SWITCH_CHANNEL_LOG, "Server=%s  %s->%s\n", pName, pVarStr, pValStr);

		if (!strcmp(pVarStr, "url") && !zstr(pVarStr)) {
			pServer->pUrl = switch_core_strdup(globals.pModulePool, pValStr);
		} else if (!strcmp(pVarStr, "secret") && !zstr(pValStr)) {
			pServer->pSecret = switch_core_strdup(globals.pModulePool, pValStr);
		} else if (!strcmp(pVarStr, "auth-token") && !zstr(pValStr)) {
			pServer->pAuthToken = switch_core_strdup(globals.pModulePool, pValStr);
		} else if (!strcmp(pVarStr, "hmac-secret") && !zstr(pValStr)) {
			pServer->pHmacSecret = switch_core_strdup(globals.pModulePool, pValStr);
		} else if (!strcmp(pVarStr, "local-network-acl") && !zstr(pValStr)) {
      if (strcasecmp(pValStr, "none")) {
	      pServer->local_network = switch_core_strdup(globals.pModulePool, pValStr);
      }
		} else if (!strcmp(pVarStr, "ext-rtp-ip") && !zstr(pValStr)) {
      char *ip = globals.guess_ip;

      if (!strcmp(pValStr, "0.0.0.0")) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid IP 0.0.0.0 replaced with %s\n",
          globals.guess_ip);
      } else if (!strcasecmp(pValStr, "auto-nat")) {
        ip = NULL;
      } else {
        ip = strcasecmp(pValStr, "auto") ? pValStr : globals.guess_ip;
        switch_clear_flag(pServer, SFLAG_AUTO_NAT);
      }
      if (ip) {
        if (!strncasecmp(ip, "autonat:", 8)) {
          pServer->extrtpip = switch_core_strdup(globals.pModulePool, ip + 8);
          switch_set_flag(pServer, SFLAG_AUTO_NAT);
        } else {
          pServer->extrtpip = switch_core_strdup(globals.pModulePool, ip);
        }
      }
		} else if (!strcmp(pVarStr, "rtp-ip")) {
      char *ip = globals.guess_ip;
      char buf[64];

      if (zstr(pValStr)) {
        ip = globals.guess_ip;
      } else if (!strcmp(pValStr, "0.0.0.0")) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid IP 0.0.0.0 replaced with %s\n", globals.guess_ip);
      } else if (!strncasecmp(pValStr, "interface:", 10)) {
        char *ifname = pValStr + 10;
        int family = AF_UNSPEC;
        if (!strncasecmp(ifname, "auto/", 5)) { ifname += 5; family = AF_UNSPEC; }
        if (!strncasecmp(ifname, "ipv4/", 5)) { ifname += 5; family = AF_INET;   }
        if (!strncasecmp(ifname, "ipv6/", 5)) { ifname += 5; family = AF_INET6;  }
        if (switch_find_interface_ip(buf, sizeof(buf), NULL, ifname, family) == SWITCH_STATUS_SUCCESS) {
          ip = buf;
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Using %s IP for interface %s for rtp-ip\n", ip, pValStr + 10);
        } else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unknown IP for interface %s for rtp-ip\n", pValStr + 10);
        }
      } else {
        ip = strcasecmp(pValStr, "auto") ? pValStr : globals.guess_ip;
      }

      if (strchr(ip, ':')) {
        pServer->rtpip6 = switch_core_strdup(globals.pModulePool, ip);
      } else {
        pServer->rtpip = switch_core_strdup(globals.pModulePool, ip);
      }
    } else if (!strcasecmp(pVarStr, "apply-candidate-acl") && !zstr(pValStr)) {
      if (!strcasecmp(pValStr, "none")) {
        pServer->cand_acl_count = 0;
      } else if (pServer->cand_acl_count < SWITCH_MAX_CAND_ACL) {
        pServer->cand_acl[pServer->cand_acl_count++] = switch_core_strdup(globals.pModulePool, pValStr);
      } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Max acl records of %d reached\n", SWITCH_MAX_CAND_ACL);
      }
    } else if (!strcasecmp(pVarStr, "codec-string") && !zstr(pValStr)) {
      pServer->codec_string = switch_core_strdup(globals.pModulePool, pValStr);
		} else if (!strcmp(pVarStr, "enabled") && !zstr(pValStr)) {
			// set the flag to the opposite state so that we will do the right thine
      if (switch_true(pValStr)) {
        switch_set_flag(pServer, SFLAG_ENABLED);
      }
		}
	}

	if (!pServer->pUrl) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Server=%s  Mandatory parameter not specified\n", pName);
		return SWITCH_STATUS_FALSE;
	}

	if (!strncasecmp(pServer->pUrl, "ws://", 5) || !strncasecmp(pServer->pUrl, "wss://", 6)) {
#if defined(HAVE_MOD_JANUS_WS)
		pServer->transport = JANUS_TP_WS;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
				"Server=%s  WebSocket transport selected (url=%s)\n", pName, pServer->pUrl);
#else
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"Server=%s  WebSocket URL '%s' but mod_janus was built without libks support\n",
				pName, pServer->pUrl);
		return SWITCH_STATUS_FALSE;
#endif
	} else {
		pServer->transport = JANUS_TP_HTTP;
	}

	if (pServer->pHmacSecret && pServer->pAuthToken) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"Server=%s  Both 'hmac-secret' and 'auth-token' are set; auth-token will be ignored and HMAC-signed tokens will be used instead\n",
				pName);
	}

	switch_core_hash_insert(globals.pServerNameLookup, pName, pServer);

	if (!globals.pod_defaults) {
		(void) serversCaptureDefaults(pServer);
	}

	return SWITCH_STATUS_SUCCESS;
}

static void serverCloneDefaults(server_t *dst, const server_t *src) {
	dst->codec_string = src->codec_string;
	dst->local_network = src->local_network;
	dst->extrtpip = src->extrtpip;
	dst->rtpip = src->rtpip;
	dst->rtpip6 = src->rtpip6;
	dst->pSecret = src->pSecret;
	dst->pAuthToken = src->pAuthToken;
	dst->pHmacSecret = src->pHmacSecret;
	dst->cand_acl_count = src->cand_acl_count;
	for (uint32_t i = 0; i < src->cand_acl_count; i++) {
		dst->cand_acl[i] = src->cand_acl[i];
	}
	if (switch_test_flag((server_t *) src, SFLAG_AUTO_NAT)) {
		switch_set_flag(dst, SFLAG_AUTO_NAT);
	}
}

switch_status_t serversCaptureDefaults(server_t *pServer) {
	switch_assert(pServer);

	if (!globals.pod_defaults) {
		globals.pod_defaults = switch_core_alloc(globals.pModulePool, sizeof(*globals.pod_defaults));
	}
	serverCloneDefaults(globals.pod_defaults, pServer);
	return SWITCH_STATUS_SUCCESS;
}

switch_bool_t serversPodNameValid(const char *pod_name) {
	size_t len;
	const char *p;

	if (zstr(pod_name)) {
		return SWITCH_FALSE;
	}

	len = strlen(pod_name);
	if (len > 63) {
		return SWITCH_FALSE;
	}

	for (p = pod_name; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) {
			return SWITCH_FALSE;
		}
	}

	if (pod_name[0] == '-' || pod_name[len - 1] == '-') {
		return SWITCH_FALSE;
	}

	return SWITCH_TRUE;
}

static unsigned int serversDynamicCount(void) {
	switch_hash_index_t *pIndex = NULL;
	server_t *pServer;
	unsigned int count = 0;

	while ((pServer = serversIterate(&pIndex)) != NULL) {
		if (switch_test_flag(pServer, SFLAG_DYNAMIC)) {
			count++;
		}
	}
	return count;
}

static char *serversExpandPodUrl(const char *pod_name) {
	char *expanded;
	const char *placeholder = "{pod}";
	const char *start, *match;
	size_t prefix_len, pod_len, total;

	switch_assert(globals.pod_url_template);

	start = globals.pod_url_template;
	match = strstr(start, placeholder);
	if (!match) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"pod-url-template missing {pod} placeholder: %s\n", globals.pod_url_template);
		return NULL;
	}

	prefix_len = (size_t) (match - start);
	pod_len = strlen(pod_name);
	total = prefix_len + pod_len + strlen(match + strlen(placeholder)) + 1;

	expanded = switch_core_alloc(globals.pModulePool, total);
	(void) snprintf(expanded, total, "%.*s%s%s", (int) prefix_len, start, pod_name, match + strlen(placeholder));
	return expanded;
}

server_t *serversEnsureFromTemplate(const char *pod_name) {
	server_t *pServer;
	char *pUrl;
	unsigned int max_servers;

	if (zstr(globals.pod_url_template) || !globals.pod_defaults) {
		return NULL;
	}

	if (!serversPodNameValid(pod_name)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"Rejecting invalid server name=%s\n", pod_name);
		return NULL;
	}

	max_servers = globals.pod_server_max ? globals.pod_server_max : 64;
	if (serversDynamicCount() >= max_servers) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"Dynamic server limit reached (%u); cannot add server=%s\n", max_servers, pod_name);
		return NULL;
	}

	pUrl = serversExpandPodUrl(pod_name);
	if (!pUrl) {
		return NULL;
	}

	pServer = switch_core_alloc(globals.pModulePool, sizeof(*pServer));
	switch_mutex_init(&pServer->mutex, SWITCH_MUTEX_NESTED, globals.pModulePool);
	switch_mutex_init(&pServer->flag_mutex, SWITCH_MUTEX_NESTED, globals.pModulePool);

	pServer->serverId = 0;
	pServer->totalCalls = 0;
	pServer->callsInProgress = 0;
	pServer->pThread = NULL;
	pServer->transport = JANUS_TP_HTTP;
	pServer->janus_ws_handle = NULL;
	pServer->ws_last_poll = 0;
	pServer->last_activity = switch_time_now();
	pServer->connect_failures = 0;
	pServer->name = switch_core_strdup(globals.pModulePool, pod_name);
	pServer->pUrl = pUrl;

	serverCloneDefaults(pServer, globals.pod_defaults);
	switch_set_flag(pServer, SFLAG_ENABLED);
	switch_set_flag(pServer, SFLAG_DYNAMIC);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"Created dynamic Janus server=%s url=%s\n", pod_name, pUrl);

	switch_core_hash_insert(globals.pServerNameLookup, pServer->name, pServer);
	return pServer;
}

void serversDynamicRecordActivity(server_t *pServer)
{
	switch_assert(pServer);

	switch_mutex_lock(pServer->mutex);
	pServer->last_activity = switch_time_now();
	switch_mutex_unlock(pServer->mutex);
}

void serversDynamicRecordConnectFailure(server_t *pServer)
{
	switch_assert(pServer);

	switch_mutex_lock(pServer->mutex);
	pServer->connect_failures++;
	switch_mutex_unlock(pServer->mutex);
}

void serversDynamicResetConnectFailures(server_t *pServer)
{
	switch_assert(pServer);

	switch_mutex_lock(pServer->mutex);
	pServer->connect_failures = 0;
	pServer->last_activity = switch_time_now();
	switch_mutex_unlock(pServer->mutex);
}

switch_bool_t serversDynamicEvictable(server_t *pServer, switch_bool_t *pIdle, switch_bool_t *pFail)
{
	unsigned int calls;
	unsigned int failures;
	janus_id_t serverId;
	switch_time_t last;
	switch_time_t idle_usec;
	switch_bool_t idle = SWITCH_FALSE;
	switch_bool_t fail = SWITCH_FALSE;

	switch_assert(pServer);

	if (!switch_test_flag(pServer, SFLAG_DYNAMIC)) {
		return SWITCH_FALSE;
	}

	switch_mutex_lock(pServer->mutex);
	calls = pServer->callsInProgress;
	failures = pServer->connect_failures;
	serverId = pServer->serverId;
	last = pServer->last_activity;
	switch_mutex_unlock(pServer->mutex);

	if (calls > 0) {
		return SWITCH_FALSE;
	}

	if (globals.pod_server_fail_max && failures >= globals.pod_server_fail_max && !serverId) {
		fail = SWITCH_TRUE;
	}

	if (globals.pod_server_idle_sec) {
		idle_usec = (switch_time_t) globals.pod_server_idle_sec * 1000000;
		if ((switch_time_now() - last) >= idle_usec) {
			idle = SWITCH_TRUE;
		}
	}

	if (pIdle) {
		*pIdle = idle;
	}
	if (pFail) {
		*pFail = fail;
	}

	return idle || fail ? SWITCH_TRUE : SWITCH_FALSE;
}

void serversDynamicRemoveFromLookup(server_t *pServer)
{
	janus_id_t serverId = 0;
	const char *name;

	switch_assert(pServer);
	switch_assert(globals.pServerNameLookup);

	switch_mutex_lock(globals.mutex);

	if (!switch_test_flag(pServer, SFLAG_DYNAMIC)) {
		switch_mutex_unlock(globals.mutex);
		return;
	}

	name = pServer->name;
	if (!name || !switch_core_hash_find(globals.pServerNameLookup, name)) {
		switch_mutex_unlock(globals.mutex);
		return;
	}

	switch_mutex_lock(pServer->mutex);
	serverId = pServer->serverId;
	pServer->serverId = 0;
	switch_mutex_unlock(pServer->mutex);

	if (serverId) {
		(void) hashDelete(&globals.serverIdLookup, serverId);
	}

	switch_core_hash_delete(globals.pServerNameLookup, name);
	switch_clear_flag_locked(pServer, SFLAG_ENABLED);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"Evicted dynamic server=%s\n", name);

	switch_mutex_unlock(globals.mutex);
}

switch_status_t serversSummary(switch_stream_handle_t *pStream) {
  switch_hash_index_t *pIndex = NULL;
	server_t *pServer;
  char text[128];

  switch_assert(globals.pServerNameLookup);

  pStream->write_function(pStream, "name|enabled|totalCalls|callsInProgress|started|id\n");
  while ((pServer = serversIterate(&pIndex)) != NULL) {
    switch_mutex_lock(pServer->mutex);
    (void) snprintf(text, sizeof(text), "%s|%s|%u|%u|%" SWITCH_INT64_T_FMT "|%" SWITCH_UINT64_T_FMT "\n", pServer->name,
        switch_test_flag(pServer, SFLAG_ENABLED) ? "true" : "false", pServer->totalCalls,
        pServer->callsInProgress, pServer->started, pServer->serverId);
    switch_mutex_unlock(pServer->mutex);

    pStream->write_function(pStream, text);

  }
  return SWITCH_STATUS_SUCCESS;
}

server_t *serversFind(const char * const pName) {
  switch_assert(globals.pServerNameLookup);

  return switch_core_hash_find(globals.pServerNameLookup, pName);
}

server_t *serversIterate(switch_hash_index_t **pIndex) {
	void *val;
	const void *vvar;

  switch_assert(globals.pServerNameLookup);

  if (!*pIndex) {
    *pIndex = switch_core_hash_first(globals.pServerNameLookup);
  } else {
    *pIndex = switch_core_hash_next(pIndex);
  }

  if (*pIndex) {
	  switch_core_hash_this(*pIndex, &vvar, NULL, &val);
		return (server_t *) val;
  } else {
    return NULL;
  }
}

switch_status_t serversDestroy() {
	return switch_core_hash_destroy(&globals.pServerNameLookup);
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
