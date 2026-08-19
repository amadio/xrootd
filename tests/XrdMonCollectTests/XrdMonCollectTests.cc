//------------------------------------------------------------------------------
// Unit tests for the xrdmoncollect decoder/correlator. Packets are hand-built
// in the on-the-wire (network byte order) layout described by
// src/XrdXrootd/XrdXrootdMonData.hh.
//------------------------------------------------------------------------------

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "XrdApps/XrdMonCollect/XrdMonDecode.hh"
#include "XrdApps/XrdMonCollect/XrdMonFilter.hh"
#include "XrdMetrics/XrdMetricsRegistry.hh"
#include "XrdMetrics/XrdMetricsSerializer.hh"
#include "XrdNet/XrdNetUtils.hh"
#include "XrdOuc/XrdOucJson.hh"

#include <gtest/gtest.h>

using json = nlohmann::json;

namespace
{
// Big-endian byte builder.
struct W
{
   std::vector<unsigned char> b;
   void u8 (uint8_t v)  {b.push_back(v);}
   void u16(uint16_t v) {b.push_back(v >> 8); b.push_back(v & 0xff);}
   void u32(uint32_t v) {for (int i = 3; i >= 0; i--) b.push_back((v >> (8*i)) & 0xff);}
   void u64(uint64_t v) {for (int i = 7; i >= 0; i--) b.push_back((v >> (8*i)) & 0xff);}
   void raw(const std::string& s) {b.insert(b.end(), s.begin(), s.end());}
   void raw(const std::vector<unsigned char>& v) {b.insert(b.end(), v.begin(), v.end());}
};

// One f-stream record: recType, recFlag, recSize(auto), then body (union+payload).
std::vector<unsigned char> rec(uint8_t type, uint8_t flag,
                               const std::vector<unsigned char>& body)
{
   W w;
   w.u8(type); w.u8(flag); w.u16((uint16_t)(4 + body.size()));
   w.raw(body);
   return w.b;
}

// Prepend an 8-byte monitor header and patch the packet length field.
std::vector<unsigned char> packet(char code, int32_t stod,
                                  const std::vector<unsigned char>& payload)
{
   W w;
   w.u8((uint8_t)code); w.u8(0); w.u16(0); w.u32((uint32_t)stod);
   w.raw(payload);
   uint16_t plen = (uint16_t)w.b.size();
   w.b[2] = plen >> 8; w.b[3] = plen & 0xff;
   return w.b;
}

// f-stream TOD (isTime) record spanning [tBeg, tEnd] with nTot following
// records (the decoder interpolates per-record times over that range).
std::vector<unsigned char> todRec(int32_t tBeg, int32_t tEnd, uint16_t nTot,
                                  int64_t sID)
{
   W body;
   body.u16(0);             // union: nRecs[0] (isXfr records)
   body.u16(nTot);          // union: nRecs[1] (records after the TOD)
   body.u32((uint32_t)tBeg);// tBeg
   body.u32((uint32_t)tEnd);// tEnd
   body.u64((uint64_t)sID); // sID
   return rec(2 /*isTime*/, 0, body.b);
}

// Degenerate TOD (tBeg == tEnd, no record count): every record in the packet
// is stamped with the window end, as before interpolation existed.
std::vector<unsigned char> todRec(int32_t tEnd, int64_t sID)
{
   return todRec(tEnd, tEnd, 0, sID);
}

const int32_t kStod  = 1700000000;
const int32_t kOpenT = 1700000000;
const int32_t kCloseT= 1700000082;

// Expected ISO-8601 rendering of a whole-second Unix time, matching the
// decoder's millisecond-precision output format.
std::string isoOf(time_t t)
{
   struct tm tmv;
   char buf[40];
   gmtime_r(&t, &tmv);
   std::size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
   snprintf(buf + n, sizeof(buf) - n, ".000Z");
   return buf;
}

// ::testing::TempDir() is not public in the GoogleTest shipped with EL8.
std::string tempDir()
{
   const char* dir = std::getenv("TMPDIR");
   std::string path = (dir && *dir) ? dir : "/tmp";
   if (path.back() != '/') path += '/';
   return path;
}
}

// Build: 'u' user map, then 'f' open, then 'f' close -> one transfer doc.
class Transfer : public ::testing::Test
{
protected:
  std::string lastDoc;
  std::vector<std::string> allDocs;
  // Declared before the decoder so it outlives it (SetFilter does not own it).
  XrdMonFilter flt;
  XrdMonDecode dec{[&](const std::string& d){ lastDoc = d;
                                              allDocs.push_back(d); }};
  XrdMonDecode* alt = nullptr;   // when set, the feed helpers feed this decoder
  XrdMonDecode& target() {return alt ? *alt : dec;}

  void feedUserMap()
  {
     W body; body.u32(7);                       // dictid
     std::vector<unsigned char> pl = body.b;
     std::string info = "xroot/alice.123:4@wn.example.org\nrole=prod";
     pl.insert(pl.end(), info.begin(), info.end());
     auto pkt = packet('u', kStod, pl);
     target().Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());
  }

  void feedOpen()
  {
     W body;
     body.u32(100);                 // fileID
     body.u64(123456);              // fsz
     body.u32(7);                   // user dictid
     std::string lfn = "/store/data/file.root";
     body.raw(lfn); body.u8(0);     // null terminated
     auto payload = todRec(kOpenT, 42);
     auto r = rec(1 /*isOpen*/, 0x01 | 0x02 /*hasLFN|hasRW*/, body.b);
     payload.insert(payload.end(), r.begin(), r.end());
     auto pkt = packet('f', kStod, payload);
     target().Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());
  }

  void feedClose()
  {
     W body;
     body.u32(100);                 // fileID
     body.u64(10485760);            // Xfr.read
     body.u64(0);                   // Xfr.readv
     body.u64(0);                   // Xfr.write
     // OPS (48 bytes)
     body.u32(320);                 // read ops
     body.u32(0);                   // readv ops
     body.u32(0);                   // write ops
     body.u16(0); body.u16(0);      // rsMin, rsMax
     body.u64(0);                   // rsegs
     body.u32(4096); body.u32(1048576); // rdMin, rdMax
     body.u32(0); body.u32(0);      // rvMin, rvMax
     body.u32(0); body.u32(0);      // wrMin, wrMax
     auto payload = todRec(kCloseT, 42);
     auto r = rec(0 /*isClose*/, 0x02 /*hasOPS*/, body.b);
     payload.insert(payload.end(), r.begin(), r.end());
     auto pkt = packet('f', kStod, payload);
     target().Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());
  }
};

TEST_F(Transfer, CorrelatesCloseWithOpenAndUser)
{
  feedUserMap();
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);

  // The event name lives in the top-level EventName LogRecord field, with the
  // deprecated attribute kept as a duplicate for Loki (grafana/loki#19260).
  EXPECT_EQ(j["eventName"], "xrootd.read");
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.read");
  EXPECT_EQ(j["attributes"]["file.path"], "/store/data/file.root");
  EXPECT_EQ(j["attributes"]["user.name"], "alice");
  EXPECT_EQ(j["attributes"]["network.protocol.name"], "xroot");
  EXPECT_FALSE(j["attributes"].contains("url.scheme"));   // not an HTTP session
  EXPECT_EQ(j["attributes"]["client.address"], "wn.example.org");
  EXPECT_EQ(j["attributes"]["xrootd.read_bytes"], 10485760);
  EXPECT_EQ(j["attributes"]["xrootd.operation.name"], "read");
  EXPECT_EQ(j["attributes"]["xrootd.read_ops"], 320);
  EXPECT_EQ(j["attributes"]["xrootd.read_max"], 1048576);
  EXPECT_EQ(j["attributes"]["xrootd.open_seen"], true);
  EXPECT_EQ(j["attributes"]["file.size"], 123456);
  EXPECT_EQ(j["attributes"]["xrootd.operation.duration"], kCloseT - kOpenT);
  EXPECT_EQ(j["resource"]["xrootd.server.id"], 42);
  // semconv session correlator: the attribute mirrors the session traceId.
  EXPECT_EQ(j["attributes"]["session.id"], j["traceId"]);
  EXPECT_EQ(j["attributes"]["network.transport"], "tcp");
  EXPECT_EQ(j["scope"]["name"], "xrdmoncollect");
  EXPECT_TRUE(j["scope"].contains("version"));

  const XrdMonDecode::Stats& s = dec.GetStats();
  EXPECT_EQ(s.docs, 1u);
  EXPECT_EQ(s.opens, 1u);
  EXPECT_EQ(s.closes, 1u);
  EXPECT_EQ(s.mapUser, 1u);
  EXPECT_EQ(s.orphanCls, 0u);
}

TEST_F(Transfer, CloseWithoutOpenIsOrphan)
{
  feedClose();  // no preceding open

  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.open_seen"], false);
  EXPECT_EQ(j["attributes"]["xrootd.read_bytes"], 10485760);
  EXPECT_FALSE(j["attributes"].contains("file.path"));
  EXPECT_EQ(dec.GetStats().orphanCls, 1u);
}

TEST_F(Transfer, SuccessfulCloseStateIsSuccessful)
{
  feedClose();  // a plain close (no error block)

  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.operation.state"], "Successful");
  EXPECT_FALSE(j["attributes"].contains("error.type"));
  EXPECT_EQ(dec.GetStats().failed, 0u);
}

// With --spans, each close also emits a companion OpenTelemetry span document
// that reuses the log's resource/attributes/trace ids and carries the span
// fields (name/kind/start-end/status), child of the session span.
TEST_F(Transfer, SpanStreamEmitsCorrelatedFileSpan)
{
  dec.SetEmitSpans(true);
  feedUserMap();
  feedOpen();
  feedClose();

  // Two documents: the transfer log (has severityText) and its span (has kind).
  json log, span;
  bool haveLog = false, haveSpan = false;
  for (const auto& d : allDocs)
     {json x = json::parse(d);
      if (x.contains("kind"))         {span = x; haveSpan = true;}
      else if (x.contains("severityText")) {log = x; haveLog = true;}
     }
  ASSERT_TRUE(haveLog);
  ASSERT_TRUE(haveSpan);

  EXPECT_EQ(span["kind"], "SPAN_KIND_SERVER");
  EXPECT_EQ(span["name"], "read");
  EXPECT_EQ(span["status"]["code"], "STATUS_CODE_OK");
  EXPECT_TRUE(span.contains("startTimeUnixNano"));
  EXPECT_TRUE(span.contains("endTimeUnixNano"));
  EXPECT_TRUE(span.contains("parentSpanId"));
  // Resource and attributes are reused verbatim from the log.
  EXPECT_EQ(span["attributes"]["file.path"], "/store/data/file.root");
  EXPECT_EQ(span["resource"]["service.name"], "xrootd");
  // The span shares the log's trace/span identity, so the two correlate.
  EXPECT_EQ(span["traceId"], log["traceId"]);
  EXPECT_EQ(span["spanId"],  log["spanId"]);
  EXPECT_EQ(dec.GetStats().spans, 1u);
}

// An open and close reported in the same packet used to compute a zero
// duration (both were stamped with the packet's flush time). With the TOD's
// tBeg/tEnd/nRecs the decoder interpolates each record's time over its
// position, so the pair yields a positive, fractional duration estimate.
TEST_F(Transfer, SamePacketOpenCloseInterpolatesDuration)
{
  dec.SetEmitSpans(true);
  feedUserMap();

  // One packet, TOD says: 3 records appended between tBeg and tBeg+5.
  // Records: isOpen (k=0 -> tBeg), isClose (k=1 -> tBeg+2.5), isXfr (k=2).
  auto payload = todRec(kOpenT, kOpenT + 5, 3, 42);
  {W body;
   body.u32(100);                 // fileID
   body.u64(123456);              // fsz
   body.u32(7);                   // user dictid
   std::string lfn = "/store/data/file.root";
   body.raw(lfn); body.u8(0);
   auto r = rec(1 /*isOpen*/, 0x01 | 0x02 /*hasLFN|hasRW*/, body.b);
   payload.insert(payload.end(), r.begin(), r.end());
  }
  {W body;
   body.u32(100);                 // fileID
   body.u64(123456);              // Xfr.read (whole file)
   body.u64(0);                   // Xfr.readv
   body.u64(0);                   // Xfr.write
   auto r = rec(0 /*isClose*/, 0, body.b);
   payload.insert(payload.end(), r.begin(), r.end());
  }
  {W body;
   body.u32(101);                 // some other open file
   body.u64(0); body.u64(0); body.u64(0);   // xfr byte counters
   auto r = rec(3 /*isXfr*/, 0, body.b);
   payload.insert(payload.end(), r.begin(), r.end());
  }
  auto pkt = packet('f', kStod, payload);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());

  json log, span;
  bool haveLog = false, haveSpan = false;
  for (const auto& d : allDocs)
     {json x = json::parse(d);
      if (x.contains("kind"))              {span = x; haveSpan = true;}
      else if (x["attributes"].value("event.name", std::string())
               == "xrootd.read")            {log  = x; haveLog  = true;}
     }
  ASSERT_TRUE(haveLog);
  ASSERT_TRUE(haveSpan);

  EXPECT_EQ(log["attributes"]["xrootd.open_seen"], true);
  EXPECT_DOUBLE_EQ(
      log["attributes"]["xrootd.operation.duration"].get<double>(), 2.5);
  // The open time carries the interpolated fraction (tBeg + 0/2 of 5s).
  EXPECT_EQ(log["attributes"]["xrootd.operation.start_time"],
            "2023-11-14T22:13:20.000Z");
  // The span covers open -> close with nanosecond-encoded fractional times.
  EXPECT_EQ(span["startTimeUnixNano"], std::to_string(
            (uint64_t)kOpenT * 1000000000ULL));
  EXPECT_EQ(span["endTimeUnixNano"], std::to_string(
            (uint64_t)kOpenT * 1000000000ULL + 2500000000ULL));
}

// A degenerate TOD (tBeg == tEnd, nRecs == 0), as produced by the two-packet
// helpers, keeps the pre-interpolation behavior: records are stamped with the
// window end, and a cross-packet pair measures window-end to window-end.
TEST_F(Transfer, CrossPacketDurationUsesWindowEnds)
{
  feedUserMap();
  feedOpen();
  feedClose();

  json j = json::parse(lastDoc);
  EXPECT_DOUBLE_EQ(j["attributes"]["xrootd.operation.duration"].get<double>(),
                   (double)(kCloseT - kOpenT));
}

namespace
{
// XrdXrootdMonStatERR: ecode(4) + ecat(1) + rsvd(3) + null-terminated message.
std::vector<unsigned char> errBlock(int32_t ecode, uint8_t ecat,
                                    const std::string& msg)
{
   W w;
   w.u32((uint32_t)ecode);
   w.u8(ecat); w.u8(0); w.u8(0); w.u8(0);   // ecat + rsvd[3]
   w.raw(msg); w.u8(0);                      // null-terminated message
   return w.b;
}
}

// A failed open emits a self-contained isError record (no open/close pair).
TEST_F(Transfer, FailedOpenEmitsFailedState)
{
  feedUserMap();

  W body;
  body.u32(0);                              // fileID union (0 for isError)
  body.u32(7);                              // inline user dictid
  std::string lfn = "/store/data/missing.root";
  body.raw(lfn); body.u8(0);                // null-terminated lfn
  auto eb = errBlock(3011 /*kXR_NotAuthorized-ish*/, 5 /*monErrAuth*/,
                     "permission denied");
  body.raw(eb);

  auto payload = todRec(kCloseT, 42);
  auto r = rec(5 /*isError*/, 0x01 /*hasLFN*/, body.b);
  payload.insert(payload.end(), r.begin(), r.end());
  auto pkt = packet('f', kStod, payload);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.auth");
  EXPECT_EQ(j["attributes"]["file.path"], "/store/data/missing.root");
  EXPECT_EQ(j["attributes"]["user.name"], "alice");   // resolved from the inline dictid
  EXPECT_EQ(j["attributes"]["xrootd.operation.state"], "Failed");
  EXPECT_EQ(j["attributes"]["xrootd.error.code"], 3011);
  EXPECT_EQ(j["attributes"]["xrootd.operation.name"], "auth");
  EXPECT_EQ(j["attributes"]["error.type"], "permission denied");
  EXPECT_EQ(dec.GetStats().failed, 1u);
}

// A failed operation's companion span carries ERROR status with the message.
TEST_F(Transfer, SpanStatusReflectsError)
{
  dec.SetEmitSpans(true);
  feedUserMap();

  W body;
  body.u32(0);                              // fileID union (0 for isError)
  body.u32(7);                              // inline user dictid
  std::string lfn = "/store/data/missing.root";
  body.raw(lfn); body.u8(0);
  auto eb = errBlock(3011, 5 /*monErrAuth*/, "permission denied");
  body.raw(eb);
  auto payload = todRec(kCloseT, 42);
  auto r = rec(5 /*isError*/, 0x01 /*hasLFN*/, body.b);
  payload.insert(payload.end(), r.begin(), r.end());
  auto pkt = packet('f', kStod, payload);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());

  json span;
  bool haveSpan = false;
  for (const auto& d : allDocs)
     {json x = json::parse(d);
      if (x.contains("kind")) {span = x; haveSpan = true;}
     }
  ASSERT_TRUE(haveSpan);
  EXPECT_EQ(span["status"]["code"], "STATUS_CODE_ERROR");
  EXPECT_EQ(span["status"]["message"], "permission denied");
}

// An aborted transfer is reported as an isClose carrying a trailing error block.
TEST_F(Transfer, AbortedTransferCloseHasError)
{
  feedUserMap();
  feedOpen();

  W body;
  body.u32(100);                            // fileID (matches the open)
  body.u64(4096);                           // Xfr.read (partial)
  body.u64(0);                              // Xfr.readv
  body.u64(0);                              // Xfr.write
  // OPS (48 bytes)
  body.u32(2); body.u32(0); body.u32(0);    // read/readv/write ops
  body.u16(0); body.u16(0);                 // rsMin/rsMax
  body.u64(0);                              // rsegs
  body.u32(0); body.u32(0);                 // rdMin/rdMax
  body.u32(0); body.u32(0);                 // rvMin/rvMax
  body.u32(0); body.u32(0);                 // wrMin/wrMax
  auto eb = errBlock(3006 /*kXR_IOError-ish*/, 2 /*monErrRead*/,
                     "read error: connection reset");
  body.raw(eb);

  auto payload = todRec(kCloseT, 42);
  auto r = rec(0 /*isClose*/, 0x02 | 0x08 /*hasOPS|hasERR*/, body.b);
  payload.insert(payload.end(), r.begin(), r.end());
  auto pkt = packet('f', kStod, payload);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["file.path"], "/store/data/file.root");  // joined to the open
  EXPECT_EQ(j["attributes"]["xrootd.read_bytes"], 4096);    // partial bytes preserved
  EXPECT_EQ(j["attributes"]["xrootd.read_ops"], 2);
  EXPECT_EQ(j["attributes"]["xrootd.operation.state"], "Failed");
  EXPECT_EQ(j["attributes"]["xrootd.operation.name"], "read");
  EXPECT_EQ(j["attributes"]["error.type"], "read error: connection reset");
  EXPECT_EQ(dec.GetStats().failed, 1u);
}

