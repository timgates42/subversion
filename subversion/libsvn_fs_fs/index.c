/* index.c indexing support for FSFS support
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

#include <assert.h>

#include "svn_io.h"
#include "svn_pools.h"
#include "svn_sorts.h"

#include "svn_private_config.h"

#include "private/svn_subr_private.h"
#include "private/svn_temp_serializer.h"

#include "index.h"
#include "pack.h"
#include "temp_serializer.h"
#include "util.h"
#include "fs_fs.h"

#include "../libsvn_fs/fs-loader.h"

/* maximum length of a uint64 in an 7/8b encoding */
#define ENCODED_INT_LENGTH 10

/* Page tables in the log-to-phys index file exclusively contain entries
 * of this type to describe position and size of a given page.
 */
typedef struct l2p_page_table_entry_t
{
  /* global offset on the page within the index file */
  apr_uint64_t offset;

  /* number of mapping entries in that page */
  apr_uint32_t entry_count;

  /* size of the page on disk (in the index file) */
  apr_uint32_t size;
} l2p_page_table_entry_t;

/* Master run-time data structure of an log-to-phys index.  It contains
 * the page tables of every revision covered by that index - but not the
 * pages themselves. 
 */
typedef struct l2p_header_t
{
  /* first revision covered by this index */
  svn_revnum_t first_revision;

  /* number of revisions covered */
  apr_size_t revision_count;

  /* (max) number of entries per page */
  apr_size_t page_size;

  /* indexes into PAGE_TABLE that mark the first page of the respective
   * revision.  PAGE_TABLE_INDEX[REVISION_COUNT] points to the end of
   * PAGE_TABLE.
   */
  apr_size_t * page_table_index;

  /* Page table covering all pages in the index */
  l2p_page_table_entry_t * page_table;
} l2p_header_t;

/* Run-time data structure containing a single log-to-phys index page.
 */
typedef struct l2p_page_t
{
  /* number of entries in the OFFSETS array */
  apr_uint32_t entry_count;

  /* global file offsets (item index is the array index) within the
   * packed or non-packed rev file.  Offset will be -1 for unused /
   * invalid item index values. */
  apr_uint64_t *offsets;
} l2p_page_t;

/* All of the log-to-phys proto index file consist of entires of this type.
 */
typedef struct l2p_proto_entry_t
{
  /* phys offset + 1 of the data container. 0 for "new revision" entries. */
  apr_uint64_t offset;

  /* corresponding item index. 0 for "new revision" entries. */
  apr_uint64_t item_index;
} l2p_proto_entry_t;

/* Master run-time data structure of an phys-to-log index.  It contains
 * an array with one offset value for each rev file cluster.
 */
typedef struct p2l_header_t
{
  /* first revision covered by the index (and rev file) */
  svn_revnum_t first_revision;

  /* number of bytes in the rev files covered by each p2l page */
  apr_uint64_t page_size;

  /* number of pages / clusters in that rev file */
  apr_size_t page_count;

  /* number of bytes in the rev file */
  apr_uint64_t file_size;

  /* offsets of the pages / cluster descriptions within the index file */
  apr_off_t *offsets;
} p2l_header_t;

/*
 * packed stream
 *
 * This is a utility object that will read files containing 7b/8b encoded
 * unsigned integers.  It decodes them in batches to minimize overhead
 * and supports random access to random file locations.
 */

/* How many numbers we will pre-fetch and buffer in a packed number stream.
 */
enum { MAX_NUMBER_PREFETCH = 64 };

/* Prefetched number entry in a packed number stream.
 */
typedef struct value_position_pair_t
{
  /* prefetched number */
  apr_uint64_t value;

  /* number of bytes read, *including* this number, since the buffer start */
  apr_size_t total_len;
} value_position_pair_t;

/* State of a prefetching packed number stream.  It will read compressed
 * index data efficiently and present it as a series of non-packed uint64.
 */
struct svn_fs_fs__packed_number_stream_t
{
  /* underlying data file containing the packed values */
  apr_file_t *file;

  /* number of used entries in BUFFER (starting at index 0) */
  apr_size_t used;

  /* index of the next number to read from the BUFFER (0 .. USED).
   * If CURRENT == USED, we need to read more data upon get() */
  apr_size_t current;

  /* offset in FILE from which the first entry in BUFFER has been read */
  apr_off_t start_offset;

  /* offset in FILE from which the next number has to be read */
  apr_off_t next_offset;

  /* read the file in chunks of this size */
  apr_size_t block_size;

  /* pool to be used for file ops etc. */
  apr_pool_t *pool;

  /* buffer for prefetched values */
  value_position_pair_t buffer[MAX_NUMBER_PREFETCH];
};

/* Return an svn_error_t * object for error ERR on STREAM with the given
 * MESSAGE string.  The latter must have a placeholder for the index file
 * name ("%s") and the current read offset (e.g. "0x%lx").
 */
static svn_error_t *
stream_error_create(svn_fs_fs__packed_number_stream_t *stream,
                    apr_status_t err,
                    const char *message)
{
  const char *file_name;
  apr_off_t offset = 0;
  SVN_ERR(svn_io_file_name_get(&file_name, stream->file,
                               stream->pool));
  SVN_ERR(svn_io_file_seek(stream->file, SEEK_CUR, &offset, stream->pool));

  return svn_error_createf(err, NULL, message, file_name,
                           (apr_uint64_t)offset);
}

/* Read up to MAX_NUMBER_PREFETCH numbers from the STREAM->NEXT_OFFSET in
 * STREAM->FILE and buffer them.
 *
 * We don't want GCC and others to inline this function into get() because
 * it prevents get() from being inlined itself.
 */
SVN__PREVENT_INLINE
static svn_error_t *
packed_stream_read(svn_fs_fs__packed_number_stream_t *stream)
{
  unsigned char buffer[MAX_NUMBER_PREFETCH];
  apr_size_t read = 0;
  apr_size_t i;
  value_position_pair_t *target;
  apr_off_t block_start = 0;
  apr_off_t block_left = 0;
  apr_status_t err;

  /* all buffered data will have been read starting here */
  stream->start_offset = stream->next_offset;

  /* packed numbers are usually not aligned to MAX_NUMBER_PREFETCH blocks,
   * i.e. the last number has been incomplete (and not buffered in stream)
   * and need to be re-read.  Therefore, always correct the file pointer.
   */
  SVN_ERR(svn_io_file_aligned_seek(stream->file, stream->block_size,
                                   &block_start, stream->next_offset,
                                   stream->pool));

  /* prefetch at least one number but, if feasible, don't cross block
   * boundaries.  This shall prevent jumping back and forth between two
   * blocks because the extra data was not actually request _now_.
   */
  read = sizeof(buffer);
  block_left = stream->block_size - (stream->next_offset - block_start);
  if (block_left >= 10 && block_left < read)
    read = block_left;

  err = apr_file_read(stream->file, buffer, &read);
  if (err && !APR_STATUS_IS_EOF(err))
    return stream_error_create(stream, err,
      _("Can't read index file '%s' at offset 0x%" APR_UINT64_T_HEX_FMT));

  /* if the last number is incomplete, trim it from the buffer */
  while (read > 0 && buffer[read-1] >= 0x80)
    --read;

  /* we call read() only if get() requires more data.  So, there must be
   * at least *one* further number. */
  if SVN__PREDICT_FALSE(read == 0)
    return stream_error_create(stream, err,
      _("Unexpected end of index file %s at offset 0x%"APR_UINT64_T_HEX_FMT));

  /* parse file buffer and expand into stream buffer */
  target = stream->buffer;
  for (i = 0; i < read;)
    {
      if (buffer[i] < 0x80)
        {
          /* numbers < 128 are relatively frequent and particularly easy
           * to decode.  Give them special treatment. */
          target->value = buffer[i];
          ++i;
          target->total_len = i;
          ++target;
        }
      else
        {
          apr_uint64_t value = 0;
          apr_uint64_t shift = 0;
          while (buffer[i] >= 0x80)
            {
              value += ((apr_uint64_t)buffer[i] & 0x7f) << shift;
              shift += 7;
              ++i;
            }

          target->value = value + ((apr_uint64_t)buffer[i] << shift);
          ++i;
          target->total_len = i;
          ++target;

          /* let's catch corrupted data early.  It would surely cause
           * havoc further down the line. */
          if SVN__PREDICT_FALSE(shift > 8 * sizeof(value))
            return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_CORRUPTION, NULL,
                                     _("Corrupt index: number too large"));
       }
    }

  /* update stream state */
  stream->used = target - stream->buffer;
  stream->next_offset = stream->start_offset + i;
  stream->current = 0;

  return SVN_NO_ERROR;
};

/* Create and open a packed number stream reading from FILE_NAME and
 * return it in *STREAM.  Access the file in chunks of BLOCK_SIZE bytes.
 * Use POOL for allocations.
 */
static svn_error_t *
packed_stream_open(svn_fs_fs__packed_number_stream_t **stream,
                   const char *file_name,
                   apr_size_t block_size,
                   apr_pool_t *pool)
{
  svn_fs_fs__packed_number_stream_t *result
    = apr_palloc(pool, sizeof(*result));
  result->pool = svn_pool_create(pool);

  SVN_ERR(svn_io_file_open(&result->file, file_name,
                           APR_READ | APR_BUFFERED, APR_OS_DEFAULT,
                           result->pool));

  result->used = 0;
  result->current = 0;
  result->start_offset = 0;
  result->next_offset = 0;
  result->block_size = block_size;

  *stream = result;
  
  return SVN_NO_ERROR;
}

/* Close STREAM which may be NULL.
 */
svn_error_t *
svn_fs_fs__packed_stream_close(svn_fs_fs__packed_number_stream_t *stream)
{
  if (stream)
    {
      SVN_ERR(svn_io_file_close(stream->file, stream->pool));
      svn_pool_destroy(stream->pool);
    }

  return SVN_NO_ERROR;
}

/*
 * The forced inline is required as e.g. GCC would inline read() into here
 * instead of lining the simple buffer access into callers of get().
 */
SVN__FORCE_INLINE
static svn_error_t*
packed_stream_get(apr_uint64_t *value,
                  svn_fs_fs__packed_number_stream_t *stream)
{
  if (stream->current == stream->used)
    SVN_ERR(packed_stream_read(stream));

  *value = stream->buffer[stream->current].value;
  ++stream->current;

  return SVN_NO_ERROR;
}

