/*
 * relocate.c: do wc repos relocation
 *
 * ====================================================================
 * Copyright (c) 2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#include "svn_wc.h"
#include "svn_error.h"
#include "svn_string.h"
#include "svn_xml.h"
#include "svn_pools.h"
#include "svn_path.h"
#include "svn_io.h"

#include "wc.h"
#include "entries.h"
#include "props.h"


svn_error_t *
svn_wc_relocate (const char *path,
                 svn_wc_adm_access_t *adm_access,
                 const char *from,
                 const char *to,
                 svn_boolean_t recurse,
                 void *validator_baton,
                 svn_wc_relocation_validator *validator,
                 apr_pool_t *pool)
{
  svn_node_kind_t kind;
  apr_hash_t *entries = NULL;
  apr_hash_index_t *hi;
  svn_boolean_t is_file = FALSE;
  char *base;
  int from_len;

  SVN_ERR(svn_io_check_path(path, &kind, pool));

  if (kind == svn_node_file)
    {
      base = svn_path_basename(path, pool);
      is_file = TRUE;
    }

  from_len = strlen(from);

  SVN_ERR(svn_wc_entries_read(&entries, adm_access, FALSE, pool));

  if (is_file)
    {
      svn_wc_entry_t *entry = apr_hash_get(entries, base, APR_HASH_KEY_STRING);
      if (!entry)
        return svn_error_create(SVN_ERR_ENTRY_NOT_FOUND, NULL,
                                "missing entry");

      if (!entry->url)
        return svn_error_createf(SVN_ERR_ENTRY_MISSING_URL, NULL,
                                 "entry '%s' has no URL", path);

      if (!strncmp(entry->url, from, from_len))
        {
          char *url = apr_psprintf(svn_wc_adm_access_pool(adm_access),
                                   "%s%s", to, entry->url + from_len);
          SVN_ERR(validator(validator_baton, entry->uuid, url));
          entry->url = url;
          SVN_ERR(svn_wc__entries_write (entries, adm_access, pool));
        }

      return SVN_NO_ERROR;
    }

  for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      void *val;
      svn_wc_entry_t *entry;

      apr_hash_this(hi, &key, NULL, &val);
      entry = val;
 
      if (recurse
          && (entry->kind == svn_node_dir)
          && (strcmp(key, SVN_WC_ENTRY_THIS_DIR) != 0))
        {
          svn_wc_adm_access_t *subdir_access;
          const char *subdir = svn_path_join (path, key, pool);
          SVN_ERR(svn_wc_adm_retrieve(&subdir_access, adm_access, subdir,
                                      pool));
          SVN_ERR(svn_wc_relocate(subdir, subdir_access, from, to,
                                  recurse, validator_baton, validator, pool));
        }

      if (entry->url &&
          (strncmp(entry->url, from, from_len) == 0)) 
        {
          char *url = apr_psprintf(svn_wc_adm_access_pool(adm_access),
                                   "%s%s", to, entry->url + from_len);
          SVN_ERR(validator(validator_baton, entry->uuid, url));
          entry->url = url;
        }
    }

  SVN_ERR(svn_wc__remove_wcprops (adm_access, FALSE, pool));
  SVN_ERR(svn_wc__entries_write (entries, adm_access, pool));
  return SVN_NO_ERROR;
}

