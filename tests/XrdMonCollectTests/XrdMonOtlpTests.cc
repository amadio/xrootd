//------------------------------------------------------------------------------
// Unit tests for XrdMonOtlpBatch: the conversion of the collector's nested-JSON
// documents into OTLP/JSON export requests (logs and traces). The HTTP transport
// (XrdMonOtlp) needs libcurl and is not exercised here.
//------------------------------------------------------------------------------

#include "XrdApps/XrdMonCollect/XrdMonOtlp.hh"
#include "XrdOuc/XrdOucJson.hh"
#include "XrdVersion.hh"

#include <gtest/gtest.h>

using json = nlohmann::json;

namespace
{
// Find an OTLP KeyValue by key in an attributes array; nullptr if absent.
const json* kv(const json& arr, const std::string& key)
{
   for (const auto& e : arr)
       if (e.value("key", std::string()) == key) return &e;
   return nullptr;
}
}

TEST(XrdMonOtlp, LogRecordEncoding)
{
   json doc;
   doc["resource"]["service.name"]              = "xrootd";
   doc["resource"]["server.address"]            = "srv1";
   doc["resource"]["xrootd.server.incarnation"] = 1700000000;
   doc["scope"]["name"]          = "xrdmoncollect";
   doc["@timestamp"]             = "2026-07-02T10:00:32Z";
   doc["timeUnixNano"]           = "1751450432000000000";
   doc["severityNumber"]         = 9;
   doc["severityText"]           = "INFO";
   doc["traceId"]                = "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371";
   doc["spanId"]                 = "3ab4c1d2e3f40516";
   doc["attributes"]["event.name"]                 = "xrootd.read";
   doc["attributes"]["file.path"]                  = "/store/data/file.root";
   doc["attributes"]["xrootd.read_bytes"] = 10485760;
   doc["attributes"]["xrootd.is_local"]   = true;

   XrdMonOtlpBatch b;
   b.add(doc);
   EXPECT_TRUE(b.haveLogs());
   EXPECT_FALSE(b.haveTraces());

   json body = json::parse(b.takeLogsBody());
   ASSERT_TRUE(body.contains("resourceLogs"));
   ASSERT_EQ(body["resourceLogs"].size(), 1u);
   const json& rl = body["resourceLogs"][0];

   // Resource attributes are re-encoded as typed OTLP KeyValues.
   const json& ra = rl["resource"]["attributes"];
   const json* svc = kv(ra, "service.name");
   ASSERT_NE(svc, nullptr);
   EXPECT_EQ((*svc)["value"]["stringValue"], "xrootd");
   const json* inc = kv(ra, "xrootd.server.incarnation");
   ASSERT_NE(inc, nullptr);
   EXPECT_EQ((*inc)["value"]["intValue"], "1700000000");   // 64-bit int as string

   const json& sl = rl["scopeLogs"][0];
   EXPECT_EQ(sl["scope"]["name"], "xrdmoncollect");
   ASSERT_EQ(sl["logRecords"].size(), 1u);
   const json& lr = sl["logRecords"][0];
   EXPECT_EQ(lr["severityNumber"], 9);
   EXPECT_EQ(lr["severityText"], "INFO");
   EXPECT_EQ(lr["timeUnixNano"], "1751450432000000000");
   EXPECT_EQ(lr["traceId"], "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371");
   EXPECT_EQ(lr["spanId"], "3ab4c1d2e3f40516");
   EXPECT_EQ(lr["body"]["stringValue"], "xrootd.read");

   const json& la = lr["attributes"];
   const json* fp = kv(la, "file.path");
   ASSERT_NE(fp, nullptr);
   EXPECT_EQ((*fp)["value"]["stringValue"], "/store/data/file.root");
   const json* rb = kv(la, "xrootd.read_bytes");
   ASSERT_NE(rb, nullptr);
   EXPECT_EQ((*rb)["value"]["intValue"], "10485760");
   const json* il = kv(la, "xrootd.is_local");
   ASSERT_NE(il, nullptr);
   EXPECT_EQ((*il)["value"]["boolValue"], true);
}