TEST(XrdMonCollect, ShortPacketIsMalformed)
{
  XrdMonDecode dec([](const std::string&){});
  char tiny[4] = {'f', 0, 0, 0};
  EXPECT_FALSE(dec.Process("1.2.3.4:5", tiny, sizeof(tiny)));
  EXPECT_EQ(dec.GetStats().malformed, 1u);
}

TEST(XrdMonCollect, MalformedPacketDebugDumpsReason)
{
  // With --debug (emitRaw=true) a rejected packet emits a diagnostic to the raw
  // sink carrying its reason category, so malformed_total ticks are traceable.
  std::string diag;
  XrdMonDecode dec([](const std::string&){},
                   [&](const std::string& r){ diag = r; },
                   /*emitRaw=*/true);
  char tiny[4] = {'f', 0, 0, 0};
  EXPECT_FALSE(dec.Process("1.2.3.4:5", tiny, sizeof(tiny)));
  EXPECT_EQ(dec.GetStats().malformed, 1u);
  ASSERT_FALSE(diag.empty());
  auto j = nlohmann::json::parse(diag);
  EXPECT_EQ(j["reason"], "short_packet");
  EXPECT_EQ(j["server"], "1.2.3.4");   // resolved name, not the UDP socket
  EXPECT_EQ(j["note"], "malformed packet");
}

TEST(XrdMonCollect, UnknownStreamCounted)
{
  XrdMonDecode dec([](const std::string&){});
  // 'Z' is not a defined monitor code -> counted as unknown.
  auto pkt = packet('Z', kStod, std::vector<unsigned char>(16, 0));
  EXPECT_TRUE(dec.Process("1.2.3.4:5", (const char*)pkt.data(), pkt.size()));
  EXPECT_EQ(dec.GetStats().unknown, 1u);
}

namespace
{
// 16-byte XrdXrootdMonTrace record from arg0(8)/arg1(4)/arg2(4).
std::vector<unsigned char> trace(const std::vector<unsigned char>& a0,
                                 uint32_t a1, uint32_t a2)
{
   W w; w.raw(a0); w.u32(a1); w.u32(a2);
   return w.b;  // a0 must already be 8 bytes
}
std::vector<unsigned char> u64v(uint64_t v) {W w; w.u64(v); return w.b;}
}

TEST(XrdMonCollect, TStreamRecordsDecoded)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); },
                   nullptr, false, /*traces=*/true);

  // 'd' path map: dictid 50 -> /path/f.root
  {
    W body; body.u32(50);
    std::string info = "alice.1:2@host\n/path/f.root";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('d', kStod, pl);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size());
  }

  // 't' packet: window, a read I/O and a close on file 50, and a disconnect.
  W payload;
  { std::vector<unsigned char> a0(8, 0); a0[0] = 0xe0;     // WINDOW
    payload.raw(trace(a0, 0, 1700000000)); }
  { auto a0 = u64v(4096);                                  // read offset 4096
    payload.raw(trace(a0, 1024, 50)); }                    // len 1024, file 50
  { std::vector<unsigned char> a0(8, 0); a0[0] = 0xc0;     // CLOSE
    a0[1] = 0; a0[2] = 0;                                   // shifts
    a0[4]=0; a0[5]=0; a0[6]=0x08; a0[7]=0x00;              // rVal = 2048
    payload.raw(trace(a0, 0, 50)); }
  { std::vector<unsigned char> a0(8, 0); a0[0] = 0xd0;     // DISC
    payload.raw(trace(a0, 5, 50)); }                       // dur 5, user 50
  auto pkt = packet('t', kStod, payload.b);
  dec.Process("h:1", (const char*)pkt.data(), pkt.size());

  EXPECT_EQ(dec.GetStats().traces, 4u);
  ASSERT_EQ(docs.size(), 3u);   // window emits nothing; read + close + disc do
  json rd = json::parse(docs[0]);
  EXPECT_EQ(rd["attributes"]["event.name"], "xrootd.io.read");
  EXPECT_EQ(rd["attributes"]["xrootd.io.offset"], 4096);
  EXPECT_EQ(rd["attributes"]["xrootd.io.length"], 1024);
  EXPECT_EQ(rd["attributes"]["file.path"], "/path/f.root");
  json cl = json::parse(docs[1]);
  EXPECT_EQ(cl["attributes"]["event.name"], "xrootd.io.close");
  EXPECT_EQ(cl["attributes"]["xrootd.read_bytes"], 2048);
  json di = json::parse(docs[2]);
  EXPECT_EQ(di["attributes"]["event.name"], "xrootd.io.disconnect");

  // Every emitted trace record carries a well-formed traceId/spanId so tracing
  // backends can correlate it (32-hex trace id, 16-hex span id); the session.id
  // attribute mirrors the traceId.
  for (const json* d : {&rd, &cl, &di})
     {ASSERT_TRUE(d->contains("traceId")) << *d;
      ASSERT_TRUE(d->contains("spanId"))  << *d;
      EXPECT_EQ((*d)["traceId"].get<std::string>().size(), 32u);
      EXPECT_EQ((*d)["spanId"].get<std::string>().size(),  16u);
      EXPECT_EQ((*d)["attributes"]["session.id"], (*d)["traceId"]);
     }
  // The read is a true I/O op, so it gets its own span id (a child of the file's
  // transfer span); the close is a marker that maps onto the file span itself,
  // and the disconnect onto the session span.
  EXPECT_NE(rd["spanId"], cl["spanId"]);
  EXPECT_NE(di["spanId"], cl["spanId"]);
}

// Records between two WINDOW marks are spread linearly across the window they
// fall in, instead of all being stamped with the window boundary.
TEST(XrdMonCollect, TStreamInterpolatesRecordTimes)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); },
                   nullptr, false, /*traces=*/true);

  const uint32_t t0 = 1700000000;
  W payload;
  { std::vector<unsigned char> a0(8, 0); a0[0] = 0xe0;     // WINDOW: opens [t0,
    payload.raw(trace(a0, 0, t0)); }                       // ...] at t0
  for (int i = 0; i < 3; i++)                              // 3 reads on file 50
  { auto a0 = u64v(4096);
    payload.raw(trace(a0, 1024, 50)); }
  { std::vector<unsigned char> a0(8, 0); a0[0] = 0xe0;     // WINDOW: closes the
    payload.raw(trace(a0, t0 + 4, t0 + 4)); }              // segment at t0+4
  auto pkt = packet('t', kStod, payload.b);
  dec.Process("h:1", (const char*)pkt.data(), pkt.size());

  ASSERT_EQ(docs.size(), 3u);
  const char* expect[] = {"1700000000000000000",           // t0
                          "1700000002000000000",           // t0 + 2
                          "1700000004000000000"};          // t0 + 4
  for (int i = 0; i < 3; i++)
     {json j = json::parse(docs[i]);
      EXPECT_EQ(j["timeUnixNano"], expect[i]) << docs[i];
     }
}

// The 't'-stream appid marker carries the application id under xrootd.app
// (consistent with the 'i'-stream appinfo mapping).
TEST(XrdMonCollect, TraceAppidMapsToXrootdApp)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); },
                   nullptr, false, /*traces=*/true);

  // appid record (disc 0xa0): a 12-byte app id string at bytes [4..15].
  std::vector<unsigned char> rec(16, 0);
  rec[0] = 0xa0;
  const char* app = "myapp";
  std::memcpy(rec.data() + 4, app, std::strlen(app));
  auto pkt = packet('t', kStod, rec);
  dec.Process("h:1", (const char*)pkt.data(), pkt.size());

  ASSERT_EQ(docs.size(), 1u);
  json j = json::parse(docs[0]);
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.io.appid");
  EXPECT_EQ(j["attributes"]["xrootd.app"], "myapp");
}

// A trace-stream I/O op must nest under the file's transfer span: with --spans
// it emits its own child span whose parent is the transfer span EmitClose emits,
// so Tempo renders session -> file -> I/O. Cross-check the emitted documents
// rather than recomputing the (internal) keys.
TEST_F(Transfer, TraceRecordsCorrelateWithTransferSpan)
{
  std::vector<std::string> tdocs;
  XrdMonDecode traced([&](const std::string& d){ tdocs.push_back(d); },
                      nullptr, false, /*traces=*/true);
  traced.SetEmitSpans(true);     // also emit companion span documents
  alt = &traced;                 // the feed helpers now target the traced decoder

  feedUserMap();                 // user dictid 7
  feedOpen();                    // file 100 opened by user 7 (populates srv.files)

  // t-stream read on file 100, fed before the close erases the file record.
  { W payload;
    { std::vector<unsigned char> a0(8, 0); a0[0] = 0xe0;   // WINDOW
      payload.raw(trace(a0, 0, (uint32_t)kCloseT)); }
    { auto a0 = u64v(4096);                                // read offset 4096
      payload.raw(trace(a0, 2048, 100)); }                 // len 2048, file 100
    auto pkt = packet('t', kStod, payload.b);
    traced.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }

  // The read yields a log record and its companion span; classify by "kind".
  ASSERT_EQ(tdocs.size(), 2u);
  json rlog, rspan;
  for (const std::string& s : tdocs)
     {json d = json::parse(s);
      if (d.contains("kind")) rspan = d; else rlog = d;
     }
  EXPECT_EQ(rlog["attributes"]["event.name"], "xrootd.io.read");
  EXPECT_EQ(rlog["attributes"]["session.id"], rlog["traceId"]);
  EXPECT_EQ(rspan["name"], "read");
  EXPECT_EQ(rspan["kind"], "SPAN_KIND_SERVER");
  // The log and its span share the same identity.
  EXPECT_EQ(rlog["traceId"], rspan["traceId"]);
  EXPECT_EQ(rlog["spanId"],  rspan["spanId"]);

  // Feed the close: it emits the transfer log and its file-operation span.
  tdocs.clear();
  feedClose();
  json xlog, xspan;
  for (const std::string& s : tdocs)
     {json d = json::parse(s);
      if (d.contains("kind")) xspan = d; else xlog = d;
     }
  ASSERT_EQ(xlog["eventName"], "xrootd.read");
  EXPECT_EQ(xspan["spanId"], xlog["spanId"]);        // the file's transfer span

  // The I/O span nests under the file's transfer span, same session trace.
  EXPECT_EQ(rspan["parentSpanId"], xlog["spanId"]);
  EXPECT_EQ(rspan["traceId"],      xlog["traceId"]);
  EXPECT_NE(rspan["spanId"],       xlog["spanId"]);   // a distinct child span
}

