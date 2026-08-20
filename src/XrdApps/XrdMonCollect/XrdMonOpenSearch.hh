#ifndef __XRDMONOPENSEARCH_HH__
#define __XRDMONOPENSEARCH_HH__
/******************************************************************************/
/*                                                                            */
/*                 X r d M o n O p e n S e a r c h . h h                      */
/*                                                                            */
/* This file is part of the XRootD software suite.                            */
/*                                                                            */
/* XRootD is free software: you can redistribute it and/or modify it under    */
/* the terms of the GNU Lesser General Public License as published by the     */
/* Free Software Foundation, either version 3 of the License, or (at your     */
/* option) any later version.                                                 */
/*                                                                            */
/* XRootD is distributed in the hope that it will be useful, but WITHOUT      */
/* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or      */
/* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public       */
/* License for more details.                                                  */
/*                                                                            */
/* You should have received a copy of the GNU Lesser General Public License   */
/* along with XRootD in a file called COPYING.LESSER (LGPL license) and file  */
/* COPYING (GPL license).  If not, see <http://www.gnu.org/licenses/>.        */
/******************************************************************************/

#include <atomic>
#include <string>

//-----------------------------------------------------------------------------
//! Append one document to a `_bulk` request body, prefixed with its action
//! metadata line. Free-standing rather than a member because the framing is a
//! property of the request format, not of a connection: the --bulk file sink
//! writes the same NDJSON for later replay with no cluster in sight, and a
//! collector feeding several clusters accumulates one body per distinct
//! (index, action) pair rather than one per destination.
//!
//! @param batch    body to append to.
//! @param index    index or data-stream name for the action metadata.
//! @param create   use the "create" action (data streams reject "index").
//! @param jsonDoc  the document, serialized, with no trailing newline.
//-----------------------------------------------------------------------------

inline void XrdMonBulkAdd(std::string& batch, const std::string& index,
                          bool create, const std::string& jsonDoc)
{
   batch += create ? "{\"create\":{\"_index\":\"" : "{\"index\":{\"_index\":\"";
   batch += index;
   batch += "\"}}\n";
   batch += jsonDoc;
   batch += '\n';
}

//-----------------------------------------------------------------------------
//! XrdMonOpenSearch posts batches of documents to an OpenSearch/Elasticsearch
//! cluster using the _bulk API over HTTP(S). It owns a libcurl handle and
//! retries transient failures with backoff. One instance is used from a single
//! thread (the collector's receive loop).
//-----------------------------------------------------------------------------

class XrdMonOpenSearch
{
public:

//! @param url        base cluster URL, e.g. "https://localhost:9200".
//! @param index      index or data-stream the documents are written to.
//! @param user       basic-auth user (empty for none).
//! @param pass       basic-auth password.
//! @param token      bearer token sent as "Authorization: Bearer <token>"
//!                   (empty for none; takes precedence over basic auth).
//! @param insecure   skip TLS certificate verification when true.
//! @param dataStream target is a data stream: use the "create" bulk action
//!                   (data streams reject "index") and rely on @timestamp.
XrdMonOpenSearch(const std::string& url, const std::string& index,
                 const std::string& user, const std::string& pass,
                 const std::string& token,
                 bool insecure, bool dataStream = false);

~XrdMonOpenSearch();

//! Initialize the libcurl handle. @return true on success, else sets err.
bool Init(std::string& err);

//! Append one document to the index-action header used by Bulk(). The caller
//! accumulates documents and periodically calls Bulk().
void Add(std::string& batch, const std::string& jsonDoc) const;

//! POST the accumulated bulk body. Retries transient errors with backoff.
//! @return true on a 2xx response with no per-item errors; else sets err.
bool Bulk(const std::string& body, std::string& err);

//! The index/data-stream name (for the action metadata line).
const std::string& Index() const {return idx;}

//! Retries applied to a transient failure (curl error, 429, 5xx), with
//! doubling backoff. Set to 0 for a live body when a disk cache is
//! configured: failing fast and spilling to disk beats holding the body for
//! the length of the retry ladder while the queue behind it overflows. The
//! replay path keeps the full ladder, which is also what paces it.
void SetMaxRetry(int n) {maxRetry = n < 0 ? 0 : n;}

//! Abandon the POST in flight and the rest of the retry ladder. Called from
//! the shutdown path: a black-holing endpoint would otherwise hold the
//! process for five 30-second timeouts and be SIGKILLed part way through.
//! Not reversible -- the instance is finished after this.
void Cancel() {cancelled = true;}

private:

void*       curl;      // CURL* (opaque to avoid leaking the header)
std::string bulkURL;   // url + "/_bulk"
std::string idx;
std::string userpwd;   // "user:pass" or empty
std::string authHdr;   // "Authorization: Bearer <token>" or empty
bool        insecure;
bool        useCreate; // "create" bulk action (data stream) vs "index"
int         maxRetry;
std::atomic<bool> cancelled{false};
};
#endif
