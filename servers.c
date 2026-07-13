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
#include  "http.h"

#include  <arpa/inet.h>
#include  <netdb.h>
#include  <sys/socket.h>

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

#define REGISTRY_MAX_ENDPOINTS 64

void serversBindStartThread(void (*start_fn)(server_t *pServer, switch_bool_t wait_for_active))
{
	globals.start_server_thread = start_fn;
}

void serversBindStopThread(void (*stop_fn)(server_t *pServer))
{
	globals.stop_server_thread = stop_fn;
}

static switch_bool_t serversParseHttpUrlParts(const char *url, char *host, size_t host_size, char *port, size_t port_size,
	char *path, size_t path_size)
{
	const char *p;
	const char *host_start;
	const char *host_end;
	const char *default_port = "80";

	if (zstr(url) || !host || host_size == 0) {
		return SWITCH_FALSE;
	}

	p = url;
	if (!strncmp(p, "http://", 7)) {
		p += 7;
		default_port = "80";
	} else if (!strncmp(p, "https://", 8)) {
		p += 8;
		default_port = "443";
	} else {
		return SWITCH_FALSE;
	}

	host_start = p;
	host_end = strpbrk(p, ":/");
	if (!host_end) {
		host_end = p + strlen(p);
	}

	if ((size_t) (host_end - host_start) >= host_size) {
		return SWITCH_FALSE;
	}

	memcpy(host, host_start, (size_t) (host_end - host_start));
	host[host_end - host_start] = '\0';

	if (*host_end == ':') {
		const char *port_start = host_end + 1;
		const char *port_end = strchr(port_start, '/');
		size_t port_len;

		if (!port_end) {
			port_end = port_start + strlen(port_start);
		}

		port_len = (size_t) (port_end - port_start);
		if (port && port_size > 0) {
			if (port_len >= port_size) {
				return SWITCH_FALSE;
			}
			memcpy(port, port_start, port_len);
			port[port_len] = '\0';
		}

		if (path && path_size > 0) {
			if (*port_end == '/') {
				switch_copy_string(path, port_end, path_size);
			} else {
				path[0] = '\0';
			}
		}
	} else {
		if (port && port_size > 0) {
			switch_copy_string(port, default_port, port_size);
		}
		if (path && path_size > 0) {
			if (*host_end == '/') {
				switch_copy_string(path, host_end, path_size);
			} else {
				path[0] = '\0';
			}
		}
	}

	return SWITCH_TRUE;
}

static char *serversBuildJanusUrl(const char *ip, const char *port, const char *path)
{
	char url[512];

	if (zstr(path)) {
		path = "/janus";
	}

	(void) snprintf(url, sizeof(url), "http://%s:%s%s", ip, port, path);
	return switch_core_strdup(globals.pModulePool, url);
}

static switch_bool_t serversProbeJanusPodAtIp(const char *ip, const char *port, const char *path,
	char *pod_name_out, size_t pod_name_size)
{
	char info_url[512];
	cJSON *json;
	cJSON *server_name;
	switch_bool_t ok = SWITCH_FALSE;

	(void) snprintf(info_url, sizeof(info_url), "http://%s:%s%s/info", ip, port, zstr(path) ? "/janus" : path);
	json = httpGet(info_url, 2000);
	if (!json) {
		return SWITCH_FALSE;
	}

	server_name = cJSON_GetObjectItemCaseSensitive(json, "server-name");
	if (cJSON_IsString(server_name) && server_name->valuestring &&
			serversPodNameValid(server_name->valuestring)) {
		switch_copy_string(pod_name_out, server_name->valuestring, pod_name_size);
		ok = SWITCH_TRUE;
	}

	cJSON_Delete(json);
	return ok;
}

typedef struct registry_endpoint_s {
	char pod_name[64];
	char pod_ip[INET_ADDRSTRLEN];
	char *url;
} registry_endpoint_t;

static server_t *serversCreateRegistryServer(const char *pod_name, const char *pod_ip, const char *url)
{
	server_t *pServer;

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
	pServer->pUrl = switch_core_strdup(globals.pModulePool, url);
	pServer->pod_ip = switch_core_strdup(globals.pModulePool, pod_ip);

	serverCloneDefaults(pServer, globals.pod_defaults);
	switch_set_flag(pServer, SFLAG_ENABLED);
	switch_set_flag(pServer, SFLAG_DYNAMIC);

	switch_core_hash_insert(globals.pServerNameLookup, pServer->name, pServer);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"Registered headless Janus pod=%s ip=%s url=%s\n", pod_name, pod_ip, url);

	if (globals.start_server_thread) {
		globals.start_server_thread(pServer, SWITCH_TRUE);
	}

	return pServer;
}

