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
 * http.h -- HTTP headers for janus endpoint module
 *
 */
#ifndef _HTTP_H_
#define _HTTP_H_

#include  "cJSON.h"
#include  "switch.h"

/* Return SWITCH_TRUE to abort an in-flight curl transfer (e.g. module shutdown). */
typedef switch_bool_t (*http_abort_fn_t)(void *userdata);

cJSON *httpPost(const char *url, const unsigned int timeout, cJSON *pJsonRequest,
	http_abort_fn_t abort_fn, void *abort_data);
cJSON *httpGet(const char *url, const unsigned int timeout,
	http_abort_fn_t abort_fn, void *abort_data);

#endif //_HTTP_H_
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