TEST_F(Transfer, AggregatesIntoMetricsRegistry)
{
  // Re-run the open/close/user sequence through a decoder bound to a registry.
  // Mirror production naming: root prefix "xrootd", subsystem "collector", so a
  // bare series name like "io_total" renders as xrootd_collector_*.
  XrdMetrics::Collector collector("xrootd");
  std::string sink;
  XrdMonDecode d([&](const std::string& s){ sink = s; }, nullptr,
                 false, false, false, false, &collector.subsystem("collector"));

  { W body; body.u32(7);
    std::string info = "xroot/alice.1:2@wn.example.org\n";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('u', kStod, pl);
    d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  { W body; body.u32(100); body.u64(123456); body.u32(7);
    std::string lfn = "/store/data/file.root"; body.raw(lfn); body.u8(0);
    auto payload = todRec(kOpenT, 42);
    auto r = rec(1, 0x03, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  { W body; body.u32(100); body.u64(10485760); body.u64(0); body.u64(0);
    body.u32(320); body.u32(0); body.u32(0); body.u16(0); body.u16(0);
    body.u64(0); body.u32(4096); body.u32(1048576);
    body.u32(0); body.u32(0); body.u32(0); body.u32(0);
    auto payload = todRec(kCloseT, 42);
    auto r = rec(0, 0x02, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }

  std::string out;
  XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_io_total{cluster=\"unknown\",server=\"10.0.0.1\",operation=\"close\"} 1"),
            std::string::npos);
  EXPECT_NE(out.find("xrootd_collector_io_bytes_total{cluster=\"unknown\","
                     "server=\"10.0.0.1\",operation=\"read\"} 10485760"),
            std::string::npos);
}

TEST_F(Transfer, AppInfoEnrichesTransfer)
{
  feedUserMap();
  // 'i' (appinfo) map: same descriptor as the user, plus an appinfo body.
  { W body; body.u32(9);
    std::string info = "xroot/alice.123:4@wn.example.org\ntest-app-v1";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('i', kStod, pl);
    dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.app"], "test-app-v1");
  // The 'u' descriptor is decomposed into user.name/protocol/client fields;
  // the raw line itself is not duplicated into the document.
  EXPECT_FALSE(j["attributes"].contains("xrootd.user.raw"));
}

// The app label prefers the 'i'-stream appid over the login's client
// executable, and falls back to "unknown" when the close matched no open (and
// so resolved no identity at all).
TEST(XrdMonCollect, AppLabelPrefersAppidOverExecutable)
{
  auto run = [](bool withAppid, const char* execName)
     {XrdMetrics::Collector collector("xrootd");
      XrdMonDecode d([](const std::string&){}, nullptr, false, false, false,
                     false, &collector.subsystem("collector"));
      { W body; body.u32(7);
        std::string info = "xroot/alice.123:4@wn.example.org";
        if (*execName) info += std::string("\n&x=") + execName;
        std::vector<unsigned char> pl = body.b;
        pl.insert(pl.end(), info.begin(), info.end());
        auto pkt = packet('u', kStod, pl);
        d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
      if (withAppid)
         { W body; body.u32(9);
           std::string info = "xroot/alice.123:4@wn.example.org\nreco-2026";
           std::vector<unsigned char> pl = body.b;
           pl.insert(pl.end(), info.begin(), info.end());
           auto pkt = packet('i', kStod, pl);
           d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
      { W body; body.u32(100); body.u64(123456); body.u32(7);
        std::string lfn = "/store/f.root"; body.raw(lfn); body.u8(0);
        auto payload = todRec(kOpenT, 42);
        auto r = rec(1 /*isOpen*/, 0x03, body.b);
        payload.insert(payload.end(), r.begin(), r.end());
        auto pkt = packet('f', kStod, payload);
        d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
      { W body; body.u32(100); body.u64(4096); body.u64(0); body.u64(0);
        auto payload = todRec(kCloseT, 42);
        auto r = rec(0 /*isClose*/, 0, body.b);
        payload.insert(payload.end(), r.begin(), r.end());
        auto pkt = packet('f', kStod, payload);
        d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
      std::string out;
      XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
      return out; };

  EXPECT_NE(run(true, "xrdcp").find(
              "xrootd_collector_app_io_bytes_total{cluster=\"unknown\","
              "app=\"reco-2026\",operation=\"read\"} 4096"),
            std::string::npos);
  EXPECT_NE(run(false, "xrdcp").find(
              "xrootd_collector_app_io_bytes_total{cluster=\"unknown\","
              "app=\"xrdcp\",operation=\"read\"} 4096"),
            std::string::npos);
  EXPECT_NE(run(false, "").find(
              "xrootd_collector_app_io_bytes_total{cluster=\"unknown\","
              "app=\"unknown\",operation=\"read\"} 4096"),
            std::string::npos);
}

// file.path is decomposed into file.name, the semconv file.directory, and
// file.extension (last extension, no leading dot).
TEST_F(Transfer, FileDirectoryDerived)
{
  feedUserMap();
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["file.path"], "/store/data/file.root");
  EXPECT_EQ(j["attributes"]["file.name"], "file.root");
  EXPECT_EQ(j["attributes"]["file.directory"], "/store/data");
  EXPECT_EQ(j["attributes"]["file.extension"], "root");
}

// file.extension is the LAST extension ("gz" for .tar.gz); a dotfile has none.
TEST_F(Transfer, FileExtensionEdgeCases)
{
  auto openClose = [&](uint32_t fileID, const std::string& lfn)
  {
     { W body;
       body.u32(fileID); body.u64(123456); body.u32(7);
       body.raw(lfn); body.u8(0);
       auto payload = todRec(kOpenT, 42);
       auto r = rec(1 /*isOpen*/, 0x01 | 0x02 /*hasLFN|hasRW*/, body.b);
       payload.insert(payload.end(), r.begin(), r.end());
       auto pkt = packet('f', kStod, payload);
       dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
     { W body;
       body.u32(fileID); body.u64(4096); body.u64(0); body.u64(0);
       auto payload = todRec(kCloseT, 42);
       auto r = rec(0 /*isClose*/, 0, body.b);
       payload.insert(payload.end(), r.begin(), r.end());
       auto pkt = packet('f', kStod, payload);
       dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
     return json::parse(lastDoc);
  };

  feedUserMap();
  json tgz = openClose(200, "/store/archive.tar.gz");
  EXPECT_EQ(tgz["attributes"]["file.extension"], "gz");
  json dotfile = openClose(201, "/home/alice/.bashrc");
  EXPECT_FALSE(dotfile["attributes"].contains("file.extension"));
  json noext = openClose(202, "/store/data/README");
  EXPECT_FALSE(noext["attributes"].contains("file.extension"));
}

// A --dataset pattern's first capture group becomes xrootd.dataset.
TEST_F(Transfer, DatasetRegexCaptures)
{
  ASSERT_TRUE(dec.SetDatasetRegex("^/store/([^/]+)/"));
  feedUserMap();
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.dataset"], "data");
}

// A path the pattern does not match gets no dataset attribute.
TEST_F(Transfer, DatasetRegexNoMatchOmitted)
{
  ASSERT_TRUE(dec.SetDatasetRegex("^/eos/([^/]+)/"));
  feedUserMap();
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_FALSE(j["attributes"].contains("xrootd.dataset"));
}

// An uncompilable pattern is rejected; an empty one clears the capture.
TEST(XrdMonCollect, DatasetRegexInvalidRejected)
{
  XrdMonDecode dec([](const std::string&){});
  EXPECT_FALSE(dec.SetDatasetRegex("(["));
  EXPECT_TRUE(dec.SetDatasetRegex("^/store/([^/]+)/"));
  EXPECT_TRUE(dec.SetDatasetRegex(""));    // clear
}

/******************************************************************************/
/*                       d o c u m e n t   f i l t e r                        */
/******************************************************************************/

// A tag rule labels the document in place; everything else about it is
// unchanged.
TEST_F(Transfer, FilterTagsMatchingDocument)
{
  std::string err;
  std::size_t r = flt.AddRule("internal");
  ASSERT_TRUE(flt.AddCondition(r, "user", "alice", err)) << err;
  dec.SetFilter(&flt);

  feedUserMap(); feedOpen(); feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.filter.labels"], json::array({"internal"}));
  EXPECT_EQ(j["attributes"]["user.name"], "alice");
  EXPECT_EQ(dec.GetStats().filtered, 0u);
}

// A rule keyed on a field this document does not carry must not fire.
TEST_F(Transfer, FilterLeavesNonMatchingDocumentAlone)
{
  std::string err;
  std::size_t r = flt.AddRule("internal");
  ASSERT_TRUE(flt.AddCondition(r, "authprot", "sss", err)) << err;  // no &p=
  dec.SetFilter(&flt);

  feedUserMap(); feedOpen(); feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_FALSE(j["attributes"].contains("xrootd.filter.labels"));
}

// A drop rule suppresses the document entirely, and says so in the stats.
TEST_F(Transfer, FilterDropsMatchingDocument)
{
  std::string err;
  std::size_t r = flt.AddRule("internal");
  ASSERT_TRUE(flt.AddCondition(r, "user", "alice", err)) << err;
  ASSERT_TRUE(flt.SetAction(r, "drop", err)) << err;
  dec.SetFilter(&flt);

  feedUserMap(); feedOpen(); feedClose();

  EXPECT_TRUE(allDocs.empty());
  EXPECT_EQ(dec.GetStats().filtered, 1u);
  // The close was still decoded and counted: only the output is suppressed.
  EXPECT_EQ(dec.GetStats().closes, 1u);
  EXPECT_EQ(dec.GetStats().docs, 1u);
}

// A tagged document's companion span inherits the label, since the span is
// built from the already-tagged log document.
TEST_F(Transfer, FilterTagIsInheritedByTheCompanionSpan)
{
  std::string err;
  std::size_t r = flt.AddRule("internal");
  ASSERT_TRUE(flt.AddCondition(r, "user", "alice", err)) << err;
  dec.SetFilter(&flt);
  dec.SetEmitSpans(true);

  feedUserMap(); feedOpen(); feedClose();

  ASSERT_EQ(allDocs.size(), 2u);           // the log, then its span
  for (const std::string& d : allDocs)
      {json j = json::parse(d);
       EXPECT_EQ(j["attributes"]["xrootd.filter.labels"],
                 json::array({"internal"})) << d;
      }
  json span = json::parse(allDocs[1]);
  EXPECT_EQ(span["kind"], "SPAN_KIND_SERVER");
}

// Dropping a log document must take its span with it, or a tracing backend is
// left with a parentless child span.
TEST_F(Transfer, FilterDropAlsoDropsTheCompanionSpan)
{
  std::string err;
  std::size_t r = flt.AddRule("internal");
  ASSERT_TRUE(flt.AddCondition(r, "user", "alice", err)) << err;
  ASSERT_TRUE(flt.SetAction(r, "drop", err)) << err;
  dec.SetFilter(&flt);
  dec.SetEmitSpans(true);

  feedUserMap(); feedOpen(); feedClose();

  EXPECT_TRUE(allDocs.empty());
  EXPECT_EQ(dec.GetStats().spans, 0u);
}

// The point of the feature: a dropped document is still fully accounted for in
// the Prometheus series, so sysadmins keep the complete picture of the cluster
// while the document stream is cleaned up.
TEST(XrdMonCollect, FilterDoesNotAffectMetrics)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonFilter flt;
  std::string err;
  std::size_t r = flt.AddRule("internal");
  ASSERT_TRUE(flt.AddCondition(r, "user", "alice", err)) << err;
  ASSERT_TRUE(flt.SetAction(r, "drop", err)) << err;

  std::vector<std::string> docs;
  XrdMonDecode d([&](const std::string& s){ docs.push_back(s); }, nullptr,
                 false, false, false, false, &collector.subsystem("collector"));
  d.SetFilter(&flt);

  { W body; body.u32(7);
    std::string info = "xroot/alice.1:2@wn.example.org\n";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('u', kStod, pl);
    d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  { W body; body.u32(100); body.u64(123456); body.u32(7);
    std::string lfn = "/store/data/file.root"; body.raw(lfn); body.u8(0);
    auto payload = todRec(kOpenT, 42);
    auto r2 = rec(1, 0x03, body.b);
    payload.insert(payload.end(), r2.begin(), r2.end());
    auto pkt = packet('f', kStod, payload);
    d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  { W body; body.u32(100); body.u64(10485760); body.u64(0); body.u64(0);
    body.u32(320); body.u32(0); body.u32(0); body.u16(0); body.u16(0);
    body.u64(0); body.u32(4096); body.u32(1048576);
    body.u32(0); body.u32(0); body.u32(0); body.u32(0);
    body.u32(0); body.u32(0); body.u32(0); body.u32(0);
    auto payload = todRec(kCloseT, 42);
    auto r2 = rec(0, 0x02, body.b);
    payload.insert(payload.end(), r2.begin(), r2.end());
    auto pkt = packet('f', kStod, payload);
    d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }

  EXPECT_TRUE(docs.empty());                  // nothing was exported ...
  EXPECT_EQ(d.GetStats().filtered, 1u);
  std::string out;                            // ... but everything was measured
  XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_io_total{cluster=\"unknown\",server=\"10.0.0.1\",operation=\"close\"} 1"),
            std::string::npos);
  EXPECT_NE(out.find("xrootd_collector_io_bytes_total{cluster=\"unknown\","
                     "server=\"10.0.0.1\",operation=\"read\"} 10485760"),
            std::string::npos);
}

// An identity rule must not reach documents that carry no identity: a
// server-identity document has no user at all, so `user = *` leaves it alone.
TEST(XrdMonCollect, FilterIdentityRuleSparesServerIdent)
{
  XrdMonFilter flt;
  std::string err;
  std::size_t r = flt.AddRule("any-user");
  ASSERT_TRUE(flt.AddCondition(r, "user", "*", err)) << err;
  ASSERT_TRUE(flt.SetAction(r, "drop", err)) << err;

  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetFilter(&flt);

  std::string info = "=/xrootd.4321:99@srv.example.org"
                     "\n&site=T1_DE_KIT&port=1094&inst=manager&pgm=xrootd&ver=v6.1.0";
  W body; body.u32(0);
  std::vector<unsigned char> pl = body.b;
  pl.insert(pl.end(), info.begin(), info.end());
  auto pkt = packet('=', kStod, pl);
  dec.Process("srv:9930", (const char*)pkt.data(), pkt.size());

  ASSERT_EQ(docs.size(), 1u);
  EXPECT_EQ(dec.GetStats().filtered, 0u);
  json j = json::parse(docs[0]);
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.server_ident");
}

// A server-level rule, by contrast, does reach it.
TEST(XrdMonCollect, FilterServerRuleDropsServerIdent)
{
  XrdMonFilter flt;
  std::string err;
  std::size_t r = flt.AddRule("no-ident");
  ASSERT_TRUE(flt.AddCondition(r, "event", "xrootd.server_ident", err)) << err;
  ASSERT_TRUE(flt.SetAction(r, "drop", err)) << err;

  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetFilter(&flt);

  std::string info = "=/xrootd.4321:99@srv.example.org\n&site=T1_DE_KIT&pgm=xrootd";
  W body; body.u32(0);
  std::vector<unsigned char> pl = body.b;
  pl.insert(pl.end(), info.begin(), info.end());
  auto pkt = packet('=', kStod, pl);
  dec.Process("srv:9930", (const char*)pkt.data(), pkt.size());

  EXPECT_TRUE(docs.empty());
  EXPECT_EQ(dec.GetStats().filtered, 1u);
}

// The 'i' blob is only emitted when it adds information over the login &y=
// (which is user_agent.original).
TEST_F(Transfer, AppSuppressedWhenEqualToUserAgentOriginal)
{
  { W body; body.u32(7);
    std::string info = "xroot/alice.123:4@wn.example.org\n&R=v5&x=xrdcp"
                       "&y=test-app-v1&I=4";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('u', kStod, pl);
    dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  { W body; body.u32(9);
    std::string info = "xroot/alice.123:4@wn.example.org\ntest-app-v1";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('i', kStod, pl);
    dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["user_agent.original"], "test-app-v1");
  EXPECT_FALSE(j["attributes"].contains("xrootd.app"));   // same as &y=
}

TEST(XrdMonCollect, RedirectStreamDecoded)
{
  std::string out;
  XrdMonDecode dec([&](const std::string& d){ out = d; }, nullptr,
                   false, false, false, /*redirects=*/true);

  { W body; body.u32(7);
    std::string info = "xroot/bob.1:2@cli.example.org\n";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('u', kStod, pl);
    dec.Process("mgr:9930", (const char*)pkt.data(), pkt.size()); }

  // r-stream: sID block (REDSID marker + 7 bytes), one redirect record, then
  // the "<host>:<path>" string occupying 4 (Dent) further 8-byte slots.
  W body;
  body.u8(0xf0); for (int i = 0; i < 7; i++) body.u8(0);
  body.u8(0x85); body.u8(4); body.u16(1094); body.u32(7);  // open-read, port, uid
  std::string hp = "host.example:/store/data/f.root";       // 31 chars + NUL = 32
  body.raw(hp); body.u8(0);
  auto pkt = packet('r', kStod, body.b);
  dec.Process("mgr:9930", (const char*)pkt.data(), pkt.size());

  EXPECT_EQ(dec.GetStats().redirs, 1u);
  json j = json::parse(out);
  // A redirect is a concluded-operation report: its own event name, with
  // operation state "Redirected" and the destination under "redirect".
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.redirect");
  EXPECT_EQ(j["attributes"]["xrootd.operation.name"], "open-read");
  EXPECT_EQ(j["attributes"]["xrootd.operation.state"], "Redirected");
  EXPECT_EQ(j["attributes"]["xrootd.redirect.kind"], "remote");
  EXPECT_EQ(j["attributes"]["xrootd.redirect.target.address"], "host.example");
  EXPECT_EQ(j["attributes"]["xrootd.redirect.target.port"], 1094);
  EXPECT_EQ(j["attributes"]["file.path"], "/store/data/f.root");
  EXPECT_EQ(j["attributes"]["user.name"], "bob");
  EXPECT_EQ(j["attributes"]["client.address"], "cli.example.org");
}

TEST_F(Transfer, TokenAndActivityEnrichTransfer)
{
  feedUserMap();
  // 'T' token map: keyed by the same user dictid (7) as the 'u' map.
  { W body; body.u32(7);
    std::string info = "&Uc=7&s=https://issuer/sub42&n=alice"
                       "&o=atlas&r=production&g=/atlas/prod";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('T', kStod, pl);
    dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  // 'U' user experiment/activity map (SciTags), same dictid.
  { W body; body.u32(7);
    std::string info = "&Uc=7&Ec=42&Ac=7";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('U', kStod, pl);
    dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["user.id"], "https://issuer/sub42");
  EXPECT_EQ(j["attributes"]["wlcg.vo"], "atlas");
  EXPECT_EQ(j["attributes"]["user.roles"], json::array({"production"}));
  EXPECT_EQ(j["attributes"]["wlcg.groups"], "/atlas/prod");
  EXPECT_EQ(j["attributes"]["scitags.experiment_id"], 42);
  EXPECT_EQ(j["attributes"]["scitags.activity_id"], 7);

  const XrdMonDecode::Stats& s = dec.GetStats();
  EXPECT_EQ(s.mapTokn, 1u);
  EXPECT_EQ(s.mapUeac, 1u);
}

namespace
{
// Feed a 'U' (SciTags) experiment/activity map for dictid 7.
void feedActivity(XrdMonDecode& dec, int expId, int actId)
{
   W body; body.u32(7);
   std::string info = "&Uc=7&Ec=" + std::to_string(expId) +
                      "&Ac=" + std::to_string(actId);
   std::vector<unsigned char> pl = body.b;
   pl.insert(pl.end(), info.begin(), info.end());
   auto pkt = packet('U', kStod, pl);
   dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());
}

// Write a SciTags registry JSON to a temp file; return its path.
std::string writeScitags(const std::string& name, const std::string& body)
{
   std::string path = tempDir() + name;
   std::ofstream(path) << body;
   return path;
}
}

// With a SciTags registry loaded, the numeric experiment/activity ids are
// additionally mapped to names. The experiment name stands on its own — it is
// deliberately not folded into wlcg.vo, which carries only genuine VO
// information from the token or a VO-bearing auth method.
TEST_F(Transfer, ScitagsRegistryMapsActivityNames)
{
  std::string collector = writeScitags("scitags-map.json",
     R"({"experiments":[{"expId":2,"expName":"atlas","activities":[
         {"activityId":7,"activityName":"production"},
         {"activityId":8,"activityName":"analysis"}]}]})");
  ASSERT_TRUE(dec.LoadScitags(collector));

  feedUserMap();          // descriptor tail has no &o= -> no auth VO, no token
  feedActivity(dec, 2, 7);
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["scitags.experiment_id"], 2);
  EXPECT_EQ(j["attributes"]["scitags.activity_id"], 7);
  EXPECT_EQ(j["attributes"]["scitags.experiment"], "atlas");
  EXPECT_EQ(j["attributes"]["scitags.activity"], "production");
  EXPECT_FALSE(j["attributes"].contains("wlcg.vo"));  // no experiment fallback
}

// A token VO and the SciTags experiment name are independent fields.
TEST_F(Transfer, ScitagsVoYieldsToToken)
{
  std::string collector = writeScitags("scitags-vo.json",
     R"({"experiments":[{"expId":2,"expName":"atlas","activities":[]}]})");
  ASSERT_TRUE(dec.LoadScitags(collector));

  feedUserMap();
  { W body; body.u32(7);
    std::string info = "&Uc=7&s=sub&o=cms&r=prod";   // token carries VO "cms"
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('T', kStod, pl);
    dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  feedActivity(dec, 2, 7);
  feedOpen();
  feedClose();

  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["wlcg.vo"], "cms");    // token VO; experiment separate
  EXPECT_EQ(j["attributes"]["scitags.experiment"], "atlas");
}

// Without a registry, only the numeric ids appear (no names, no VO fallback).
TEST_F(Transfer, ScitagsNumericWithoutRegistry)
{
  feedUserMap();
  feedActivity(dec, 2, 7);
  feedOpen();
  feedClose();

  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["scitags.experiment_id"], 2);
  EXPECT_EQ(j["attributes"]["scitags.activity_id"], 7);
  EXPECT_FALSE(j["attributes"].contains("scitags.experiment"));
  EXPECT_FALSE(j["attributes"].contains("scitags.activity"));
  EXPECT_FALSE(j["attributes"].contains("wlcg.vo"));
}

// A missing registry file is reported, leaving numeric ids untouched.
TEST(XrdMonCollect, ScitagsMissingFileReturnsFalse)
{
  XrdMonDecode dec([](const std::string&){});
  EXPECT_FALSE(dec.LoadScitags("/nonexistent/scitags.json"));
}

// A background refresh (LoadScitagsJson) swaps the registry whole: a later load
// with the same ids but new names is reflected on subsequent transfers.
TEST_F(Transfer, ScitagsJsonReloadReflectsUpdate)
{
  ASSERT_TRUE(dec.LoadScitagsJson(
     R"({"experiments":[{"expId":2,"expName":"atlas","activities":[
        {"activityId":7,"activityName":"production"}]}]})"));
  ASSERT_TRUE(dec.LoadScitagsJson(           // the published registry changed
     R"({"experiments":[{"expId":2,"expName":"cms","activities":[
        {"activityId":7,"activityName":"analysis"}]}]})"));

  feedUserMap();
  feedActivity(dec, 2, 7);
  feedOpen();
  feedClose();

  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["scitags.experiment"], "cms");
  EXPECT_EQ(j["attributes"]["scitags.activity"], "analysis");
  EXPECT_FALSE(j["attributes"].contains("wlcg.vo"));
}

// A failed re-fetch (unparseable, or no "experiments" array) returns false and
// leaves the previously loaded registry intact.
TEST_F(Transfer, ScitagsJsonBadInputKeepsRegistry)
{
  ASSERT_TRUE(dec.LoadScitagsJson(
     R"({"experiments":[{"expId":2,"expName":"atlas","activities":[
        {"activityId":7,"activityName":"production"}]}]})"));
  EXPECT_FALSE(dec.LoadScitagsJson("not json at all"));
  EXPECT_FALSE(dec.LoadScitagsJson(R"({"no_experiments":true})"));

  feedUserMap();
  feedActivity(dec, 2, 7);
  feedOpen();
  feedClose();

  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["scitags.experiment"], "atlas");   // unchanged
  EXPECT_FALSE(j["attributes"].contains("wlcg.vo"));
}

// Feed a 'u' map for dictid 7 with a custom CGI tail after the descriptor.
static void feedUserMapTail(XrdMonDecode& dec, const std::string& tail)
{
   W body; body.u32(7);
   std::string info = "xroot/alice.123:4@198.51.100.7\n" + tail;
   std::vector<unsigned char> pl = body.b;
   pl.insert(pl.end(), info.begin(), info.end());
   auto pkt = packet('u', kStod, pl);
   dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());
}

TEST_F(Transfer, AuthTailEnrichesTransfer)
{
  // Full auth payload (as built by MonAuth, with the login appinfo appended).
  feedUserMapTail(dec, "&p=gsi&n=alice&h=198.51.100.7&o=atlas&r=production"
                       "&g=/atlas&m=&R=v5.6.1&x=xrdcp&y=&I=4");
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.auth.method"], "gsi");
  EXPECT_EQ(j["attributes"]["wlcg.vo"], "atlas");   // auth-derived VO (no token here)
  EXPECT_EQ(j["attributes"]["user.roles"], json::array({"production"}));
  EXPECT_EQ(j["attributes"]["user_agent.version"], "v5.6.1");
  EXPECT_EQ(j["attributes"]["network.type"], "ipv4");
  EXPECT_EQ(j["attributes"]["user_agent.name"], "xrdcp");   // &x= executable
  EXPECT_EQ(j["attributes"]["user.id"], "alice");           // &n= login DN
  EXPECT_EQ(j["attributes"]["client.address"], "198.51.100.7");
}

// An HTTP(S) session (descriptor prot "https") surfaces the semconv
// url.scheme, with network.protocol.name normalized to "http".
TEST_F(Transfer, HttpSessionGetsUrlScheme)
{
  W body; body.u32(7);
  std::string info = "https/alice.123:4@wn.example.org\n&p=ztn&I=4";
  std::vector<unsigned char> pl = body.b;
  pl.insert(pl.end(), info.begin(), info.end());
  auto pkt = packet('u', kStod, pl);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["url.scheme"], "https");
  EXPECT_EQ(j["attributes"]["network.protocol.name"], "http");
}

