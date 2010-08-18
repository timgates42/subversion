/* temp_serializer.c: serialization functions for caching of FSFS structures
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <apr_pools.h>

#include "svn_pools.h"
#include "svn_hash.h"

#include "id.h"
#include "svn_fs.h"

#include "private/svn_fs_util.h"
#include "private/svn_temp_serializer.h"

#include "temp_serializer.h"

/* Utility to encode a signed NUMBER into a variable-length sequence of 
 * 8-bit chars in KEY_BUFFER and return the last writen position.
 *
 * Numbers will be stored in 7 bits / byte and using byte values above 
 * 32 (' ') to make them combinable with other string by simply separating 
 * individual parts with spaces.
 */
static char*
encode_number(apr_int64_t number, char *key_buffer)
{
  /* encode the sign in the first byte */
  if (number < 0)
  {
    number = -number;
    *key_buffer = (number & 63) + ' ' + 65;
  }
  else
    *key_buffer = (number & 63) + ' ' + 1;
  number /= 64;

  /* write 7 bits / byte until no significant bits are left */
  while (number)
  {
    *++key_buffer = (number & 127) + ' ' + 1;
    number /= 128;
  }

  /* return the last written position */
  return key_buffer;
}

/* Prepend the NUMBER to the STRING in a space efficient way that no other
 * (number,string) combination can produce the same result. 
 * Allocate temporaries as well as the result from POOL.
 */
const char*
svn_fs_fs__combine_number_and_string(apr_int64_t number,
                                     const char *string,
                                     apr_pool_t *pool)
{
  apr_size_t len = strlen(string);

  /* number part requires max. 10x7 bits + 1 space. 
   * Add another 1 for the terminal 0 */
  char *key_buffer = apr_palloc(pool, len + 12);
  const char *key = key_buffer;

  /* Prepend the number to the string and separate them by space. No other 
   * number can result in the same prefix, no other string in the same 
   * postfix nor can the boundary between them be ambiguous. */
  key_buffer = encode_number(number, key_buffer);
  *++key_buffer = ' ';
  memcpy(++key_buffer, string, len+1);

  /* return the start of the key */
  return key;
}

/* Combine the numbers A and B a space efficient way that no other
 * combination of numbers can produce the same result.
 * Allocate temporaries as well as the result from POOL.
 */
const char*
svn_fs_fs__combine_two_numbers(apr_int64_t a,
                               apr_int64_t b,
                               apr_pool_t *pool)
{
  /* encode numbers as 2x 10x7 bits + 1 space + 1 terminating \0*/
  char *key_buffer = apr_palloc(pool, 22);
  const char *key = key_buffer;

  /* combine the numbers. Since the separator is disjoint from any part
   * of the encoded numbers, there is no other combination that can yield
   * the same result */
  key_buffer = encode_number(a, key_buffer);
  *++key_buffer = ' ';
  key_buffer = encode_number(b, ++key_buffer);
  *++key_buffer = '\0';

  /* return the start of the key */
  return key;
}

/* Utility function to serialize string S in the given serialization CONTEXT.
 */
static void
serialize_svn_string(svn_temp_serializer__context_t *context,
                     const svn_string_t * const *s)
{
  const svn_string_t *string = *s;

  /* Nothing to do for NULL string references. */
  if (string == NULL)
    return;

  svn_temp_serializer__push(context,
                            (const void * const *)s,
                            sizeof(*string));

  /* the "string" content may actually be arbitrary binary data.
   * Thus, we cannot use svn_temp_serializer__add_string. */
  svn_temp_serializer__push(context,
                            (const void * const *)&string->data,
                            string->len);

  /* back to the caller's nesting level */
  svn_temp_serializer__pop(context);
  svn_temp_serializer__pop(context);
}

/* Utility function to deserialize the STRING inside the BUFFER.
 */
static void
deserialize_svn_string(void *buffer, svn_string_t **string)
{
  if (*string == NULL)
    return;

  svn_temp_deserializer__resolve(buffer, (void **)string);
  svn_temp_deserializer__resolve(*string, (void **)&(*string)->data);
}