TEST(XrdMonOtlp, SpanEncoding)
{
   json doc;
   doc["resource"]["service.name"] = "xrootd";
   doc["scope"]["name"]            = "xrdmoncollect";
   doc["traceId"]                  = "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371";
   doc["spanId"]                   = "3ab4c1d2e3f40516";
   doc["parentSpanId"]             = "1122334455667788";
   doc["name"]                     = "read";
   doc["kind"]                     = "SPAN_KIND_SERVER";
   doc["startTimeUnixNano"]        = "1700000000000000000";
   doc["endTimeUnixNano"]          = "1700000082000000000";
   doc["status"]["code"]           = "STATUS_CODE_ERROR";
   doc["status"]["message"]        = "permission denied";
   doc["attributes"]["file.path"]  = "/store/data/x.root";

   XrdMonOtlpBatch b;
   b.add(doc);
   EXPECT_TRUE(b.haveTraces());
   EXPECT_FALSE(b.haveLogs());

   json body = json::parse(b.takeTracesBody());
   ASSERT_TRUE(body.contains("resourceSpans"));
   ASSERT_EQ(body["resourceSpans"].size(), 1u);
   const json& sp = body["resourceSpans"][0]["scopeSpans"][0]["spans"][0];
   EXPECT_EQ(sp["name"], "read");
   EXPECT_EQ(sp["kind"], "SPAN_KIND_SERVER");
   EXPECT_EQ(sp["startTimeUnixNano"], "1700000000000000000");
   EXPECT_EQ(sp["endTimeUnixNano"], "1700000082000000000");
   EXPECT_EQ(sp["parentSpanId"], "1122334455667788");
   EXPECT_EQ(sp["status"]["code"], "STATUS_CODE_ERROR");
   EXPECT_EQ(sp["status"]["message"], "permission denied");
   const json* fp = kv(sp["attributes"], "file.path");
   ASSERT_NE(fp, nullptr);
   EXPECT_EQ((*fp)["value"]["stringValue"], "/store/data/x.root");
}

// Records sharing a resource collapse into one resource block; taking a body
// clears the accumulator for that signal.
TEST(XrdMonOtlp, GroupsByResourceAndResets)
{
   json doc;
   doc["resource"]["service.name"] = "xrootd";
   doc["severityText"]             = "INFO";
   doc["attributes"]["event.name"] = "xrootd.read";

   XrdMonOtlpBatch b;
   b.add(doc);
   b.add(doc);

   json body = json::parse(b.takeLogsBody());
   ASSERT_EQ(body["resourceLogs"].size(), 1u);
   EXPECT_EQ(body["resourceLogs"][0]["scopeLogs"][0]["logRecords"].size(), 2u);
   EXPECT_FALSE(b.haveLogs());   // cleared by takeLogsBody()
}

// Distinct resources must not be merged: two servers reporting the same event
// are two resource blocks, each with its own attribute set.
TEST(XrdMonOtlp, DistinctResourcesSplitIntoBlocks)
{
   json a, b;
   a["resource"]["service.name"]   = "xrootd";
   a["resource"]["server.address"] = "srv1";
   a["attributes"]["event.name"]   = "xrootd.read";
   b = a;
   b["resource"]["server.address"] = "srv2";

   XrdMonOtlpBatch batch;
   batch.add(a);
   batch.add(b);
   batch.add(a);

   json body = json::parse(batch.takeLogsBody());
   ASSERT_EQ(body["resourceLogs"].size(), 2u);

   // Two records under srv1, one under srv2, whichever order they come out in.
   std::size_t one = 0, two = 0;
   for (const json& rl : body["resourceLogs"])
       {const json* sa = kv(rl["resource"]["attributes"], "server.address");
        ASSERT_NE(sa, nullptr);
        const std::size_t n = rl["scopeLogs"][0]["logRecords"].size();
        if ((*sa)["value"]["stringValue"] == "srv1") one = n; else two = n;
       }
   EXPECT_EQ(one, 2u);
   EXPECT_EQ(two, 1u);
}

