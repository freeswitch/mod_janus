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
 * auth.h -- HMAC-SHA1 signed-token generation for Janus signed-mode auth
 *
 * Produces tokens in the exact format Janus core validates in
 * janus_auth_check_signature / janus_auth_check_signature_contains:
 *
 *     <expiry>,janus,<descriptor1>[,<descriptor2>...]:<base64(HMAC-SHA1(data, secret))>
 *
 * The caller is responsible for providing the HMAC secret (the same value
 * as Janus core's `token_auth_secret`), the token TTL, and the list of
 * descriptors that must be embedded in the token (e.g. the plugin package
 * "janus.plugin.audiobridge", optionally followed by "room=<id>" for the
 * AudioBridge/VideoRoom per-room signed_tokens enforcement).
 */

#ifndef _AUTH_H_
#define _AUTH_H_

#include  "switch.h"

/*! \brief Generate a Janus HMAC-SHA1 signed token.
 *
 * \param pSecret       NUL-terminated HMAC key (must match Janus core's
 *                      token_auth_secret). Must be non-NULL and non-empty.
 * \param ttlSeconds    Token lifetime in seconds from now. Must be > 0.
 * \param ppDescriptors Array of NUL-terminated descriptor strings that will
 *                      be embedded in the token data part (e.g.
 *                      "janus.plugin.audiobridge", "room=1234"). May be NULL
 *                      if ndesc == 0.
 * \param ndesc         Number of entries in ppDescriptors.
 *
 * \returns A malloc'd NUL-terminated token string on success, or NULL on
 *          failure. Caller must free() the returned pointer.
 */
char *authSignToken(const char *pSecret, int ttlSeconds,
		const char *const *ppDescriptors, int ndesc);

#endif /* _AUTH_H_ */
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