/* Utility function to serialize checkum CS within the given serialization
 * CONTEXT.
 */
static void
serialize_checksum(svn_temp_serializer__context_t *context,
                   svn_checksum_t * const *cs)
{
  const svn_checksum_t *checksum = *cs;
  if (checksum == NULL)
    return;

  svn_temp_serializer__push(context,
                            (const void * const *)cs,
                            sizeof(*checksum));

  /* The digest is arbitrary binary data.
   * Thus, we cannot use svn_temp_serializer__add_string. */
  svn_temp_serializer__push(context,
                            (const void * const *)&checksum->digest,
                            svn_checksum_size(checksum));

  /* return to the caller's nesting level */
  svn_temp_serializer__pop(context);
  svn_temp_serializer__pop(context);
}

/* Utility function to deserialize the checksum CS inside the BUFFER.
 */
static void
deserialize_checksum(void *buffer, svn_checksum_t * const *cs)
{
  if (*cs == NULL)
    return;

  svn_temp_deserializer__resolve(buffer, (void **)cs);
  svn_temp_deserializer__resolve(*cs, (void **)&(*cs)->digest);
}

/* Utility function to serialize the REPRESENTATION within the given
 * serialization CONTEXT.
 */
static void
serialize_representation(svn_temp_serializer__context_t *context,
                         representation_t * const *representation)
{
  const representation_t * rep = *representation;
  if (rep == NULL)
    return;

  /* serialize the representation struct itself */
  svn_temp_serializer__push(context,
                            (const void * const *)representation,
                            sizeof(*rep));

  /* serialize sub-structures */
  serialize_checksum(context, &rep->md5_checksum);
  serialize_checksum(context, &rep->sha1_checksum);

  svn_temp_serializer__add_string(context, &rep->txn_id);
  svn_temp_serializer__add_string(context, &rep->uniquifier);

  /* return to the caller's nesting level */
  svn_temp_serializer__pop(context);
}

/* Utility function to deserialize the REPRESENTATIONS inside the BUFFER.
 */
static void
deserialize_representation(void *buffer,
                           representation_t **representation)
{
  representation_t *rep;
  if (*representation == NULL)
    return;

  /* fixup the reference to the representation itself */
  svn_temp_deserializer__resolve(buffer, (void **)representation);
  rep = *representation;

  /* fixup of sub-structures */
  deserialize_checksum(rep, &rep->md5_checksum);
  deserialize_checksum(rep, &rep->sha1_checksum);

  svn_temp_deserializer__resolve(rep, (void **)&rep->txn_id);
  svn_temp_deserializer__resolve(rep, (void **)&rep->uniquifier);
}

/* auxilliary structure representing the content of a directory hash */
typedef struct hash_data_t
{
  /* number of entries in the directory */
  apr_size_t count;

  /* reference to the entries */
  svn_fs_dirent_t **entries;
} hash_data_t;

int
compare_dirent_id_names(svn_fs_dirent_t **lhs, svn_fs_dirent_t **rhs)
{
  return strcmp((*lhs)->name, (*rhs)->name);
}

/* Utility function to serialize the ENTRIES into a new serialization
 * context to be returned. Allocation will be made form POOL.
 */
