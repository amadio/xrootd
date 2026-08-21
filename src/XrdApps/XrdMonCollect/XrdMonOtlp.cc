/******************************************************************************/
/*                                                                            */
/*                       X r d M o n O t l p . c c                            */
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

#include <cstring>

#include "XrdVersion.hh"

#include "XrdApps/XrdMonCollect/XrdMonOtlp.hh"
#include "XrdApps/XrdMonCollect/XrdMonUtil.hh"

using json = nlohmann::json;

/******************************************************************************/
/*                       X r d M o n O t l p B a t c h                        */
/******************************************************************************/

namespace
{
// Encode one JSON scalar (or, as a fallback, a nested value) as an OTLP AnyValue.
// 64-bit integers are strings per proto3 JSON; arrays of scalars (e.g. the
// semconv user.roles string array) become a proper arrayValue; nested objects
// (and arrays holding objects) are encoded as their JSON text under stringValue
// (a pragmatic choice that keeps the export flat and lossless without an
// explicit kvlist mapping).
//
json toAnyValue(const json& v)
{
   if (v.is_string())           return json{{"stringValue", v.get<std::string>()}};
   if (v.is_boolean())          return json{{"boolValue",   v.get<bool>()}};
   if (v.is_number_unsigned())
      return json{{"intValue", std::to_string(v.get<std::uint64_t>())}};
   if (v.is_number_integer())
      return json{{"intValue", std::to_string(v.get<std::int64_t>())}};
   if (v.is_number_float())     return json{{"doubleValue", v.get<double>()}};
   if (v.is_array())
      {bool scalars = true;
       for (const json& e : v)
           if (e.is_structured()) {scalars = false; break;}
       if (scalars)
          {json vals = json::array();
           for (const json& e : v) vals.push_back(toAnyValue(e));
           return json{{"arrayValue", json{{"values", std::move(vals)}}}};
          }
      }
   return json{{"stringValue", XrdMonDump(v)}};
}

// Re-encode a flat dotted-key object (the collector's resource/attributes) as an
// OTLP KeyValue array: [{"key":"file.path","value":{"stringValue":"..."}}, ...].
//
json toKeyValues(const json& obj)
{
   json arr = json::array();
   if (obj.is_object())
      for (auto it = obj.begin(); it != obj.end(); ++it)
          arr.push_back({{"key", it.key()}, {"value", toAnyValue(it.value())}});
   return arr;
}

// The instrumentation scope, identical in every block. Written out rather than
// built, so it costs nothing per block; the version is a literal, so adjacent
// string concatenation does the whole thing at compile time.
const char kScopeBlock[] =
   "\"scope\":{\"name\":\"xrdmoncollect\",\"version\":\"" XrdVERSION "\"}";

// What one resource block costs beyond its own content: the scope object,
// which is the bulk of it, plus room for the brackets and keys around it and
// for the request envelope. An upper bound on purpose -- the figure it feeds
// is a memory bound, so erring high is the safe direction, and pinning it to
// the character would only make the assembly below harder to change.
const std::size_t kBlockOverhead = sizeof(kScopeBlock) + 96;

// Ceiling on a record buffer kept warm between bodies. The caller ships well
// below this, so reaching it means an outlier burst whose buffer is not worth
// holding on to.
const std::size_t kKeepBytes = 8u << 20;

// A present string field, else empty.
std::string strField(const json& doc, const char* key)
{
   auto it = doc.find(key);
   return (it != doc.end() && it->is_string()) ? it->get<std::string>()
                                               : std::string();
}
}