static void serversUpdateRegistryServer(server_t *pServer, const char *pod_ip, const char *url)
{
	switch_bool_t url_changed = SWITCH_FALSE;

	switch_mutex_lock(pServer->mutex);

	if (!zstr(pod_ip) && (!pServer->pod_ip || strcmp(pServer->pod_ip, pod_ip))) {
		pServer->pod_ip = switch_core_strdup(globals.pModulePool, pod_ip);
	}

	if (!zstr(url) && (!pServer->pUrl || strcmp(pServer->pUrl, url))) {
		pServer->pUrl = switch_core_strdup(globals.pModulePool, url);
		url_changed = SWITCH_TRUE;
	}

	pServer->last_activity = switch_time_now();
	pServer->connect_failures = 0;
	switch_mutex_unlock(pServer->mutex);

	if (url_changed) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"Updated headless Janus pod=%s ip=%s url=%s\n", pServer->name, pod_ip, url);
	}
}

switch_status_t serversRegistryRefresh(void)
{
	char headless_host[256];
	char port[16];
	char path[128];
	struct addrinfo hints, *res, *rp;
	registry_endpoint_t endpoints[REGISTRY_MAX_ENDPOINTS];
	int endpoint_count = 0;
	switch_hash_t *seen_pods = NULL;
	switch_hash_index_t *pIndex = NULL;
	server_t *pServer;
	int i;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (zstr(globals.headless_service_url) || !globals.pod_defaults) {
		return SWITCH_STATUS_FALSE;
	}

	if (!serversParseHttpUrlParts(globals.headless_service_url, headless_host, sizeof(headless_host), port,
			sizeof(port), path, sizeof(path))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Invalid headless-service-url: %s\n", globals.headless_service_url);
		return SWITCH_STATUS_FALSE;
	}

	if (zstr(path)) {
		switch_copy_string(path, "/janus", sizeof(path));
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(headless_host, NULL, &hints, &res) != 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"Headless service DNS lookup failed for %s\n", headless_host);
		return SWITCH_STATUS_FALSE;
	}

	for (rp = res; rp && endpoint_count < REGISTRY_MAX_ENDPOINTS; rp = rp->ai_next) {
		char ip[INET_ADDRSTRLEN];
		char pod_name[64];
		int j;
		switch_bool_t duplicate = SWITCH_FALSE;

		if (rp->ai_family != AF_INET) {
			continue;
		}

		if (!inet_ntop(AF_INET, &((struct sockaddr_in *) rp->ai_addr)->sin_addr, ip, sizeof(ip))) {
			continue;
		}

		if (!serversProbeJanusPodAtIp(ip, port, path, pod_name, sizeof(pod_name))) {
			continue;
		}

		for (j = 0; j < endpoint_count; j++) {
			if (!strcmp(endpoints[j].pod_name, pod_name)) {
				duplicate = SWITCH_TRUE;
				break;
			}
		}
		if (duplicate) {
			continue;
		}

		endpoints[endpoint_count].url = serversBuildJanusUrl(ip, port, path);
		switch_copy_string(endpoints[endpoint_count].pod_name, pod_name, sizeof(endpoints[endpoint_count].pod_name));
		switch_copy_string(endpoints[endpoint_count].pod_ip, ip, sizeof(endpoints[endpoint_count].pod_ip));
		endpoint_count++;
	}

	freeaddrinfo(res);

	switch_core_hash_init(&seen_pods);
	switch_mutex_lock(globals.mutex);

	for (i = 0; i < endpoint_count; i++) {
		pServer = serversFind(endpoints[i].pod_name);
		if (!pServer) {
			(void) serversCreateRegistryServer(endpoints[i].pod_name, endpoints[i].pod_ip, endpoints[i].url);
		} else if (switch_test_flag(pServer, SFLAG_DYNAMIC)) {
			serversUpdateRegistryServer(pServer, endpoints[i].pod_ip, endpoints[i].url);
			if (!switch_test_flag(pServer, SFLAG_ENABLED) && globals.start_server_thread) {
				globals.start_server_thread(pServer, SWITCH_TRUE);
			}
		}
		switch_core_hash_insert(seen_pods, endpoints[i].pod_name, (void *) 1);
	}

	pIndex = NULL;
	while ((pServer = serversIterate(&pIndex)) != NULL) {
		if (!switch_test_flag(pServer, SFLAG_DYNAMIC)) {
			continue;
		}

		if (switch_core_hash_find(seen_pods, pServer->name)) {
			continue;
		}

		switch_mutex_lock(pServer->mutex);
		if (pServer->callsInProgress > 0) {
			switch_mutex_unlock(pServer->mutex);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
				"Headless Janus pod=%s no longer in registry but has active calls; keeping server\n", pServer->name);
			continue;
		}
		switch_mutex_unlock(pServer->mutex);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"Removing headless Janus pod=%s from registry\n", pServer->name);
		switch_set_flag(pServer, SFLAG_EVICTED);
		if (globals.stop_server_thread) {
			globals.stop_server_thread(pServer);
		} else {
			serversDynamicRemoveFromLookup(pServer);
		}
	}

	switch_mutex_unlock(globals.mutex);
	switch_core_hash_destroy(&seen_pods);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"Headless Janus registry refresh: %d pod(s) from %s\n", endpoint_count, headless_host);

	return status;
}