/* Navigate STREAM to packed file offset OFFSET.  There will be no checks
 * whether the given OFFSET is valid.
 */
static void
packed_stream_seek(svn_fs_fs__packed_number_stream_t *stream,
                   apr_off_t offset)
{
  if (   stream->used == 0
      || offset < stream->start_offset
      || offset >= stream->next_offset)
    {
      /* outside buffered data.  Next get() will read() from OFFSET. */
      stream->start_offset = offset;
      stream->next_offset = offset;
      stream->current = 0;
      stream->used = 0;
    }
  else
    {
      /* Find the suitable location in the stream buffer.
       * Since our buffer is small, it is efficient enough to simply scan
       * it for the desired position. */
      apr_size_t i;
      for (i = 0; i < stream->used; ++i)
        if (stream->buffer[i].total_len > offset - stream->start_offset)
          break;

      stream->current = i;
    }
}

/* Return the packed file offset of at which the next number in the stream
 * can be found.
 */
static apr_off_t
packed_stream_offset(svn_fs_fs__packed_number_stream_t *stream)
{
  return stream->current == 0
       ? stream->start_offset
       : stream->buffer[stream->current-1].total_len + stream->start_offset;
}

/* Encode VALUE as 7/8b into P and return the number of bytes written.
 * This will be used when _writing_ packed data.  packed_stream_* is for
 * read operations only.
 */
static apr_size_t
encode_uint(unsigned char *p, apr_uint64_t value)
{
  unsigned char *start = p;
  while (value >= 0x80)
    {
      *p = (unsigned char)((value % 0x80) + 0x80);
      value /= 0x80;
      ++p;
    }

  *p = (unsigned char)(value % 0x80);
  return (p - start) + 1;
}

/* Encode VALUE as 7/8b into P and return the number of bytes written.
 * This maps signed ints onto unsigned ones.
 */
static apr_size_t
encode_int(unsigned char *p, apr_int64_t value)
{
  return encode_uint(p, (apr_uint64_t)(value < 0 ? -1 - 2*value : 2*value));
}

/* Map unsigned VALUE back to signed integer.
 */
static apr_int64_t
decode_int(apr_uint64_t value)
{
  return (apr_int64_t)(value % 2 ? -1 - value / 2 : value / 2);
}

/*
 * general utilities
 */

/* Return the base revision used to identify the p2l or lp2 index covering
 * REVISION in FS.
 */
static svn_revnum_t
base_revision(svn_fs_t *fs, svn_revnum_t revision)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  return svn_fs_fs__is_packed_rev(fs, revision)
       ? revision - (revision % ffd->max_files_per_dir)
       : revision;
}

/*
 * log-to-phys index
 */

/* Write ENTRY to log-to-phys PROTO_INDEX file and verify the results.
 * Use POOL for allocations.
 */
static svn_error_t *
write_entry_to_proto_index(apr_file_t *proto_index,
                           l2p_proto_entry_t entry,
                           apr_pool_t *pool)
{
  apr_size_t written = sizeof(entry);

  SVN_ERR(svn_io_file_write(proto_index, &entry, &written, pool));
  SVN_ERR_ASSERT(written == sizeof(entry));

  return SVN_NO_ERROR;
}

/* Write the log-2-phys index page description for the l2p_page_entry_t
 * array ENTRIES, starting with element START up to but not including END.
 * Write the resulting representation into BUFFER.  Use POOL for temporary
 * allocations.
 */
static svn_error_t *
encode_l2p_page(apr_array_header_t *entries,
                int start,
                int end,
                svn_spillbuf_t *buffer,
                apr_pool_t *pool)
{
  unsigned char encoded[ENCODED_INT_LENGTH];
  int i;
  const apr_uint64_t *values = (const apr_uint64_t *)entries->elts;
  apr_uint64_t last_value = 0;

  /* encode items */
  for (i = start; i < end; ++i)
    {
      apr_int64_t diff = values[i] - last_value;
      last_value = values[i];
      SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                  encode_int(encoded, diff), pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__l2p_proto_index_open(apr_file_t **proto_index,
                                const char *file_name,
                                apr_pool_t *pool)
{
  SVN_ERR(svn_io_file_open(proto_index, file_name, APR_READ | APR_WRITE
                           | APR_CREATE | APR_APPEND | APR_BUFFERED,
                           APR_OS_DEFAULT, pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__l2p_proto_index_add_revision(apr_file_t *proto_index,
                                        apr_pool_t *pool)
{
  l2p_proto_entry_t entry;
  entry.offset = 0;
  entry.item_index = 0;

  return svn_error_trace(write_entry_to_proto_index(proto_index, entry,
                                                    pool));
}

svn_error_t *
svn_fs_fs__l2p_proto_index_add_entry(apr_file_t *proto_index,
                                     apr_off_t offset,
                                     apr_uint64_t item_index,
                                     apr_pool_t *pool)
{
  l2p_proto_entry_t entry;

  /* make sure the conversion to uint64 works */
  SVN_ERR_ASSERT(offset >= -1);

  /* we support offset '-1' as a "not used" indication */
  entry.offset = (apr_uint64_t)offset + 1;

  /* make sure we can use item_index as an array index when building the
   * final index file */
  SVN_ERR_ASSERT(item_index < UINT_MAX / 2);
  entry.item_index = item_index;

  return svn_error_trace(write_entry_to_proto_index(proto_index, entry,
                                                    pool));
}

svn_error_t *
svn_fs_fs__l2p_index_create(svn_fs_t *fs,
                            const char *file_name,
                            const char *proto_file_name,
                            svn_revnum_t revision,
                            apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  apr_file_t *proto_index = NULL;
  int i;
  apr_uint64_t entry;
  svn_boolean_t eof = FALSE;
  apr_file_t *index_file;
  unsigned char encoded[ENCODED_INT_LENGTH];

  int last_page_count = 0;          /* total page count at the start of
                                       the current revision */

  /* temporary data structures that collect the data which will be moved
     to the target file in a second step */
  apr_pool_t *local_pool = svn_pool_create(pool);
  apr_pool_t *iterpool = svn_pool_create(local_pool);
  apr_array_header_t *page_counts
    = apr_array_make(local_pool, 16, sizeof(apr_uint64_t));
  apr_array_header_t *page_sizes
    = apr_array_make(local_pool, 16, sizeof(apr_uint64_t));
  apr_array_header_t *entry_counts
    = apr_array_make(local_pool, 16, sizeof(apr_uint64_t));

  /* collect the item offsets and sub-item value for the current revision */
  apr_array_header_t *entries
    = apr_array_make(local_pool, 256, sizeof(apr_uint64_t));

  /* 64k blocks, spill after 16MB */
  svn_spillbuf_t *buffer
    = svn_spillbuf__create(0x10000, 0x1000000, local_pool);

  /* start at the beginning of the source file */
  SVN_ERR(svn_io_file_open(&proto_index, proto_file_name,
                           APR_READ | APR_CREATE | APR_BUFFERED,
                           APR_OS_DEFAULT, pool));

  /* process all entries until we fail due to EOF */
  for (entry = 0; !eof; ++entry)
    {
      l2p_proto_entry_t proto_entry;
      apr_size_t read = 0;

      /* (attempt to) read the next entry from the source */
      SVN_ERR(svn_io_file_read_full2(proto_index,
                                     &proto_entry, sizeof(proto_entry),
                                     &read, &eof, local_pool));
      SVN_ERR_ASSERT(eof || read == sizeof(proto_entry));

      /* handle new revision */
      if ((entry > 0 && proto_entry.offset == 0) || eof)
        {
          /* dump entries, grouped into pages */

          int entry_count = 0;
          for (i = 0; i < entries->nelts; i += entry_count)
            {
              /* 1 page with up to 8k entries */
              apr_size_t last_buffer_size = svn_spillbuf__get_size(buffer);
              entry_count = MIN(entries->nelts - i, ffd->l2p_page_size);

              SVN_ERR(encode_l2p_page(entries, i, i + entry_count,
                                      buffer, iterpool));

              APR_ARRAY_PUSH(entry_counts, apr_uint64_t) = entry_count;
              APR_ARRAY_PUSH(page_sizes, apr_uint64_t)
                = svn_spillbuf__get_size(buffer) - last_buffer_size;

              svn_pool_clear(iterpool);
            }

          apr_array_clear(entries);

          /* store the number of pages in this revision */
          APR_ARRAY_PUSH(page_counts, apr_uint64_t)
            = page_sizes->nelts - last_page_count;

          last_page_count = page_sizes->nelts;
        }
      else
        {
          /* store the mapping in our array */
          int idx = (apr_size_t)proto_entry.item_index;
          while (idx >= entries->nelts)
            APR_ARRAY_PUSH(entries, apr_uint64_t) = 0;

          APR_ARRAY_IDX(entries, idx, apr_uint64_t) = proto_entry.offset;
        }
    }

  /* create the target file */
  SVN_ERR(svn_io_file_open(&index_file, file_name, APR_WRITE
                           | APR_CREATE | APR_TRUNCATE | APR_BUFFERED,
                           APR_OS_DEFAULT, local_pool));

  /* write header info */
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, revision),
                                 NULL, local_pool));
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, ffd->l2p_page_size),
                                 NULL, local_pool));
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, page_counts->nelts),
                                 NULL, local_pool));
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, page_sizes->nelts),
                                 NULL, local_pool));

  /* write the revision table */
  for (i = 0; i < page_counts->nelts; ++i)
    {
      apr_uint64_t value = APR_ARRAY_IDX(page_counts, i, apr_uint64_t);
      SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                     encode_uint(encoded, value),
                                     NULL, local_pool));
    }
    
  /* write the page table */
  for (i = 0; i < page_sizes->nelts; ++i)
    {
      apr_uint64_t value = APR_ARRAY_IDX(page_sizes, i, apr_uint64_t);
      SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                     encode_uint(encoded, value),
                                     NULL, local_pool));
      value = APR_ARRAY_IDX(entry_counts, i, apr_uint64_t);
      SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                     encode_uint(encoded, value),
                                     NULL, local_pool));
    }

  /* append page contents */
  SVN_ERR(svn_stream_copy3(svn_stream__from_spillbuf(buffer, local_pool),
                           svn_stream_from_aprfile2(index_file, TRUE,
                                                    local_pool),
                           NULL, NULL, local_pool));

  /* finalize the index file */
  SVN_ERR(svn_io_file_close(index_file, local_pool));
  SVN_ERR(svn_io_set_file_read_only(file_name, FALSE, local_pool));

  svn_pool_destroy(local_pool);

  return SVN_NO_ERROR;
}