static svn_temp_serializer__context_t *
serialize_dir(apr_hash_t *entries, apr_pool_t *pool)
{
  hash_data_t hash_data;
  apr_hash_index_t *hi;
  apr_size_t i = 0;
  svn_temp_serializer__context_t *context;

  /* calculate sizes */
  apr_size_t count = apr_hash_count(entries);
  apr_size_t entries_len = sizeof(svn_fs_dirent_t*[count]);

  /* copy the hash entries to an auxilliary struct of known layout */
  hash_data.count = count;
  hash_data.entries = apr_palloc(pool, entries_len);

  for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi), ++i)
    hash_data.entries[i] = svn__apr_hash_index_val(hi);

  /* sort entry index by ID name */
  qsort(hash_data.entries,
        count,
        sizeof(*hash_data.entries),
        (comparison_fn_t)compare_dirent_id_names);

  /* serialize that aux. structure into a new  */
  context = svn_temp_serializer__init(&hash_data,
                                      sizeof(hash_data),
                                      50 + count * 200 + entries_len,
                                      pool);

  /* serialize entries references */
  svn_temp_serializer__push(context,
                            (const void * const *)&hash_data.entries,
                            entries_len);

  /* serialize the individual entries and their sub-structures */
  for (i = 0; i < count; ++i)
    {
      svn_fs_dirent_t **entry = &hash_data.entries[i];
      svn_temp_serializer__push(context,
                                (const void * const *)entry,
                                sizeof(svn_fs_dirent_t));

      svn_fs_fs__id_serialize(context, &(*entry)->id);
      svn_temp_serializer__add_string(context, &(*entry)->name);

      svn_temp_serializer__pop(context);
    }

  return context;
}

/* Utility function to reconstruct a dir entries hash from serialized data
 * in BUFFER and HASH_DATA. Allocation will be made form POOL.
 */
static apr_hash_t *
deserialize_dir(void *buffer, hash_data_t *hash_data, apr_pool_t *pool)
{
  apr_hash_t *result = apr_hash_make(pool);
  apr_size_t i;
  apr_size_t count;
  svn_fs_dirent_t *entry;
  svn_fs_dirent_t **entries;

  /* resolve the reference to the entries array */
  svn_temp_deserializer__resolve(buffer, (void **)&hash_data->entries);
  entries = hash_data->entries;

  /* fixup the references within each entry and add it to the hash */
  for (i = 0, count = hash_data->count; i < count; ++i)
    {
      svn_temp_deserializer__resolve(entries, (void **)&entries[i]);
      entry = hash_data->entries[i];

      /* pointer fixup */
      svn_temp_deserializer__resolve(entry, (void **)&entry->name);
      svn_fs_fs__id_deserialize(entry, (svn_fs_id_t **)&entry->id);

      /* add the entry to the hash */
      apr_hash_set(result, entry->name, APR_HASH_KEY_STRING, entry);
    }

  /* return the now complete hash */
  return result;
}

/* Serialize a NODEREV_P within the serialization CONTEXT.
 */
void
svn_fs_fs__noderev_serialize(svn_temp_serializer__context_t *context,
                             node_revision_t * const *noderev_p)
{
  const node_revision_t *noderev = *noderev_p;
  if (noderev == NULL)
    return;

  /* serialize the representation struct itself */
  svn_temp_serializer__push(context,
                            (const void * const *)noderev_p,
                            sizeof(*noderev));

  /* serialize sub-structures */
  svn_fs_fs__id_serialize(context, &noderev->id);
  svn_fs_fs__id_serialize(context, &noderev->predecessor_id);
  serialize_representation(context, &noderev->prop_rep);
  serialize_representation(context, &noderev->data_rep);

  svn_temp_serializer__add_string(context, &noderev->copyfrom_path);
  svn_temp_serializer__add_string(context, &noderev->copyroot_path);
  svn_temp_serializer__add_string(context, &noderev->created_path);

  /* return to the caller's nesting level */
  svn_temp_serializer__pop(context);
}


/* Deserialize a NODEREV_P within the BUFFER.
 */