// client.address is name-first (semconv): the server-resolved descriptor host
// wins, and the numeric '&a=' IP is then kept as network.peer.address.
TEST_F(Transfer, ClientNameWinsAddressAndIpBecomesPeer)
{
  W body; body.u32(7);
  std::string info = "xroot/alice.123:4@wn.example.org\n&p=gsi&a=198.51.100.7&I=4";
  std::vector<unsigned char> pl = body.b;
  pl.insert(pl.end(), info.begin(), info.end());
  auto pkt = packet('u', kStod, pl);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["client.address"], "wn.example.org");
  EXPECT_EQ(j["attributes"]["network.peer.address"], "198.51.100.7");
  EXPECT_FALSE(j["attributes"].contains("xrootd.client.host"));
}

// The auth-CGI &o= is honoured only for methods that can convey a real VO;
// a unix-auth &o= (e.g. a unix group from a custom seclib) is dropped.
TEST_F(Transfer, AuthVoIgnoredForNonVoMethod)
{
  feedUserMapTail(dec, "&p=unix&n=alice&h=198.51.100.7&o=zp&r=&g=zp users"
                       "&m=&R=v5.6.1&x=xrdcp&y=&I=4");
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.auth.method"], "unix");
  EXPECT_FALSE(j["attributes"].contains("wlcg.vo"));    // &o= gated out
  EXPECT_EQ(j["attributes"]["wlcg.groups"], "zp users"); // groups stay
}

// sss registers the entity through a trusted key holder, so its &o= is a VO.
TEST_F(Transfer, AuthVoKeptForSss)
{
  feedUserMapTail(dec, "&p=sss&n=daemon&h=h&o=eos&r=&g=&m=&I=4");
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["wlcg.vo"], "eos");
}

TEST_F(Transfer, NoAuthLoginAppinfoStillEnriches)
{
  // Without "... auth" the 'u' tail is only the login appinfo (no &p=/&o=).
  feedUserMapTail(dec, "&R=v5.6.1&x=xrdcp&y=&I=6");
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["user_agent.version"], "v5.6.1");
  EXPECT_EQ(j["attributes"]["network.type"], "ipv6");
  EXPECT_FALSE(j["attributes"].contains("xrootd.auth.method"));  // no &p= without auth
  EXPECT_FALSE(j["attributes"].contains("wlcg.vo"));             // no &o= and no token
  EXPECT_FALSE(j["attributes"].contains("xrootd.client.site")); // no &S= -> no client.site
}

TEST_F(Transfer, ClientSiteAdvertised)
{
  // A client that sets XRD_SITE/XRDSITE makes the server append &S= to the
  // login appinfo; the collector surfaces it as client.site.
  feedUserMapTail(dec, "&R=v5.6.1&x=xrdcp&y=&S=CERN-PROD&I=4");
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.client.site"], "CERN-PROD");
  EXPECT_EQ(j["attributes"]["user_agent.version"], "v5.6.1");  // neighbouring fields intact
  EXPECT_EQ(j["attributes"]["user_agent.name"], "xrdcp");
}

// user_agent.name falls back to "xrootd" when only a client release (&R=) is
// known (no &x= executable name).
TEST_F(Transfer, UserAgentNameFallsBackToXrootd)
{
  feedUserMapTail(dec, "&R=v5.6.1&I=4");   // no &x=
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["user_agent.name"], "xrootd");
  EXPECT_EQ(j["attributes"]["user_agent.version"], "v5.6.1");
}

// The auth-reported '&h=' hostname is preferred for client.address (semconv:
// name over IP) when the descriptor '@host' is only an IP literal; the numeric
// address is then kept as network.peer.address.
TEST_F(Transfer, AuthHostNameWinsOverDescriptorIp)
{
  W body; body.u32(7);
  // Descriptor host is an IP; &h= carries a resolved name.
  std::string info = "xroot/alice.123:4@198.51.100.7\n&p=gsi&h=wn.example.org&I=4";
  std::vector<unsigned char> pl = body.b;
  pl.insert(pl.end(), info.begin(), info.end());
  auto pkt = packet('u', kStod, pl);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["client.address"], "wn.example.org");
  EXPECT_EQ(j["attributes"]["network.peer.address"], "198.51.100.7");
}

// The token's mapped username (&n=) is authoritative over the descriptor's
// unverified unix name for user.name; a login DN (&n=) maps to user.id but the
// token subject (&s=) wins.
TEST_F(Transfer, TokenUsernameAndSubjectWin)
{
  // Descriptor user "alice"; login DN via &n=.
  feedUserMapTail(dec, "&p=gsi&n=/DC=org/CN=alice&I=4");
  { W body; body.u32(7);
    std::string info = "&Uc=7&s=https://issuer/sub42&n=bob";  // mapped user "bob"
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('T', kStod, pl);
    dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  feedOpen();
  feedClose();

  ASSERT_FALSE(lastDoc.empty());
  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["user.name"], "bob");                 // token wins
  EXPECT_EQ(j["attributes"]["user.id"], "https://issuer/sub42");  // subject wins over DN
}

TEST_F(Transfer, WriteOperationDerived)
{
  feedUserMap();
  feedOpen();
  // Custom close carrying only write bytes -> operation "write".
  W body;
  body.u32(100);                 // fileID
  body.u64(0);                   // Xfr.read
  body.u64(0);                   // Xfr.readv
  body.u64(2097152);             // Xfr.write
  auto payload = todRec(kCloseT, 42);
  auto r = rec(0 /*isClose*/, 0 /*no OPS*/, body.b);
  payload.insert(payload.end(), r.begin(), r.end());
  auto pkt = packet('f', kStod, payload);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());

  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.operation.name"], "write");
  EXPECT_EQ(j["attributes"]["xrootd.write_bytes"], 2097152);
  EXPECT_EQ(j["eventName"], "xrootd.write");
}

namespace
{
// A close carrying chosen byte totals and recFlag, for the fixture's file 100.
std::vector<unsigned char> closePkt(int64_t rd, int64_t rv, int64_t wr,
                                    uint8_t flag)
{
   W body;
   body.u32(100);              // fileID (matches feedOpen)
   body.u64((uint64_t)rd);     // Xfr.read
   body.u64((uint64_t)rv);     // Xfr.readv
   body.u64((uint64_t)wr);     // Xfr.write
   auto payload = todRec(kCloseT, 42);
   auto r = rec(0 /*isClose*/, flag, body.b);
   payload.insert(payload.end(), r.begin(), r.end());
   return packet('f', kStod, payload);
}
}

// The ops block's readv segment extremes reach the document, so a consumer can
// tell a few large vectored reads from many small ones at the same byte total.
TEST_F(Transfer, ReportsReadvSegmentExtremes)
{
  feedUserMap();
  feedOpen();

  W body;
  body.u32(100);                     // fileID
  body.u64(0);                       // Xfr.read
  body.u64(65536);                   // Xfr.readv
  body.u64(0);                       // Xfr.write
  body.u32(0);                       // read ops
  body.u32(4);                       // readv ops
  body.u32(0);                       // write ops
  body.u16(2); body.u16(17);         // rsMin, rsMax
  body.u64(41);                      // rsegs
  body.u32(0); body.u32(0);          // rdMin, rdMax
  body.u32(1024); body.u32(32768);   // rvMin, rvMax
  body.u32(0); body.u32(0);          // wrMin, wrMax
  auto payload = todRec(kCloseT, 42);
  auto r = rec(0 /*isClose*/, 0x02 /*hasOPS*/, body.b);
  payload.insert(payload.end(), r.begin(), r.end());
  auto pkt = packet('f', kStod, payload);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());

  json j = json::parse(lastDoc);
  const json& a = j["attributes"];
  EXPECT_EQ(a["xrootd.readv_ops"],       4);
  EXPECT_EQ(a["xrootd.readv_segs"],      41);
  EXPECT_EQ(a["xrootd.readv_segs_min"],  2);
  EXPECT_EQ(a["xrootd.readv_segs_max"],  17);
  EXPECT_EQ(a["xrootd.readv_min"],       1024);
  EXPECT_EQ(a["xrootd.readv_max"],       32768);
  // The server zeroes the pairs whose operation never ran, so they are present
  // as 0/0 rather than absent: every close carries the same field set.
  EXPECT_EQ(a["xrootd.read_min"],  0);
  EXPECT_EQ(a["xrootd.write_max"], 0);
}

// A close reports its raw byte totals and nothing derived from them: whether
// the file moved in its entirety is left to the consumer, which has file.size
// and the counters right here in the document.
TEST_F(Transfer, ReportsRawByteTotalsOnly)
{
  feedUserMap();
  feedOpen();                                  // fsz = 123456
  auto pkt = closePkt(4096, 0, 0, 0);          // read a small slice
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());

  json j = json::parse(lastDoc);
  const json& a = j["attributes"];
  EXPECT_EQ(a["file.size"],            123456);
  EXPECT_EQ(a["xrootd.read_bytes"],    4096);
  EXPECT_EQ(a["xrootd.readv_bytes"],   0);
  EXPECT_EQ(a["xrootd.write_bytes"],   0);
  EXPECT_EQ(a["xrootd.operation.name"], "read");
  EXPECT_FALSE(a.contains("xrootd.transfer.kind"));
  EXPECT_FALSE(a.contains("xrootd.whole_file"));
}

// A disconnect-driven close is flagged, so a consumer can tell an interrupted
// operation from one the client concluded itself.
TEST_F(Transfer, ForcedCloseIsFlagged)
{
  feedUserMap();
  feedOpen();
  auto pkt = closePkt(0, 0, 2097152, 0x01 /*forced*/);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());

  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.forced_close"], true);
  EXPECT_EQ(j["attributes"]["xrootd.operation.name"], "write");
}

// A close with no matching open carries no file.size and says so, which is what
// a consumer needs to know before comparing bytes against a size.
TEST_F(Transfer, OrphanCloseReportsNoOpen)
{
  auto pkt = closePkt(10485760, 0, 0, 0);      // no preceding open
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());

  json j = json::parse(lastDoc);
  EXPECT_EQ(j["attributes"]["xrootd.open_seen"], false);
  EXPECT_FALSE(j["attributes"].contains("file.size"));
  EXPECT_EQ(j["attributes"]["xrootd.read_bytes"], 10485760);
}

// Opens, closes and -- when the server sends the ops block -- the individual
// requests all land in one series, separated by the operation label.
TEST_F(Transfer, OperationsAggregateIntoIoTotal)
{
  XrdMetrics::Collector collector("xrootd");
  std::string sink;
  XrdMonDecode d([&](const std::string& s){ sink = s; }, nullptr,
                 false, false, false, false, &collector.subsystem("collector"));

  { W body; body.u32(7);
    std::string info = "xroot/alice.1:2@wn.example.org\n";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('u', kStod, pl);
    d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  { W body; body.u32(100); body.u64(123456); body.u32(7);
    std::string lfn = "/store/data/file.root"; body.raw(lfn); body.u8(0);
    auto payload = todRec(kOpenT, 42);
    auto r = rec(1 /*isOpen*/, 0x03, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }
  { W body; body.u32(100); body.u64(4096); body.u64(8192); body.u64(0);
    body.u32(3);                   // read ops
    body.u32(2);                   // readv ops
    body.u32(0);                   // write ops
    body.u16(0); body.u16(0);      // rsMin, rsMax
    body.u64(0);                   // rsegs
    body.u32(0); body.u32(0);      // rdMin, rdMax
    body.u32(0); body.u32(0);      // rvMin, rvMax
    body.u32(0); body.u32(0);      // wrMin, wrMax
    auto payload = todRec(kCloseT, 42);
    auto r = rec(0 /*isClose*/, 0x02 /*hasOPS*/, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    d.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); }

  std::string out;
  XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  const std::string id  = "{cluster=\"unknown\",server=\"10.0.0.1\"";
  const std::string pfx = "xrootd_collector_io_total" + id;
  EXPECT_NE(out.find(pfx + ",operation=\"open\"} 1"),  std::string::npos) << out;
  EXPECT_NE(out.find(pfx + ",operation=\"close\"} 1"), std::string::npos) << out;
  EXPECT_NE(out.find(pfx + ",operation=\"read\"} 3"),  std::string::npos) << out;
  EXPECT_NE(out.find(pfx + ",operation=\"readv\"} 2"), std::string::npos) << out;
  // A write count of zero is not a series: nothing was written.
  EXPECT_EQ(out.find("xrootd_collector_io_total" + id + ",operation=\"write\""),
            std::string::npos) << out;

  // Volumes split the same three ways, and come from the xfr block rather than
  // the ops one -- so readv bytes are their own series, not folded into read.
  const std::string bpx = "xrootd_collector_io_bytes_total" + id;
  EXPECT_NE(out.find(bpx + ",operation=\"read\"} 4096"),  std::string::npos) << out;
  EXPECT_NE(out.find(bpx + ",operation=\"readv\"} 8192"), std::string::npos) << out;
  EXPECT_EQ(out.find(bpx + ",operation=\"write\""), std::string::npos) << out;

  // The same volumes attributed to the client application. No server label:
  // app x server is the one product with real cardinality risk, and the login
  // here carries only '&x=', so user_agent.name supplies the name.
  const std::string apx = "xrootd_collector_app_io_bytes_total"
                          "{cluster=\"unknown\",app=\"unknown\"";
  EXPECT_NE(out.find(apx + ",operation=\"read\"} 4096"),  std::string::npos) << out;
  EXPECT_NE(out.find(apx + ",operation=\"readv\"} 8192"), std::string::npos) << out;

  // Metrics retired with the whole-file classification and with the per-close
  // aggregates, none of which described a whole-file transfer.
  for (const char* gone : {"xrootd_collector_transfers_total",
                           "xrootd_collector_accesses_total",
                           "xrootd_collector_read_bytes_total",
                           "xrootd_collector_write_bytes_total",
                           "xrootd_collector_vo_transfers_total",
                           "xrootd_collector_locality_transfers_total",
                           "xrootd_collector_transfer_size_bytes",
                           "xrootd_collector_transfer_duration_seconds",
                           "xrootd_collector_active_transfers",
                           "xrootd_collector_failed_operations_total"})
      EXPECT_EQ(out.find(gone), std::string::npos) << gone << "\n" << out;
}

// Feed a '=' server-ident record so srv.ident.host is populated for the
// LAN/WAN heuristic. The fixture decoder is keyed by ("10.0.0.1:9930", kStod).
static void feedIdent(XrdMonDecode& dec, const std::string& host)
{
   std::string info = "=/xrootd.1:2@" + host +
                      "\n&site=S&port=1094&inst=mgr&pgm=xrootd&ver=v6";
   W body; body.u32(0);
   std::vector<unsigned char> pl = body.b;
   pl.insert(pl.end(), info.begin(), info.end());
   auto pkt = packet('=', kStod, pl);
   dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());
}

// Both metric labels come from the '=' ident record, so both are provisional
// until it lands and both flip at once. Before it, a server is an unknown
// cluster at its bare source IP; after it, the advertised cluster and host.
// Nothing
// retroactively merges the two series -- the earlier one simply stops
// advancing, which is why rate() heals but increase() across the flip does not.
TEST(XrdMonCollect, IdentRelabelsMetricSeries)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr, false, false, false,
                   false, &collector.subsystem("collector"));

  auto ping = [&]{ auto pkt = packet('Z', kStod, std::vector<unsigned char>(16, 0));
                   dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size()); };

  ping();                              // unidentified: 1 packet
  feedIdent(dec, "srv.example.org");   // itself counted before it relabels: 2
  ping();                              // identified: 1 packet

  std::string out;
  XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_packets_total{cluster=\"unknown\","
                     "server=\"10.0.0.1\",stream=\"main\"} 2"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_packets_total{cluster=\"S\","
                     "server=\"srv.example.org\",stream=\"main\"} 1"),
            std::string::npos) << out;
  // The UDP source port never reaches a label. It is the server's ephemeral
  // monitor socket, so keeping it would fork every series on a server restart.
  EXPECT_EQ(out.find("10.0.0.1:9930"), std::string::npos) << out;
}

// servers{cluster} answers "how many are reporting", and moves a server
// between clusters when its ident lands. server_info carries the identity that
// would otherwise need a label on every series.
TEST(XrdMonCollect, ServersAndInfoPerCluster)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr, false, false, false,
                   false, &collector.subsystem("collector"));

  auto ping = [&](const char* from)
     {auto pkt = packet('Z', kStod, std::vector<unsigned char>(16, 0));
      dec.Process(from, (const char*)pkt.data(), pkt.size()); };
  auto scrape = [&]{ std::string out;
                     XrdMetrics::PrometheusTextSerializer ser(out);
                     collector.serialize(ser); return out; };

  ping("10.0.0.1:9930");
  ping("10.0.0.2:9930");
  EXPECT_NE(scrape().find("xrootd_collector_servers{cluster=\"unknown\"} 2"),
            std::string::npos) << scrape();

  feedIdent(dec, "srv.example.org");   // relabels 10.0.0.1 into cluster S
  std::string out = scrape();
  EXPECT_NE(out.find("xrootd_collector_servers{cluster=\"S\"} 1"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_servers{cluster=\"unknown\"} 1"),
            std::string::npos) << out;
  // The identity moves into labels, with the numeric address kept alongside
  // the resolved name so a dashboard can map either way.
  EXPECT_NE(out.find("xrootd_collector_server_info{cluster=\"S\","
                     "server=\"srv.example.org\",ip=\"10.0.0.1\","
                     "service_name=\"mgr\",program=\"xrootd\","
                     "version=\"v6\"} 1"),
            std::string::npos) << out;
  // Prometheus stamps its own `instance` at scrape time and would rename a
  // collision to exported_instance, so ours must not be called that.
  EXPECT_EQ(out.find("instance=\""), std::string::npos) << out;
  // The WLCG site is not on the wire and is added downstream, so the collector
  // must not invent one: all.sitename names the cluster here.
  EXPECT_EQ(out.find("site=\""), std::string::npos) << out;
}

// A name of only dots is XrdOucSiteName's sanitization of an all-invalid name;
// it carries no more information than an absent one, so it must not become a
// label value of its own.
TEST(XrdMonCollect, AllDotClusterIsUnknown)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr, false, false, false,
                   false, &collector.subsystem("collector"));

  std::string info = "=/xrootd.1:2@srv.example.org"
                     "\n&site=...&port=1094&inst=mgr&pgm=xrootd&ver=v6";
  W body; body.u32(0);
  std::vector<unsigned char> pl = body.b;
  pl.insert(pl.end(), info.begin(), info.end());
  auto pkt = packet('=', kStod, pl);
  dec.Process("10.0.0.1:9930", (const char*)pkt.data(), pkt.size());

  auto z = packet('Z', kStod, std::vector<unsigned char>(16, 0));
  dec.Process("10.0.0.1:9930", (const char*)z.data(), z.size());

  std::string out;
  XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("cluster=\"unknown\",server=\"srv.example.org\""),
            std::string::npos) << out;
  EXPECT_EQ(out.find("cluster=\"...\""), std::string::npos) << out;
}