/* If *STREAM is NULL, create a new stream for the log-to-phys index for
 * REVISION in FS and return it in *STREAM.  Use POOL for allocations.
 */
static svn_error_t *
auto_open_l2p_index(svn_fs_fs__packed_number_stream_t **stream,
                    svn_fs_t *fs,
                    svn_revnum_t revision,
                    apr_pool_t *pool)
{
  if (*stream == NULL)
    {
      fs_fs_data_t *ffd = fs->fsap_data;
      SVN_ERR(packed_stream_open(stream,
                                 svn_fs_fs__path_l2p_index(fs, revision,
                                                           pool),
                                 ffd->block_size,
                                 pool));
    }

  return SVN_NO_ERROR;
}

/* Read the header data structure of the log-to-phys index for REVISION
 * in FS and return it in *HEADER.  To maximize efficiency, use or return
 * the data stream in *STREAM.  Use POOL for allocations.
 */
static svn_error_t *
get_l2p_header_body(l2p_header_t **header,
                    svn_fs_fs__packed_number_stream_t **stream,
                    svn_fs_t *fs,
                    svn_revnum_t revision,
                    apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  apr_uint64_t value;
  int i;
  apr_size_t page, page_count;
  apr_off_t offset;
  l2p_header_t *result = apr_pcalloc(pool, sizeof(*result));
  apr_size_t page_table_index;

  pair_cache_key_t key;
  key.revision = base_revision(fs, revision);
  key.second = svn_fs_fs__is_packed_rev(fs, revision);

  SVN_ERR(auto_open_l2p_index(stream, fs, revision, pool));
  packed_stream_seek(*stream, 0);

  /* read the table sizes */
  SVN_ERR(packed_stream_get(&value, *stream));
  result->first_revision = (svn_revnum_t)value;
  SVN_ERR(packed_stream_get(&value, *stream));
  result->page_size = (apr_size_t)value;
  SVN_ERR(packed_stream_get(&value, *stream));
  result->revision_count = (int)value;
  SVN_ERR(packed_stream_get(&value, *stream));
  page_count = (apr_size_t)value;

  /* allocate the page tables */
  result->page_table
    = apr_pcalloc(pool, page_count * sizeof(*result->page_table));
  result->page_table_index
    = apr_pcalloc(pool, (result->revision_count + 1)
                      * sizeof(*result->page_table_index));

  /* read per-revision page table sizes (i.e. number of pages per rev) */
  page_table_index = 0;
  result->page_table_index[0] = page_table_index;

  for (i = 0; i < result->revision_count; ++i)
    {
      SVN_ERR(packed_stream_get(&value, *stream));
      page_table_index += (apr_size_t)value;
      result->page_table_index[i+1] = page_table_index;
    }

  /* read actual page tables */
  for (page = 0; page < page_count; ++page)
    {
      SVN_ERR(packed_stream_get(&value, *stream));
      result->page_table[page].size = (apr_uint32_t)value;
      SVN_ERR(packed_stream_get(&value, *stream));
      result->page_table[page].entry_count = (apr_uint32_t)value;
    }

  /* correct the page description offsets */
  offset = packed_stream_offset(*stream);
  for (page = 0; page < page_count; ++page)
    {
      result->page_table[page].offset = offset;
      offset += result->page_table[page].size;
    }

  /* return and cache the header */
  *header = result;
  SVN_ERR(svn_cache__set(ffd->l2p_header_cache, &key, result, pool));

  return SVN_NO_ERROR;
}

/* Data structure that describes which l2p page info shall be extracted
 * from the cache and contains the fields that receive the result.
 */
typedef struct l2p_page_info_baton_t
{
  /* input data: we want the page covering (REVISION,ITEM_INDEX) */
  svn_revnum_t revision;
  apr_uint64_t item_index;

  /* out data */
  /* page location and size of the page within the l2p index file */
  l2p_page_table_entry_t entry;

  /* page number within the pages for REVISION (not l2p index global!) */
  apr_size_t page_no;

  /* offset of ITEM_INDEX within that page */
  apr_uint32_t page_offset;

  /* revision identifying the l2p index file, also the first rev in that */
  svn_revnum_t first_revision;
} l2p_page_info_baton_t;


/* Utility function that copies the info requested by BATON->REVISION and
 * BATON->ITEM_INDEX and from HEADER and PAGE_TABLE into the output fields
 * of *BATON.
 */
static svn_error_t *
l2p_page_info_copy(l2p_page_info_baton_t *baton,
                   const l2p_header_t *header,
                   const l2p_page_table_entry_t *page_table,
                   const apr_size_t *page_table_index)
{
  /* revision offset within the index file */
  apr_size_t rel_revision = baton->revision - header->first_revision;
  if (rel_revision >= header->revision_count)
    return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_REVISION , NULL,
                             _("Revision %ld not covered by item index"),
                             baton->revision);

  /* select the relevant page */
  if (baton->item_index < header->page_size)
    {
      /* most revs fit well into a single page */
      baton->page_offset = (apr_size_t)baton->item_index;
      baton->page_no = 0;
      baton->entry = page_table[page_table_index[rel_revision]];
    }
  else
    {
      const l2p_page_table_entry_t *first_entry;
      const l2p_page_table_entry_t *last_entry;
      
      /* all pages are of the same size and full, except for the last one */
      baton->page_offset = (apr_size_t)(baton->item_index % header->page_size);
      baton->page_no = (apr_uint32_t)(baton->item_index / header->page_size);

      /* range of pages for this rev */
      first_entry = page_table + page_table_index[rel_revision];
      last_entry = page_table + page_table_index[rel_revision + 1];

      if (last_entry - first_entry > baton->page_no)
        {
          baton->entry = first_entry[baton->page_no];
        }
      else
        {
          /* limit page index to the valid range */
          baton->entry = last_entry[-1];

          /* cause index overflow further down the road */
          baton->page_offset = header->page_size + 1;
        }
    }
    
  baton->first_revision = header->first_revision;

  return SVN_NO_ERROR;
}

/* Implement svn_cache__partial_getter_func_t: copy the data requested in
 * l2p_page_info_baton_t *BATON from l2p_header_t *DATA into the output
 * fields in *BATON.
 */
static svn_error_t *
l2p_page_info_access_func(void **out,
                          const void *data,
                          apr_size_t data_len,
                          void *baton,
                          apr_pool_t *result_pool)
{
  /* resolve all pointer values of in-cache data */
  const l2p_header_t *header = data;
  const l2p_page_table_entry_t *page_table
    = svn_temp_deserializer__ptr(header,
                                 (const void *const *)&header->page_table);
  const apr_size_t *page_table_index
    = svn_temp_deserializer__ptr(header,
                           (const void *const *)&header->page_table_index);

  /* copy the info */
  return l2p_page_info_copy(baton, header, page_table, page_table_index);
}

/* Get the page info requested in *BATON from FS and set the output fields
 * in *BATON.
 * To maximize efficiency, use or return the data stream in *STREAM.
 * Use POOL for allocations.
 */
static svn_error_t *
get_l2p_page_info(l2p_page_info_baton_t *baton,
                  svn_fs_fs__packed_number_stream_t **stream,
                  svn_fs_t *fs,
                  apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  l2p_header_t *result;
  svn_boolean_t is_cached = FALSE;
  void *dummy = NULL;

  /* try to find the info in the cache */
  pair_cache_key_t key;
  key.revision = base_revision(fs, baton->revision);
  key.second = svn_fs_fs__is_packed_rev(fs, baton->revision);
  SVN_ERR(svn_cache__get_partial((void**)&dummy, &is_cached,
                                 ffd->l2p_header_cache, &key,
                                 l2p_page_info_access_func, baton,
                                 pool));
  if (is_cached)
    return SVN_NO_ERROR;

  /* read from disk, cache and copy the result */
  SVN_ERR(get_l2p_header_body(&result, stream, fs, baton->revision, pool));
  SVN_ERR(l2p_page_info_copy(baton, result, result->page_table,
                             result->page_table_index));

  return SVN_NO_ERROR;
}

/* Data request structure used by l2p_page_table_access_func.
 */
typedef struct l2p_page_table_baton_t
{
  /* revision for which to read the page table */
  svn_revnum_t revision;

  /* page table entries (of type l2p_page_table_entry_t).
   * Must be created by caller and will be filled by callee. */
  apr_array_header_t *pages;
} l2p_page_table_baton_t;

/* Implement svn_cache__partial_getter_func_t: copy the data requested in
 * l2p_page_baton_t *BATON from l2p_page_t *DATA into BATON->PAGES and *OUT.
 */
static svn_error_t *
l2p_page_table_access_func(void **out,
                           const void *data,
                           apr_size_t data_len,
                           void *baton,
                           apr_pool_t *result_pool)
{
  /* resolve in-cache pointers */
  l2p_page_table_baton_t *table_baton = baton;
  const l2p_header_t *header = (const l2p_header_t *)data;
  const l2p_page_table_entry_t *page_table
    = svn_temp_deserializer__ptr(header,
                                 (const void *const *)&header->page_table);
  const apr_size_t *page_table_index
    = svn_temp_deserializer__ptr(header,
                           (const void *const *)&header->page_table_index);

  /* copy the revision's page table into BATON */
  apr_size_t rel_revision = table_baton->revision - header->first_revision;
  if (rel_revision < header->revision_count)
    {
      const l2p_page_table_entry_t *entry
        = page_table + page_table_index[rel_revision];
      const l2p_page_table_entry_t *last_entry
        = page_table + page_table_index[rel_revision + 1];

      for (; entry < last_entry; ++entry)
        APR_ARRAY_PUSH(table_baton->pages, l2p_page_table_entry_t)
          = *entry;
    }

  /* set output as a courtesy to the caller */
  *out = table_baton->pages;
  
  return SVN_NO_ERROR;
}

/* Read the l2p index page table for REVISION in FS from cache and return
 * it in PAGES.  The later must be provided by the caller (and can be
 * re-used); existing entries will be removed before writing the result.
 * If the data cannot be found in the cache, the result will be empty
 * (it never can be empty for a valid REVISION if the data is cached).
 * Use POOL for temporary allocations.
 */