void
svn_fs_fs__noderev_deserialize(void *buffer,
                               node_revision_t **noderev_p)
{
  node_revision_t *noderev;
  if (*noderev_p == NULL)
    return;

  /* fixup the reference to the representation itself,
   * if this is part of a parent structure. */
  if (buffer != *noderev_p)
    svn_temp_deserializer__resolve(buffer, (void **)noderev_p);

  noderev = *noderev_p;

  /* fixup of sub-structures */
  svn_fs_fs__id_deserialize(noderev, (svn_fs_id_t **)&noderev->id);
  svn_fs_fs__id_deserialize(noderev, (svn_fs_id_t **)&noderev->predecessor_id);
  deserialize_representation(noderev, &noderev->prop_rep);
  deserialize_representation(noderev, &noderev->data_rep);

  svn_temp_deserializer__resolve(noderev, (void **)&noderev->copyfrom_path);
  svn_temp_deserializer__resolve(noderev, (void **)&noderev->copyroot_path);
  svn_temp_deserializer__resolve(noderev, (void **)&noderev->created_path);
}


/* Utility function to serialize COUNT svn_txdelta_op_t objects 
 * at OPS in the given serialization CONTEXT.
 */
static void
serialize_txdelta_ops(svn_temp_serializer__context_t *context,
                      const svn_txdelta_op_t * const * ops,
                      apr_size_t count)
{
  if (*ops == NULL)
    return;

  /* the ops form a simple chunk of memory with no further references */
  svn_temp_serializer__push(context,
                            (const void * const *)ops,
                            sizeof(svn_txdelta_op_t[count]));
  svn_temp_serializer__pop(context);
}

/* Utility function to serialize W in the given serialization CONTEXT.
 */
static void
serialize_txdeltawindow(svn_temp_serializer__context_t *context,
                        svn_txdelta_window_t * const * w)
{
  svn_txdelta_window_t *window = *w;

  /* serialize the window struct itself */
  svn_temp_serializer__push(context,
                            (const void * const *)w,
                            sizeof(svn_txdelta_window_t));

  /* serialize its sub-structures */
  serialize_txdelta_ops(context, &window->ops, window->num_ops);
  serialize_svn_string(context, &window->new_data);

  svn_temp_serializer__pop(context);
}

/* Implements svn_cache__serialize_fn_t for svn_fs_fs__txdelta_cached_window_t
 */
svn_error_t *
svn_fs_fs__serialize_txdelta_window(char **buffer,
                                    apr_size_t *buffer_size,
                                    void *item,
                                    apr_pool_t *pool)
{
  svn_fs_fs__txdelta_cached_window_t *window_info = item;
  svn_stringbuf_t *serialized;

  /* initialize the serialization process and allocate a buffer large
   * enough to do without the need of re-allocations in most cases. */
  apr_size_t text_len = window_info->window->new_data
                      ? window_info->window->new_data->len
                      : 0;
  svn_temp_serializer__context_t *context =
      svn_temp_serializer__init(window_info,
                                sizeof(*window_info),
                                500 + text_len,
                                pool);

  /* serialize the sub-structure(s) */
  serialize_txdeltawindow(context, &window_info->window);

  /* return the serialized result */
  serialized = svn_temp_serializer__get(context);

  *buffer = serialized->data;
  *buffer_size = serialized->len;

  return SVN_NO_ERROR;
}

/* Implements svn_cache__deserialize_fn_t for
 * svn_fs_fs__txdelta_cached_window_t.
 */
svn_error_t *
svn_fs_fs__deserialize_txdelta_window(void **item,
                                      const char *buffer,
                                      apr_size_t buffer_size,
                                      apr_pool_t *pool)
{
  svn_txdelta_window_t *window;

  /* Copy the _full_ buffer as it also contains the sub-structures. */
  svn_fs_fs__txdelta_cached_window_t *window_info =
      apr_palloc(pool, buffer_size);

  memcpy(window_info, buffer, buffer_size);

  /* pointer reference fixup */
  svn_temp_deserializer__resolve(window_info,
                                 (void **)&window_info->window);
  window = window_info->window;

  svn_temp_deserializer__resolve(window, (void **)&window->ops);

  deserialize_svn_string(window, (svn_string_t**)&window->new_data);

  /* done */
  *item = window_info;

  return SVN_NO_ERROR;
}

/* Implements svn_cache__serialize_fn_t for manifests.
 */
