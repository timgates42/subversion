/*
 * file_revs.c: handle the file-revs-report request and response
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
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


#define APR_WANT_STRFUNC
#include <apr_want.h> /* for strcmp() */

#include "svn_types.h"
#include "svn_xml.h"
#include "svn_pools.h"
#include "svn_base64.h"

#include "dav_svn.h"

struct file_rev_baton {
  /* this buffers the output for a bit and is automatically flushed,
     at appropriate times, by the Apache filter system. */
  apr_bucket_brigade *bb;

  /* where to deliver the output */
  ap_filter_t *output;

  /* Whether we've written the <S:file-revs-report> header.  Allows for lazy
     writes to support mod_dav-based error handling. */
  svn_boolean_t needs_header;

  /* Used by the delta iwndow handler. */
  svn_txdelta_window_handler_t window_handler;
  void *window_baton;
};

static svn_error_t *send_xml(struct file_rev_baton *frb, 
                             const char *fmt, ...)
{
  apr_status_t apr_err;
  va_list ap;

  va_start(ap, fmt);
  apr_err = apr_brigade_vprintf(frb->bb, ap_filter_flush, 
                                frb->output, fmt, ap);
  va_end(ap);
  if (apr_err)
    return svn_error_create(apr_err, 0, NULL);
  /* ### check for an aborted connection, since the brigade functions
     don't appear to be return useful errors when the connection is
     dropped. */
  if (frb->output->c->aborted)
    return svn_error_create(SVN_ERR_APMOD_CONNECTION_ABORTED, 0, NULL);
  return SVN_NO_ERROR;
}

/* If FRB->needs_header is true, send the "<S:file-revs-report>" start
   tag and set FRB->needs_header to zero.  Else do nothing. */
static svn_error_t *maybe_send_header(struct file_rev_baton *frb)
{
  if (frb->needs_header)
    {
      SVN_ERR(send_xml(frb,
                       DAV_XML_HEADER DEBUG_CR
                       "<S:file-revs-report xmlns:S=\"" SVN_XML_NAMESPACE "\" "
                       "xmlns:D=\"DAV:\">" DEBUG_CR));
      frb->needs_header = FALSE;
    }
  return SVN_NO_ERROR;
}

/* Send a property named NAME with value VAL in an element named ELEM_NAME. 
   Quote NAME and base64-encode VAL if necessary. */
static svn_error_t *
send_prop(struct file_rev_baton *frb, const char *elem_name,
          const char *name, const svn_string_t *val, apr_pool_t *pool)
{
  name = apr_xml_quote_string(pool, name, 1);

  if (svn_xml_is_xml_safe(val->data, val->len))
    {
      svn_stringbuf_t *tmp = NULL;
      svn_xml_escape_cdata_string(&tmp, val, pool);
      val = svn_string_create(tmp->data, pool);
      SVN_ERR(send_xml(frb, "<S:%s name=\"%s\">%s</S:%s>" DEBUG_CR,
                       elem_name, name, val->data, elem_name));
    }
  else
    {
      val = svn_base64_encode_string(val, pool);
      SVN_ERR(send_xml(frb, "<S:%s name=\"%s\" encoding=\"base64\">"
                       "%s</S:%s>" DEBUG_CR,
                       elem_name, name, val->data, elem_name));
    }

  return SVN_NO_ERROR;
}

/* This implements the svn_txdelta_window_handler interface.
   Forward to a more interesting window handler and if we're done, terminate
   the txdelta and file-rev elements. */
static svn_error_t *
delta_window_handler(svn_txdelta_window_t *window, void *baton)
{
  struct file_rev_baton *frb = baton;

  SVN_ERR(frb->window_handler(window, frb->window_baton));

  /* Terminate elements if we're done. */
  if (!window)
    {
      frb->window_handler = NULL;
      frb->window_baton = NULL;
      SVN_ERR(send_xml(frb, "</S:txdelta></S:file-rev>" DEBUG_CR));
    }
  return SVN_NO_ERROR;
}