TEST_F(Transfer, IsLocalWhenSameDomain)
{
  feedIdent(dec, "srv.example.org");   // client is wn.example.org -> same domain
  feedUserMap();
  feedOpen();
  feedClose();

  json j = json::parse(lastDoc);
  ASSERT_TRUE(j["attributes"].contains("xrootd.is_local"));
  EXPECT_EQ(j["attributes"]["xrootd.is_local"], true);
}

TEST_F(Transfer, IsRemoteWhenDifferentDomain)
{
  feedIdent(dec, "srv.other.net");     // different registered domain -> remote
  feedUserMap();
  feedOpen();
  feedClose();

  json j = json::parse(lastDoc);
  ASSERT_TRUE(j["attributes"].contains("xrootd.is_local"));
  EXPECT_EQ(j["attributes"]["xrootd.is_local"], false);
}

TEST_F(Transfer, IsLocalAbsentWithoutServerHost)
{
  // No '=' ident -> server host unknown -> heuristic cannot decide.
  feedUserMap();
  feedOpen();
  feedClose();

  json j = json::parse(lastDoc);
  EXPECT_FALSE(j["attributes"].contains("xrootd.is_local"));
}

namespace
{
// Feed a u/open/close trio from a chosen UDP source so server.* reflects that
// sender (the fixture hardwires 10.0.0.1; loopback needs ::1).
void feedTransferFrom(XrdMonDecode& dec, const std::string& src)
{
  { W body; body.u32(7);
    std::string info = "xroot/alice.123:4@wn.example.org\n";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('u', kStod, pl);
    dec.Process(src, (const char*)pkt.data(), pkt.size()); }
  { W body; body.u32(100); body.u64(123456); body.u32(7);
    std::string lfn = "/store/data/file.root"; body.raw(lfn); body.u8(0);
    auto payload = todRec(kOpenT, 42);
    auto r = rec(1, 0x01 | 0x02, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process(src, (const char*)pkt.data(), pkt.size()); }
  { W body; body.u32(100); body.u64(1024); body.u64(0); body.u64(0);
    auto payload = todRec(kCloseT, 42);
    auto r = rec(0, 0, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process(src, (const char*)pkt.data(), pkt.size()); }
}
}

// A co-located server reports from the loopback address; the collector should
// substitute a real local identity (the FQDN, or the kernel host name when the
// FQDN is itself a loopback name) rather than emit the literal "::1".
TEST(XrdMonCollect, LoopbackServerResolvesToLocalHost)
{
  std::string doc;
  XrdMonDecode dec([&](const std::string& d){ doc = d; });  // resolve on (default)
  feedTransferFrom(dec, "::1:9930");

  ASSERT_FALSE(doc.empty());
  json j = json::parse(doc);
  ASSERT_TRUE(j["resource"].contains("server.address"));  // always usable
  std::string name = j["resource"]["server.address"];
  EXPECT_NE(name, "::1");
  EXPECT_NE(name, "localhost");                   // never a loopback name
  EXPECT_FALSE(j["resource"].contains("host.name"));  // single canonical field
  // When the advertised FQDN is usable, it is the name substituted.
  const char* me = XrdNetUtils::MyHostName();
  if (me && *me && std::string(me).find(':') == std::string::npos
      && strncmp(me, "localhost", 9) != 0)
     {EXPECT_EQ(name, me);}
}

// With resolution disabled, the loopback source stays numeric.
TEST(XrdMonCollect, NoResolveKeepsLoopbackNumeric)
{
  std::string doc;
  XrdMonDecode dec([&](const std::string& d){ doc = d; });
  dec.SetResolveHosts(false);
  feedTransferFrom(dec, "::1:9930");

  json j = json::parse(doc);
  EXPECT_EQ(j["resource"]["server.address"], "::1");
}

namespace
{
// A minimal valid 'u' map packet (dictid + descriptor) with a chosen pseq.
std::vector<unsigned char> userPkt(uint32_t dictid, uint8_t pseq)
{
   W body; body.u32(dictid);
   std::string info = "xroot/u.1:2@h\n";
   std::vector<unsigned char> pl = body.b;
   pl.insert(pl.end(), info.begin(), info.end());
   auto pkt = packet('u', kStod, pl);
   pkt[1] = pseq;        // header pseq is the second byte
   return pkt;
}
}

TEST(XrdMonCollect, PacketLossDetected)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));

  for (uint8_t seq : {0, 1, 3, 4})   // 2 is missing -> one lost packet
     {auto p = userPkt(seq, seq);
      dec.Process("h:1", (const char*)p.data(), p.size());}

  EXPECT_EQ(dec.GetStats().lost, 1u);
  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_packets_lost_total{cluster=\"unknown\",server=\"h\",stream=\"main\"} 1"),
            std::string::npos) << out;
  // packets_total mirrors the lost metric's {server, stream} labels: the four
  // 'u' packets received are the denominator for a per-source loss percentage.
  EXPECT_NE(out.find("xrootd_collector_packets_total{cluster=\"unknown\",server=\"h\",stream=\"main\"} 4"),
            std::string::npos) << out;
}

// The server does NOT stamp one pseq per destination: the f-stream (and each
// g-stream provider) runs its own counter while everything else shares the
// per-destination one. Interleaving two independent counters must not be
// mistaken for loss, and a real gap must be attributed to its stream.
TEST(XrdMonCollect, PacketLossTrackedPerStream)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));

  // An empty (header-only after the TOD-less payload) f-stream packet with a
  // chosen pseq: one 24-byte isTime record keeps it structurally valid.
  auto fstatPkt = [](uint8_t pseq)
     {auto pkt = packet('f', kStod, todRec(kOpenT, 42));
      pkt[1] = pseq;
      return pkt;
     };

  // Interleave the two independent counters, each gap-free: u 0, f 0, u 1,
  // f 1, ... A shared tracker would see 0,0,1,1,2,2 and count phantom gaps.
  for (uint8_t seq : {0, 1, 2, 3})
     {auto u = userPkt(seq, seq);
      dec.Process("h:1", (const char*)u.data(), u.size());
      auto f = fstatPkt(seq);
      dec.Process("h:1", (const char*)f.data(), f.size());}
  EXPECT_EQ(dec.GetStats().lost, 0u);

  // Now a real gap on the f stream only (4 -> 6): attributed to stream "f".
  auto f = fstatPkt(6);
  dec.Process("h:1", (const char*)f.data(), f.size());
  EXPECT_EQ(dec.GetStats().lost, 2u);

  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_packets_lost_total{cluster=\"unknown\",server=\"h\",stream=\"f\"} 2"),
            std::string::npos) << out;
  // No phantom loss is attributed to the shared "main" stream.
  EXPECT_EQ(out.find("xrootd_collector_packets_lost_total{cluster=\"unknown\",server=\"h\",stream=\"main\""),
            std::string::npos) << out;
  // packets_total is split per stream too: 4 'u' (main) and 5 'f' received.
  EXPECT_NE(out.find("xrootd_collector_packets_total{cluster=\"unknown\",server=\"h\",stream=\"main\"} 4"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_packets_total{cluster=\"unknown\",server=\"h\",stream=\"f\"} 5"),
            std::string::npos) << out;
}

// Malformed packets are counted per stream and reason.
TEST(XrdMonCollect, MalformedLabeledByStreamAndReason)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));

  // A 'u' packet whose plen claims more than the datagram carries.
  auto u = userPkt(1, 0);
  u[2] = 0x40; u[3] = 0;               // plen = 16384 > actual size
  EXPECT_FALSE(dec.Process("h:1", (const char*)u.data(), u.size()));

  // A 't' payload that is not a whole number of 16-byte records.
  std::vector<unsigned char> tr(24, 0); // 1.5 records
  auto t = packet('t', kStod, tr);
  dec.Process("h:1", (const char*)t.data(), t.size());

  // An 'f' packet with an impossible record size.
  W bad; bad.u8(1); bad.u8(0); bad.u16(3); bad.u32(0);  // recSize 3 < 8
  auto f = packet('f', kStod, bad.b);
  dec.Process("h:1", (const char*)f.data(), f.size());

  EXPECT_EQ(dec.GetStats().malformed, 3u);
  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_malformed_total{cluster=\"unknown\",server=\"h\",stream=\"u\",reason=\"bad_plen\"} 1"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_malformed_total{cluster=\"unknown\",server=\"h\",stream=\"t\",reason=\"trailing_bytes\"} 1"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_malformed_total{cluster=\"unknown\",server=\"h\",stream=\"f\",reason=\"bad_record\"} 1"),
            std::string::npos) << out;
}

TEST(XrdMonCollect, DictionaryEviction)
{
  XrdMonDecode dec([](const std::string&){});
  dec.SetMaxEntries(10);

  // Feed 100 distinct user dictids; the cap keeps the table bounded.
  for (uint32_t id = 1; id <= 100; id++)
     {auto p = userPkt(id, (uint8_t)id);
      dec.Process("h:1", (const char*)p.data(), p.size());}

  EXPECT_GT(dec.GetStats().evicted, 0u);
}

namespace
{
// Minimal 'u' user map for dictid 7 on a chosen sender.
void feedUser7(XrdMonDecode& dec, const std::string& src)
{
   W body; body.u32(7);
   std::string info = "xroot/alice.1:2@wn.example.org\n";
   std::vector<unsigned char> pl = body.b;
   pl.insert(pl.end(), info.begin(), info.end());
   auto pkt = packet('u', kStod, pl);
   dec.Process(src, (const char*)pkt.data(), pkt.size());
}

// An 'f' open record for a chosen fileID/lfn (user dictid 7).
void feedOpenId(XrdMonDecode& dec, const std::string& src, uint32_t fileID,
                const std::string& lfn)
{
   W body; body.u32(fileID); body.u64(123456); body.u32(7);
   body.raw(lfn); body.u8(0);
   auto payload = todRec(kOpenT, 42);
   auto r = rec(1 /*isOpen*/, 0x01 | 0x02 /*hasLFN|hasRW*/, body.b);
   payload.insert(payload.end(), r.begin(), r.end());
   auto pkt = packet('f', kStod, payload);
   dec.Process(src, (const char*)pkt.data(), pkt.size());
}

// An 'f' in-flight transfer (isXfr) snapshot for a fileID; touches the open.
void feedXfrId(XrdMonDecode& dec, const std::string& src, uint32_t fileID)
{
   W body; body.u32(fileID);
   auto payload = todRec(kCloseT, 42);
   auto r = rec(3 /*isXfr*/, 0, body.b);
   payload.insert(payload.end(), r.begin(), r.end());
   auto pkt = packet('f', kStod, payload);
   dec.Process(src, (const char*)pkt.data(), pkt.size());
}

// An 'f' close record for a fileID (no OPS, minimal byte totals).
void feedCloseId(XrdMonDecode& dec, const std::string& src, uint32_t fileID)
{
   W body; body.u32(fileID); body.u64(1024); body.u64(0); body.u64(0);
   auto payload = todRec(kCloseT, 42);
   auto r = rec(0 /*isClose*/, 0, body.b);
   payload.insert(payload.end(), r.begin(), r.end());
   auto pkt = packet('f', kStod, payload);
   dec.Process(src, (const char*)pkt.data(), pkt.size());
}
}

// A long-lived but still-active open (kept warm by its in-flight xfr snapshots)
// must survive eviction while cold, stranded opens are dropped first.
TEST(XrdMonCollect, WarmEntrySurvivesEviction)
{
  std::string doc;
  XrdMonDecode dec([&](const std::string& d){ doc = d; });
  dec.SetMaxBytes(2000);                         // room for ~16 open entries
  feedUser7(dec, "h:1");
  feedOpenId(dec, "h:1", 1, "/warm/file.root");  // the long-lived, active open

  for (uint32_t id = 100; id < 200; id++)        // a flood of cold opens
     {feedOpenId(dec, "h:1", id, "/cold/file.root");
      feedXfrId(dec, "h:1", 1);                   // keep the warm open at the MRU
     }
  EXPECT_GT(dec.GetStats().evicted, 0u);         // the budget was enforced

  feedCloseId(dec, "h:1", 1);                     // the warm open still joins
  json jw = json::parse(doc);
  EXPECT_EQ(jw["attributes"]["xrootd.open_seen"], true);
  EXPECT_EQ(jw["attributes"]["file.path"], "/warm/file.root");

  feedCloseId(dec, "h:1", 100);                   // an early cold open was evicted
  json jc = json::parse(doc);
  EXPECT_EQ(jc["attributes"]["xrootd.open_seen"], false);
}

// The resident state stays within the byte budget no matter how many distinct
// opens arrive without their closes.
TEST(XrdMonCollect, MemoryStaysUnderBudget)
{
  XrdMonDecode dec([](const std::string&){});
  dec.SetMaxBytes(4000);
  feedUser7(dec, "h:1");
  for (uint32_t id = 1; id <= 500; id++)
     feedOpenId(dec, "h:1", id, "/store/data/some/long/path/file.root");

  EXPECT_LE(dec.ResidentBytes(), 4000u);
  EXPECT_GT(dec.GetStats().evicted, 0u);
}

// On the normal path every open is released by its close: with session
// correlation off (the default) resident memory returns to the baseline and
// nothing is evicted.
TEST(XrdMonCollect, CloseReleasesMemory)
{
  XrdMonDecode dec([](const std::string&){});   // unbounded, sessions off
  feedUser7(dec, "h:1");
  std::size_t base = dec.ResidentBytes();        // just the user entry

  for (uint32_t id = 1; id <= 50; id++)
     feedOpenId(dec, "h:1", id, "/store/data/file.root");
  EXPECT_GT(dec.ResidentBytes(), base);          // opens are charged

  for (uint32_t id = 1; id <= 50; id++)
     feedCloseId(dec, "h:1", id);
  EXPECT_EQ(dec.ResidentBytes(), base);          // each close releases its open
  EXPECT_EQ(dec.GetStats().evicted, 0u);
}

// With session correlation on, closes fold a bounded record into the user's
// rollup, so resident memory stays bounded by the recent-file cap across many
// open/close cycles (it does not grow without limit).
TEST(XrdMonCollect, SessionRollupStaysBounded)
{
  XrdMonDecode dec([](const std::string&){});   // unbounded budget
  dec.SetEmitSessions(true);
  feedUser7(dec, "h:1");

  for (uint32_t id = 1; id <= 80; id++)          // fill past the recent-file cap
     {feedOpenId(dec, "h:1", id, "/store/data/file.root");
      feedCloseId(dec, "h:1", id);}
  std::size_t capped = dec.ResidentBytes();

  for (uint32_t id = 81; id <= 200; id++)        // many more cycles
     {feedOpenId(dec, "h:1", id, "/store/data/file.root");
      feedCloseId(dec, "h:1", id);}
  EXPECT_LE(dec.ResidentBytes(), capped);        // capped: no unbounded growth
  EXPECT_EQ(dec.GetStats().evicted, 0u);
}

// An incarnation idle past the server TTL is reclaimed whole; a freshly-seen
// incarnation is left alone.
TEST(XrdMonCollect, IdleServerReaped)
{
  XrdMonDecode dec([](const std::string&){});
  dec.SetServerTTL(100);

  feedTransferFrom(dec, "10.0.0.1:9930");         // server A (leaves a user entry)
  EXPECT_GT(dec.ResidentBytes(), 0u);

  dec.ReapServers(time(nullptr) + 1000);          // A is now well past its TTL
  EXPECT_EQ(dec.GetStats().reaped, 1u);
  EXPECT_EQ(dec.ResidentBytes(), 0u);             // A's state was reclaimed

  feedTransferFrom(dec, "10.0.0.2:9930");         // server B, just seen
  EXPECT_GT(dec.ResidentBytes(), 0u);
  dec.ReapServers(time(nullptr));                 // B is fresh -> survives
  EXPECT_EQ(dec.GetStats().reaped, 1u);
  EXPECT_GT(dec.ResidentBytes(), 0u);
}

// With no budget, no count cap and no TTL, behaviour is unchanged: everything
// is retained and nothing is evicted.
TEST(XrdMonCollect, UnboundedKeepsEverything)
{
  XrdMonDecode dec([](const std::string&){});
  for (uint32_t id = 1; id <= 200; id++)
     {auto p = userPkt(id, (uint8_t)id);
      dec.Process("h:1", (const char*)p.data(), p.size());}

  EXPECT_EQ(dec.GetStats().evicted, 0u);
  EXPECT_GT(dec.ResidentBytes(), 0u);
}

TEST(XrdMonCollect, FrmStageAndPurge)
{
  XrdMetrics::Collector collector("xrootd");
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); }, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));

  auto frm = [&](char code, const std::string& info)
     {W body; body.u32(0);                // dictid is 0 for x/p
      std::vector<unsigned char> pl = body.b;
      pl.insert(pl.end(), info.begin(), info.end());
      auto pkt = packet(code, kStod, pl);
      dec.Process("h:1", (const char*)pkt.data(), pkt.size());};

  frm('x', "xroot/alice.1:2@wn.example.org\n/store/data/f.root");
  frm('p', "xroot/alice.1:2@wn.example.org\n/store/data/f.root"
           "\n&tod=1700000000&sz=1048576&at=1&ct=2&mt=3&fn=l");

  ASSERT_EQ(docs.size(), 2u);
  json x = json::parse(docs[0]);
  EXPECT_EQ(x["attributes"]["event.name"], "xrootd.frm");
  EXPECT_EQ(x["attributes"]["xrootd.operation.name"], "transfer");
  EXPECT_EQ(x["attributes"]["user.name"], "alice");
  EXPECT_EQ(x["attributes"]["file.path"], "/store/data/f.root");
  json p = json::parse(docs[1]);
  EXPECT_EQ(p["attributes"]["xrootd.operation.name"], "purge");
  EXPECT_EQ(p["attributes"]["file.size"], 1048576);
  EXPECT_EQ(dec.GetStats().frmEvents, 2u);

  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_frm_total{cluster=\"unknown\",server=\"h\",op=\"purge\"} 1"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_frm_purge_bytes_total{cluster=\"unknown\",server=\"h\"} 1048576"),
            std::string::npos) << out;
}