static svn_error_t *
get_l2p_page_table(apr_array_header_t *pages,
                   svn_fs_t *fs,
                   svn_revnum_t revision,
                   apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  svn_boolean_t is_cached = FALSE;
  l2p_page_table_baton_t baton;

  pair_cache_key_t key;
  key.revision = base_revision(fs, revision);
  key.second = svn_fs_fs__is_packed_rev(fs, revision);

  apr_array_clear(pages);
  baton.revision = revision;
  baton.pages = pages;
  SVN_ERR(svn_cache__get_partial((void**)&pages, &is_cached,
                                 ffd->l2p_header_cache, &key,
                                 l2p_page_table_access_func, &baton, pool));

  return SVN_NO_ERROR;
}

/* From the log-to-phys index file starting at START_REVISION in FS, read
 * the mapping page identified by TABLE_ENTRY and return it in *PAGE.
 * To maximize efficiency, use or return the data stream in *STREAM.
 * Use POOL for allocations.
 */
static svn_error_t *
get_l2p_page(l2p_page_t **page,
             svn_fs_fs__packed_number_stream_t **stream,
             svn_fs_t *fs,
             svn_revnum_t start_revision,
             l2p_page_table_entry_t *table_entry,
             apr_pool_t *pool)
{
  apr_uint32_t i;
  l2p_page_t *result = apr_pcalloc(pool, sizeof(*result));
  apr_uint64_t last_value = 0;

  /* open index file and select page */
  SVN_ERR(auto_open_l2p_index(stream, fs, start_revision, pool));
  packed_stream_seek(*stream, table_entry->offset);

  /* initialize the page content */
  result->entry_count = table_entry->entry_count;
  result->offsets = apr_pcalloc(pool, result->entry_count
                                    * sizeof(*result->offsets));

  /* read all page entries (offsets in rev file and container sub-items) */
  for (i = 0; i < result->entry_count; ++i)
    {
      apr_uint64_t value = 0;
      SVN_ERR(packed_stream_get(&value, *stream));
      last_value += decode_int(value);
      result->offsets[i] = last_value - 1;
    }

  *page = result;

  return SVN_NO_ERROR;
}

/* Utility function.  Read the l2p index pages for REVISION in FS from
 * STREAM and put them into the cache.  Skip page number EXLCUDED_PAGE_NO
 * (use -1 for 'skip none') and pages outside the MIN_OFFSET, MAX_OFFSET
 * range in the l2p index file.  The index is being identified by
 * FIRST_REVISION.  PAGES is a scratch container provided by the caller.
 * SCRATCH_POOL is used for temporary allocations.
 *
 * This function may be a no-op if the header cache lookup fails / misses.
 */
