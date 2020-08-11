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

	switch_core_hash_insert(globals.pServerNameLookup, pName, pServer);

	return SWITCH_STATUS_SUCCESS;
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
