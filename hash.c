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
 * hash.c -- Hash functions for janus endpoint module
 *
 * Thin wrapper that takes a key as a uin64_t and translates it
 * to a string that is acceptable by the standard hash functions
 *
 */
#include  "globals.h"
#include  "hash.h"

// calling process must free the returned value
static char *idToStr(const janus_id_t id) {
  char *pResult = switch_mprintf("%" SWITCH_UINT64_T_FMT, id);
  if (pResult == NULL) {
    fprintf(stderr,"ABORT! Realloc failure at: %s:%d", __FILE__, __LINE__);
    abort();
  }
  return pResult;
}

switch_status_t hashCreate(hash_t *pHash, switch_memory_pool_t *pPool) {
  switch_status_t status;

  switch_assert(pHash);
  switch_assert(pPool);

  status = switch_mutex_init(&pHash->pMutex, SWITCH_MUTEX_NESTED, pPool);
  if (status == SWITCH_STATUS_SUCCESS) {
    status = switch_core_hash_init(&pHash->pTable);
    if (status != SWITCH_STATUS_SUCCESS) {
      (void) switch_mutex_destroy(pHash->pMutex);
    }
  }

  return status;
}

switch_status_t hashInsert(const hash_t *pHash, const janus_id_t id, const  void * const pData) {
  switch_status_t status;
  char *pIdStr = idToStr(id);

  switch_assert(pHash);
  switch_assert(pData);

  DEBUG(SWITCH_CHANNEL_LOG, "Insert id=%" SWITCH_UINT64_T_FMT "\n", id);

  status = switch_core_hash_insert_locked(pHash->pTable, pIdStr, pData, pHash->pMutex);
  switch_safe_free(pIdStr);

  return status;
}

void *hashFind(const hash_t *pHash, const janus_id_t id) {
  void *pResult;
  char *pIdStr = idToStr(id);

  switch_assert(pHash);

  DEBUG(SWITCH_CHANNEL_LOG, "Find id=%" SWITCH_UINT64_T_FMT "\n", id);

  pResult = switch_core_hash_find_locked(pHash->pTable, pIdStr, pHash->pMutex);
  switch_safe_free(pIdStr);

  return pResult;
}

switch_status_t hashDelete(const hash_t *pHash, const janus_id_t id) {
  void *pResult;
  char *pIdStr = idToStr(id);

  switch_assert(pHash);

  DEBUG(SWITCH_CHANNEL_LOG, "Delete id=%" SWITCH_UINT64_T_FMT "\n", id);

  pResult = switch_core_hash_delete_locked(pHash->pTable, pIdStr, pHash->pMutex);
  switch_safe_free(pIdStr);

  return pResult ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

void *hashIterate(hash_t *pHash, switch_hash_index_t **pIndex) {
	void *pVal = NULL;
	const void *pVar;

  switch_assert(pHash);

  if (!*pIndex) {
    *pIndex = switch_core_hash_first(pHash->pTable);
  } else {
    *pIndex = switch_core_hash_next(pIndex);
  }

  if (*pIndex) {
    switch_core_hash_this(*pIndex, &pVar, NULL, &pVal);
  }

  return pVal;
}

switch_status_t hashDestroy(hash_t *pHash) {
  switch_status_t status;

  status = switch_core_hash_destroy(&pHash->pTable);
  status = switch_mutex_destroy(pHash->pMutex) | status;
  pHash->pMutex = NULL;

  return status;
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