// The group key is the resource object, not a serialization of it, so two
// documents whose resource keys were inserted in a different order still share
// a block. (nlohmann objects are ordered maps, which is what makes this hold --
// it is the property the old dump-as-key relied on implicitly.)
TEST(XrdMonOtlp, ResourceKeyOrderDoesNotSplitGroups)
{
   json a, b;
   a["resource"]["service.name"]   = "xrootd";
   a["resource"]["server.address"] = "srv1";
   a["attributes"]["event.name"]   = "xrootd.read";

   b["resource"]["server.address"] = "srv1";      // reverse insertion order
   b["resource"]["service.name"]   = "xrootd";
   b["attributes"]["event.name"]   = "xrootd.read";

   XrdMonOtlpBatch batch;
   batch.add(a);
   batch.add(b);

   json body = json::parse(batch.takeLogsBody());
   ASSERT_EQ(body["resourceLogs"].size(), 1u);
   EXPECT_EQ(body["resourceLogs"][0]["scopeLogs"][0]["logRecords"].size(), 2u);
}

// A resource attribute written as a signed int and one that arrived through a
// parse as unsigned compare numerically, so a document replayed from the disk
// cache groups with a freshly decoded one instead of opening a second block.
TEST(XrdMonOtlp, IntegerSignednessDoesNotSplitGroups)
{
   json a;
   a["resource"]["service.name"]              = "xrootd";
   a["resource"]["xrootd.server.incarnation"] = 1700000000;      // signed
   a["attributes"]["event.name"]              = "xrootd.read";

   json b = a;
   b["resource"]["xrootd.server.incarnation"] = 1700000000u;     // unsigned
   ASSERT_NE(a["resource"]["xrootd.server.incarnation"].type(),
             b["resource"]["xrootd.server.incarnation"].type());

   XrdMonOtlpBatch batch;
   batch.add(a);
   batch.add(b);

   json body = json::parse(batch.takeLogsBody());
   ASSERT_EQ(body["resourceLogs"].size(), 1u);
   EXPECT_EQ(body["resourceLogs"][0]["scopeLogs"][0]["logRecords"].size(), 2u);
}

// Logs and spans are accumulated apart, so taking one body leaves the other
// intact -- with several destinations sharing one batch, a partial take would
// silently halve somebody's export.
TEST(XrdMonOtlp, TakingLogsLeavesTracesIntact)
{
   json log, span;
   log["resource"]["service.name"] = "xrootd";
   log["attributes"]["event.name"] = "xrootd.read";
   span["resource"]["service.name"] = "xrootd";
   span["kind"] = "SPAN_KIND_SERVER";
   span["name"] = "read";

   XrdMonOtlpBatch batch;
   batch.add(log);
   batch.add(span);
   ASSERT_TRUE(batch.haveLogs());
   ASSERT_TRUE(batch.haveTraces());

   json body = json::parse(batch.takeLogsBody());
   EXPECT_EQ(body["resourceLogs"].size(), 1u);
   EXPECT_FALSE(batch.haveLogs());
   ASSERT_TRUE(batch.haveTraces());

   json traces = json::parse(batch.takeTracesBody());
   EXPECT_EQ(traces["resourceSpans"][0]["scopeSpans"][0]["spans"].size(), 1u);
   EXPECT_FALSE(batch.haveTraces());
}

//------------------------------------------------------------------------------
// The body is now assembled as text rather than built as a tree and dumped, so
// its layout is written out by hand and nothing above would notice it drifting
// -- every test up to here parses the result, and parsing is order-blind.
//------------------------------------------------------------------------------