static svn_error_t *
prefetch_l2p_pages(svn_boolean_t *end,
                   svn_fs_t *fs,
                   svn_fs_fs__packed_number_stream_t *stream,
                   svn_revnum_t first_revision,
                   svn_revnum_t revision,
                   apr_array_header_t *pages,
                   int exlcuded_page_no,
                   apr_off_t min_offset,
                   apr_off_t max_offset,
                   apr_pool_t *scratch_pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  int i;
  apr_pool_t *iterpool;
  svn_fs_fs__page_cache_key_t key = { 0 };

  /* get the page table for REVISION from cache */
  *end = FALSE;
  SVN_ERR(get_l2p_page_table(pages, fs, revision, scratch_pool));
  if (pages->nelts == 0)
    {
      /* not found -> we can't continue without hitting the disk again */
      *end = TRUE;
      return SVN_NO_ERROR;
    }

  /* prefetch pages individually until all are done or we found one in
   * the cache */
  iterpool = svn_pool_create(scratch_pool);
  assert(revision <= APR_UINT32_MAX);
  key.revision = (apr_uint32_t)revision;
  key.is_packed = svn_fs_fs__is_packed_rev(fs, revision);

  for (i = 0; i < pages->nelts && !*end; ++i)
    {
      l2p_page_table_entry_t *entry
        = &APR_ARRAY_IDX(pages, i, l2p_page_table_entry_t);
      if (i == exlcuded_page_no)
        continue;

      /* skip pages outside the specified index file range */
      if (   entry->offset < min_offset
          || entry->offset + entry->size > max_offset)
        {
          *end = TRUE;
          continue;
        }

      /* page already in cache? */
      key.page = i;
      SVN_ERR(svn_cache__has_key(end, ffd->l2p_page_cache,
                                 &key, iterpool));
      if (!*end)
        {
          /* no in cache -> read from stream (data already buffered in APR)
           * and cache the result */
          l2p_page_t *page = NULL;
          SVN_ERR(get_l2p_page(&page, &stream, fs, first_revision,
                               entry, iterpool));

          SVN_ERR(svn_cache__set(ffd->l2p_page_cache, &key, page,
                                 iterpool));
        }

      svn_pool_clear(iterpool);
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Request data structure for l2p_entry_access_func.
 */
typedef struct l2p_entry_baton_t
{
  /* in data */
  /* revision. Used for error messages only */
  svn_revnum_t revision;

  /* item index to look up. Used for error messages only */
  apr_uint64_t item_index;

  /* offset within the cached page */
  apr_uint32_t page_offset;

  /* out data */
  /* absolute item or container offset in rev / pack file */
  apr_uint64_t offset;
} l2p_entry_baton_t;

/* Return the rev / pack file offset of the item at BATON->PAGE_OFFSET in
 * OFFSETS of PAGE and write it to *OFFSET.
 */
static svn_error_t *
l2p_page_get_entry(l2p_entry_baton_t *baton,
                   const l2p_page_t *page,
                   const apr_uint64_t *offsets)
{
  /* overflow check */
  if (page->entry_count <= baton->page_offset)
    return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_OVERFLOW , NULL,
                             _("Item index %" APR_UINT64_T_FMT
                               " too large in revision %ld"),
                             baton->item_index, baton->revision);

  /* return the result */
  baton->offset = offsets[baton->page_offset];

  return SVN_NO_ERROR;
}

/* Implement svn_cache__partial_getter_func_t: copy the data requested in
 * l2p_entry_baton_t *BATON from l2p_page_t *DATA into BATON->OFFSET.
 * *OUT remains unchanged.
 */
static svn_error_t *
l2p_entry_access_func(void **out,
                      const void *data,
                      apr_size_t data_len,
                      void *baton,
                      apr_pool_t *result_pool)
{
  /* resolve all in-cache pointers */
  const l2p_page_t *page = data;
  const apr_uint64_t *offsets
    = svn_temp_deserializer__ptr(page, (const void *const *)&page->offsets);

  /* return the requested data */
  return l2p_page_get_entry(baton, page, offsets);
}

/* Using the log-to-phys indexes in FS, find the absolute offset in the
 * rev file for (REVISION, ITEM_INDEX) and return it in *OFFSET.
 * Use POOL for allocations.
 */
static svn_error_t *
l2p_index_lookup(apr_off_t *offset,
                 svn_fs_t *fs,
                 svn_revnum_t revision,
                 apr_uint64_t item_index,
                 apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  l2p_page_info_baton_t info_baton;
  l2p_entry_baton_t page_baton;
  l2p_page_t *page = NULL;
  svn_fs_fs__packed_number_stream_t *stream = NULL;
  svn_fs_fs__page_cache_key_t key = { 0 };
  svn_boolean_t is_cached = FALSE;
  void *dummy = NULL;

  /* read index master data structure and extract the info required to
   * access the l2p index page for (REVISION,ITEM_INDEX)*/
  info_baton.revision = revision;
  info_baton.item_index = item_index;
  SVN_ERR(get_l2p_page_info(&info_baton, &stream, fs, pool));

  /* try to find the page in the cache and get the OFFSET from it */
  page_baton.revision = revision;
  page_baton.item_index = item_index;
  page_baton.page_offset = info_baton.page_offset;

  assert(revision <= APR_UINT32_MAX);
  key.revision = (apr_uint32_t)revision;
  key.is_packed = svn_fs_fs__is_packed_rev(fs, revision);
  key.page = info_baton.page_no;

  SVN_ERR(svn_cache__get_partial(&dummy, &is_cached,
                                 ffd->l2p_page_cache, &key,
                                 l2p_entry_access_func, &page_baton, pool));

  if (!is_cached)
    {
      /* we need to read the info from disk (might already be in the
       * APR file buffer, though) */
      apr_array_header_t *pages;
      svn_revnum_t prefetch_revision;
      svn_revnum_t last_revision
        = info_baton.first_revision
          + (key.is_packed ? ffd->max_files_per_dir : 1);
      apr_pool_t *iterpool = svn_pool_create(pool);
      svn_boolean_t end;
      apr_off_t max_offset
        = APR_ALIGN(info_baton.entry.offset + info_baton.entry.size,
                    0x10000);
      apr_off_t min_offset = max_offset - 0x10000;

      /* read the relevant page */
      SVN_ERR(get_l2p_page(&page, &stream, fs, info_baton.first_revision,
                           &info_baton.entry, pool));

      /* cache the page and extract the result we need */
      SVN_ERR(svn_cache__set(ffd->l2p_page_cache, &key, page, pool));
      SVN_ERR(l2p_page_get_entry(&page_baton, page, page->offsets));

      /* prefetch pages from following and preceding revisions */
      pages = apr_array_make(pool, 16, sizeof(l2p_page_table_entry_t));
      end = FALSE;
      for (prefetch_revision = revision;
           prefetch_revision < last_revision && !end;
           ++prefetch_revision)
        {
          int excluded_page_no = prefetch_revision == revision
                               ? info_baton.page_no
                               : -1;
          SVN_ERR(prefetch_l2p_pages(&end, fs, stream,
                                     info_baton.first_revision,
                                     prefetch_revision, pages,
                                     excluded_page_no, min_offset,
                                     max_offset, iterpool));
          svn_pool_clear(iterpool);
        }

      end = FALSE;
      for (prefetch_revision = revision-1;
           prefetch_revision >= info_baton.first_revision && !end;
           --prefetch_revision)
        {
          SVN_ERR(prefetch_l2p_pages(&end, fs, stream,
                                     info_baton.first_revision,
                                     prefetch_revision, pages, -1,
                                     min_offset, max_offset, iterpool));
          svn_pool_clear(iterpool);
        }

      svn_pool_destroy(iterpool);
    }

  SVN_ERR(svn_fs_fs__packed_stream_close(stream));

  *offset = page_baton.offset;

  return SVN_NO_ERROR;
}

/* Using the log-to-phys proto index in transaction TXN_ID in FS, find the
 * absolute offset in the proto rev file for the given ITEM_INDEX and return
 * it in *OFFSET.  Use POOL for allocations.
 */
static svn_error_t *
l2p_proto_index_lookup(apr_off_t *offset,
                       svn_fs_t *fs,
                       const svn_fs_fs__id_part_t *txn_id,
                       apr_uint64_t item_index,
                       apr_pool_t *pool)
{
  svn_boolean_t eof = FALSE;
  apr_file_t *file = NULL;
  SVN_ERR(svn_io_file_open(&file,
                           svn_fs_fs__path_l2p_proto_index(fs, txn_id, pool),
                           APR_READ | APR_BUFFERED, APR_OS_DEFAULT, pool));

  /* process all entries until we fail due to EOF */
  *offset = -1;
  while (!eof)
    {
      l2p_proto_entry_t entry;
      apr_size_t read = 0;

      /* (attempt to) read the next entry from the source */
      SVN_ERR(svn_io_file_read_full2(file, &entry, sizeof(entry),
                                     &read, &eof, pool));
      SVN_ERR_ASSERT(eof || read == sizeof(entry));

      /* handle new revision */
      if (!eof && entry.item_index == item_index)
        {
          *offset = (apr_off_t)entry.offset - 1;
          break;
        }
    }

  SVN_ERR(svn_io_file_close(file, pool));
  
  return SVN_NO_ERROR;
}

/* Read the log-to-phys header info of the index covering REVISION from FS
 * and return it in *HEADER.  To maximize efficiency, use or return the
 * data stream in *STREAM.  Use POOL for allocations.
 */
static svn_error_t *
get_l2p_header(l2p_header_t **header,
               svn_fs_fs__packed_number_stream_t **stream,
               svn_fs_t *fs,
               svn_revnum_t revision,
               apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  svn_boolean_t is_cached = FALSE;

  /* first, try cache lookop */
  pair_cache_key_t key;
  key.revision = base_revision(fs, revision);
  key.second = svn_fs_fs__is_packed_rev(fs, revision);
  SVN_ERR(svn_cache__get((void**)header, &is_cached, ffd->l2p_header_cache,
                         &key, pool));
  if (is_cached)
    return SVN_NO_ERROR;

  /* read from disk and cache the result */
  SVN_ERR(get_l2p_header_body(header, stream, fs, revision, pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__l2p_get_max_ids(apr_array_header_t **max_ids,
                           svn_fs_t *fs,
                           svn_revnum_t start_rev,
                           apr_size_t count,
                           apr_pool_t *pool)
{
  l2p_header_t *header = NULL;
  svn_revnum_t revision;
  svn_revnum_t last_rev = (svn_revnum_t)(start_rev + count);
  svn_fs_fs__packed_number_stream_t *stream = NULL;
  apr_pool_t *header_pool = svn_pool_create(pool);

  /* read index master data structure for the index covering START_REV */
  SVN_ERR(get_l2p_header(&header, &stream, fs, start_rev, header_pool));
  SVN_ERR(svn_fs_fs__packed_stream_close(stream));
  stream = NULL;

  /* Determine the length of the item index list for each rev.
   * Read new index headers as required. */
  *max_ids = apr_array_make(pool, (int)count, sizeof(apr_uint64_t));
  for (revision = start_rev; revision < last_rev; ++revision)
    {
      apr_uint64_t full_page_count;
      apr_uint64_t item_count;
      apr_size_t first_page_index, last_page_index;

      if (revision >= header->first_revision + header->revision_count)
        {
          /* need to read the next index. Clear up memory used for the
           * previous one. */
          svn_pool_clear(header_pool);
          SVN_ERR(get_l2p_header(&header, &stream, fs, revision,
                                 header_pool));
          SVN_ERR(svn_fs_fs__packed_stream_close(stream));
          stream = NULL;
        }

      /* in a revision with N index pages, the first N-1 index pages are
       * "full", i.e. contain HEADER->PAGE_SIZE entries */
      first_page_index
         = header->page_table_index[revision - header->first_revision];
      last_page_index
         = header->page_table_index[revision - header->first_revision + 1];
      full_page_count = last_page_index - first_page_index - 1;
      item_count = full_page_count * header->page_size
                 + header->page_table[last_page_index - 1].entry_count;

      APR_ARRAY_PUSH(*max_ids, apr_uint64_t) = item_count;
    }

  svn_pool_destroy(header_pool);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__item_offset(apr_off_t *absolute_position,
                       svn_fs_t *fs,
                       svn_revnum_t revision,
                       const svn_fs_fs__id_part_t *txn_id,
                       apr_uint64_t item_index,
                       apr_pool_t *pool)
{
  if (txn_id)
    {
      if (svn_fs_fs__use_log_addressing(fs, txn_id->revision + 1))
        {
          /* the txn is going to produce a rev with logical addressing.
             So, we need to get our info from the (proto) index file. */
          SVN_ERR(l2p_proto_index_lookup(absolute_position, fs, txn_id,
                                         item_index, pool));
        }
      else
        {
          /* for data in txns, item_index *is* the offset */
          *absolute_position = item_index;
        }
    }
  else if (svn_fs_fs__use_log_addressing(fs, revision))
    {
      /* ordinary index lookup */
      SVN_ERR(l2p_index_lookup(absolute_position, fs, revision,
                               item_index, pool));
    }
  else if (svn_fs_fs__is_packed_rev(fs, revision))
    {
      /* pack file with physical addressing */
      apr_off_t rev_offset;
      SVN_ERR(svn_fs_fs__get_packed_offset(&rev_offset, fs, revision, pool));
      *absolute_position = rev_offset + item_index;
    }
  else
    {
      /* for non-packed revs with physical addressing,
         item_index *is* the offset */
      *absolute_position = item_index;
    }

  return SVN_NO_ERROR;
}

/*
 * phys-to-log index
 */
svn_error_t *
svn_fs_fs__p2l_proto_index_open(apr_file_t **proto_index,
                                const char *file_name,
                                apr_pool_t *pool)
{
  SVN_ERR(svn_io_file_open(proto_index, file_name, APR_READ | APR_WRITE
                           | APR_CREATE | APR_APPEND | APR_BUFFERED,
                           APR_OS_DEFAULT, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__p2l_proto_index_add_entry(apr_file_t *proto_index,
                                     svn_fs_fs__p2l_entry_t *entry,
                                     apr_pool_t *pool)
{
  apr_size_t written = sizeof(*entry);

  SVN_ERR(svn_io_file_write_full(proto_index, entry, sizeof(*entry),
                                 &written, pool));
  SVN_ERR_ASSERT(written == sizeof(*entry));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__p2l_index_create(svn_fs_t *fs,
                            const char *file_name,
                            const char *proto_file_name,
                            svn_revnum_t revision,
                            apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  apr_uint64_t page_size = ffd->p2l_page_size;
  apr_file_t *proto_index = NULL;
  int i;
  svn_boolean_t eof = FALSE;
  apr_file_t *index_file;
  unsigned char encoded[ENCODED_INT_LENGTH];
  svn_revnum_t last_revision = revision;
  apr_uint64_t last_compound = 0;

  apr_uint64_t last_entry_end = 0;
  apr_uint64_t last_page_end = 0;
  apr_size_t last_buffer_size = 0;  /* byte offset in the spill buffer at
                                       the begin of the current revision */
  apr_uint64_t file_size = 0;

  /* temporary data structures that collect the data which will be moved
     to the target file in a second step */
  apr_pool_t *local_pool = svn_pool_create(pool);
  apr_array_header_t *table_sizes
     = apr_array_make(local_pool, 16, sizeof(apr_uint64_t));

  /* 64k blocks, spill after 16MB */
  svn_spillbuf_t *buffer
     = svn_spillbuf__create(0x10000, 0x1000000, local_pool);

  /* for loop temps ... */
  apr_pool_t *iter_pool = svn_pool_create(pool);

  /* start at the beginning of the source file */
  SVN_ERR(svn_io_file_open(&proto_index, proto_file_name,
                           APR_READ | APR_CREATE | APR_BUFFERED,
                           APR_OS_DEFAULT, pool));

  /* process all entries until we fail due to EOF */
  while (!eof)
    {
      svn_fs_fs__p2l_entry_t entry;
      apr_size_t read = 0;
      apr_uint64_t entry_end;
      svn_boolean_t new_page = svn_spillbuf__get_size(buffer) == 0;
      apr_uint64_t compound;
      apr_int64_t rev_diff, compound_diff;

      /* (attempt to) read the next entry from the source */
      SVN_ERR(svn_io_file_read_full2(proto_index, &entry, sizeof(entry),
                                     &read, &eof, iter_pool));
      SVN_ERR_ASSERT(eof || read == sizeof(entry));

      /* "unused" (and usually non-existent) section to cover the offsets
         at the end the of the last page. */
      if (eof)
        {
          file_size = last_entry_end;

          entry.offset = last_entry_end;
          entry.size = APR_ALIGN(entry.offset, page_size) - entry.offset;
          entry.type = 0;
          entry.item.revision = last_revision;
          entry.item.number = 0;
        }
      else
        {
          /* fix-up items created when the txn's target rev was unknown */
          if (entry.item.revision == SVN_INVALID_REVNUM)
            entry.item.revision = revision;
        }
      
      /* end pages if entry is extending beyond their boundaries */
      entry_end = entry.offset + entry.size;
      while (entry_end - last_page_end > page_size)
        {
          apr_uint64_t buffer_size = svn_spillbuf__get_size(buffer);
          APR_ARRAY_PUSH(table_sizes, apr_uint64_t)
             = buffer_size - last_buffer_size;

          last_buffer_size = buffer_size;
          last_page_end += page_size;
          new_page = TRUE;
        }

      /* this entry starts a new table -> store its offset
         (all following entries in the same table will store sizes only) */
      if (new_page)
        {
          SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                      encode_uint(encoded, entry.offset),
                                      iter_pool));
          last_revision = revision;
          last_compound = 0;
        }

      /* write simple item entry */
      SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                  encode_uint(encoded, entry.size),
                                  iter_pool));

      rev_diff = entry.item.revision - last_revision;
      last_revision = entry.item.revision;

      compound = entry.item.number * 8 + entry.type;
      compound_diff = compound - last_compound;
      last_compound = compound;
      
      SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                  encode_int(encoded, compound_diff),
                                  iter_pool));
      SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                  encode_int(encoded, rev_diff),
                                  iter_pool));
      SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                  encode_uint(encoded, entry.fnv1_checksum),
                                  iter_pool));
     
      last_entry_end = entry_end;

      svn_pool_clear(iter_pool);
    }

  /* store length of last table */
  APR_ARRAY_PUSH(table_sizes, apr_uint64_t)
      = svn_spillbuf__get_size(buffer) - last_buffer_size;

  /* create the target file */
  SVN_ERR(svn_io_file_open(&index_file, file_name, APR_WRITE
                           | APR_CREATE | APR_TRUNCATE | APR_BUFFERED,
                           APR_OS_DEFAULT, local_pool));

  /* write the start revision, file size and page size */
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, revision),
                                 NULL, local_pool));
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, file_size),
                                 NULL, local_pool));
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, page_size),
                                 NULL, local_pool));

  /* write the page table (actually, the sizes of each page description) */
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, table_sizes->nelts),
                                 NULL, local_pool));
  for (i = 0; i < table_sizes->nelts; ++i)
    {
      apr_uint64_t value = APR_ARRAY_IDX(table_sizes, i, apr_uint64_t);
      SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                     encode_uint(encoded, value),
                                     NULL, local_pool));
    }

  /* append page contents */
  SVN_ERR(svn_stream_copy3(svn_stream__from_spillbuf(buffer, local_pool),
                           svn_stream_from_aprfile2(index_file, TRUE,
                                                    local_pool),
                           NULL, NULL, local_pool));

  /* finalize the index file */
  SVN_ERR(svn_io_file_close(index_file, local_pool));
  SVN_ERR(svn_io_set_file_read_only(file_name, FALSE, local_pool));

  svn_pool_destroy(iter_pool);
  svn_pool_destroy(local_pool);

  return SVN_NO_ERROR;
}