TEST(XrdMonCollect, SessionDiscAndFilesOpenGauge)
{
  XrdMetrics::Collector collector("xrootd");
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); }, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));
  dec.SetEmitSessions(true);

  // 'u' user map: dictid 7 -> bob.
  { W body; body.u32(7);
    std::string info = "xroot/bob.1:2@cli.example.org\n";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('u', kStod, pl);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  // f-packet: an open (file 100) then a disconnect for user 7.
  { W body; body.u32(100); body.u64(123456); body.u32(7);
    std::string lfn = "/store/f.root"; body.raw(lfn); body.u8(0);
    auto payload = todRec(kOpenT, 42);
    auto r = rec(1 /*isOpen*/, 0x03 /*hasLFN|hasRW*/, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    W disc; disc.u32(7);                       // userID in the Hdr union
    auto dr = rec(4 /*isDisc*/, 0, disc.b);
    payload.insert(payload.end(), dr.begin(), dr.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  ASSERT_EQ(docs.size(), 1u);                  // the session document
  json j = json::parse(docs[0]);
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.session");
  EXPECT_EQ(j["attributes"]["user.name"], "bob");
  EXPECT_EQ(j["attributes"]["client.address"], "cli.example.org");
  // The file was opened but never closed, so the session rollup counts no files.
  ASSERT_TRUE(j["attributes"].contains("xrootd.session.files"));
  EXPECT_EQ(j["attributes"]["xrootd.session.files"], 0);
  EXPECT_FALSE(j["attributes"].contains("xrootd.session.recent_files"));
  EXPECT_EQ(dec.GetStats().discs, 1u);

  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_sessions_total{cluster=\"unknown\",server=\"h\"} 1"),
            std::string::npos) << out;
  // The file was opened but its close was never seen; the server reports a
  // session's closes before its disconnect, so the disconnect sweeps the
  // leaked open out of the table instead of inflating the gauge forever.
  EXPECT_NE(out.find("xrootd_collector_files_open{cluster=\"unknown\",server=\"h\"} 0"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_stale_opens_total{cluster=\"unknown\",server=\"h\"} 1"),
            std::string::npos) << out;
  EXPECT_EQ(dec.GetStats().staleOpens, 1u);
  // The session opened and closed within this test, so nothing is live.
  EXPECT_NE(out.find("xrootd_collector_sessions_open{cluster=\"unknown\",server=\"h\"} 0"),
            std::string::npos) << out;
  // One observation, whatever its bucket.
  EXPECT_NE(out.find("xrootd_collector_session_duration_seconds_count"
                     "{cluster=\"unknown\"} 1"),
            std::string::npos) << out;
}

// sessions_open must count sessions, not user-dictionary entries. The entry
// outlives the disconnect (a straggling close still resolves its identity
// against it), so users.size() would never come back down.
TEST(XrdMonCollect, SessionsOpenTracksLiveSessionsOnly)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr, false, false, false,
                   false, &collector.subsystem("collector"));
  dec.SetEmitSessions(true);

  auto userMap = [&](uint32_t dictid, const char* who)
     {W body; body.u32(dictid);
      std::string info = std::string("xroot/") + who + ".1:2@cli.example.org\n";
      std::vector<unsigned char> pl = body.b;
      pl.insert(pl.end(), info.begin(), info.end());
      auto pkt = packet('u', kStod, pl);
      dec.Process("h:1", (const char*)pkt.data(), pkt.size()); };
  auto disconnect = [&](uint32_t dictid)
     {auto payload = todRec(kOpenT, 42);
      W d; d.u32(dictid);
      auto dr = rec(4 /*isDisc*/, 0, d.b);
      payload.insert(payload.end(), dr.begin(), dr.end());
      auto pkt = packet('f', kStod, payload);
      dec.Process("h:1", (const char*)pkt.data(), pkt.size()); };
  auto gauge = [&]{
      std::string out;
      XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
      auto at = out.find("xrootd_collector_sessions_open"
                         "{cluster=\"unknown\",server=\"h\"} ");
      EXPECT_NE(at, std::string::npos) << out;
      return out.substr(at, out.find('\n', at) - at); };

  userMap(7, "bob");
  userMap(8, "eve");
  EXPECT_EQ(gauge().back(), '2');
  // A re-sent 'u' map for a live session is a retransmit, not a new session.
  userMap(7, "bob");
  EXPECT_EQ(gauge().back(), '2');
  disconnect(7);
  EXPECT_EQ(gauge().back(), '1');
  // A duplicated disconnect datagram must not decrement twice.
  disconnect(7);
  EXPECT_EQ(gauge().back(), '1');
  // The same dictid reappearing after its disconnect is a new session.
  userMap(7, "carol");
  EXPECT_EQ(gauge().back(), '2');
  disconnect(7);
  disconnect(8);
  EXPECT_EQ(gauge().back(), '0');
}

// The session document and its root span begin at the login (the 'u' map
// record's arrival, sent by the server at connect time), not at the first
// file close, so the trace covers the whole session.
//
// The collector clock is driven explicitly: the login is stamped when the 'u'
// map is decoded and the rest of the session arrives over the following minute,
// which is what lets the login be the earliest thing known about the session.
// Decoding the whole session in one instant (as a test without a clock does)
// leaves the collector unable to tell the login from the first activity, and it
// then reports the tighter of the two -- see SessionStartsAtFirstActivity.
TEST(XrdMonCollect, SessionStartsAtLogin)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);
  dec.SetEmitSpans(true);

  const time_t now = time(nullptr);   // login stamped on the map's arrival
  time_t clock = now;
  dec.SetClock([&]{ return clock; });

  { W body; body.u32(7);
    std::string info = "xroot/bob.1:2@cli.example.org\n";
    std::vector<unsigned char> pl = body.b;
    pl.insert(pl.end(), info.begin(), info.end());
    auto pkt = packet('u', kStod, pl);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  // Open at now+10, close at now+30 (folds into the session), disc at now+60,
  // each packet arriving when its window ends (server and collector agree).
  clock = now + 10;
  { W body; body.u32(100); body.u64(1000); body.u32(7);
    std::string lfn = "/store/f.root"; body.raw(lfn); body.u8(0);
    auto payload = todRec((int32_t)(now + 10), 42);
    auto r = rec(1 /*isOpen*/, 0x03, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }
  clock = now + 30;
  { W body; body.u32(100); body.u64(1000); body.u64(0); body.u64(0);
    auto payload = todRec((int32_t)(now + 30), 42);
    auto r = rec(0 /*isClose*/, 0, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }
  clock = now + 60;
  { W disc; disc.u32(7);
    auto payload = todRec((int32_t)(now + 60), 42);
    auto dr = rec(4 /*isDisc*/, 0, disc.b);
    payload.insert(payload.end(), dr.begin(), dr.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  json sess, span;
  bool haveSess = false, haveSpan = false;
  for (const auto& d : docs)
     {json x = json::parse(d);
      if (x.contains("kind") && x["name"] == "session")
         {span = x; haveSpan = true;}
      else if (x.contains("severityText")
           &&  x["attributes"].value("event.name", std::string())
               == "xrootd.session") {sess = x; haveSess = true;}
     }
  ASSERT_TRUE(haveSess);
  ASSERT_TRUE(haveSpan);

  // Duration spans login -> disconnect (60s), not first close -> last close
  // (which would be 0 here: only one close was folded), nor first open ->
  // disconnect (50s), and the document says which of those it is.
  EXPECT_EQ(sess["attributes"]["xrootd.session.start_time"], isoOf(now));
  EXPECT_EQ(sess["attributes"]["xrootd.session.duration"].get<double>(), 60.0);
  EXPECT_EQ(sess["attributes"]["xrootd.session.end_time"], isoOf(now + 60));
  EXPECT_EQ(sess["attributes"]["xrootd.session.start_time_source"], "connect");

  // The root span covers the same login -> disconnect range.
  uint64_t sBeg = std::stoull(span["startTimeUnixNano"].get<std::string>());
  uint64_t sEnd = std::stoull(span["endTimeUnixNano"].get<std::string>());
  EXPECT_EQ(sEnd, (uint64_t)(now + 60) * 1000000000ULL);
  EXPECT_EQ(sBeg, (uint64_t)now * 1000000000ULL);
}

namespace {
// Defined with the other session helpers below (same anonymous namespace).
void feedUserN(XrdMonDecode& dec, const std::string& src, uint32_t dictid);

// Pull the session log document (not its span) out of a decoder's output.
json sessionDoc(const std::vector<std::string>& docs)
{
   for (const auto& d : docs)
      {json x = json::parse(d);
       if (!x.contains("kind") && x.contains("attributes")
       &&  x["attributes"].value("event.name", std::string()) == "xrootd.session")
          return x;
      }
   return json();
}

// The session span (the trace root), which carries the same resolved bounds.
json sessionSpan(const std::vector<std::string>& docs)
{
   for (const auto& d : docs)
      {json x = json::parse(d);
       if (x.contains("kind") && x.value("name", std::string()) == "session")
          return x;
      }
   return json();
}

// One 'f' packet: a TOD window ending at `win` followed by `body` records.
void feedF(XrdMonDecode& dec, const std::string& src, int32_t win,
           const std::vector<unsigned char>& body)
{
   auto payload = todRec(win, 42);
   payload.insert(payload.end(), body.begin(), body.end());
   auto pkt = packet('f', kStod, payload);
   dec.Process(src, (const char*)pkt.data(), pkt.size());
}

std::vector<unsigned char> openRec(uint32_t fileID, uint32_t user,
                                   const std::string& lfn)
{
   W b; b.u32(fileID); b.u64(1000); b.u32(user); b.raw(lfn); b.u8(0);
   return rec(1 /*isOpen*/, 0x03, b.b);
}

std::vector<unsigned char> closeRec(uint32_t fileID)
{
   W b; b.u32(fileID); b.u64(1000); b.u64(0); b.u64(0);
   return rec(0 /*isClose*/, 0, b.b);
}

std::vector<unsigned char> discRec(uint32_t user)
{
   W b; b.u32(user);
   return rec(4 /*isDisc*/, 0, b.b);
}
}

// The reported failure: the collector's clock runs far ahead of the reporting
// server's and the session closed no files, so the login stamp used to be
// discarded outright and the document went out with no start time at all (and
// the root span with no startTimeUnixNano, which is not a valid OTLP span).
// The login is now translated into the server's clock instead of being dropped.
TEST(XrdMonCollect, SessionStartTimeAlwaysPresent)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);
  dec.SetEmitSpans(true);

  feedUserN(dec, "h:1", 7);            // stamped on the real (2020s) clock
  feedF(dec, "h:1", kOpenT, discRec(7));   // server windows back in 2023

  json j = sessionDoc(docs);
  ASSERT_FALSE(j.is_null());
  const json& a = j["attributes"];
  ASSERT_TRUE(a.contains("xrootd.session.start_time")) << j.dump();
  ASSERT_TRUE(a.contains("xrootd.session.end_time"));
  ASSERT_TRUE(a.contains("xrootd.session.duration"));
  EXPECT_GE(a["xrootd.session.duration"].get<double>(), 0.0);
  // The login lands on the server's clock, not two years into its future.
  EXPECT_EQ(a["xrootd.session.start_time"], isoOf(kOpenT));
  EXPECT_EQ(a["xrootd.session.start_time_source"], "connect");

  json sp = sessionSpan(docs);
  ASSERT_FALSE(sp.is_null());
  ASSERT_TRUE(sp.contains("startTimeUnixNano")) << sp.dump();
  EXPECT_LE(std::stoull(sp["startTimeUnixNano"].get<std::string>()),
            std::stoull(sp["endTimeUnixNano"].get<std::string>()));
}

// A disconnect for a dictid whose 'u' map was never seen (lost datagram, user
// monitoring off, entry evicted) still yields a complete session document: the
// rollup reads as zero rather than the whole xrootd.session.* block vanishing,
// and the start falls back to the disconnect itself.
TEST(XrdMonCollect, SessionSurvivesLostUserMap)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);
  dec.SetEmitSpans(true);

  feedF(dec, "h:1", kCloseT, discRec(7));   // no 'u' map for dictid 7

  json j = sessionDoc(docs);
  ASSERT_FALSE(j.is_null());
  const json& a = j["attributes"];
  EXPECT_EQ(a["xrootd.session.files"], 0);
  EXPECT_EQ(a["xrootd.session.reads"],  0);
  EXPECT_EQ(a["xrootd.session.writes"], 0);
  EXPECT_EQ(a["xrootd.session.start_time"], isoOf(kCloseT));
  EXPECT_EQ(a["xrootd.session.end_time"],   isoOf(kCloseT));
  EXPECT_EQ(a["xrootd.session.duration"].get<double>(), 0.0);
  EXPECT_EQ(a["xrootd.session.start_time_source"], "disconnect");

  ASSERT_TRUE(sessionSpan(docs).contains("startTimeUnixNano"));
}

// When the login stamp is not the earliest thing known about a session, the
// first *activity* dates it -- and that is the open, not the close: an open
// precedes its close by the whole transfer.
TEST(XrdMonCollect, SessionStartsAtFirstActivity)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);

  const time_t now = time(nullptr);
  time_t clock = now;
  dec.SetClock([&]{ return clock; });

  // Activity is reported before the 'u' map arrives (a retransmitted login),
  // so the login stamp postdates the session's first observed record.
  const int32_t W = (int32_t)now;
  feedF(dec, "h:1", W, openRec(100, 7, "/store/f.root"));
  clock = now + 50;
  feedUserN(dec, "h:1", 7);                       // connT = now + 50
  clock = now + 60;
  feedF(dec, "h:1", W + 60, closeRec(100));
  clock = now + 100;
  feedF(dec, "h:1", W + 100, discRec(7));

  json j = sessionDoc(docs);
  ASSERT_FALSE(j.is_null());
  const json& a = j["attributes"];
  EXPECT_EQ(a["xrootd.session.start_time_source"], "first_activity");
  EXPECT_EQ(a["xrootd.session.start_time"], isoOf(W));        // the open...
  EXPECT_NE(a["xrootd.session.start_time"], isoOf(W + 60));   // ...not the close
  EXPECT_EQ(a["xrootd.session.duration"].get<double>(), 100.0);
}

// The login stamp comes off this collector's clock while every other session
// time comes off the reporting server's. Translating between them is what makes
// the login usable; without it a skewed server loses the start entirely.
TEST(XrdMonCollect, SessionClockSkewTranslatesLoginTime)
{
  for (int32_t skew : {(int32_t)100000, (int32_t)-100000})
     {std::vector<std::string> docs;
      XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
      dec.SetEmitSessions(true);

      const time_t now = time(nullptr);
      time_t clock = now;
      dec.SetClock([&]{ return clock; });

      feedUserN(dec, "h:1", 7);                     // connT = now
      clock = now + 10;
      feedF(dec, "h:1", (int32_t)now + 10 + skew, openRec(100, 7, "/f.root"));
      clock = now + 60;
      feedF(dec, "h:1", (int32_t)now + 60 + skew, discRec(7));

      json j = sessionDoc(docs);
      ASSERT_FALSE(j.is_null()) << "skew " << skew;
      const json& a = j["attributes"];
      EXPECT_EQ(a["xrootd.session.start_time_source"], "connect") << "skew " << skew;
      EXPECT_EQ(a["xrootd.session.start_time"], isoOf(now + skew)) << "skew " << skew;
      EXPECT_EQ(a["xrootd.session.end_time"],   isoOf(now + 60 + skew));
      EXPECT_EQ(a["xrootd.session.duration"].get<double>(), 60.0) << "skew " << skew;
     }
}

// The window end is seen after a delay that is never negative, so it is a lower
// bound on the offset and the running maximum converges on it. A mean would sit
// a whole batching interval low and a minimum would track the worst delay.
TEST(XrdMonCollect, ClockOffsetTracksMaximumNotMean)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);

  const time_t now = time(nullptr);
  dec.SetClock([&]{ return now; });      // collector clock stands still

  feedUserN(dec, "h:1", 7);              // connT = now
  // Windows observed 10s, 5s and 10s ahead: offset is 10, not 5 and not ~8.3.
  feedF(dec, "h:1", (int32_t)now + 10, {});
  feedF(dec, "h:1", (int32_t)now +  5, {});
  feedF(dec, "h:1", (int32_t)now + 10, discRec(7));

  json j = sessionDoc(docs);
  ASSERT_FALSE(j.is_null());
  const json& a = j["attributes"];
  EXPECT_EQ(a["xrootd.session.start_time_source"], "connect");
  EXPECT_EQ(a["xrootd.session.start_time"], isoOf(now + 10));
  EXPECT_EQ(a["xrootd.session.duration"].get<double>(), 0.0);
}

// An 'f' packet whose TOD was lost leaves every record in it with no time. The
// session still has to be dated, or the document goes out with no @timestamp
// and the span with no start.
TEST(XrdMonCollect, SessionWithoutTodStillTimestamped)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);
  dec.SetEmitSpans(true);

  const time_t now = time(nullptr);
  dec.SetClock([&]{ return now; });

  feedUserN(dec, "h:1", 7);
  { auto payload = discRec(7);           // no TOD record in the packet
    auto pkt = packet('f', kStod, payload);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  json j = sessionDoc(docs);
  ASSERT_FALSE(j.is_null());
  EXPECT_TRUE(j.contains("@timestamp"));
  EXPECT_TRUE(j.contains("timeUnixNano"));
  const json& a = j["attributes"];
  EXPECT_EQ(a["xrootd.session.start_time"], isoOf(now));
  EXPECT_EQ(a["xrootd.session.end_time"],   isoOf(now));
  ASSERT_TRUE(sessionSpan(docs).contains("startTimeUnixNano"));
}

namespace {
// A 't' packet: a WINDOW mark closing at `win`, then a DISC for `user` whose
// connect duration is `csec` seconds.
void feedTraceDisc(XrdMonDecode& dec, const std::string& src, int32_t win,
                   uint32_t user, uint32_t csec)
{
   W payload;
   { std::vector<unsigned char> a0(8, 0); a0[0] = 0xe0;   // WINDOW
     payload.raw(trace(a0, (uint32_t)win, (uint32_t)win)); }
   { std::vector<unsigned char> a0(8, 0); a0[0] = 0xd0;   // DISC
     payload.raw(trace(a0, csec, user)); }
   auto pkt = packet('t', kStod, payload.b);
   dec.Process(src, (const char*)pkt.data(), pkt.size());
}
}