svn_error_t *
svn_fs_fs__serialize_manifest(char **data,
                              apr_size_t *data_len,
                              void *in,
                              apr_pool_t *pool)
{
  apr_array_header_t *manifest = in;

  *data_len = sizeof(apr_off_t) *manifest->nelts;
  *data = apr_palloc(pool, *data_len);
  memcpy(*data, manifest->elts, *data_len);

  return SVN_NO_ERROR;
}

/* Implements svn_cache__deserialize_fn_t for manifests.
 */
svn_error_t *
svn_fs_fs__deserialize_manifest(void **out,
                                const char *data,
                                apr_size_t data_len,
                                apr_pool_t *pool)
{
  apr_array_header_t *manifest = apr_array_make(pool,
                                                data_len / sizeof(apr_off_t),
                                                sizeof(apr_off_t));
  memcpy(manifest->elts, data, data_len);
  manifest->nelts = data_len / sizeof(apr_off_t);
  *out = manifest;

  return SVN_NO_ERROR;
}

/* Implements svn_cache__serialize_fn_t for svn_fs_id_t
 */
svn_error_t *
svn_fs_fs__serialize_id(char **data,
                        apr_size_t *data_len,
                        void *in,
                        apr_pool_t *pool)
{
  const svn_fs_id_t *id = in;
  svn_stringbuf_t *serialized;

  /* create an (empty) serialization context with plenty of buffer space */
  svn_temp_serializer__context_t *context =
      svn_temp_serializer__init(NULL, 0, 250, pool);

  /* serialize the id */
  svn_fs_fs__id_serialize(context, &id);

  /* return serialized data */
  serialized = svn_temp_serializer__get(context);
  *data = serialized->data;
  *data_len = serialized->len;

  return SVN_NO_ERROR;
}

/* Implements svn_cache__deserialize_fn_t for svn_fs_id_t
 */
svn_error_t *
svn_fs_fs__deserialize_id(void **out,
                          const char *data,
                          apr_size_t data_len,
                          apr_pool_t *pool)
{
  /* Copy the _full_ buffer as it also contains the sub-structures. */
  svn_fs_id_t *id = apr_palloc(pool, data_len);
  memcpy(id, data, data_len);

  /* fixup of all pointers etc. */
  svn_fs_fs__id_deserialize(id, &id);

  /* done */
  *out = id;
  return SVN_NO_ERROR;
}

/** Caching node_revision_t objects. **/

/* Implements vn_cache__serialize_fn_t for node_revision_t.
 */
svn_error_t *
svn_fs_fs__serialize_node_revision(char **buffer,
                                    apr_size_t *buffer_size,
                                    void *item,
                                    apr_pool_t *pool)
{
  svn_stringbuf_t *serialized;
  node_revision_t *noderev = item;

  /* create an (empty) serialization context with plenty of buffer space */
  svn_temp_serializer__context_t *context =
      svn_temp_serializer__init(NULL, 0, 503, pool);

  /* serialize the noderev */
  svn_fs_fs__noderev_serialize(context, &noderev);

  /* return serialized data */
  serialized = svn_temp_serializer__get(context);
  *buffer = serialized->data;
  *buffer_size = serialized->len;

  return SVN_NO_ERROR;
}

/* Implements svn_cache__deserialize_fn_t for node_revision_t
 */
svn_error_t *
svn_fs_fs__deserialize_node_revision(void **item,
                                     const char *buffer,
                                     apr_size_t buffer_size,
                                     apr_pool_t *pool)
{
  /* Copy the _full_ buffer as it also contains the sub-structures. */
  node_revision_t *noderev = apr_palloc(pool, buffer_size);
  memcpy(noderev, buffer, buffer_size);

  /* fixup of all pointers etc. */
  svn_fs_fs__noderev_deserialize(noderev, &noderev);

  /* done */
  *item = noderev;
  return SVN_NO_ERROR;
}

/* Implements svn_cache__serialize_func_t for a directory contents hash.
 */