/* This implements the svn_repos_file_rev_handler_t interface. */
static svn_error_t *
file_rev_handler(void *baton,
                 const char *path,
                 svn_revnum_t revnum,
                 apr_hash_t *rev_props,
                 svn_txdelta_window_handler_t *window_handler,
                 void **window_baton,
                 apr_array_header_t *props,
                 apr_pool_t *pool)
{
  struct file_rev_baton *frb = baton;
  apr_pool_t *subpool = svn_pool_create(pool);
  apr_hash_index_t *hi;
  int i;

  SVN_ERR(maybe_send_header(frb));

  SVN_ERR(send_xml(frb, "<S:file-rev path=\"%s\" rev=\"%ld\">" DEBUG_CR,
                   apr_xml_quote_string(pool, path, 1), revnum));

  /* Send rev props. */
  for (hi = apr_hash_first(pool, rev_props); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      void *val;
      const char *pname;
      const svn_string_t *pval;

      svn_pool_clear(subpool);
      apr_hash_this (hi, &key, NULL, &val);
      pname = key;
      pval = val;
      SVN_ERR(send_prop (frb, "rev-prop", pname, pval, subpool));
    }

  /* Send file prop changes. */
  for (i = 0; i < props->nelts; ++i)
    {
      const svn_prop_t *prop = &APR_ARRAY_IDX(props, i, svn_prop_t);

      if (prop->value)
        SVN_ERR(send_prop(frb, "set-prop", prop->name, prop->value,
                          subpool));
      else
        {
          /* Property was removed. */
          SVN_ERR(send_xml(frb,
                           "<S:remove-prop name=\"%s\"/>" DEBUG_CR,
                           apr_xml_quote_string (subpool, prop->name, 1)));
        }
    }

  /* Maybe send text delta. */
  if (window_handler)
    {
      svn_stream_t *base64_stream;

      base64_stream = dav_svn_make_base64_output_stream(frb->bb, frb->output,
                                                        pool);
      svn_txdelta_to_svndiff(base64_stream, pool, &frb->window_handler,
                             &frb->window_baton);
      *window_handler = delta_window_handler;
      *window_baton = frb;
      /* Start the txdelta element wich will be terminated by the window
         handler together with the file-rev element. */
      SVN_ERR(send_xml(frb, "<S:txdelta>"));
    }
  else
    /* No txdelta, so terminate the element here. */
    SVN_ERR(send_xml (frb, "</S:file-rev>" DEBUG_CR));

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

dav_error *
dav_svn__file_revs_report(const dav_resource *resource,
                          const apr_xml_doc *doc,
                          ap_filter_t *output)
{
  svn_error_t *serr;
  dav_error *derr = NULL;
  apr_status_t apr_err;
  apr_xml_elem *child;
  int ns;
  struct file_rev_baton frb;
  dav_svn_authz_read_baton arb;
  const char *path = NULL;
  
  /* These get determined from the request document. */
  svn_revnum_t start = SVN_INVALID_REVNUM;
  svn_revnum_t end = SVN_INVALID_REVNUM;

  /* Construct the authz read check baton. */
  arb.r = resource->info->r;
  arb.repos = resource->info->repos;

  /* Sanity check. */
  ns = dav_svn_find_ns(doc->namespaces, SVN_XML_NAMESPACE);
  /* ### This is done on other places, but the document element is
     in this namespace, so is this necessary at all? */
  if (ns == -1)
    {
      return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
                           "The request does not contain the 'svn:' "
                           "namespace, so it is not going to have certain "
                           "required elements.");
    }

  /* Get request information. */
  for (child = doc->root->first_child; child != NULL; child = child->next)
    {
      /* if this element isn't one of ours, then skip it */
      if (child->ns != ns)
        continue;

      if (strcmp(child->name, "start-revision") == 0)
        start = SVN_STR_TO_REV(dav_xml_get_cdata(child, resource->pool, 1));
      else if (strcmp(child->name, "end-revision") == 0)
        end = SVN_STR_TO_REV(dav_xml_get_cdata(child, resource->pool, 1));
      else if (strcmp(child->name, "path") == 0)
        {
          /* Convert this relative path to an absolute path in the
             repository. */
          path = apr_pstrdup(resource->pool, resource->info->repos_path);

          if (child->first_cdata.first)
            path = svn_path_join(path, child->first_cdata.first->text,
                                   resource->pool);
        }
      /* else unknown element; skip it */
    }

  frb.bb = apr_brigade_create(resource->pool,
                              output->c->bucket_alloc);
  frb.output = output;
  frb.needs_header = TRUE;

  /* file_rev_handler will send header first time it is called. */

  /* Get the revisions and send them. */
  serr = svn_repos_get_file_revs(resource->info->repos->repos,
                                 path, start, end, dav_svn_authz_read, &arb,
                                 file_rev_handler, &frb, resource->pool);

  if (serr)
    {
      derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR, serr->message,
                                 resource->pool);
      goto cleanup;
    }
  
  if ((serr = maybe_send_header(&frb)))
    {
      derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "Error beginning REPORT reponse",
                                 resource->pool);
      goto cleanup;
    }
    
  if ((serr = send_xml(&frb, "</S:file-revs-report>" DEBUG_CR)))
    {
      derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "Error ending REPORT reponse",
                                 resource->pool);
      goto cleanup;
    }

 cleanup:

  /* Flush the contents of the brigade (returning an error only if we
     don't already have one). */
  if (((apr_err = ap_fflush(output, frb.bb))) && (! derr))
    derr = dav_svn_convert_err(svn_error_create(apr_err, 0, NULL),
                               HTTP_INTERNAL_SERVER_ERROR,
                               "Error flushing brigade",
                               resource->pool);
  return derr;
}