// The trace stream's disconnect carries the connect duration the server
// measured itself, which dates the login exactly and in the server's own clock
// -- better than any estimate. It is taken even with trace emission off, since
// a site can run session documents without the far higher volume trace stream.
TEST(XrdMonCollect, TraceDiscSuppliesExactLoginTime)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);           // note: --traces is NOT enabled

  const int32_t T = kOpenT + 500;
  feedUserN(dec, "h:1", 7);
  feedTraceDisc(dec, "h:1", T, 7, 45);   // logged in 45s before T
  feedF(dec, "h:1", T, discRec(7));

  json j = sessionDoc(docs);
  ASSERT_FALSE(j.is_null());
  const json& a = j["attributes"];
  EXPECT_EQ(a["xrootd.session.start_time_source"], "login");
  EXPECT_EQ(a["xrootd.session.start_time"], isoOf(T - 45));
  EXPECT_EQ(a["xrootd.session.end_time"],   isoOf(T));
  EXPECT_EQ(a["xrootd.session.duration"].get<double>(), 45.0);
  EXPECT_EQ(docs.size(), 1u);          // trace emission off: no trace documents
}

// The two disconnect records travel in independent datagrams with no ordering
// guarantee. When the f-stream one wins the race the session document is
// already out and is not revised, so the fallback has to have produced a usable
// start -- and the late trace record must not emit a second session document.
TEST(XrdMonCollect, TraceDiscAfterSessionStillLeavesAStart)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);

  const int32_t T = kOpenT + 500;
  feedUserN(dec, "h:1", 7);
  feedF(dec, "h:1", T, discRec(7));       // disconnect reported first
  feedTraceDisc(dec, "h:1", T, 7, 45);    // the exact login arrives too late

  json j = sessionDoc(docs);
  ASSERT_FALSE(j.is_null());
  const json& a = j["attributes"];
  ASSERT_TRUE(a.contains("xrootd.session.start_time"));
  EXPECT_NE(a["xrootd.session.start_time_source"], "login");
  EXPECT_GE(a["xrootd.session.duration"].get<double>(), 0.0);
  EXPECT_EQ(docs.size(), 1u);             // exactly one session document
}

// The disconnect document and the session document describe the same session,
// so they report its bounds with the same keys and the same types.
TEST(XrdMonCollect, TraceDiscDocCarriesSessionTimes)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); },
                   nullptr, false, /*traces=*/true);

  const int32_t T = kOpenT + 500;
  feedTraceDisc(dec, "h:1", T, 7, 45);

  ASSERT_EQ(docs.size(), 1u);
  json j = json::parse(docs[0]);
  const json& a = j["attributes"];
  EXPECT_EQ(a["event.name"], "xrootd.io.disconnect");
  EXPECT_EQ(a["xrootd.session.start_time"], isoOf(T - 45));
  EXPECT_EQ(a["xrootd.session.end_time"],   isoOf(T));
  EXPECT_EQ(a["xrootd.session.duration"].get<double>(), 45.0);
  // Same JSON type as the session document's, so a strict consumer mapping
  // sees one type for the key rather than two.
  EXPECT_TRUE(a["xrootd.session.duration"].is_number_float());
}

// Whatever the inputs, a session document always carries the three time fields,
// the start never follows the end, and the duration is exactly their difference.
TEST(XrdMonCollect, SessionStartNeverAfterEnd)
{
  struct Case {const char* name; bool user; bool activity; int32_t skew;};
  const Case cases[] = {
     {"no user map",        false, false,       0},
     {"login only",         true,  false,       0},
     {"login and activity", true,  true,        0},
     {"server far behind",  true,  true,  -200000},
     {"server far ahead",   true,  true,   200000},
  };

  for (const Case& c : cases)
     {std::vector<std::string> docs;
      XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
      dec.SetEmitSessions(true);

      const time_t now = time(nullptr);
      time_t clock = now;
      dec.SetClock([&]{ return clock; });

      if (c.user) feedUserN(dec, "h:1", 7);
      if (c.activity)
         {clock = now + 10;
          feedF(dec, "h:1", (int32_t)now + 10 + c.skew,
                openRec(100, 7, "/f.root"));
         }
      clock = now + 60;
      feedF(dec, "h:1", (int32_t)now + 60 + c.skew, discRec(7));

      json j = sessionDoc(docs);
      ASSERT_FALSE(j.is_null()) << c.name;
      const json& a = j["attributes"];
      ASSERT_TRUE(a.contains("xrootd.session.start_time")) << c.name;
      ASSERT_TRUE(a.contains("xrootd.session.end_time"))   << c.name;
      ASSERT_TRUE(a.contains("xrootd.session.duration"))   << c.name;
      const std::string beg = a["xrootd.session.start_time"];
      const std::string end = a["xrootd.session.end_time"];
      EXPECT_FALSE(beg.empty()) << c.name;
      EXPECT_LE(beg, end) << c.name;   // ISO-8601 UTC sorts lexicographically
      EXPECT_GE(a["xrootd.session.duration"].get<double>(), 0.0) << c.name;
     }
}

// A leaked open with no disconnect (its user map was lost too) is expired by
// the file TTL, but only for servers that report in-flight snapshots (isXfr):
// there a live transfer refreshes its entry every interval, so an untouched
// entry can only be a leak.
TEST(XrdMonCollect, FileTTLExpiresLeakedOpens)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));
  dec.SetFileTTL(600);

  // One packet: an open (file 100, user 7) and an in-flight snapshot for it
  // (marks the server as xfr-reporting). No close ever arrives.
  { W body; body.u32(100); body.u64(123456); body.u32(7);
    std::string lfn = "/store/leak.root"; body.raw(lfn); body.u8(0);
    auto payload = todRec(kOpenT, 42);
    auto r = rec(1 /*isOpen*/, 0x03 /*hasLFN|hasRW*/, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    W xfr; xfr.u32(100); xfr.u64(0); xfr.u64(0); xfr.u64(0);
    auto xr = rec(3 /*isXfr*/, 0, xfr.b);
    payload.insert(payload.end(), xr.begin(), xr.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  // Not stale yet: a sweep "now" keeps the entry.
  dec.ReapServers(time(nullptr));
  EXPECT_EQ(dec.GetStats().staleOpens, 0u);

  // An hour later the entry is well past the 600s TTL.
  dec.ReapServers(time(nullptr) + 3600);
  EXPECT_EQ(dec.GetStats().staleOpens, 1u);

  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_files_open{cluster=\"unknown\",server=\"h\"} 0"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_stale_opens_total{cluster=\"unknown\",server=\"h\"} 1"),
            std::string::npos) << out;
}

// Without xfr reporting the TTL must not touch anything: a long-running
// transfer is indistinguishable from a leak.
TEST(XrdMonCollect, FileTTLSkipsServersWithoutXfr)
{
  XrdMonDecode dec([](const std::string&){});
  dec.SetFileTTL(600);

  { W body; body.u32(100); body.u64(123456); body.u32(7);
    std::string lfn = "/store/longrunning.root"; body.raw(lfn); body.u8(0);
    auto payload = todRec(kOpenT, 42);
    auto r = rec(1 /*isOpen*/, 0x03, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  dec.ReapServers(time(nullptr) + 3600);
  EXPECT_EQ(dec.GetStats().staleOpens, 0u);
}

// Reaping the last incarnation of a sender parks its files_open gauge
// at zero, so a restarted (or retired) server does not strand a nonzero
// series in the metrics output forever.
TEST(XrdMonCollect, ReapZeroesFilesOpenGauge)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));
  dec.SetServerTTL(60);

  { W body; body.u32(100); body.u64(123456); body.u32(7);
    std::string lfn = "/store/f.root"; body.raw(lfn); body.u8(0);
    auto payload = todRec(kOpenT, 42);
    auto r = rec(1 /*isOpen*/, 0x03, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  {std::string out; XrdMetrics::PrometheusTextSerializer ser(out);
   collector.serialize(ser);
   EXPECT_NE(out.find("xrootd_collector_files_open{cluster=\"unknown\",server=\"h\"} 1"),
             std::string::npos) << out;
  }

  dec.ReapServers(time(nullptr) + 3600);
  EXPECT_EQ(dec.GetStats().reaped, 1u);

  {std::string out; XrdMetrics::PrometheusTextSerializer ser(out);
   collector.serialize(ser);
   EXPECT_NE(out.find("xrootd_collector_files_open{cluster=\"unknown\",server=\"h\"} 0"),
             std::string::npos) << out;
  }
}

// A close with no matching open is visible next to the gauge as a counter,
// so correlation loss (dropped open packets) can be monitored.
TEST(XrdMonCollect, OrphanCloseCountsMetric)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));

  { W body; body.u32(100); body.u64(1000); body.u64(0); body.u64(0);
    auto payload = todRec(kCloseT, 42);
    auto r = rec(0 /*isClose*/, 0, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_orphan_closes_total{cluster=\"unknown\",server=\"h\"} 1"),
            std::string::npos) << out;
}

namespace
{
// A 'u' user map for an arbitrary dictid.
void feedUserN(XrdMonDecode& dec, const std::string& src, uint32_t dictid)
{
   W body; body.u32(dictid);
   std::string info = "xroot/u" + std::to_string(dictid) + ".1:2@wn.example.org\n";
   std::vector<unsigned char> pl = body.b;
   pl.insert(pl.end(), info.begin(), info.end());
   auto pkt = packet('u', kStod, pl);
   dec.Process(src, (const char*)pkt.data(), pkt.size());
}

// Open then close one file (fileID/user/lfn) moving `rd` read bytes / `wr` write
// bytes, with file size `fsz` captured at open.
void openClose(XrdMonDecode& dec, const std::string& src, uint32_t fileID,
               uint32_t user, int64_t fsz, int64_t rd, int64_t wr,
               const std::string& lfn, int64_t rv = 0)
{
   { W body; body.u32(fileID); body.u64((uint64_t)fsz); body.u32(user);
     body.raw(lfn); body.u8(0);
     auto payload = todRec(kOpenT, 42);
     auto r = rec(1 /*isOpen*/, 0x01 | 0x02, body.b);
     payload.insert(payload.end(), r.begin(), r.end());
     auto pkt = packet('f', kStod, payload);
     dec.Process(src, (const char*)pkt.data(), pkt.size()); }
   { W body; body.u32(fileID); body.u64((uint64_t)rd); body.u64((uint64_t)rv);
     body.u64((uint64_t)wr);
     auto payload = todRec(kCloseT, 42);
     auto r = rec(0 /*isClose*/, 0, body.b);
     payload.insert(payload.end(), r.begin(), r.end());
     auto pkt = packet('f', kStod, payload);
     dec.Process(src, (const char*)pkt.data(), pkt.size()); }
}

// A session disconnect (isDisc) for a user dictid.
void feedDisc(XrdMonDecode& dec, const std::string& src, uint32_t user)
{
   W disc; disc.u32(user);
   auto payload = todRec(kCloseT, 42);
   auto dr = rec(4 /*isDisc*/, 0, disc.b);
   payload.insert(payload.end(), dr.begin(), dr.end());
   auto pkt = packet('f', kStod, payload);
   dec.Process(src, (const char*)pkt.data(), pkt.size());
}
}

// A session's closed files are aggregated into the 'session' document at
// disconnect: running totals plus a recent-file list. The per-file documents
// are still emitted independently.
TEST(XrdMonCollect, SessionAggregatesFileActivity)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);

  feedUserN(dec, "h:1", 7);
  openClose(dec, "h:1", 1, 7, 1000,  1000, 0, "/a.root");
  openClose(dec, "h:1", 2, 7, 1000,   600, 0, "/b.root", 400);  // 600 read, 400 readv
  openClose(dec, "h:1", 3, 7, 100000,   0, 4096, "/c.root");    // a write
  feedDisc(dec, "h:1", 7);

  // Three close documents, then the session document.
  ASSERT_EQ(docs.size(), 4u);
  json j = json::parse(docs.back());
  const json& a = j["attributes"];
  EXPECT_EQ(a["event.name"], "xrootd.session");
  EXPECT_EQ(a["user.name"], "u7");
  EXPECT_EQ(a["xrootd.session.files"],  3);
  EXPECT_EQ(a["xrootd.session.reads"],  2);
  EXPECT_EQ(a["xrootd.session.writes"], 1);
  // read() and readv() bytes are reported apart, so a consumer can tell
  // sequential access from vectored.
  EXPECT_EQ(a["xrootd.session.read_bytes"],  1000 + 600);
  EXPECT_EQ(a["xrootd.session.readv_bytes"], 400);
  EXPECT_EQ(a["xrootd.session.write_bytes"], 4096);
  ASSERT_TRUE(a.contains("xrootd.session.recent_files"));
  ASSERT_EQ(a["xrootd.session.recent_files"].size(), 3u);
  EXPECT_EQ(a["xrootd.session.recent_files"][2]["file.path"], "/c.root");
  EXPECT_EQ(a["xrootd.session.recent_files"][2]["xrootd.operation.name"], "write");
  EXPECT_EQ(a["xrootd.session.recent_files"][0]["xrootd.operation.name"], "read");
  // The classification the rollup used to carry is gone with the counters.
  EXPECT_FALSE(a["xrootd.session.recent_files"][0].contains("xrootd.transfer.kind"));

  // The individual close documents were still emitted (not replaced).
  EXPECT_EQ(json::parse(docs[0])["eventName"], "xrootd.read");
  EXPECT_EQ(json::parse(docs[2])["eventName"], "xrootd.write");
}

// Dropping the per-file documents must not disturb the session rollup: the
// session document still reports every file that was closed.
TEST(XrdMonCollect, FilterDoesNotAffectSessionRollup)
{
  XrdMonFilter flt;
  std::string err;
  std::size_t r = flt.AddRule("no-transfers");
  ASSERT_TRUE(flt.AddCondition(r, "event", "xrootd.read", err)) << err;
  ASSERT_TRUE(flt.SetAction(r, "drop", err)) << err;

  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);
  dec.SetFilter(&flt);

  feedUserN(dec, "h:1", 7);
  openClose(dec, "h:1", 1, 7, 1000, 1000, 0, "/a.root");
  openClose(dec, "h:1", 2, 7, 1000, 1000, 0, "/b.root");
  feedDisc(dec, "h:1", 7);

  // Both transfer documents were dropped; the session document survives and
  // still accounts for both files.
  ASSERT_EQ(docs.size(), 1u);
  EXPECT_EQ(dec.GetStats().filtered, 2u);
  json j = json::parse(docs.back());
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.session");
  EXPECT_EQ(j["attributes"]["xrootd.session.files"], 2);
  EXPECT_EQ(j["attributes"]["xrootd.session.read_bytes"], 2000);
}


// A 'u' map re-sent for a dictid whose session is already under way is a
// retransmit of the same login (the server mints dictids from a monotonic
// counter), so it must refresh the identity without discarding the rollup or
// the set of files still awaiting their close.
TEST(XrdMonCollect, RepeatedUserMapKeepsSessionState)
{
  XrdMetrics::Collector collector("xrootd");
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); }, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));
  dec.SetEmitSessions(true);

  feedUserN(dec, "h:1", 7);
  openClose(dec, "h:1", 1, 7, 1000, 1000, 0, "/a.root");   // folds one file

  // A second file is opened and left open: it lives in the user's openFiles.
  { W body; body.u32(2); body.u64(1000); body.u32(7);
    std::string lfn = "/b.root"; body.raw(lfn); body.u8(0);
    auto payload = todRec(kOpenT, 42);
    auto r = rec(1 /*isOpen*/, 0x01 | 0x02, body.b);
    payload.insert(payload.end(), r.begin(), r.end());
    auto pkt = packet('f', kStod, payload);
    dec.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  feedUserN(dec, "h:1", 7);                                // the retransmit
  feedDisc(dec, "h:1", 7);

  json j = json::parse(docs.back());
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.session");
  EXPECT_EQ(j["attributes"]["user.name"], "u7");           // identity refreshed
  EXPECT_EQ(j["attributes"]["xrootd.session.files"], 1);   // rollup survived
  ASSERT_TRUE(j["attributes"].contains("xrootd.session.recent_files"));
  ASSERT_EQ(j["attributes"]["xrootd.session.recent_files"].size(), 1u);
  EXPECT_EQ(j["attributes"]["xrootd.session.recent_files"][0]["file.path"], "/a.root");

  // openFiles survived too, so the disconnect still sweeps the leaked open
  // instead of stranding it in the file table.
  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_stale_opens_total{cluster=\"unknown\",server=\"h\"} 1"),
            std::string::npos) << out;
  EXPECT_EQ(dec.GetStats().staleOpens, 1u);
}

// The recent-file list is capped while the running totals cover every file.
TEST(XrdMonCollect, SessionRecentFilesCapped)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);

  feedUserN(dec, "h:1", 7);
  for (uint32_t i = 1; i <= 100; i++)
     openClose(dec, "h:1", i, 7, 1000, 1000, 0, "/data/file.root");
  feedDisc(dec, "h:1", 7);

  json j = json::parse(docs.back());
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.session");
  EXPECT_EQ(j["attributes"]["xrootd.session.files"], 100);        // every file counted
  EXPECT_EQ(j["attributes"]["xrootd.session.reads"], 100);
  EXPECT_EQ(j["attributes"]["xrootd.session.recent_files"].size(), 64u);// list bounded (cap)
}

// Closes are folded into the owning user's session only; two concurrent users
// do not co-mingle their file activity.
TEST(XrdMonCollect, SessionsDoNotCrossUsers)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  dec.SetEmitSessions(true);

  feedUserN(dec, "h:1", 7);
  feedUserN(dec, "h:1", 8);
  openClose(dec, "h:1", 1, 7, 1000, 1000, 0, "/seven.root");
  openClose(dec, "h:1", 2, 8, 1000, 1000, 0, "/eight.root");
  openClose(dec, "h:1", 3, 8, 1000, 1000, 0, "/eight2.root");
  feedDisc(dec, "h:1", 7);

  json j = json::parse(docs.back());
  EXPECT_EQ(j["attributes"]["user.name"], "u7");
  EXPECT_EQ(j["attributes"]["xrootd.session.files"], 1);     // only user 7's one file
  ASSERT_EQ(j["attributes"]["xrootd.session.recent_files"].size(), 1u);
  EXPECT_EQ(j["attributes"]["xrootd.session.recent_files"][0]["file.path"], "/seven.root");
}

// Session correlation is opt-in: with it off (the default) a disconnect emits
// no session document and no per-session rollup is accumulated, so the closes
// release their memory just like the non-session path.
TEST(XrdMonCollect, SessionsDisabledByDefault)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });
  // No SetEmitSessions(true): sessions are disabled.

  feedUserN(dec, "h:1", 7);
  std::size_t base = dec.ResidentBytes();
  openClose(dec, "h:1", 1, 7, 1000, 1000, 0, "/a.root");
  openClose(dec, "h:1", 2, 7, 1000, 1000, 0, "/b.root");
  feedDisc(dec, "h:1", 7);

  // Two close documents were emitted; the disconnect produced nothing.
  EXPECT_EQ(docs.size(), 2u);
  for (const auto& d : docs)
     EXPECT_NE(json::parse(d)["attributes"]["event.name"], "xrootd.session");
  EXPECT_EQ(dec.GetStats().discs, 1u);             // disconnect still counted
  EXPECT_EQ(dec.ResidentBytes(), base);            // no rollup retained
}