/* Read the header data structure of the phys-to-log index for REVISION in
 * FS and return it in *HEADER. 
 * 
 * To maximize efficiency, use or return the data stream in *STREAM.
 * If *STREAM is yet to be constructed, do so in STREAM_POOL.
 * Use POOL for allocations.
 */
static svn_error_t *
get_p2l_header(p2l_header_t **header,
               svn_fs_fs__packed_number_stream_t **stream,
               svn_fs_t *fs,
               svn_revnum_t revision,
               apr_pool_t *stream_pool,
               apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  apr_uint64_t value;
  apr_size_t i;
  apr_off_t offset;
  p2l_header_t *result;
  svn_boolean_t is_cached = FALSE;

  /* look for the header data in our cache */
  pair_cache_key_t key;
  key.revision = base_revision(fs, revision);
  key.second = svn_fs_fs__is_packed_rev(fs, revision);

  SVN_ERR(svn_cache__get((void**)header, &is_cached, ffd->p2l_header_cache,
                         &key, pool));
  if (is_cached)
    return SVN_NO_ERROR;

  /* not found -> must read it from disk.
   * Open index file or position read pointer to the begin of the file */
  if (*stream == NULL)
    SVN_ERR(packed_stream_open(stream,
                               svn_fs_fs__path_p2l_index(fs, key.revision,
                                                         pool),
                               ffd->block_size, stream_pool));
  else
    packed_stream_seek(*stream, 0);

  /* allocate result data structure */
  result = apr_pcalloc(pool, sizeof(*result));
  
  /* read table sizes and allocate page array */
  SVN_ERR(packed_stream_get(&value, *stream));
  result->first_revision = (svn_revnum_t)value;
  SVN_ERR(packed_stream_get(&value, *stream));
  result->file_size = value;
  SVN_ERR(packed_stream_get(&value, *stream));
  result->page_size = value;
  SVN_ERR(packed_stream_get(&value, *stream));
  result->page_count = (apr_size_t)value;
  result->offsets
    = apr_pcalloc(pool, (result->page_count + 1) * sizeof(*result->offsets));

  /* read page sizes and derive page description offsets from them */
  result->offsets[0] = 0;
  for (i = 0; i < result->page_count; ++i)
    {
      SVN_ERR(packed_stream_get(&value, *stream));
      result->offsets[i+1] = result->offsets[i] + (apr_off_t)value;
    }

  /* correct the offset values */
  offset = packed_stream_offset(*stream);
  for (i = 0; i <= result->page_count; ++i)
    result->offsets[i] += offset;

  /* cache the header data */
  SVN_ERR(svn_cache__set(ffd->p2l_header_cache, &key, result, pool));

  /* return the result */
  *header = result;

  return SVN_NO_ERROR;
}

/* Data structure that describes which p2l page info shall be extracted
 * from the cache and contains the fields that receive the result.
 */
typedef struct p2l_page_info_baton_t
{
  /* input variables */
  /* revision identifying the index file */
  svn_revnum_t revision;

  /* offset within the page in rev / pack file */
  apr_off_t offset;

  /* output variables */
  /* page containing OFFSET */
  apr_size_t page_no;

  /* first revision in this p2l index */
  svn_revnum_t first_revision;

  /* offset within the p2l index file describing this page */
  apr_off_t start_offset;

  /* offset within the p2l index file describing the following page */
  apr_off_t next_offset;

  /* PAGE_NO * PAGE_SIZE (is <= OFFSET) */
  apr_off_t page_start;

  /* total number of pages indexed */
  apr_size_t page_count;

  /* size of each page in pack / rev file */
  apr_uint64_t page_size;
} p2l_page_info_baton_t;

/* From HEADER and the list of all OFFSETS, fill BATON with the page info
 * requested by BATON->OFFSET.
 */
static void
p2l_page_info_copy(p2l_page_info_baton_t *baton,
                   const p2l_header_t *header,
                   const apr_off_t *offsets)
{
  /* if the requested offset is out of bounds, return info for 
   * a zero-sized empty page right behind the last page.
   */
  if (baton->offset / header->page_size < header->page_count)
    {
      baton->page_no = baton->offset / header->page_size;
      baton->start_offset = offsets[baton->page_no];
      baton->next_offset = offsets[baton->page_no + 1];
      baton->page_size = header->page_size;
    }
  else
    {
      baton->page_no = header->page_count;
      baton->start_offset = offsets[baton->page_no];
      baton->next_offset = offsets[baton->page_no];
      baton->page_size = 0;
    }

  baton->first_revision = header->first_revision;
  baton->page_start = (apr_off_t)(header->page_size * baton->page_no);
  baton->page_count = header->page_count;
}

/* Implement svn_cache__partial_getter_func_t: extract the p2l page info
 * requested by BATON and return it in BATON.
 */
static svn_error_t *
p2l_page_info_func(void **out,
                   const void *data,
                   apr_size_t data_len,
                   void *baton,
                   apr_pool_t *result_pool)
{
  /* all the pointers to cached data we need */
  const p2l_header_t *header = data;
  const apr_off_t *offsets
    = svn_temp_deserializer__ptr(header,
                                 (const void *const *)&header->offsets);

  /* copy data from cache to BATON */
  p2l_page_info_copy(baton, header, offsets);
  return SVN_NO_ERROR;
}

/* Read the header data structure of the phys-to-log index for revision
 * BATON->REVISION in FS.  Return in *BATON all info relevant to read the
 * index page for the rev / pack file offset BATON->OFFSET.
 * 
 * To maximize efficiency, use or return the data stream in *STREAM.
 * If *STREAM is yet to be constructed, do so in STREAM_POOL.
 * Use POOL for allocations.
 */
static svn_error_t *
get_p2l_page_info(p2l_page_info_baton_t *baton,
                  svn_fs_fs__packed_number_stream_t **stream,
                  svn_fs_t *fs,
                  apr_pool_t *stream_pool,
                  apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  p2l_header_t *header;
  svn_boolean_t is_cached = FALSE;
  void *dummy = NULL;

  /* look for the header data in our cache */
  pair_cache_key_t key;
  key.revision = base_revision(fs, baton->revision);
  key.second = svn_fs_fs__is_packed_rev(fs, baton->revision);

  SVN_ERR(svn_cache__get_partial(&dummy, &is_cached, ffd->p2l_header_cache,
                                 &key, p2l_page_info_func, baton, pool));
  if (is_cached)
    return SVN_NO_ERROR;

  SVN_ERR(get_p2l_header(&header, stream, fs, baton->revision,
                         stream_pool, pool));

  /* copy the requested info into *BATON */
  p2l_page_info_copy(baton, header, header->offsets);

  return SVN_NO_ERROR;
}

/* Read a mapping entry from the phys-to-log index STREAM and append it to
 * RESULT.  *ITEM_INDEX contains the phys offset for the entry and will
 * be moved forward by the size of entry.  Use POOL for allocations.
 */
static svn_error_t *
read_entry(svn_fs_fs__packed_number_stream_t *stream,
           apr_off_t *item_offset,
           svn_revnum_t *last_revision,
           apr_uint64_t *last_compound,
           apr_array_header_t *result,
           apr_pool_t *pool)
{
  apr_uint64_t value;

  svn_fs_fs__p2l_entry_t entry;

  entry.offset = *item_offset;
  SVN_ERR(packed_stream_get(&value, stream));
  entry.size = (apr_off_t)value;

  SVN_ERR(packed_stream_get(&value, stream));
  *last_compound += decode_int(value);
  
  entry.type = (int)(*last_compound & 7);
  entry.item.number = *last_compound / 8;

  SVN_ERR(packed_stream_get(&value, stream));
  *last_revision += (svn_revnum_t)decode_int(value);
  entry.item.revision = *last_revision;

  SVN_ERR(packed_stream_get(&value, stream));
  entry.fnv1_checksum = (apr_uint32_t)value;

  APR_ARRAY_PUSH(result, svn_fs_fs__p2l_entry_t) = entry;
  *item_offset += entry.size;

  return SVN_NO_ERROR;
}

/* Read the phys-to-log mappings for the cluster beginning at rev file
 * offset PAGE_START from the index for START_REVISION in FS.  The data
 * can be found in the index page beginning at START_OFFSET with the next
 * page beginning at NEXT_OFFSET.  Return the relevant index entries in
 * *ENTRIES.  To maximize efficiency, use or return the data stream in
 * STREAM.  If the latter is yet to be constructed, do so in STREAM_POOL.
 * Use POOL for other allocations.
 */