void XrdMonOtlpBatch::add(const json& doc)
{
   const bool isSpan = doc.contains("kind");
   auto& groups = isSpan ? spanGroups : logGroups;

// The resource object is the group key. Keying on its serialization instead
// would re-serialize it for every single document; as an object it is copied
// only when a group is created, i.e. once per distinct resource. nlohmann
// objects are ordered maps, so the comparison is a total order and two
// resources built in a different key order still land in the same group.
//
   const json  empty = json::object();
   const json& res   = doc.contains("resource") ? doc["resource"] : empty;
   Group& g = groups[res];
   XrdMonPublished<std::size_t>& acct = isSpan ? spanSize : logSize;
   if (g.resource.empty())                        // once per resource, ever
      g.resource = XrdMonDump(toKeyValues(res));
   if (g.records.empty())                         // once per resource per body
      acct += g.resource.size() + kBlockOverhead;

   json rec;
   json attrs = doc.contains("attributes")
              ? toKeyValues(doc["attributes"]) : json::array();
   if (isSpan)
      {// The span document already carries OTLP-compatible name/kind/status and
       // the *UnixNano times; pass them through and attach the id linkage.
       rec["traceId"] = strField(doc, "traceId");
       rec["spanId"]  = strField(doc, "spanId");
       std::string parent = strField(doc, "parentSpanId");
       if (!parent.empty()) rec["parentSpanId"] = parent;
       if (doc.contains("name"))              rec["name"] = doc["name"];
       if (doc.contains("kind"))              rec["kind"] = doc["kind"];
       if (doc.contains("startTimeUnixNano")) rec["startTimeUnixNano"] = doc["startTimeUnixNano"];
       if (doc.contains("endTimeUnixNano"))   rec["endTimeUnixNano"]   = doc["endTimeUnixNano"];
       if (doc.contains("status"))            rec["status"] = doc["status"];
       rec["attributes"] = std::move(attrs);
      }
      else
      {if (doc.contains("timeUnixNano"))         rec["timeUnixNano"] = doc["timeUnixNano"];
       if (doc.contains("observedTimeUnixNano")) rec["observedTimeUnixNano"] = doc["observedTimeUnixNano"];
       if (doc.contains("severityNumber"))       rec["severityNumber"] = doc["severityNumber"];
       if (doc.contains("severityText"))         rec["severityText"] = doc["severityText"];
       std::string tr = strField(doc, "traceId"), sp = strField(doc, "spanId");
       if (!tr.empty()) rec["traceId"] = tr;
       if (!sp.empty()) rec["spanId"]  = sp;
       // The event name: the top-level LogRecord EventName field (semconv home
       // since the event.name attribute was deprecated) plus a human-readable
       // body. The attribute duplicate rides along in attrs until Loki can
       // query the field itself (grafana/loki#19260).
       std::string ev = strField(doc, "eventName");
       if (ev.empty())
          {// Older cached documents predate the top-level field.
           auto ait = doc.find("attributes");
           if (ait != doc.end())
              {auto eit = ait->find("event.name");
               if (eit != ait->end() && eit->is_string())
                  ev = eit->get<std::string>();
              }
          }
       if (!ev.empty())
          {rec["eventName"] = ev;
           rec["body"] = json{{"stringValue", std::move(ev)}};
          }
       rec["attributes"] = std::move(attrs);
      }

   const std::string text = XrdMonDump(rec);
   if (!g.records.empty()) {g.records += ','; acct++;}
   g.records += text;
   acct += text.size();
   (isSpan ? spanRecs : logRecs)++;
}

std::string XrdMonOtlpBatch::takeBody(std::map<nlohmann::json, Group>& groups,
                                      const char* resourceKey,
                                      const char* scopeKey,
                                      const char* recordsKey)
{
// nlohmann objects are sorted maps, so the body this used to build came out in
// alphabetical key order; reproduce it rather than leaving the layout to drift
// on a change nobody meant to make. The order differs between the two signals:
// "logRecords" sorts before "scope", "spans" after it.
//
   const bool recsFirst = std::strcmp(recordsKey, "scope") < 0;

   std::size_t need = 32;
   for (const auto& kv : groups)
       need += kv.second.resource.size() + kv.second.records.size() + 128;

   std::string body;
   body.reserve(need);
   body += "{\"";  body += resourceKey;  body += "\":[";

   bool first = true;
   for (auto it = groups.begin(); it != groups.end(); )
       {Group& g = it->second;
        if (g.records.empty())
           {// Nothing since the last take: this resource has gone quiet, so
            // stop holding its buffer.
            it = groups.erase(it);
            continue;
           }
        if (!first) body += ',';
        first = false;
        body += "{\"resource\":{\"attributes\":";
        body += g.resource;
        body += "},\"";  body += scopeKey;  body += "\":[{";
        if (recsFirst)
           {body += '"'; body += recordsKey; body += "\":[";
            body += g.records;
            body += "],";
           }
        body += kScopeBlock;
        if (!recsFirst)
           {body += ",\""; body += recordsKey; body += "\":[";
            body += g.records;
            body += ']';
           }
        body += "}]}";

// Keep the group, and with it the buffer's capacity. Rebuilding a
// multi-megabyte string by repeated append, every cycle, through a heap the
// decoder is churning at the same time, costs more than everything else here
// put together -- measured at 6 us per document in the running collector,
// against nothing at all in a benchmark where the batch is the only thing
// allocating. The resource text does not need serializing again either.
//
        if (g.records.capacity() > kKeepBytes) g.records = std::string();
           else g.records.clear();
        ++it;
       }
   body += "]}";

   (&groups == &logGroups ? logSize : spanSize) = 0;
   (&groups == &logGroups ? logRecs : spanRecs) = 0;
   return body;
}

