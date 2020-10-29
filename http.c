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
#include  "libks/ks.h"
#include  "libks/ks_json.h"
#include  "switch.h"
#include  "switch_curl.h"

#include  "globals.h"
#include  "http.h"

#define INITIAL_BODY_SIZE 1000

#define HTTP_TIMEOUT_NONE     0
#define HTTP_TIMEOUT_DEFAULT  3000

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  switch_buffer_t *pBuffer = (switch_buffer_t *) userdata;
  switch_buffer_write(pBuffer, ptr, size * nmemb);
  return nmemb;
}

ks_json_t *httpPost(server_t *pServer, const char *pUrl, ks_json_t *pJsonRequest)
{
  ks_json_t *pJsonResponse = NULL;
  switch_CURL *curl_handle = NULL;
	switch_CURLcode curl_status = CURLE_UNKNOWN_OPTION;
  long httpRes = 0;
  switch_curl_slist_t *headers = NULL;
  char *pJsonStr = NULL;
  unsigned int timeout = HTTP_TIMEOUT_NONE;

  switch_assert(pUrl);

  curl_handle = switch_curl_easy_init();
  if (!curl_handle) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't get CURL handle\n");
    return NULL;
  }

  headers = switch_curl_slist_append(headers, "Content-Type: application/json");
  headers = switch_curl_slist_append(headers, "Accept: application/json");

  switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
  switch_curl_easy_setopt(curl_handle, CURLOPT_URL, pUrl);
  pJsonStr = ks_json_print_unformatted(pJsonRequest);
  switch_curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, pJsonStr);
  switch_curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "freeswitch-janus/1.0");
  if (timeout) {
    switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, timeout);
    switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1);
  }

  DEBUG(SWITCH_CHANNEL_LOG, "HTTP POST url=%s\n", pUrl);


  //DEBUG(SWITCH_CHANNEL_LOG, "multi=%d\n", *pServer->transport.http.multi_handle);

  (void) curl_multi_add_handle(pServer->transport.http.multi_handle, curl_handle);

  //DEBUG(SWITCH_CHANNEL_LOG, "2multi=%d\n", *pServer->transport.http.multi_handle);

  switch_curl_slist_free_all(headers);
  ks_free(pJsonStr);

  return NULL;//TODO - OK???
}


ks_json_t *httpGet(server_t *pServer, const char *pUrl)
{
  ks_json_t *pJsonResponse = NULL;
  switch_CURL *curl_handle = NULL;
	switch_CURLcode curl_status = CURLE_UNKNOWN_OPTION;
  long httpRes = 0;
  switch_curl_slist_t *headers = NULL;
  unsigned int timeout = HTTP_TIMEOUT_NONE;

  switch_assert(pUrl);

  curl_handle = switch_curl_easy_init();
  if (!curl_handle) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't get CURL handle\n");
    return NULL;
  }

  headers = switch_curl_slist_append(headers, "Accept: application/json");

  switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
  switch_curl_easy_setopt(curl_handle, CURLOPT_URL, pUrl);
  switch_curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "freeswitch-janus/1.0");
  if (timeout) {
    switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, timeout);
    switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1);
  }

  DEBUG(SWITCH_CHANNEL_LOG, "HTTP GET url=%s\n", pUrl);



  (void) curl_multi_add_handle(pServer->transport.http.multi_handle, curl_handle);


  switch_curl_slist_free_all(headers);

  return NULL;//TODO - OK???
}


void *SWITCH_THREAD_FUNC httpThread(switch_thread_t *pThread, void *pObj) {
  server_t *pServer = (server_t *) pObj;

  CURLMcode mc;
  int still_running; /* keep number of running handles */
  char finished = 0; //TODO
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Thread started\n");

  pServer->transport.http.multi_handle = curl_multi_init();
  if (!pServer->transport.http.multi_handle) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed multi handle\n");
    //TODO print error
    //TODO bomb out
  }

  while (!finished) {
    still_running = FALSE;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Thread continuing\n");

    while (!still_running) {
      fd_set fdread;
      fd_set fdwrite;
      fd_set fdexcep;
      int maxfd = -1;
      int rc;

      long curl_timeo;
      struct timeval timeout;

      //TODO - do we not timeout
      (void) curl_multi_timeout(pServer->transport.http.multi_handle, &curl_timeo);
      if(curl_timeo >= 0) {
        timeout.tv_sec = curl_timeo / 1000;
        if(timeout.tv_sec > 1) {
          timeout.tv_sec = 1;
        } else {
          timeout.tv_usec = (curl_timeo % 1000) * 1000;
        }
      }

      FD_ZERO(&fdread);
      FD_ZERO(&fdwrite);
      FD_ZERO(&fdexcep);

      //TODO - do I need to mutex
      /* get file descriptors from the transfers */
      (void) curl_multi_fdset(pServer->transport.http.multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);
      DEBUG(SWITCH_CHANNEL_LOG, "maxfd=%d\n", maxfd);

      if(maxfd == -1) {
        switch_sleep(1000000);
        rc = 0;
      } else {
        rc = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout);
      }

      switch(rc) {
      case -1:
        DEBUG(SWITCH_CHANNEL_LOG, "select returned an error\n");
        break;
      case 0:
      default:
        /* timeout or readable/writable sockets */
        DEBUG(SWITCH_CHANNEL_LOG, "perform start\n");
        (void) curl_multi_perform(pServer->transport.http.multi_handle, &still_running);
        DEBUG(SWITCH_CHANNEL_LOG, "perform done\n");
      }
    }

    CURLMsg *pMsg; /* for picking up messages with the transfer status */

    do {
      int msgq =0;
      DEBUG(SWITCH_CHANNEL_LOG, "A\n");

      pMsg = curl_multi_info_read(pServer->transport.http.multi_handle, &msgq);
      DEBUG(SWITCH_CHANNEL_LOG, "B\n");

      if (pMsg && (pMsg->msg == CURLMSG_DONE)) {
        DEBUG(SWITCH_CHANNEL_LOG, "C\n");

        CURL *pCurl = pMsg->easy_handle;
        long httpRes = 0;

        switch_curl_easy_getinfo(pCurl, CURLINFO_RESPONSE_CODE, &httpRes);

        if ((pMsg->data.result == CURLE_OK) && (httpRes == 200)) {
          switch_buffer_t *pBody = NULL;
          const char *pBodyStr;

          switch_buffer_create_dynamic(&pBody, INITIAL_BODY_SIZE, INITIAL_BODY_SIZE, 0);

          switch_curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, write_callback);
          switch_curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, (void *) pBody);


          // terminate the string
          (void) switch_buffer_write(pBody, "\0", 1);

          (void) switch_buffer_peek_zerocopy(pBody, (const void **) &pBodyStr);

          DEBUG(SWITCH_CHANNEL_LOG, "code=%ld result=%s\n", httpRes, pBodyStr);

          //TODO_ called process should call: pJsonResponse = ks_json_parse(pBodyStr);
          //callback(pServer, pBodyStr);

          switch_buffer_destroy(&pBody);

        } else {
          // nothing downloaded or download interrupted
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Received curl error %d HTTP error code %ld trying to fetch %s\n", pMsg->data.result, httpRes, "pUrlTODO");
        }

        curl_multi_remove_handle(pServer->transport.http.multi_handle, pCurl);
        curl_easy_cleanup(pCurl);
      }
    } while (pMsg);
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Thread finished\n");

  (void) curl_multi_cleanup(pServer->transport.http.multi_handle);
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
