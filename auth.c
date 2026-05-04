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
 * Contributor(s):
 *
 *
 * auth.c -- HMAC-SHA1 signed-token generation for Janus signed-mode auth
 *
 * This mirrors the signing side of janus-gateway's src/auth.c: the resulting
 * token is of the form `<expiry>,janus[,desc...]:<base64(HMAC-SHA1(data,key))>`
 * and can be validated by Janus core (token_auth=true, token_auth_secret=<key>)
 * and by plugins that enforce signed_tokens on a per-room basis (VideoRoom
 * and, from PR #3635 onwards, AudioBridge).
 */

#include  <string.h>
#include  <stdlib.h>
#include  <stdio.h>
#include  <time.h>

#include  <openssl/hmac.h>
#include  <openssl/evp.h>

#include  "switch.h"
#include  "globals.h"
#include  "auth.h"

#define AUTH_REALM "janus"

char *authSignToken(const char *pSecret, int ttlSeconds,
		const char *const *ppDescriptors, int ndesc) {
	char *pData = NULL;
	char *pB64 = NULL;
	char *pToken = NULL;
	unsigned char sig[EVP_MAX_MD_SIZE];
	unsigned int sigLen = 0;
	size_t dataCap;
	size_t dataLen;
	int b64Len;
	int i;

	if (!pSecret || !*pSecret || ttlSeconds <= 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"authSignToken: invalid arguments (secret=%s ttl=%d)\n",
				pSecret ? "set" : "null", ttlSeconds);
		return NULL;
	}

	/*
	 * Data part: "<expiry>,janus[,desc1,desc2...]"
	 * Reserve enough room for a 20-digit epoch + realm + each descriptor
	 * plus separators and a trailing NUL.
	 */
	dataCap = 21 /* expiry */ + 1 + strlen(AUTH_REALM);
	for (i = 0; i < ndesc; i++) {
		if (ppDescriptors && ppDescriptors[i]) {
			dataCap += 1 /* comma */ + strlen(ppDescriptors[i]);
		}
	}
	dataCap += 1; /* NUL */

	pData = malloc(dataCap);
	if (!pData) {
		goto fail;
	}

	{
		long long expiry = (long long) time(NULL) + (long long) ttlSeconds;
		int written = snprintf(pData, dataCap, "%lld,%s", expiry, AUTH_REALM);
		if (written < 0 || (size_t) written >= dataCap) {
			goto fail;
		}
		dataLen = (size_t) written;
	}

	for (i = 0; i < ndesc; i++) {
		if (!ppDescriptors || !ppDescriptors[i] || !*ppDescriptors[i]) {
			continue;
		}
		int written = snprintf(pData + dataLen, dataCap - dataLen, ",%s",
				ppDescriptors[i]);
		if (written < 0 || (size_t) written >= dataCap - dataLen) {
			goto fail;
		}
		dataLen += (size_t) written;
	}

	/* HMAC-SHA1 over the data part, keyed with the shared secret. */
	if (!HMAC(EVP_sha1(), pSecret, (int) strlen(pSecret),
			(const unsigned char *) pData, dataLen,
			sig, &sigLen) || sigLen == 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"authSignToken: HMAC-SHA1 failed\n");
		goto fail;
	}

	/*
	 * Standard base64 (RFC 4648) without line wrapping, with '=' padding.
	 * EVP_EncodeBlock writes this format and NUL-terminates the output;
	 * its return value is the number of bytes written excluding the NUL.
	 */
	{
		size_t b64Cap = 4 * ((sigLen + 2) / 3) + 1;
		pB64 = malloc(b64Cap);
		if (!pB64) {
			goto fail;
		}
		b64Len = EVP_EncodeBlock((unsigned char *) pB64, sig, (int) sigLen);
		if (b64Len <= 0) {
			goto fail;
		}
		pB64[b64Len] = '\0';
	}

	{
		size_t total = dataLen + 1 /* ':' */ + (size_t) b64Len + 1 /* NUL */;
		pToken = malloc(total);
		if (!pToken) {
			goto fail;
		}
		int written = snprintf(pToken, total, "%s:%s", pData, pB64);
		if (written < 0 || (size_t) written >= total) {
			goto fail;
		}
	}

	free(pData);
	free(pB64);
	return pToken;

fail:
	free(pData);
	free(pB64);
	free(pToken);
	return NULL;
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