static svn_error_t *
get_p2l_page(apr_array_header_t **entries,
             svn_fs_fs__packed_number_stream_t **stream,
             svn_fs_t *fs,
             svn_revnum_t start_revision,
             apr_off_t start_offset,
             apr_off_t next_offset,
             apr_off_t page_start,
             apr_uint64_t page_size,
             apr_pool_t *stream_pool,
             apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  apr_uint64_t value;
  apr_array_header_t *result
    = apr_array_make(pool, 16, sizeof(svn_fs_fs__p2l_entry_t));
  apr_off_t item_offset;
  apr_off_t offset;
  svn_revnum_t last_revision;
  apr_uint64_t last_compound;

  /* open index and navigate to page start */
  if (*stream == NULL)
    SVN_ERR(packed_stream_open(stream,
                               svn_fs_fs__path_p2l_index(fs, start_revision,
                                                         pool),
                               ffd->block_size, stream_pool));
  packed_stream_seek(*stream, start_offset);

  /* read rev file offset of the first page entry (all page entries will
   * only store their sizes). */
  SVN_ERR(packed_stream_get(&value, *stream));
  item_offset = (apr_off_t)value;

  /* read all entries of this page */
  last_revision = start_revision;
  last_compound = 0;
  do
    {
      SVN_ERR(read_entry(*stream, &item_offset, &last_revision, &last_compound,
                         result, pool));
      offset = packed_stream_offset(*stream);
    }
  while (offset < next_offset);

  /* if we haven't covered the cluster end yet, we must read the first
   * entry of the next page */
  if (item_offset < page_start + page_size)
    {
      SVN_ERR(packed_stream_get(&value, *stream));
      item_offset = (apr_off_t)value;
      last_revision = start_revision;
      last_compound = 0;
      SVN_ERR(read_entry(*stream, &item_offset, &last_revision, &last_compound,
                         result, pool));
    }

  *entries = result;

  return SVN_NO_ERROR;
}

/* If it cannot be found in FS's caches, read the p2l index page selected
 * by BATON->OFFSET from *STREAM.  If the latter is yet to be constructed,
 * do so in STREAM_POOL.  Don't read the page if it precedes MIN_OFFSET.
 * Set *END to TRUE if the caller should stop refeching.
 *
 * *BATON will be updated with the selected page's info and SCRATCH_POOL
 * will be used for temporary allocations.  If the data is alread in the
 * cache, descrease *LEAKING_BUCKET and increase it otherwise.  With that
 * pattern we will still read all pages from the block even if some of
 * them survived in the cached.
 */
static svn_error_t *
prefetch_p2l_page(svn_boolean_t *end,
                  int *leaking_bucket,
                  svn_fs_t *fs,
                  svn_fs_fs__packed_number_stream_t **stream,
                  p2l_page_info_baton_t *baton,
                  apr_off_t min_offset,
                  apr_pool_t *stream_pool,
                  apr_pool_t *scratch_pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  svn_boolean_t already_cached;
  apr_array_header_t *page;
  svn_fs_fs__page_cache_key_t key = { 0 };

  /* fetch the page info */
  *end = FALSE;
  baton->revision = baton->first_revision;
  SVN_ERR(get_p2l_page_info(baton, stream, fs, stream_pool, scratch_pool));
  if (baton->start_offset < min_offset)
    {
      /* page outside limits -> stop prefetching */
      *end = TRUE;
      return SVN_NO_ERROR;
    }

  /* do we have that page in our caches already? */
  assert(baton->first_revision <= APR_UINT32_MAX);
  key.revision = (apr_uint32_t)baton->first_revision;
  key.is_packed = svn_fs_fs__is_packed_rev(fs, baton->first_revision);
  key.page = baton->page_no;
  SVN_ERR(svn_cache__has_key(&already_cached, ffd->p2l_page_cache,
                             &key, scratch_pool));

  /* yes, already cached */
  if (already_cached)
    {
      /* stop prefetching if most pages are already cached. */
      if (!--*leaking_bucket)
        *end = TRUE;

      return SVN_NO_ERROR;
    }

  ++*leaking_bucket;

  /* read from disk */
  SVN_ERR(get_p2l_page(&page, stream, fs,
                       baton->first_revision,
                       baton->start_offset,
                       baton->next_offset,
                       baton->page_start,
                       baton->page_size,
                       stream_pool,
                       scratch_pool));

  /* and put it into our cache */
  SVN_ERR(svn_cache__set(ffd->p2l_page_cache, &key, page, scratch_pool));

  return SVN_NO_ERROR;
}

/* Lookup & construct the baton and key information that we will need for
 * a P2L page cache lookup.  We want the page covering OFFSET in the rev /
 * pack file containing REVSION in FS.  Return the results in *PAGE_INFO_P
 * and *KEY_P.  Read data through the auto-allocated *STREAM.
 * Use POOL for allocations.
 */
static svn_error_t *
get_p2l_keys(p2l_page_info_baton_t *page_info_p,
             svn_fs_fs__page_cache_key_t *key_p,
             svn_fs_fs__packed_number_stream_t **stream,
             svn_fs_t *fs,
             svn_revnum_t revision,
             apr_off_t offset,
             apr_pool_t *pool)
{
  p2l_page_info_baton_t page_info;
  
  /* request info for the index pages that describes the pack / rev file
   * contents at pack / rev file position OFFSET. */
  page_info.offset = offset;
  page_info.revision = revision;
  SVN_ERR(get_p2l_page_info(&page_info, stream, fs, pool, pool));

  /* if the offset refers to a non-existent page, bail out */
  if (page_info.page_count <= page_info.page_no)
    {
      SVN_ERR(svn_fs_fs__packed_stream_close(*stream));
      return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_OVERFLOW , NULL,
                               _("Offset %s too large in revision %ld"),
                               apr_off_t_toa(pool, offset), revision);
    }

  /* return results */
  if (page_info_p)
    *page_info_p = page_info;
  
  /* construct cache key */
  if (key_p)
    {
      svn_fs_fs__page_cache_key_t key = { 0 };
      assert(page_info.first_revision <= APR_UINT32_MAX);
      key.revision = (apr_uint32_t)page_info.first_revision;
      key.is_packed = svn_fs_fs__is_packed_rev(fs, revision);
      key.page = page_info.page_no;

      *key_p = key;  
    }

  return SVN_NO_ERROR;
}

/* Body of svn_fs_fs__p2l_index_lookup.  Use / autoconstruct *STREAM as
 * your input based on REVISION.
 */
