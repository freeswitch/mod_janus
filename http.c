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
 * http.c -- HTTP functions for janus endpoint module
 *
 */
#include  "cJSON.h"
#include  "switch.h"
#include  "switch_curl.h"

#include  "globals.h"
#include  "http.h"

#define INITIAL_BODY_SIZE 1000

typedef struct {
	http_abort_fn_t fn;
	void *data;
} http_abort_ctx_t;

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  switch_buffer_t *pBuffer = (switch_buffer_t *) userdata;
  switch_buffer_write(pBuffer, ptr, size * nmemb);
  return nmemb;
}

#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x072000)
static int http_xferinfo(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
	curl_off_t ultotal, curl_off_t ulnow)
{
	http_abort_ctx_t *ctx = (http_abort_ctx_t *) clientp;
	(void) dltotal;
	(void) dlnow;
	(void) ultotal;
	(void) ulnow;
	if (ctx && ctx->fn && ctx->fn(ctx->data) == SWITCH_TRUE) {
		return 1;
	}
	return 0;
}
#else
static int http_progress(void *clientp, double dltotal, double dlnow,
	double ultotal, double ulnow)
{
	http_abort_ctx_t *ctx = (http_abort_ctx_t *) clientp;
	(void) dltotal;
	(void) dlnow;
	(void) ultotal;
	(void) ulnow;
	if (ctx && ctx->fn && ctx->fn(ctx->data) == SWITCH_TRUE) {
		return 1;
	}
	return 0;
}
#endif

static void http_setopt_abort(switch_CURL *curl_handle, http_abort_ctx_t *ctx)
{
	if (!curl_handle || !ctx || !ctx->fn) {
		return;
	}
	/* Progress ticks about once/sec even with no body bytes — enough to wake shutdown. */
	switch_curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x072000)
	switch_curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, http_xferinfo);
	switch_curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, ctx);
#else
	switch_curl_easy_setopt(curl_handle, CURLOPT_PROGRESSFUNCTION, http_progress);
	switch_curl_easy_setopt(curl_handle, CURLOPT_PROGRESSDATA, ctx);
#endif
}

cJSON *httpPost(const char *pUrl, const unsigned int timeout, cJSON *pJsonRequest,
	http_abort_fn_t abort_fn, void *abort_data)
{
  cJSON *pJsonResponse = NULL;
  switch_CURL *curl_handle = NULL;
  switch_CURLcode curl_status = CURLE_UNKNOWN_OPTION;
  long httpRes = 0;
  switch_curl_slist_t *headers = NULL;
  char *pJsonStr = NULL;
  switch_buffer_t *pBody = NULL;
  const char *pBodyStr;
  http_abort_ctx_t abort_ctx = { abort_fn, abort_data };

  switch_assert(pUrl);

  switch_buffer_create_dynamic(&pBody, INITIAL_BODY_SIZE, INITIAL_BODY_SIZE, 0);

  curl_handle = switch_curl_easy_init();
  if (!curl_handle) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't get CURL handle\n");
    return NULL;
  }

  headers = switch_curl_slist_append(headers, "Content-Type: application/json");
  headers = switch_curl_slist_append(headers, "Accept: application/json");

  switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
  switch_curl_easy_setopt(curl_handle, CURLOPT_URL, pUrl);
  pJsonStr = cJSON_PrintUnformatted(pJsonRequest);
  switch_curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, pJsonStr);
  switch_curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "freeswitch-janus/1.0");
  switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
  switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) pBody);
  if (timeout) {
    switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, timeout);
    switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1);
  }
  http_setopt_abort(curl_handle, &abort_ctx);

  DEBUG(SWITCH_CHANNEL_LOG, "HTTP POST url=%s json=%s\n", pUrl, pJsonStr);

  curl_status = switch_curl_easy_perform(curl_handle);
  switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &httpRes);

  if (curl_status == CURLE_OK) {
    // terminate the string
    (void) switch_buffer_write(pBody, "\0", 1);

    (void) switch_buffer_peek_zerocopy(pBody, (const void **) &pBodyStr);

    DEBUG(SWITCH_CHANNEL_LOG, "result=%s\n", pBodyStr);

    pJsonResponse = cJSON_Parse(pBodyStr);
  } else if (curl_status == CURLE_ABORTED_BY_CALLBACK) {
    DEBUG(SWITCH_CHANNEL_LOG, "HTTP POST aborted url=%s\n", pUrl);
  } else {
    // nothing downloaded or download interrupted
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Received curl error %d HTTP error code %ld trying to fetch %s\n", curl_status, httpRes, pUrl);
  }

  switch_curl_easy_cleanup(curl_handle);
  switch_safe_free(pJsonStr);
  switch_buffer_destroy(&pBody);
  switch_curl_slist_free_all(headers);

  return pJsonResponse;
}

cJSON *httpGet(const char *pUrl, const unsigned int timeout,
	http_abort_fn_t abort_fn, void *abort_data)
{
  cJSON *pJsonResponse = NULL;
  switch_CURL *curl_handle = NULL;
	switch_CURLcode curl_status = CURLE_UNKNOWN_OPTION;
  long httpRes = 0;
  switch_curl_slist_t *headers = NULL;
  switch_buffer_t *pBody = NULL;
  const char *pBodyStr;
  http_abort_ctx_t abort_ctx = { abort_fn, abort_data };

  switch_assert(pUrl);

  switch_buffer_create_dynamic(&pBody, INITIAL_BODY_SIZE, INITIAL_BODY_SIZE, 0);

  curl_handle = switch_curl_easy_init();
  if (!curl_handle) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't get CURL handle\n");
    return NULL;
  }

  headers = switch_curl_slist_append(headers, "Accept: application/json");

  switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
  switch_curl_easy_setopt(curl_handle, CURLOPT_URL, pUrl);
  switch_curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "freeswitch-janus/1.0");
  switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
  switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) pBody);
  if (timeout) {
    switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, timeout);
    switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1);
  }
  http_setopt_abort(curl_handle, &abort_ctx);

  DEBUG(SWITCH_CHANNEL_LOG, "HTTP GET url=%s\n", pUrl);

  curl_status = switch_curl_easy_perform(curl_handle);
  switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &httpRes);

  if (curl_status == CURLE_OK) {
    // terminate the string
    (void) switch_buffer_write(pBody, "\0", 1);

    (void) switch_buffer_peek_zerocopy(pBody, (const void **) &pBodyStr);

    DEBUG(SWITCH_CHANNEL_LOG, "code=%ld result=%s\n", httpRes, pBodyStr);

    pJsonResponse = cJSON_Parse(pBodyStr);
  } else if (curl_status == CURLE_ABORTED_BY_CALLBACK) {
    DEBUG(SWITCH_CHANNEL_LOG, "HTTP GET aborted url=%s\n", pUrl);
  } else {
    // nothing downloaded or download interrupted
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Received curl error %d HTTP error code %ld trying to fetch %s\n", curl_status, httpRes, pUrl);
  }

  switch_curl_easy_cleanup(curl_handle);
  switch_buffer_destroy(&pBody);
  switch_curl_slist_free_all(headers);

  return pJsonResponse;
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