// Byte for byte, including the key order a sorted nlohmann object produced.
// Note where "scope" falls: after "logRecords" here, before "spans" below.
TEST(XrdMonOtlp, LogsBodyLayoutIsExact)
{
   json doc;
   doc["resource"]["service.name"] = "xrootd";
   doc["attributes"]["file.path"]  = "/f.root";
   doc["eventName"]                = "xrootd.read";
   doc["severityNumber"]           = 9;

   XrdMonOtlpBatch b;
   b.add(doc);

   const std::string want =
      "{\"resourceLogs\":[{\"resource\":{\"attributes\":"
        "[{\"key\":\"service.name\",\"value\":{\"stringValue\":\"xrootd\"}}]},"
      "\"scopeLogs\":[{"
        "\"logRecords\":[{"
          "\"attributes\":[{\"key\":\"file.path\","
                           "\"value\":{\"stringValue\":\"/f.root\"}}],"
          "\"body\":{\"stringValue\":\"xrootd.read\"},"
          "\"eventName\":\"xrootd.read\","
          "\"severityNumber\":9}],"
        "\"scope\":{\"name\":\"xrdmoncollect\",\"version\":\"" XrdVERSION "\"}"
      "}]}]}";
   EXPECT_EQ(b.takeLogsBody(), want);
}

TEST(XrdMonOtlp, TracesBodyLayoutIsExact)
{
   json doc;
   doc["resource"]["service.name"] = "xrootd";
   doc["attributes"]["file.path"]  = "/f.root";
   doc["kind"]                     = 3;
   doc["name"]                     = "xrootd.file";

   XrdMonOtlpBatch b;
   b.add(doc);

   const std::string want =
      "{\"resourceSpans\":[{\"resource\":{\"attributes\":"
        "[{\"key\":\"service.name\",\"value\":{\"stringValue\":\"xrootd\"}}]},"
      "\"scopeSpans\":[{"
        "\"scope\":{\"name\":\"xrdmoncollect\",\"version\":\"" XrdVERSION "\"},"
        "\"spans\":[{"
          "\"attributes\":[{\"key\":\"file.path\","
                           "\"value\":{\"stringValue\":\"/f.root\"}}],"
          "\"kind\":3,"
          "\"name\":\"xrootd.file\","
          "\"spanId\":\"\","
          "\"traceId\":\"\"}]"
      "}]}]}";
   EXPECT_EQ(b.takeTracesBody(), want);
}

// Records within a group are comma separated, and a second group is a second
// element of the outer array -- the two places a hand-built body can lose or
// gain a separator.
TEST(XrdMonOtlp, SeparatorsSurviveSeveralRecordsAndGroups)
{
   json a;
   a["resource"]["server.address"] = "srv1";
   a["attributes"]["event.name"]   = "xrootd.read";
   json b2 = a;
   b2["resource"]["server.address"] = "srv2";

   XrdMonOtlpBatch b;
   b.add(a); b.add(a); b.add(a);
   b.add(b2); b.add(b2);

   const std::string text = b.takeLogsBody();
   json body = json::parse(text);          // no stray or missing commas
   ASSERT_EQ(body["resourceLogs"].size(), 2u);
   EXPECT_EQ(body["resourceLogs"][0]["scopeLogs"][0]["logRecords"].size(), 3u);
   EXPECT_EQ(body["resourceLogs"][1]["scopeLogs"][0]["logRecords"].size(), 2u);
}

// The byte count is what the caller's coalescing bound consults, so an
// estimate that drifts from the body it is bounding would silently loosen the
// only limit on how large the accumulator may grow.
TEST(XrdMonOtlp, ByteCountTracksTheBodyItWillProduce)
{
   json doc;
   doc["resource"]["server.address"] = "srv1";
   doc["attributes"]["file.path"]    = std::string("/store/") +
                                       std::string(200, 'z') + ".root";
   doc["attributes"]["event.name"]   = "xrootd.read";

   XrdMonOtlpBatch b;
   EXPECT_EQ(b.logBytes(), 0u);
   EXPECT_EQ(b.traceBytes(), 0u);

   for (int i = 0; i < 500; i++) b.add(doc);
   const std::size_t claimed = b.logBytes();
   EXPECT_EQ(b.traceBytes(), 0u);          // spans accounted separately

   const std::size_t actual = b.takeLogsBody().size();
   EXPECT_GE(claimed, actual);             // never under-reports what it holds
   EXPECT_LT(claimed - actual, 256u);      // and is tight, not a guess
   EXPECT_EQ(b.logBytes(), 0u);            // reset by the take
}
