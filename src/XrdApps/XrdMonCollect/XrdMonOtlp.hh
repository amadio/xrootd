#ifndef __XRDMONOTLP_HH__
#define __XRDMONOTLP_HH__
/******************************************************************************/
/*                                                                            */
/*                       X r d M o n O t l p . h h                            */
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
#include <cstddef>
#include <map>
#include <string>

#include "XrdOuc/XrdOucJson.hh"

#include "XrdApps/XrdMonCollect/XrdMonUtil.hh"

//-----------------------------------------------------------------------------
//! XrdMonOtlpBatch converts the collector's decoded documents (the nested-JSON
//! OpenTelemetry-data-model form emitted by XrdMonDecode) into OTLP/JSON export
//! requests: an ExportLogsServiceRequest (resourceLogs -> scopeLogs ->
//! logRecords) for the concluded-operation logs, and an ExportTraceServiceRequest
//! (resourceSpans -> scopeSpans -> spans) for the --spans span documents.
//!
//! Records are grouped by their resource so one request carries one resource
//! block per server incarnation. The lightweight nested attributes/resource
//! objects are re-encoded into the OTLP typed KeyValue arrays; the log/span
//! envelope fields (severity, timeUnixNano, traceId/spanId, name/kind/status)
//! are already OTLP-compatible in the source document and pass through.
//!
//! What accumulates between flushes is the serialized text, not a tree. A
//! materialised nlohmann::json runs several times the size of its own output,
//! and this is memory the --max-memory controller cannot evict: it belongs to
//! neither the correlation state nor the output queues. Holding text instead
//! costs one small dump per record -- the same bytes that would have been
//! dumped at flush, plus an allocation -- and in return the accumulator is
//! about as large as the body it will produce, and knows exactly how large
//! that is.
//!
//! No network dependency, so it is unit-testable on its own; the HTTP transport
//! lives in XrdMonOtlp below.
//-----------------------------------------------------------------------------

class XrdMonOtlpBatch
{
public:

//! Add one decoded document. A document carrying a "kind" field is a span (it
//! joins the traces request); any other document is a log record.
void add(const nlohmann::json& doc);

bool haveLogs()   const {return logRecs  != 0;}
bool haveTraces() const {return spanRecs != 0;}

//! Assemble and clear the accumulated logs (resp. traces) as one OTLP/JSON
//! ExportLogsServiceRequest (resp. ExportTraceServiceRequest) body.
std::string takeLogsBody();
std::string takeTracesBody();

void clear()
     {logGroups.clear(); spanGroups.clear();
      logSize = spanSize = logRecs = spanRecs = 0;}

//! Bytes the accumulated logs (resp. traces) hold: the length of the body
//! they will produce, rounded up by the fixed envelope. Measured rather than
//! estimated, which is what lets the caller's byte bound bound actual memory.
//!
//! Published, because the metrics exporter reads them from its own thread
//! while the serializer adds to the batch (otlp_batch_bytes).
std::size_t logBytes()   const {return logSize;}
std::size_t traceBytes() const {return spanSize;}

private:

// One resource block: the resource attributes as a serialized OTLP KeyValue
// array, and the serialized log records / spans sharing them, comma separated
// and ready to drop between brackets.
struct Group
{
   std::string resource;
   std::string records;
};

// Keyed by the resource object itself, so identical resources share a block
// without the key having to be serialized once per document. There is one key
// per server incarnation, so the trees kept here are bounded by the number of
// servers reporting, not by the document rate.
//
// A group outlives the body it contributed to, keeping its serialized resource
// and its record buffer's capacity; one that contributes nothing to the next
// body is dropped. Hence the separate record counts: "has a group" no longer
// means "has anything to send".
std::map<nlohmann::json, Group> logGroups;
std::map<nlohmann::json, Group> spanGroups;
std::size_t logRecs  = 0;
std::size_t spanRecs = 0;
XrdMonPublished<std::size_t> logSize;
XrdMonPublished<std::size_t> spanSize;

std::string takeBody(std::map<nlohmann::json, Group>& groups,
                     const char* resourceKey, const char* scopeKey,
                     const char* recordsKey);
};

#ifdef XRDMON_HAVE_CURL
//-----------------------------------------------------------------------------
//! XrdMonOtlp posts OTLP/JSON bodies to an OTLP/HTTP receiver (an OpenTelemetry
//! Collector, Grafana Alloy, or any OTLP endpoint): logs to <url>/v1/logs and
//! traces to <url>/v1/traces, with Content-Type application/json. It owns a
//! libcurl handle and retries transient failures with backoff. One instance is
//! used from a single (output) thread.
//-----------------------------------------------------------------------------

class XrdMonOtlp
{
public:

//! @param url      base OTLP/HTTP endpoint, e.g. "http://collector:4318". The
//!                 signal paths /v1/logs and /v1/traces are appended.
//! @param token    bearer token sent as "Authorization: Bearer <token>"
//!                 (empty for none).
//! @param insecure skip TLS certificate verification when true.
XrdMonOtlp(const std::string& url, const std::string& token, bool insecure);
~XrdMonOtlp();

//! Initialize the libcurl handle. @return true on success, else sets err.
bool Init(std::string& err);

bool PostLogs(const std::string& body, std::string& err);
bool PostTraces(const std::string& body, std::string& err);

//! Retries applied to a transient failure (curl error, 429, 5xx), with
//! doubling backoff. Set to 0 for a live body when a disk cache is
//! configured: failing fast and spilling to disk beats holding the body for
//! the length of the retry ladder while the queue behind it overflows. The
//! replay path keeps the full ladder, which is also what paces it.
void SetMaxRetry(int n) {maxRetry = n < 0 ? 0 : n;}

//! Abandon the POST in flight and the rest of the retry ladder; see
//! XrdMonOpenSearch::Cancel(). Not reversible.
void Cancel() {cancelled = true;}

private:

bool post(const std::string& url, const std::string& body, std::string& err);

void*       curl;       // CURL* (opaque to avoid leaking the header)
std::string logsURL;
std::string tracesURL;
std::string authHdr;    // "Authorization: Bearer <token>" or empty
bool        insecure;
int         maxRetry;
std::atomic<bool> cancelled{false};
};
#endif
#endif
