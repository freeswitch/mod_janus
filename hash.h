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
 * hash.h -- Hash headers for janus endpoint module
 *
 * Thin wrapper that takes a key as a uin64_t and translates it
 * to a string that is acceptable by the standard hash functions
 *
 */
#ifndef _HASH_H_
#define _HASH_H_

#include  "switch.h"
#include  "globals.h"

typedef struct {
  switch_mutex_t *pMutex;
  switch_hash_t *pTable;
} hash_t;

switch_status_t hashCreate(hash_t *pHash, switch_memory_pool_t *pPool);
switch_status_t hashInsert(const hash_t *pHash, const janus_id_t id, const  void *pData);
void *hashFind(const hash_t *pHash, const janus_id_t id);
switch_status_t hashDelete(const hash_t *pHash, const janus_id_t id);
void *hashIterate(hash_t *pHash, switch_hash_index_t **pIndex);
switch_status_t hashDestroy(hash_t *pHash);

#endif //_HASH_H_
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