TEST(XrdMonCollect, ServerIdentDecoded)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });

  std::string info = "=/xrootd.4321:99@srv.example.org"
                     "\n&site=T1_DE_KIT&port=1094&inst=manager&pgm=xrootd&ver=v6.1.0";
  W body; body.u32(0);   // dictid is 0 for '='
  std::vector<unsigned char> pl = body.b;
  pl.insert(pl.end(), info.begin(), info.end());
  auto pkt = packet('=', kStod, pl);
  dec.Process("srv:9930", (const char*)pkt.data(), pkt.size());
  // A second identical ident must not produce a duplicate document.
  dec.Process("srv:9930", (const char*)pkt.data(), pkt.size());

  ASSERT_EQ(docs.size(), 1u);
  json j = json::parse(docs[0]);
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.server_ident");
  // all.sitename is the storage cluster, and the namespace the other two
  // service attributes are unique within.
  EXPECT_EQ(j["resource"]["service.namespace"], "T1_DE_KIT");
  EXPECT_EQ(j["resource"]["service.name"], "manager");
  // Unique per running daemon, unlike &inst= which is one value for a whole
  // fleet of storage nodes.
  EXPECT_EQ(j["resource"]["service.instance.id"], "srv.example.org:1094");
  EXPECT_EQ(j["resource"]["server.address"], "srv.example.org");
  EXPECT_FALSE(j["resource"].contains("host.name"));
  EXPECT_FALSE(j["resource"].contains("xrootd.server.instance"));
  EXPECT_FALSE(j["resource"].contains("xrootd.server.site"));
  EXPECT_FALSE(j["resource"].contains("xrootd.server.program"));
  EXPECT_EQ(j["resource"]["process.executable.name"], "xrootd");
  EXPECT_EQ(j["resource"]["service.version"], "v6.1.0");
  EXPECT_EQ(j["resource"]["server.port"], 1094);
  EXPECT_EQ(dec.GetStats().mapIdnt, 2u);
}

// An unnamed daemon -- no -n, so the ident carries xrootd's "anon" filler --
// must not report "anon" as its service name: every unnamed daemon in the
// world would share one. semconv's fallback is the program name, which also
// keeps such a server reporting service.name=xrootd exactly as before.
TEST(XrdMonCollect, AnonInstanceFallsBackToProgram)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); });

  std::string info = "=/xrootd.4321:99@srv.example.org"
                     "\n&site=T1_DE_KIT&port=1094&inst=anon&pgm=xrootd&ver=v6";
  W body; body.u32(0);
  std::vector<unsigned char> pl = body.b;
  pl.insert(pl.end(), info.begin(), info.end());
  auto pkt = packet('=', kStod, pl);
  dec.Process("srv:9930", (const char*)pkt.data(), pkt.size());

  ASSERT_EQ(docs.size(), 1u);
  json j = json::parse(docs[0]);
  EXPECT_EQ(j["resource"]["service.name"], "xrootd");
  EXPECT_EQ(j["resource"]["service.instance.id"], "srv.example.org:1094");
}

TEST(XrdMonCollect, GStreamForwarded)
{
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); },
                   nullptr, false, false, /*gstream=*/true);

  W payload;
  payload.u32(1700000000);                 // tBeg
  payload.u32(1700000060);                 // tEnd
  payload.u64(((uint64_t)'O' << 56) | 7);  // sID, provider 'O' = oss
  payload.raw(std::string("{\"event\":\"oss_stats\",\"reads\":5}"));
  auto pkt = packet('g', kStod, payload.b);
  dec.Process("h:1", (const char*)pkt.data(), pkt.size());

  ASSERT_EQ(docs.size(), 1u);
  json j = json::parse(docs[0]);
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.gstream");
  EXPECT_EQ(j["attributes"]["xrootd.gstream.provider"], "oss");
  EXPECT_EQ(j["attributes"]["xrootd.gstream.data"]["reads"], 5);
  EXPECT_EQ(dec.GetStats().gevents, 1u);
}

namespace
{
// A 'g' (g-stream) packet for one provider carrying a single JSON record.
std::vector<unsigned char> gPacket(char prov, const std::string& jsonRec)
{
   W payload;
   payload.u32(1700000000);                  // tBeg
   payload.u32(1700000060);                  // tEnd
   payload.u64(((uint64_t)(unsigned char)prov << 56) | 7);  // sID + provider
   payload.raw(jsonRec);
   return packet('g', kStod, payload.b);
}
}

TEST(XrdMonCollect, GStreamOssMetricsDelta)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));

  // First snapshot establishes the baseline (no counter movement).
  auto p1 = gPacket('O', "{\"event\":\"oss_stats\",\"reads\":100,\"writes\":10,"
                         "\"slow_reads\":4}");
  dec.Process("h:1", (const char*)p1.data(), p1.size());
  // Second snapshot: +50 reads, +5 writes, +1 slow_read.
  auto p2 = gPacket('O', "{\"event\":\"oss_stats\",\"reads\":150,\"writes\":15,"
                         "\"slow_reads\":5}");
  dec.Process("h:1", (const char*)p2.data(), p2.size());

  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_oss_ops_total{cluster=\"unknown\",server=\"h\",op=\"read\"} 50"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_oss_ops_total{cluster=\"unknown\",server=\"h\",op=\"write\"} 5"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_oss_slow_ops_total{cluster=\"unknown\",server=\"h\",op=\"read\"} 1"),
            std::string::npos) << out;
}

namespace
{
// A g-stream datagram in `send json` form (xrootd.mongstream ... send json): a
// header object (code 'g', gs.type = provider) followed by one payload record,
// newline-delimited, instead of the binary XrdXrootdMonGS protocol.
std::string gJson(char prov, const std::string& jsonRec, int pseq)
{
   return std::string("{\"code\":\"g\",\"pseq\":") + std::to_string(pseq)
        + ",\"stod\":" + std::to_string(kStod)
        + ",\"sid\":123,\"gs\":{\"type\":\"" + prov
        + "\",\"tbeg\":1700000000,\"tend\":1700000060}}\n" + jsonRec + "\n";
}
}

TEST(XrdMonCollect, JsonGStreamForwarded)
{
  // A `send json` g-stream leads with '{' rather than a binary code byte; it
  // must be decoded into the same document as the binary path, not rejected as
  // bad_plen.
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); },
                   nullptr, false, false, /*gstream=*/true);

  auto p = gJson('H', "{\"HTTP_GET_200\":{\"count\":240,\"success\":240}}", 3);
  EXPECT_TRUE(dec.Process("h:1", p.data(), (int)p.size()));

  ASSERT_EQ(docs.size(), 1u);
  json j = json::parse(docs[0]);
  EXPECT_EQ(j["attributes"]["event.name"], "xrootd.gstream");
  EXPECT_EQ(j["attributes"]["xrootd.gstream.provider"], "http");
  EXPECT_EQ(j["attributes"]["xrootd.gstream.data"]["HTTP_GET_200"]["count"], 240);
  EXPECT_EQ(dec.GetStats().gevents, 1u);
  EXPECT_EQ(dec.GetStats().malformed, 0u);   // not flagged bad_plen
  EXPECT_EQ(dec.GetStats().unknown, 0u);
}

TEST(XrdMonCollect, JsonGStreamNohdrProviderUnknown)
{
  // `send json nohdr` omits the header, so the datagram leads straight with a
  // payload object. It is still forwarded (provider unknown), not rejected.
  std::vector<std::string> docs;
  XrdMonDecode dec([&](const std::string& d){ docs.push_back(d); },
                   nullptr, false, false, /*gstream=*/true);

  std::string p = "{\"some_event\":{\"n\":1}}\n";
  EXPECT_TRUE(dec.Process("h:1", p.data(), (int)p.size()));
  ASSERT_EQ(docs.size(), 1u);
  json j = json::parse(docs[0]);
  EXPECT_EQ(j["attributes"]["xrootd.gstream.provider"], "unknown");
  EXPECT_EQ(dec.GetStats().malformed, 0u);
}

TEST(XrdMonCollect, JsonGStreamOssMetricsDelta)
{
  // The metrics-aggregation path works identically whether the g-stream arrives
  // in binary or `send json` form.
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));

  auto p1 = gJson('O', "{\"event\":\"oss_stats\",\"reads\":100,\"writes\":10,"
                       "\"slow_reads\":4}", 1);
  dec.Process("h:1", p1.data(), (int)p1.size());
  auto p2 = gJson('O', "{\"event\":\"oss_stats\",\"reads\":150,\"writes\":15,"
                       "\"slow_reads\":5}", 2);
  dec.Process("h:1", p2.data(), (int)p2.size());

  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_oss_ops_total{cluster=\"unknown\",server=\"h\",op=\"read\"} 50"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_oss_ops_total{cluster=\"unknown\",server=\"h\",op=\"write\"} 5"),
            std::string::npos) << out;
}

TEST(XrdMonCollect, GStreamPfcAndTpcMetrics)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));

  auto pfc = gPacket('C', "{\"event\":\"file_close\",\"b_hit\":2048,"
                          "\"b_miss\":1024,\"b_prefetch\":512}");
  dec.Process("h:1", (const char*)pfc.data(), pfc.size());

  auto tpc = gPacket('P', "{\"TPC\":\"xroot\",\"Xeq\":{\"RC\":0,\"Type\":\"pull\"},"
                          "\"Size\":1048576}");
  dec.Process("h:1", (const char*)tpc.data(), tpc.size());

  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_pfc_files_total{cluster=\"unknown\",server=\"h\"} 1"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_pfc_bytes_total{cluster=\"unknown\",server=\"h\",source=\"hit\"} 2048"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_tpc_total{cluster=\"unknown\",server=\"h\",type=\"pull\",result=\"ok\"} 1"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_tpc_bytes_total{cluster=\"unknown\",server=\"h\",type=\"pull\"} 1048576"),
            std::string::npos) << out;
}

TEST(XrdMonCollect, GStreamThrottleAndHttpMetrics)
{
  XrdMetrics::Collector collector("xrootd");
  XrdMonDecode dec([](const std::string&){}, nullptr,
                   false, false, false, false, &collector.subsystem("collector"));

  // throttle: baseline then +30 io_total, io_active gauge = 4.
  auto t1 = gPacket('R', "{\"event\":\"throttle_update\",\"io_wait\":1.5,"
                         "\"io_active\":2,\"io_total\":100}");
  dec.Process("h:1", (const char*)t1.data(), t1.size());
  auto t2 = gPacket('R', "{\"event\":\"throttle_update\",\"io_wait\":2.0,"
                         "\"io_active\":4,\"io_total\":130}");
  dec.Process("h:1", (const char*)t2.data(), t2.size());

  // http: baseline then +5 GET/200.
  auto h1 = gPacket('H', "{\"HTTP_GET_200\":{\"count\":10,\"success\":10}}");
  dec.Process("h:1", (const char*)h1.data(), h1.size());
  auto h2 = gPacket('H', "{\"HTTP_GET_200\":{\"count\":15,\"success\":15}}");
  dec.Process("h:1", (const char*)h2.data(), h2.size());

  std::string out; XrdMetrics::PrometheusTextSerializer ser(out); collector.serialize(ser);
  EXPECT_NE(out.find("xrootd_collector_throttle_io_total{cluster=\"unknown\",server=\"h\"} 30"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_throttle_io_active{cluster=\"unknown\",server=\"h\"} 4"),
            std::string::npos) << out;
  EXPECT_NE(out.find("xrootd_collector_http_requests_total{cluster=\"unknown\",server=\"h\",method=\"GET\",status=\"200\"} 5"),
            std::string::npos) << out;
}

//------------------------------------------------------------------------------
// State persistence: SaveState/LoadState round-trip the correlation state so a
// close arriving after a restart still correlates against pre-restart maps.
//------------------------------------------------------------------------------

class StateFile : public Transfer
{
protected:
  std::string path = tempDir() + "xrdmon-state-" +
                     std::to_string(::getpid()) + ".json";
  void TearDown() override {::unlink(path.c_str());}
};

TEST_F(StateFile, CloseCorrelatesAfterReload)
{
  feedUserMap();
  feedOpen();
  ASSERT_TRUE(dec.SaveState(path));

  std::string doc2;
  XrdMonDecode dec2([&](const std::string& d){doc2 = d;});
  std::string note;
  ASSERT_TRUE(dec2.LoadState(path, 900, note));
  EXPECT_NE(note.find("restored 1 server incarnation(s)"), std::string::npos)
      << note;
  EXPECT_FALSE(std::ifstream(path).good());  // single-use snapshot was removed
  EXPECT_GT(dec2.ResidentBytes(), 0u);       // LRU accounting was rebuilt

  alt = &dec2;
  feedClose();

  ASSERT_FALSE(doc2.empty());
  json j = json::parse(doc2);
  EXPECT_EQ(j["attributes"]["file.path"], "/store/data/file.root");
  EXPECT_EQ(j["attributes"]["user.name"], "alice");
  EXPECT_EQ(j["attributes"]["xrootd.open_seen"], true);
  EXPECT_EQ(j["attributes"]["file.size"], 123456);
  EXPECT_EQ(j["attributes"]["xrootd.operation.duration"], kCloseT - kOpenT);
  EXPECT_EQ(j["resource"]["xrootd.server.id"], 42);

  const XrdMonDecode::Stats& s = dec2.GetStats();
  EXPECT_EQ(s.opens, 1u);       // stats restored from the snapshot...
  EXPECT_EQ(s.closes, 1u);      // ...and advanced by the post-reload close
  EXPECT_EQ(s.orphanCls, 0u);
}

// The clock offset describes this collector and this server incarnation, so it
// is still valid after a restart -- and it has to survive, or a disconnect
// arriving before the first new window would have its login mistranslated.
TEST_F(StateFile, ClockOffsetSurvivesReload)
{
  const time_t now = time(nullptr);
  const int32_t skew = 100000;

  time_t clock = now;
  dec.SetClock([&]{ return clock; });
  dec.SetEmitSessions(true);

  feedUserN(dec, "h:1", 7);                                  // connT = now
  clock = now + 10;
  feedF(dec, "h:1", (int32_t)now + 10 + skew, {});           // offset acquired
  ASSERT_TRUE(dec.SaveState(path));

  std::vector<std::string> docs;
  XrdMonDecode dec2([&](const std::string& d){ docs.push_back(d); });
  dec2.SetEmitSessions(true);
  time_t clock2 = now + 60;
  dec2.SetClock([&]{ return clock2; });
  std::string note;
  ASSERT_TRUE(dec2.LoadState(path, 900, note));

  // The disconnect is the first packet after the restart, so the restored
  // offset is the only thing that can place the login on the server's clock.
  feedF(dec2, "h:1", (int32_t)now + 60 + skew, discRec(7));

  json j = sessionDoc(docs);
  ASSERT_FALSE(j.is_null());
  const json& a = j["attributes"];
  EXPECT_EQ(a["xrootd.session.start_time_source"], "connect");
  EXPECT_EQ(a["xrootd.session.start_time"], isoOf(now + skew));
  EXPECT_EQ(a["xrootd.session.duration"].get<double>(), 60.0);
}

// A restored offset is not re-derived, so a corrupted snapshot can carry one
// large enough to push a session time off the epoch -- where isoTime renders an
// empty string, and an empty string in a date field is exactly what a strict
// consumer rejects. The session must still be datable.
TEST_F(StateFile, AbsurdRestoredClockOffsetStillDatesSessions)
{
  dec.SetEmitSessions(true);
  feedUserN(dec, "h:1", 7);
  ASSERT_TRUE(dec.SaveState(path));

  { json st;                             // corrupt the saved offset
    { std::ifstream in(path); in >> st; }
    for (auto& [key, o] : st.at("servers").items()) o["clkoff"] = -2000000000;
    std::ofstream out(path); out << st.dump(); }

  std::vector<std::string> docs;
  XrdMonDecode dec2([&](const std::string& d){ docs.push_back(d); });
  dec2.SetEmitSessions(true);
  dec2.SetEmitSpans(true);
  std::string note;
  ASSERT_TRUE(dec2.LoadState(path, 900, note));

  { auto payload = discRec(7);           // no TOD, so no time on the wire
    auto pkt = packet('f', kStod, payload);
    dec2.Process("h:1", (const char*)pkt.data(), pkt.size()); }

  json j = sessionDoc(docs);
  ASSERT_FALSE(j.is_null());
  const json& a = j["attributes"];
  EXPECT_NE(a["xrootd.session.start_time"].get<std::string>(), "");
  EXPECT_NE(a["xrootd.session.end_time"].get<std::string>(), "");
  EXPECT_GE(a["xrootd.session.duration"].get<double>(), 0.0);
  ASSERT_TRUE(sessionSpan(docs).contains("startTimeUnixNano"));
}

TEST_F(StateFile, StaleSnapshotStartsFresh)
{
  feedUserMap();
  feedOpen();
  ASSERT_TRUE(dec.SaveState(path));

  // Age the snapshot beyond the reload limit.
  json j = json::parse(std::ifstream(path));
  j["saved"] = (int64_t)time(nullptr) - 3600;
  std::ofstream(path) << j.dump();

  XrdMonDecode dec2([](const std::string&){});
  std::string note;
  EXPECT_FALSE(dec2.LoadState(path, 900, note));
  EXPECT_NE(note.find("starting fresh"), std::string::npos) << note;
  EXPECT_FALSE(std::ifstream(path).good());  // discarded snapshot was removed
  EXPECT_EQ(dec2.GetStats().opens, 0u);
}

TEST_F(StateFile, VersionMismatchStartsFresh)
{
  feedUserMap();
  ASSERT_TRUE(dec.SaveState(path));

  json j = json::parse(std::ifstream(path));
  j["version"] = 999;
  std::ofstream(path) << j.dump();

  XrdMonDecode dec2([](const std::string&){});
  std::string note;
  EXPECT_FALSE(dec2.LoadState(path, 900, note));
  EXPECT_NE(note.find("format version"), std::string::npos) << note;
  EXPECT_EQ(dec2.GetStats().mapUser, 0u);
}

TEST_F(StateFile, CorruptSnapshotStartsFresh)
{
  std::ofstream(path) << "this is not json {";

  XrdMonDecode dec2([](const std::string&){});
  std::string note;
  EXPECT_FALSE(dec2.LoadState(path, 900, note));
  EXPECT_NE(note.find("starting fresh"), std::string::npos) << note;
}

TEST_F(StateFile, MissingSnapshotIsSilent)
{
  XrdMonDecode dec2([](const std::string&){});
  std::string note;
  EXPECT_FALSE(dec2.LoadState(path + ".nonexistent", 900, note));
  EXPECT_TRUE(note.empty());
}