static void *SWITCH_THREAD_FUNC servers_registry_run(switch_thread_t *pThread, void *pObj)
{
	unsigned int refresh_usec;

	(void) pThread;
	(void) pObj;

	refresh_usec = (globals.registry_refresh_sec ? globals.registry_refresh_sec : 30) * 1000000;

	(void) serversRegistryRefresh();

	while (!globals.registry_terminating) {
		switch_yield(refresh_usec);
		if (globals.registry_terminating || zstr(globals.headless_service_url)) {
			break;
		}
		(void) serversRegistryRefresh();
	}

	return NULL;
}

void serversStartRegistry(void)
{
	switch_threadattr_t *pThreadAttr = NULL;

	if (globals.registry_thread || zstr(globals.headless_service_url)) {
		return;
	}

	globals.registry_terminating = SWITCH_FALSE;
	switch_threadattr_create(&pThreadAttr, globals.pModulePool);
	switch_threadattr_stacksize_set(pThreadAttr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&globals.registry_thread, pThreadAttr, servers_registry_run, NULL, globals.pModulePool);
}

void serversStopRegistry(void)
{
	switch_status_t status;
	switch_status_t returnValue;

	if (!globals.registry_thread) {
		return;
	}

	globals.registry_terminating = SWITCH_TRUE;
	status = switch_thread_join(&returnValue, globals.registry_thread);
	globals.registry_thread = NULL;

	if (status != SWITCH_STATUS_SUCCESS || returnValue != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"Headless registry thread join status=%d return=%d\n", status, returnValue);
	}
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
	switch_bool_t fail = SWITCH_FALSE;

	switch_assert(pServer);

	if (!switch_test_flag(pServer, SFLAG_DYNAMIC)) {
		return SWITCH_FALSE;
	}

	switch_mutex_lock(pServer->mutex);
	calls = pServer->callsInProgress;
	failures = pServer->connect_failures;
	serverId = pServer->serverId;
	switch_mutex_unlock(pServer->mutex);

	if (calls > 0) {
		return SWITCH_FALSE;
	}

	if (globals.pod_server_fail_max && failures >= globals.pod_server_fail_max && !serverId) {
		fail = SWITCH_TRUE;
	}

	if (pIdle) {
		*pIdle = SWITCH_FALSE;
	}
	if (pFail) {
		*pFail = fail;
	}

	return fail ? SWITCH_TRUE : SWITCH_FALSE;
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
  char text[512];

  switch_assert(globals.pServerNameLookup);

  pStream->write_function(pStream, "name|enabled|registry|pod_ip|url|totalCalls|callsInProgress|started|id\n");
  while ((pServer = serversIterate(&pIndex)) != NULL) {
    switch_mutex_lock(pServer->mutex);
    (void) snprintf(text, sizeof(text),
		"%s|%s|%s|%s|%s|%u|%u|%" SWITCH_INT64_T_FMT "|%" SWITCH_UINT64_T_FMT "\n",
		pServer->name,
        switch_test_flag(pServer, SFLAG_ENABLED) ? "true" : "false",
		switch_test_flag(pServer, SFLAG_DYNAMIC) ? "true" : "false",
		pServer->pod_ip ? pServer->pod_ip : "",
		pServer->pUrl ? pServer->pUrl : "",
		pServer->totalCalls,
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