svn_error_t *
svn_fs_fs__serialize_dir_entries(char **data,
                                 apr_size_t *data_len,
                                 void *in,
                                 apr_pool_t *pool)
{
  apr_hash_t *dir = in;

  /* serialize the dir content into a new serialization context */
  svn_temp_serializer__context_t *context = serialize_dir(dir, pool);

  /* return serialized data */
  svn_stringbuf_t *serialized = svn_temp_serializer__get(context);
  *data = serialized->data;
  *data_len = serialized->len;

  return SVN_NO_ERROR;
}

/* Implements svn_cache__deserialize_func_t for a directory contents hash
 */
svn_error_t *
svn_fs_fs__deserialize_dir_entries(void **out,
                                   const char *data,
                                   apr_size_t data_len,
                                   apr_pool_t *pool)
{
  /* Copy the _full_ buffer as it also contains the sub-structures. */
  hash_data_t *hash_data = apr_palloc(pool, data_len);
  memcpy(hash_data, data, data_len);

  /* reconstruct the hash from the serialized data */
  *out = deserialize_dir(hash_data, hash_data, pool);

  return SVN_NO_ERROR;
}

/* Implements svn_cache__partial_getter_func_t for manifests.
 */
svn_error_t *
svn_fs_fs__get_sharded_offset(void **out,
                              const char *data,
                              apr_size_t data_len,
                              void *baton,
                              apr_pool_t *pool)
{
  apr_off_t *manifest = (apr_off_t *)data;
  apr_int64_t shard_pos = *(apr_int64_t *)baton;

  *(apr_int64_t *)out = manifest[shard_pos];

  return SVN_NO_ERROR;
}

/* Implements svn_cache__partial_getter_func_t for a directory contents hash.
 */
svn_error_t *
svn_fs_fs__extract_dir_entry(void **out,
                             const char *data,
                             apr_size_t data_len,
                             void *baton,
                             apr_pool_t *pool)
{
  hash_data_t *hash_data = (hash_data_t *)data;
  const char* name = baton;

  /* resolve the reference to the entries array */
  const svn_fs_dirent_t * const *entries =
      svn_temp_deserializer__ptr(data, (const void **)&hash_data->entries);

  /* binary search for the desired entry by name */
  apr_size_t lower = 0;
  apr_size_t upper = hash_data->count;
  apr_size_t middle;

  for (middle = upper / 2; lower < upper; middle = (upper + lower) / 2)
    {
      const svn_fs_dirent_t *entry =
          svn_temp_deserializer__ptr(entries, (const void **)&entries[middle]);
      const char* entry_name =
          svn_temp_deserializer__ptr(entry, (const void **)&entry->name);

      int diff = strcmp(entry_name, name);
      if (diff < 0)
        lower = middle + 1;
      else
        upper = middle;
    }

  /* de-serialize that entry or return NULL, if no match has been found */
  *out = NULL;
  if (lower < hash_data->count)
    {
      const svn_fs_dirent_t *source =
          svn_temp_deserializer__ptr(entries, (const void **)&entries[lower]);

      /* Entries have been serialized one-by-one, each time including all
       * nestes structures and strings. Therefore, they occupy a single
       * block of memory whose end-offset is either the beginning of the
       * next entry or the end of the buffer
       */
      apr_size_t end_offset = lower + 1 < hash_data->count
                            ? ((apr_size_t*)entries)[lower+1]
                            : data_len;
      apr_size_t size = end_offset - ((apr_size_t*)entries)[lower];

      /* copy & deserialize the entry */
      svn_fs_dirent_t *new_entry = apr_palloc(pool, size);
      memcpy(new_entry, source, size);

      svn_temp_deserializer__resolve(new_entry, (void **)&new_entry->name);
      if (strcmp(new_entry->name, name) == 0)
        {
          svn_fs_fs__id_deserialize(new_entry, (svn_fs_id_t **)&new_entry->id);
          *(svn_fs_dirent_t **)out = new_entry;
        }
    }

  return SVN_NO_ERROR;
}