std::string XrdMonOtlpBatch::takeLogsBody()
{
   return takeBody(logGroups, "resourceLogs", "scopeLogs", "logRecords");
}

std::string XrdMonOtlpBatch::takeTracesBody()
{
   return takeBody(spanGroups, "resourceSpans", "scopeSpans", "spans");
}

/******************************************************************************/
/*                           X r d M o n O t l p                              */
/******************************************************************************/

#ifdef XRDMON_HAVE_CURL

#include <ctime>
#include <unistd.h>

#include <curl/curl.h>

namespace
{
size_t otlpDiscardCB(char* ptr, size_t sz, size_t nm, void* userp)
{
   ((std::string*)userp)->append(ptr, sz * nm);
   return sz * nm;
}

// A non-zero return aborts the POST. curl calls this at least once a
// second even while a connection is hanging, which is what lets Cancel()
// interrupt a POST into a black hole rather than waiting out its timeout.
int otlpAbortCB(void* flag, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
   return ((std::atomic<bool>*)flag)->load() ? 1 : 0;
}
}

XrdMonOtlp::XrdMonOtlp(const std::string& url, const std::string& token,
                       bool insec)
          : curl(nullptr), insecure(insec), maxRetry(4)
{
   std::string base = url;
   if (!base.empty() && base.back() == '/') base.pop_back();
   logsURL   = base + "/v1/logs";
   tracesURL = base + "/v1/traces";
   if (!token.empty()) authHdr = "Authorization: Bearer " + token;
}

XrdMonOtlp::~XrdMonOtlp()
{
   if (curl) curl_easy_cleanup((CURL*)curl);
}

bool XrdMonOtlp::Init(std::string& err)
{
   curl_global_init(CURL_GLOBAL_DEFAULT);
   curl = curl_easy_init();
   if (!curl) {err = "failed to create curl handle"; return false;}
   return true;
}

XrdMonOtlp::PostResult XrdMonOtlp::post(const std::string& url,
                                        const std::string& body,
                                        std::string& err)
{
   CURL* c = (CURL*)curl;

   struct curl_slist* hdrs = nullptr;
   hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
   // Suppress Expect: 100-continue (older curl sends it for bodies > 1KB):
   // a receiver that never answers the interim response stalls every POST by
   // curl's 1s expect timeout, throttling the export pipeline.
   hdrs = curl_slist_append(hdrs, "Expect:");
   if (!authHdr.empty()) hdrs = curl_slist_append(hdrs, authHdr.c_str());

   int        backoff = 1;
   PostResult result  = PostResult::Transient;
   for (int attempt = 0; attempt <= maxRetry; attempt++)
       {if (cancelled) {err = "cancelled"; break;}
        std::string resp;
        curl_easy_reset(c);
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, otlpDiscardCB);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, otlpAbortCB);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &cancelled);
        if (insecure)
           {curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
           }

        CURLcode rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

        if (rc == CURLE_OK && code >= 200 && code < 300)
           {err.clear(); result = PostResult::Ok; break;}

        if (rc != CURLE_OK) err = curl_easy_strerror(rc);
           else
           {err = "HTTP " + std::to_string(code) + " from OTLP endpoint";
            // The response body is the receiver's own account of what it
            // refused (e.g. Loki names the failed validation and the stream).
            // Fold it onto the error line, control characters and all.
            if (!resp.empty())
               {for (char& ch : resp)
                    if ((unsigned char)ch < 0x20) ch = ' ';
                err += ": " + resp;
               }
           }

        bool transient = (rc != CURLE_OK) || code == 429 || (code >= 500);
        if (!transient) {result = PostResult::Rejected; break;}
        result = PostResult::Transient;
        if (attempt == maxRetry) break;
        // Sleep in slices so Cancel() is not held off for the whole backoff.
        for (int i = 0; i < backoff && !cancelled; i++) sleep(1);
        if (backoff < 16) backoff *= 2;
       }

   curl_slist_free_all(hdrs);
   return result;
}

XrdMonOtlp::PostResult XrdMonOtlp::PostLogs(const std::string& body,
                                            std::string& err)
{
   return post(logsURL, body, err);
}

XrdMonOtlp::PostResult XrdMonOtlp::PostTraces(const std::string& body,
                                              std::string& err)
{
   return post(tracesURL, body, err);
}

#endif