static svn_error_t *
p2l_index_lookup(apr_array_header_t **entries,
                 svn_fs_fs__packed_number_stream_t **stream,
                 svn_fs_t *fs,
                 svn_revnum_t revision,
                 apr_off_t offset,
                 apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  svn_fs_fs__page_cache_key_t key;
  svn_boolean_t is_cached = FALSE;
  p2l_page_info_baton_t page_info;

  /* look for this page in our cache */
  SVN_ERR(get_p2l_keys(&page_info, &key, stream, fs, revision, offset,
                       pool));
  SVN_ERR(svn_cache__get((void**)entries, &is_cached, ffd->p2l_page_cache,
                         &key, pool));
  if (!is_cached)
    {
      svn_boolean_t end;
      apr_pool_t *iterpool = svn_pool_create(pool);
      apr_off_t original_page_start = page_info.page_start;
      int leaking_bucket = 4;
      p2l_page_info_baton_t prefetch_info = page_info;

      apr_off_t max_offset
        = APR_ALIGN(page_info.next_offset, ffd->block_size);
      apr_off_t min_offset
        = APR_ALIGN(page_info.start_offset, ffd->block_size) - ffd->block_size;

      /* Since we read index data in larger chunks, we probably got more
       * page data than we requested.  Parse & cache that until either we
       * encounter pages already cached or reach the end of the buffer.
       */

      /* pre-fetch preceding pages */
      end = FALSE;
      prefetch_info.offset = original_page_start;
      while (prefetch_info.offset >= prefetch_info.page_size && !end)
        {
          prefetch_info.offset -= prefetch_info.page_size;
          SVN_ERR(prefetch_p2l_page(&end, &leaking_bucket, fs, stream,
                                    &prefetch_info, min_offset,
                                    pool, iterpool));
          svn_pool_clear(iterpool);
        }

      /* fetch page from disk and put it into the cache */
      SVN_ERR(get_p2l_page(entries, stream, fs,
                           page_info.first_revision,
                           page_info.start_offset,
                           page_info.next_offset,
                           page_info.page_start,
                           page_info.page_size, pool, pool));

      SVN_ERR(svn_cache__set(ffd->p2l_page_cache, &key, *entries, pool));

      /* pre-fetch following pages */
      end = FALSE;
      leaking_bucket = 4;
      prefetch_info = page_info;
      prefetch_info.offset = original_page_start;
      while (   prefetch_info.next_offset < max_offset
             && prefetch_info.page_no + 1 < prefetch_info.page_count
             && !end)
        {
          prefetch_info.offset += prefetch_info.page_size;
          SVN_ERR(prefetch_p2l_page(&end, &leaking_bucket, fs, stream,
                                    &prefetch_info, min_offset,
                                    pool, iterpool));
          svn_pool_clear(iterpool);
        }

      svn_pool_destroy(iterpool);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__p2l_index_lookup(apr_array_header_t **entries,
                            svn_fs_t *fs,
                            svn_revnum_t revision,
                            apr_off_t offset,
                            apr_pool_t *pool)
{
  svn_fs_fs__packed_number_stream_t *stream = NULL;

  /* look for this page in our cache */
  SVN_ERR(p2l_index_lookup(entries, &stream, fs, revision, offset, pool));

  /* make sure we close files after usage */
  SVN_ERR(svn_fs_fs__packed_stream_close(stream));

  return SVN_NO_ERROR;
}

/* compare_fn_t comparing a svn_fs_fs__p2l_entry_t at LHS with an offset
 * RHS.
 */
static int
compare_p2l_entry_offsets(const void *lhs, const void *rhs)
{
  const svn_fs_fs__p2l_entry_t *entry = (const svn_fs_fs__p2l_entry_t *)lhs;
  apr_off_t offset = *(const apr_off_t *)rhs;

  return entry->offset < offset ? -1 : (entry->offset == offset ? 0 : 1);
}

/* Cached data extraction utility.  DATA is a P2L index page, e.g. an APR
 * array of svn_fs_fs__p2l_entry_t elements.  Return the entry for the item
 * starting at OFFSET or NULL if that's not an the start offset of any item.
 */
static svn_fs_fs__p2l_entry_t *
get_p2l_entry_from_cached_page(const void *data,
                               apr_uint64_t offset,
                               apr_pool_t *pool)
{
  /* resolve all pointer values of in-cache data */
  const apr_array_header_t *page = data;
  apr_array_header_t *entries = apr_pmemdup(pool, page, sizeof(*page));
  int idx;

  entries->elts = (char *)svn_temp_deserializer__ptr(page,
                                     (const void *const *)&page->elts);

  /* search of the offset we want */
  idx = svn_sort__bsearch_lower_bound(&offset, entries,
      (int (*)(const void *, const void *))compare_p2l_entry_offsets);

  /* return it, if it is a perfect match */
  if (idx < entries->nelts)
    {
      svn_fs_fs__p2l_entry_t *entry
        = &APR_ARRAY_IDX(entries, idx, svn_fs_fs__p2l_entry_t);
      if (entry->offset == offset)
        return apr_pmemdup(pool, entry, sizeof(*entry));
    }

  return NULL;
}

/* Implements svn_cache__partial_getter_func_t for P2L index pages, copying
 * the entry for the apr_off_t at BATON into *OUT.  *OUT will be NULL if
 * there is no matching entry in the index page at DATA.
 */
static svn_error_t *
p2l_entry_lookup_func(void **out,
                      const void *data,
                      apr_size_t data_len,
                      void *baton,
                      apr_pool_t *result_pool)
{
  svn_fs_fs__p2l_entry_t *entry
    = get_p2l_entry_from_cached_page(data, *(apr_off_t *)baton, result_pool);

  *out = entry && entry->offset == *(apr_off_t *)baton
       ? apr_pmemdup(result_pool, entry, sizeof(*entry))
       : NULL;

  return SVN_NO_ERROR;
}

static svn_error_t *
p2l_entry_lookup(svn_fs_fs__p2l_entry_t **entry_p,
                 svn_fs_fs__packed_number_stream_t **stream,
                 svn_fs_t *fs,
                 svn_revnum_t revision,
                 apr_off_t offset,
                 apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  svn_fs_fs__page_cache_key_t key = { 0 };
  svn_boolean_t is_cached = FALSE;
  p2l_page_info_baton_t page_info;

  *entry_p = NULL;

  /* look for this info in our cache */
  SVN_ERR(get_p2l_keys(&page_info, &key, stream, fs, revision, offset, pool));
  SVN_ERR(svn_cache__get_partial((void**)entry_p, &is_cached,
                                 ffd->p2l_page_cache, &key,
                                 p2l_entry_lookup_func, &offset, pool));
  if (!is_cached)
    {
      int idx;

      /* do a standard index lookup.  This is will automatically prefetch
       * data to speed up future lookups. */
      apr_array_header_t *entries;
      SVN_ERR(p2l_index_lookup(&entries, stream, fs, revision, offset, pool));

      /* Find the entry that we want. */
      idx = svn_sort__bsearch_lower_bound(&offset, entries, 
          (int (*)(const void *, const void *))compare_p2l_entry_offsets);

      /* return it, if it is a perfect match */
      if (idx < entries->nelts)
        {
          svn_fs_fs__p2l_entry_t *entry
            = &APR_ARRAY_IDX(entries, idx, svn_fs_fs__p2l_entry_t);
          if (entry->offset == offset)
            *entry_p = entry;
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__p2l_entry_lookup(svn_fs_fs__p2l_entry_t **entry_p,
                            svn_fs_t *fs,
                            svn_revnum_t revision,
                            apr_off_t offset,
                            apr_pool_t *pool)
{
  svn_fs_fs__packed_number_stream_t *stream = NULL;

  /* look for this info in our cache */
  SVN_ERR(p2l_entry_lookup(entry_p, &stream, fs, revision, offset, pool));

  /* make sure we close files after usage */
  SVN_ERR(svn_fs_fs__packed_stream_close(stream));

  return SVN_NO_ERROR;
}

/* Implements svn_cache__partial_getter_func_t for P2L headers, setting *OUT
 * to the largest the first offset not covered by this P2L index.
 */
static svn_error_t *
p2l_get_max_offset_func(void **out,
                        const void *data,
                        apr_size_t data_len,
                        void *baton,
                        apr_pool_t *result_pool)
{
  const p2l_header_t *header = data;
  apr_off_t max_offset = header->file_size;
  *out = apr_pmemdup(result_pool, &max_offset, sizeof(max_offset));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__p2l_get_max_offset(apr_off_t *offset,
                              svn_fs_t *fs,
                              svn_revnum_t revision,
                              apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  svn_fs_fs__packed_number_stream_t *stream = NULL;
  p2l_header_t *header;
  svn_boolean_t is_cached = FALSE;
  apr_off_t *offset_p;

  /* look for the header data in our cache */
  pair_cache_key_t key;
  key.revision = base_revision(fs, revision);
  key.second = svn_fs_fs__is_packed_rev(fs, revision);

  SVN_ERR(svn_cache__get_partial((void **)&offset_p, &is_cached,
                                 ffd->p2l_header_cache, &key,
                                 p2l_get_max_offset_func, NULL, pool));
  if (is_cached)
    {
      *offset = *offset_p;
      return SVN_NO_ERROR;
    }

  SVN_ERR(get_p2l_header(&header, &stream, fs, revision, pool, pool));
  *offset = header->file_size;
  
  /* make sure we close files after usage */
  SVN_ERR(svn_fs_fs__packed_stream_close(stream));

  return SVN_NO_ERROR;
}

/*
 * Standard (de-)serialization functions
 */

svn_error_t *
svn_fs_fs__serialize_l2p_header(void **data,
                                apr_size_t *data_len,
                                void *in,
                                apr_pool_t *pool)
{
  l2p_header_t *header = in;
  svn_temp_serializer__context_t *context;
  svn_stringbuf_t *serialized;
  apr_size_t page_count = header->page_table_index[header->revision_count];
  apr_size_t page_table_size = page_count * sizeof(*header->page_table);
  apr_size_t index_size
    = (header->revision_count + 1) * sizeof(*header->page_table_index);
  apr_size_t data_size = sizeof(*header) + index_size + page_table_size;

  /* serialize header and all its elements */
  context = svn_temp_serializer__init(header,
                                      sizeof(*header),
                                      data_size + 32,
                                      pool);

  /* page table index array */
  svn_temp_serializer__add_leaf(context,
                                (const void * const *)&header->page_table_index,
                                index_size);

  /* page table array */
  svn_temp_serializer__add_leaf(context,
                                (const void * const *)&header->page_table,
                                page_table_size);

  /* return the serialized result */
  serialized = svn_temp_serializer__get(context);

  *data = serialized->data;
  *data_len = serialized->len;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__deserialize_l2p_header(void **out,
                                  void *data,
                                  apr_size_t data_len,
                                  apr_pool_t *pool)
{
  l2p_header_t *header = (l2p_header_t *)data;

  /* resolve the pointers in the struct */
  svn_temp_deserializer__resolve(header, (void**)&header->page_table_index);
  svn_temp_deserializer__resolve(header, (void**)&header->page_table);

  /* done */
  *out = header;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__serialize_l2p_page(void **data,
                              apr_size_t *data_len,
                              void *in,
                              apr_pool_t *pool)
{
  l2p_page_t *page = in;
  svn_temp_serializer__context_t *context;
  svn_stringbuf_t *serialized;
  apr_size_t of_table_size = page->entry_count * sizeof(*page->offsets);

  /* serialize struct and all its elements */
  context = svn_temp_serializer__init(page,
                                      sizeof(*page),
                                      of_table_size + sizeof(*page) + 32,
                                      pool);

  /* offsets and sub_items arrays */
  svn_temp_serializer__add_leaf(context,
                                (const void * const *)&page->offsets,
                                of_table_size);

  /* return the serialized result */
  serialized = svn_temp_serializer__get(context);

  *data = serialized->data;
  *data_len = serialized->len;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__deserialize_l2p_page(void **out,
                                void *data,
                                apr_size_t data_len,
                                apr_pool_t *pool)
{
  l2p_page_t *page = data;

  /* resolve the pointers in the struct */
  svn_temp_deserializer__resolve(page, (void**)&page->offsets);

  /* done */
  *out = page;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__serialize_p2l_header(void **data,
                                apr_size_t *data_len,
                                void *in,
                                apr_pool_t *pool)
{
  p2l_header_t *header = in;
  svn_temp_serializer__context_t *context;
  svn_stringbuf_t *serialized;
  apr_size_t table_size = (header->page_count + 1) * sizeof(*header->offsets);

  /* serialize header and all its elements */
  context = svn_temp_serializer__init(header,
                                      sizeof(*header),
                                      table_size + sizeof(*header) + 32,
                                      pool);

  /* offsets array */
  svn_temp_serializer__add_leaf(context,
                                (const void * const *)&header->offsets,
                                table_size);

  /* return the serialized result */
  serialized = svn_temp_serializer__get(context);

  *data = serialized->data;
  *data_len = serialized->len;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__deserialize_p2l_header(void **out,
                                  void *data,
                                  apr_size_t data_len,
                                  apr_pool_t *pool)
{
  p2l_header_t *header = data;

  /* resolve the only pointer in the struct */
  svn_temp_deserializer__resolve(header, (void**)&header->offsets);

  /* done */
  *out = header;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__serialize_p2l_page(void **data,
                              apr_size_t *data_len,
                              void *in,
                              apr_pool_t *pool)
{
  apr_array_header_t *page = in;
  svn_temp_serializer__context_t *context;
  svn_stringbuf_t *serialized;
  apr_size_t table_size = page->elt_size * page->nelts;

  /* serialize array header and all its elements */
  context = svn_temp_serializer__init(page,
                                      sizeof(*page),
                                      table_size + sizeof(*page) + 32,
                                      pool);

  /* items in the array */
  svn_temp_serializer__add_leaf(context,
                                (const void * const *)&page->elts,
                                table_size);

  /* return the serialized result */
  serialized = svn_temp_serializer__get(context);

  *data = serialized->data;
  *data_len = serialized->len;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__deserialize_p2l_page(void **out,
                                void *data,
                                apr_size_t data_len,
                                apr_pool_t *pool)
{
  apr_array_header_t *page = (apr_array_header_t *)data;

  /* resolve the only pointer in the struct */
  svn_temp_deserializer__resolve(page, (void**)&page->elts);

  /* patch up members */
  page->pool = pool;
  page->nalloc = page->nelts;

  /* done */
  *out = page;

  return SVN_NO_ERROR;
}
