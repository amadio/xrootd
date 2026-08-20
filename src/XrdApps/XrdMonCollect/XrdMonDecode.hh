#ifndef __XRDMONDECODE_HH__
#define __XRDMONDECODE_HH__
/******************************************************************************/
/*                                                                            */
/*                     X r d M o n D e c o d e . h h                          */
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

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <regex.h>

#include "XrdOuc/XrdOucJson.hh"

namespace XrdMetrics { class Subsystem; }

class XrdMonFilter;

//-----------------------------------------------------------------------------
//! XrdMonDecode decodes XRootD detailed-monitoring UDP packets
//! (xrootd.monitor) and correlates the "f" (file-stats) stream against the
//! user dictionary into one document per completed transfer (file close).
//!
//! It is deliberately transport-agnostic: feed it datagrams via Process() and
//! receive finished transfer documents (and, optionally, raw per-record dumps)
//! through callbacks. All multi-byte fields in the wire format are network
//! byte order; see src/XrdXrootd/XrdXrootdMonData.hh for the layouts.
//-----------------------------------------------------------------------------

class XrdMonDecode
{
public:

//! Called with one finished transfer document, serialized as a single JSON
//! object (no trailing newline). The caller frames it for its sink.
using DocSink = std::function<void(const std::string& jsonDoc)>;

//! Optional: called with one JSON object per decoded record (dump mode).
using RawSink = std::function<void(const std::string& jsonRec)>;

//! Counters describing what has been decoded so far.
struct Stats
{
   uint64_t packets   = 0;   // datagrams seen
   uint64_t malformed = 0;   // datagrams rejected as too short / inconsistent
   uint64_t records   = 0;   // individual records decoded
   uint64_t mapUser   = 0;   // 'u' dictionary records
   uint64_t mapPath   = 0;   // 'd' dictionary records
   uint64_t mapInfo   = 0;   // 'i' dictionary records
   uint64_t mapIdnt   = 0;   // '=' server-identification records
   uint64_t mapTokn   = 0;   // 'T' token dictionary records
   uint64_t mapUeac   = 0;   // 'U' user experiment/activity records
   uint64_t opens     = 0;   // 'f' open records
   uint64_t closes    = 0;   // 'f' close records
   uint64_t xfrs      = 0;   // 'f' in-flight transfer snapshot records
   uint64_t discs     = 0;   // 'f' session disconnect records
   uint64_t docs      = 0;   // transfer documents emitted
   uint64_t filtered  = 0;   // documents of any type suppressed by a filter
                             // rule before reaching the sink
   uint64_t failed    = 0;   // 'f' failed/aborted operation records (isError +
                             // hasERR closes)
   uint64_t orphanCls = 0;   // closes with no matching open
   uint64_t staleOpens = 0;  // open-file entries dropped without a close
                             // (disconnect sweep or file TTL: the close was
                             // lost, e.g. to UDP packet loss)
   uint64_t traces    = 0;   // 't' stream records decoded
   uint64_t gevents   = 0;   // 'g' stream records decoded
   uint64_t redirs    = 0;   // 'r' stream redirect records decoded
   uint64_t spans     = 0;   // OpenTelemetry span documents emitted (--spans)
   uint64_t frmEvents = 0;   // 'x'/'p' FRM stage/migrate/purge records
   uint64_t lost      = 0;   // estimated lost packets (pseq gaps)
   uint64_t evicted   = 0;   // dictionary/open-file entries evicted (budget/cap)
   uint64_t reaped    = 0;   // idle server incarnations reclaimed (server TTL)
   uint64_t unknown   = 0;   // packets with an unhandled code
};

//! Process one UDP datagram received from sender `src` (used, together with
//! the server start time in the header, to key per-server state).
//!
//! @return true if the packet was structurally valid, false if malformed.
bool Process(const std::string& src, const char* buff, int blen);

const Stats& GetStats() const {return stats;}

//! The weight charged against the memory budget by the correlation state
//! currently held: the sum of the bytesOf() estimates driving LRU eviction.
//!
//! This is a *relative* cost used to rank entries, not a measure of memory.
//! It counts held strings plus a flat per-entry constant, and charges nothing
//! for the Server structs, gsPrev, UserInfo::openFiles or the unordered_map
//! bucket arrays, so it runs 4-8x below the real cost. Anyone asking how much
//! memory the process uses wants XrdMonProcessRss(), which is what
//! xrootd_process_resident_memory_bytes reports.
std::size_t StateWeight() const {return lruBytes;}

//! Number of correlation entries held across every incarnation. Exact, unlike
//! StateWeight(), and O(1) -- std::list::size() is constant since C++11.
std::size_t StateEntries() const {return lru.size();}

//! Bound the resident correlation state (per-server dictionaries plus the
//! open-file table) to approximately `n` bytes (0 = unbounded). When exceeded,
//! the least-recently-used entries are evicted first, so a still-active
//! transfer kept warm by its in-flight ('f' xfr) snapshots survives even when
//! it has been open a long time; only cold, stranded entries (e.g. an open
//! whose close was lost) are dropped. A dropped entry merely yields a document
//! missing that field, or an orphan close.
void SetMaxBytes(std::size_t n) {maxBytes = n;}

//! Optional hard cap on the total number of correlation entries (0 = off).
//! A secondary backstop to the byte budget, evicting least-recently-used.
void SetMaxEntries(std::size_t n) {maxEntries = n;}

//! Reclaim a whole server incarnation (its dictionaries and g-stream counter
//! baselines) once it has gone silent for more than `secs` seconds, bounding
//! the accumulation of dead incarnations across restarts/upgrades. 0 disables.
void SetServerTTL(long secs) {serverTTL = secs;}

//! Expire open-file entries untouched for `secs` seconds (0 = off). Only
//! applied to servers that report in-flight transfer snapshots ("xfr"
//! configured on xrootd.monitor fstat), where a live transfer refreshes its
//! entry every interval — so anything older than the TTL is a leaked open
//! whose close was lost, not a long-running transfer.
void SetFileTTL(long secs) {fileTTL = secs;}

//! Drop server incarnations idle past the server TTL (see SetServerTTL). Cheap
//! (scans only the small per-incarnation table); call it periodically, e.g.
//! from the receive loop's idle tick. `now` is the current wall-clock time.
void ReapServers(time_t now);

//! Enable/disable substituting the local FQDN for a loopback (co-located)
//! server's hostname (on by default). When off, server.name falls back to the
//! numeric source IP unless a '=' ident host is received. Remote servers are
//! always identified from their '=' ident, never reverse-resolved here (a
//! blocking lookup would stall the UDP receive loop).
void SetResolveHosts(bool v) {resolveHosts = v;}

//! Name the site this collector serves. It is not on the monitoring wire --
//! all.sitename names the storage cluster, and one site holds several -- so it
//! comes from the collector's own configuration, on the deployment model of one
//! collector per site. When set it is emitted as the `xrootd.site` resource
//! attribute on every document; when empty, documents carry no site at all.
void SetSite(const std::string& s) {site = s;}

//! Enable per-session activity correlation (off by default). When on, each file
//! close is folded into its user's session rollup and a `session` document
//! (identity plus aggregated file activity) is emitted on disconnect. When off,
//! no rollup is accumulated and no session document is produced — saving the
//! per-session memory and the receive-thread work for deployments that only
//! consume the per-transfer/access documents.
void SetEmitSessions(bool v) {emitSessions = v;}

//! Emit companion OpenTelemetry span documents alongside the log records (off
//! by default): a file-operation span per close/failed operation and a session
//! span (the trace root) per disconnect. The logs already carry the same
//! traceId/spanId, so this is purely additive for a tracing backend; like the
//! logs it can be high volume. Building the session span uses the per-session
//! rollup, so it is only meaningful together with SetEmitSessions.
void SetEmitSpans(bool v) {emitSpans = v;}

//! Override the wall clock the decoder samples. Map records carry no time of
//! their own, so their arrival has to be timed here; the same clock anchors the
//! per-incarnation offset estimate (NoteClock) and the `observedTime` of every
//! document. Replaying a captured stream against the real clock therefore dates
//! it as if it had just arrived — supply the capture's clock instead. Tests use
//! this to drive sessions whose login and disconnect are hours apart. Passing an
//! empty function restores time(nullptr).
void SetClock(std::function<time_t()> f) {nowFn = std::move(f);}

//! Load a SciTags registry (scitags.org schema: a top-level "experiments"
//! array of {expId, expName, activities:[{activityId, activityName}]}). When
//! loaded, the numeric SciTags experiment/activity ids carried on the 'U'
//! stream are additionally mapped to their names (and the experiment name is
//! used as a VO fallback). Returns false on a missing/unparseable file (the
//! numeric ids are still emitted).
bool LoadScitags(const std::string& path);

//! Replace the SciTags registry from an in-memory JSON document (the same
//! schema as LoadScitags). The swap is mutex-guarded, so a background thread
//! can refresh the registry while the decode thread reads it. Returns false
//! (leaving the current registry intact) on a parse error or a document with
//! no "experiments" array.
bool LoadScitagsJson(const std::string& text);

//! @param emitTraces   emit a document per 't'-stream record (I/O, open,
//!                     close, disconnect) — high volume, off by default.
//! @param emitGstream  emit a document per 'g'-stream (plugin) record.
//! @param subsystem  optional metrics registry; when set, completed transfers are
//!             aggregated into bounded-cardinality Prometheus series.
         XrdMonDecode(DocSink docSink, RawSink rawSink = nullptr,
                      bool emitRaw = false, bool emitTraces = false,
                      bool emitGstream = false, bool emitRedirects = false,
                      XrdMetrics::Subsystem* subsystem = nullptr)
                     : doc(std::move(docSink)), raw(std::move(rawSink)),
                       dumpRaw(emitRaw), traces(emitTraces),
                       gstream(emitGstream), redirects(emitRedirects),
                       metrics(subsystem) {resolveLocalHost();}
        ~XrdMonDecode() {if (hasDataset) regfree(&datasetRe);}

//! Persist the whole correlation state (server incarnations with their
//! dictionaries and open-file tables, g-stream counter baselines, and the
//! decode statistics) to `path` as a single versioned JSON document, written
//! atomically (.tmp + rename). Meant to be called once, at shutdown, after the
//! decode thread has stopped. Returns false when the file cannot be written
//! (errno is left set).
bool SaveState(const std::string& path) const;

//! Reload the state written by SaveState. The snapshot is discarded (and the
//! file removed) when its format version does not match or it is older than
//! `maxAgeSec` seconds — a long-stopped collector starts fresh rather than
//! resurrecting stale dictionaries. On success the file is also removed (a
//! snapshot is only ever good for one restart) and the LRU index and memory
//! accounting are rebuilt. Call before the first Process(). `note` receives a
//! one-line human-readable outcome (empty when there is no state file).
//! Returns true only when state was actually restored.
bool LoadState(const std::string& path, long maxAgeSec, std::string& note);

//! Set (or clear, with an empty pattern) the dataset-capture expression: an
//! extended POSIX regex whose first capture group, matched against each
//! emitted file path, becomes the xrootd.dataset attribute. Compiled once
//! here; matched once per emitted document (never in the receive path).
//! Returns false (leaving no pattern set) when the expression does not
//! compile.
bool SetDatasetRegex(const std::string& pattern);

//! Attach a document filter, applied to each finished document immediately
//! before it is handed to the sink: a matching rule either tags the document
//! with a label or suppresses it entirely. Everything upstream of the emission
//! — correlation state, the session rollups and the Prometheus aggregation —
//! runs regardless, so filtering changes what is exported, never what is
//! measured. The filter must outlive the decoder and must not be modified once
//! attached (it is read from the decode thread without locking). A null
//! pointer disables filtering.
void SetFilter(const XrdMonFilter* f) {filter = f;}

private:

struct Server;   // per-incarnation state; defined below

// Which per-server map an LRU entry lives in, so the least-recently-used
// victim can be erased from the right table.
//
enum class Dict : uint8_t {Users, Files, Paths, Infos, Tokens, Activity};

// One node of the process-wide LRU index. Every evictable correlation entry
// owns one (and stores an iterator back to it), letting eviction find the
// coldest entry in O(1) and erase it from its map. `srv` is a pointer into the
// `servers` table, which is reference-stable across other insert/erase/rehash;
// a Server is only erased after all of its nodes have been removed.
//
struct LruNode
{
   Server*     srv;     // owning incarnation
   Dict        dict;    // which of its maps holds the entry
   uint32_t    ikey;    // numeric key (dictid/fileID); unused for Infos
   std::string skey;    // string key (Infos only; empty otherwise)
   std::size_t bytes;   // approximate memory charged for the entry
};
using LruIt = std::list<LruNode>::iterator;

// Identity parsed from a 'u' (MAPUSER) dictionary entry. The first line is the
// "<prot>/<user>.<pid>:<sfd>@<host>" descriptor; the rest is a CGI tail carrying
// the login appinfo (always: &a= &R= &x= &y= &I=) and, when "xrootd.monitor ...
// auth" is configured, the authentication info (&p= &o= &r= &g=).
//
struct UserInfo
{
   std::string raw;        // full first line of the map info
   std::string user;
   std::string prot;
   std::string host;       // descriptor @host: may be a reverse-resolved name
   std::string addr;       // &a= numeric client IP (DNS-free; empty pre-6.x)
   std::string authHost;   // &h= auth-reported client host (name candidate)
   std::string dn;         // &n= client distinguished name (auth subject)
   std::string authMethod; // &p= security protocol (gsi/krb5/sss/unix/ztn)
   std::string vo;         // &o= VO / organisation (auth-derived; token preferred)
   std::string role;       // &r= role
   std::string groups;     // &g= groups
   std::string clientVer;  // &R= client release (e.g. v5.6.1)
   std::string appName;    // &x= xrd.appname
   std::string appInfo;    // &y= xrd.info
   std::string site;       // &S= client-advertised site (xrd.site)
   int         ipVersion = 0; // &I= IP protocol (4 or 6); 0 = unknown
   time_t      connT = 0;  // login time: the 'u' map record's arrival at the
                           // collector (sent by the server at login), so the
                           // session span can start at the connect rather than
                           // at the first file close
   LruIt       lru;        // back-reference into the LRU index

   // Session activity rollup. A session is the lifetime of one user dictid
   // within an incarnation; each file close belonging to it is folded in here
   // (see EmitClose), then emitted as the j["session"] object when the client
   // disconnects (EmitDisc). The running totals always cover every closed file;
   // the recent-file list is capped (kSessionFilesMax) so a long-lived session
   // stays bounded.
   //
   struct FileSummary
   {
      std::string lfn;
      int64_t     bytes = 0;     // total bytes moved (read+readv+write)
      bool        write = false; // a write (vs a read)
   };
   uint32_t sFiles      = 0;  // closed files folded into this session
   uint32_t sReads      = 0;  // of which read-only
   uint32_t sWrites     = 0;  // of which carrying write bytes
   uint32_t sErrors     = 0;  // closes that ended in error
   int64_t  sReadBytes  = 0;  // read() bytes across the session
   int64_t  sReadvBytes = 0;  // readv() bytes across the session
   int64_t  sWriteBytes = 0;  // write bytes across the session
   int32_t  sLogin      = 0;  // exact login time in the server's own clock,
                              // from the trace stream's disconnect record
                              // (its time less the connect duration it carries)
   // The session's observed activity, in the server's clock: the earliest and
   // latest time of any record naming it (open, I/O trace, transfer snapshot,
   // close, error or redirect). Every such record also produces a document, and
   // with --spans a span parented by the session's, so these two are what the
   // reported window has to enclose -- see sessionSpanOf. Fractional, because
   // record times are interpolated within a packet window and the documents
   // carry them at that precision; truncating to seconds would leave the bound
   // up to a second short of the record it exists to cover.
   double   sFirst      = 0;
   double   sLast       = 0;
   std::deque<FileSummary> sRecent;  // capped most-recent file summaries

   // The session's disconnect has been reported. The entry outlives it -- a
   // straggling close still needs the identity to resolve against -- so this
   // is what separates a live session from a spent one for Server::sessions.
   bool     disc        = false;

   // fileIDs of this user's open files still awaiting their close. The server
   // closes a session's files before reporting its disconnect, so anything
   // still here at the isDisc is a leaked open (its close record was lost)
   // and is swept out of the open-file table (see DropUserFiles).
   std::unordered_set<uint32_t> openFiles;

   // Carry the accumulated session over a re-sent 'u' map for the same dictid.
   // The server mints dictids from a monotonic counter (XrdXrootdMonitor::
   // GetDictID), so a repeat within one incarnation is a retransmit of the same
   // login, never a new session: replacing the entry wholesale (what lruPut
   // does for the stateless dictionaries) would drop the rollup, the login time
   // and the open-file set mid-session. The identity fields are deliberately
   // not carried: the newer copy of the map is the better parse. The LRU
   // back-reference belongs to the entry, not to the session, and is left to
   // lruPut.
   //
   void adopt(UserInfo&& p)
        {if (p.connT > 0 && (connT <= 0 || p.connT < connT)) connT = p.connT;
         sLogin      = p.sLogin;
         sFiles      = p.sFiles;
         sReads      = p.sReads;
         sWrites     = p.sWrites;
         sErrors     = p.sErrors;
         sReadBytes  = p.sReadBytes;
         sReadvBytes = p.sReadvBytes;
         sWriteBytes = p.sWriteBytes;
         sFirst      = p.sFirst;
         sLast       = p.sLast;
         sRecent     = std::move(p.sRecent);
         openFiles   = std::move(p.openFiles);
        }
};

// Token identity from a 'T' (MAPTOKN) record, keyed by the user dictid.
//
struct TokenInfo
{
   std::string subject;   // s= token subject (sub claim)
   std::string username;  // n= mapped local user
   std::string vo;        // o= organisation / VO
   std::string role;      // r= role
   std::string groups;    // g= groups
   LruIt       lru;       // back-reference into the LRU index
};

// User experiment/activity from a 'U' (MAPUEAC) record (SciTags packet-marking
// flow labels), keyed by the user dictid.
//
struct UserActivity
{
   int   experiment = 0;  // Ec= experiment id
   int   activity   = 0;  // Ac= activity id
   LruIt lru;             // back-reference into the LRU index
};

// A 'd' (MAPPATH) lfn or 'i' (MAPINFO) appinfo string plus its LRU node.
//
struct StringEntry
{
   std::string val;
   LruIt       lru;       // back-reference into the LRU index
};

// Server self-identification from a '=' (MAPIDNT) record.
//
struct ServerIdent
{
   std::string site;
   std::string host;
   std::string inst;
   std::string pgm;
   std::string ver;
   std::string user;      // login user.pid of the reporting daemon
   int         port = 0;  // &port= the reporting server's listen port
};

// An open file awaiting its close, from the 'f' stream open record.
//
struct OpenFile
{
   std::string lfn;
   uint32_t    user = 0;  // user dictid (0 if user monitoring off)
   int64_t     fsz  = 0;  // file size at open
   double      tOpen = 0; // open time, interpolated within the open packet's
                          // reporting window (fractional Unix seconds)
   double      tIoLast = 0; // latest t-stream I/O record attributed to this
                          // file, after clamping (see DecodeTStream). The
                          // file's span has to reach it, since those records
                          // are emitted as its children long before the close
                          // that would have bounded them. 0 = none seen.
   time_t      lastSeen = 0; // wall-clock of the open or last xfr snapshot
                             // naming this file (drives the file TTL sweep)
   bool        rw   = false;
   LruIt       lru;       // back-reference into the LRU index
};

// Per server-incarnation state, keyed by sender + server start time (stod).
//
struct Server
{
   std::unordered_map<uint32_t, UserInfo>     users;
   std::unordered_map<uint32_t, OpenFile>     files;
   std::unordered_map<uint32_t, StringEntry>  paths;  // 'd' dictid -> lfn
   std::unordered_map<std::string, StringEntry> infos; // 'i' descriptor -> appinfo
   std::unordered_map<uint32_t, TokenInfo>    tokens;  // 'T' user dictid -> token
   std::unordered_map<uint32_t, UserActivity> activity;// 'U' user dictid -> scitag
   // Live client sessions: user dictids seen but not yet disconnected. Kept as
   // a counter rather than derived from users.size(), which also holds the
   // entries of sessions already ended (see UserInfo::disc). Maintained at the
   // three places the set can change: a 'u' map, a disconnect, and eviction.
   uint64_t sessions = 0;
   ServerIdent ident;        // '=' server self-identification
   std::string identRaw;     // last emitted identity (to de-duplicate docs)
   std::string resolvedHost; // reverse-resolved sender hostname (cached)
   bool    resolved = false; // resolvedHost computed yet (once per incarnation)
   // The Prometheus {cluster, server} label pair, cached so the metric path is
   // two member reads rather than the name-precedence walk. Both derive from
   // the '=' ident, so both are provisional ("unknown" and the numeric source
   // IP) until it arrives; LabelServer refreshes them when it does. mtrCluster
   // is all.sitename, which this collector reads as the storage cluster.
   std::string mtrCluster;
   std::string mtrServer;
   int64_t sID = 0;
   // Last packet sequence (header pseq) per stream class, for loss detection.
   // The server does NOT stamp one sequence per destination: the f-stream and
   // each g-stream provider run their own independent counters, while the
   // trace/redirect/map streams share the per-destination one ("main").
   // Tracking them together would register the interleaving of independent
   // counters as permanent phantom gaps.
   std::unordered_map<std::string, int> lastPseq;
   time_t  lastSeen = 0;     // wall-clock of the last packet (for idle reaping)
   // Offset from this collector's clock to the reporting server's, in seconds:
   // serverTime ~= collectorTime + clkOff (0 also means "assume synchronised").
   // Map records carry no time of their own, so a login can only be stamped
   // with the collector's clock; this converts that stamp into the clock the
   // f/t-stream window times are expressed in. See NoteClock.
   int32_t clkOff = 0;
   time_t  clkAt  = 0;       // when the current estimate was taken
   bool    sawXfr = false;   // this incarnation reports in-flight snapshots
                             // ("xfr" configured), so the file TTL is safe
};

Server&  ServerFor(const std::string& src, int32_t stod);
//! The canonical name of a reporting server: the '=' ident's advertised host
//! when it is a real name, else the local FQDN for a loopback sender, else the
//! numeric source IP. otelResource() and the Prometheus `server` label share
//! it, so a Grafana panel and an OpenSearch query name a server identically.
std::string ServerName(const Server& srv, const std::string& src) const;
//! Recompute a server's cached {cluster, server} metric labels. Called when an
//! incarnation appears and whenever a '=' ident changes what we know of it.
void     LabelServer(Server& srv, const std::string& src);
//! Publish the per-server live-state gauges (files_open, sessions_open) from
//! the correlation tables. Call after anything that grows or shrinks them.
//! sessions_open counts live client sessions, so it stays at zero when the
//! server's monitor config omits the `user` destination.
void     LiveGauges(const Server& srv);
//! Recount servers per cluster into the servers{cluster} gauge. Call when an
//! incarnation appears, when a '=' ident moves one between clusters, and after
//! reaping. Walks the (small) incarnation table.
void     ServerGauges();
//! Publish (or, with `live` false, retire) one server's identity as
//! server_info{...} = 1. `ip` is the numeric source address.
void     ServerInfo(const Server& srv, const std::string& ip, bool live);
//! The decoder's view of the wall clock (see SetClock).
time_t   Now() const {return nowFn ? nowFn() : time(nullptr);}
//! Fold one f-stream window end into the incarnation's clock-offset estimate.
void     NoteClock(Server& srv, int32_t tEnd);
//! Convert a time sampled from this collector's clock into the reporting
//! server's, using the offset NoteClock maintains.
double   toServerClock(const Server& srv, double t) const
            {return t + (double)srv.clkOff;}
//! Count one structural problem in a packet from `src`: bumps the aggregate
//! stats.malformed and the labeled
//! malformed_total{cluster,server,stream,reason} metric series (stream is
//! derived from the header code byte). Pass the incarnation when it is known; a
//! header too short to carry a stod has none, and falls back to an unknown
//! cluster and the bare source IP.
void     Malformed(const std::string& src, unsigned char code,
                   const char* reason, const Server* srv = nullptr);
void     DecodeMap(unsigned char code, Server& srv,
                   uint32_t dictid, const char* info, int ilen);
void     DecodeIdent(const std::string& src, int32_t stod, Server& srv,
                     const char* info, int ilen);
void     DecodeFrm(const std::string& src, int32_t stod, Server& srv,
                   unsigned char code, const char* info, int ilen);
void     DecodeFStream(const std::string& src, int32_t stod, Server& srv,
                       const unsigned char* p, int len);
void     EmitClose(const std::string& src, int32_t stod, Server& srv,
                   uint32_t fileID, unsigned char recFlag,
                   const unsigned char* rec, int recSize, double tRec);
void     EmitDisc(const std::string& src, int32_t stod, Server& srv,
                  uint32_t userID, double tRec);
//! Drop a disconnecting user's leftover open-file entries (their closes were
//! lost): the server reports a session's closes before its disconnect, so the
//! open-file table must not keep carrying them (they would inflate the
//! active_transfers gauge forever). Bumps stats.staleOpens and
//! stale_opens_total{server}.
void     DropUserFiles(const std::string& src, Server& srv, uint32_t userID);
void     EmitError(const std::string& src, int32_t stod, Server& srv,
                   unsigned char recFlag, const unsigned char* rec,
                   int recSize, double tRec);
//! Fill the terminal-error attributes (error.type carries the server's verbatim
//! reason, plus vendor xrootd.error.code, xrootd.operation.state, and — unless
//! already set — xrootd.operation.name) into the event `attributes` object from
//! a XrdXrootdMonStatERR block of `errLen` bytes. Returns the low-cardinality
//! error category (open/read/write/close/auth) for metric labels.
std::string otelError(nlohmann::json& attrs, const unsigned char* err,
                      int errLen);
void     DecodeTStream(const std::string& src, int32_t stod, Server& srv,
                       const unsigned char* p, int len);
void     DecodeGStream(const std::string& src, int32_t stod, Server& srv,
                       const unsigned char* p, int plen);
//! Decode a g-stream datagram sent as newline-delimited text rather than the
//! binary XrdXrootdMonGS protocol (xrootd.mongstream `send json`/`cgi`). Such a
//! datagram leads with '{' and would otherwise be flagged bad_plen. Feeds the
//! same events/metrics as DecodeGStream via EmitGStreamRecord.
void     DecodeGStreamJson(const std::string& src, const char* buff, int blen);
//! Emit one g-stream plugin record (a single JSON/CGI line): bumps stats.gevents,
//! aggregates known providers into metrics, and forwards the record as a document.
//! Shared by the binary (DecodeGStream) and JSON (DecodeGStreamJson) paths.
void     EmitGStreamRecord(const std::string& src, int32_t stod, Server& srv,
                           unsigned char provByte, int32_t tBeg, int32_t tEnd,
                           const std::string& line);
void     DecodeRStream(const std::string& src, int32_t stod, Server& srv,
                       const unsigned char* p, int plen);

// LRU index management. lruPut inserts (or refreshes) a correlation entry and
// charges its weight against the memory budget, evicting the least-recently-
// used entries when the budget (or count cap) is exceeded. Touch promotes an
// entry to most-recently-used; LruDrop unlinks one whose map entry is being
// erased; EvictFront removes the current LRU victim.
//
template<class Map, class Key>
void lruPut(Server* owner, Dict dict, Map& m, const Key& key, uint32_t ikey,
            const std::string& skey, typename Map::mapped_type&& val,
            std::size_t bytes)
{
   auto it = m.find(key);
   if (it == m.end())
      {it = m.emplace(key, std::move(val)).first;
       it->second.lru = lru.insert(lru.end(),
                                   LruNode{owner, dict, ikey, skey, bytes});
       lruBytes += bytes;
      }
      else
      {LruIt node = it->second.lru;          // preserve across the assignment
       it->second = std::move(val);
       it->second.lru = node;
       lruBytes = lruBytes - node->bytes + bytes;
       node->bytes = bytes;
       Touch(node);
      }
   EnforceBudget();
}
void     Touch(LruIt node) {lru.splice(lru.end(), lru, node);}
void     LruDrop(LruIt node) {lruBytes -= node->bytes; lru.erase(node);}
// Re-charge an existing entry whose held size changed (e.g. its session rollup
// grew), keeping lruBytes and the node weight in step.
void     Recharge(LruIt node, std::size_t bytes)
            {lruBytes = lruBytes - node->bytes + bytes; node->bytes = bytes;}
void     EnforceBudget();
void     EvictFront();
// Approximate resident size of a correlation entry (struct + held strings +
// fixed per-entry container overhead), charged against the memory budget.
static std::size_t bytesOf(const UserInfo& u);
static std::size_t bytesOf(const OpenFile& f);
static std::size_t bytesOf(const TokenInfo& t);
static std::size_t bytesOf(const UserActivity& a);
static std::size_t bytesOf(const StringEntry& s, const std::string& key);

// Document-building helpers shared by every emitter so all document types use
// one OpenTelemetry-aligned schema: a process-level j["resource"] object and an
// event-level j["attributes"] object, both keyed by dotted semantic-convention
// names (service.*, server.*, host.*, file.*, client.*, user.*, error.*) with
// XRootD-specific fields under the xrootd.* vendor namespace.
//
//! Fill the j["resource"] object (service.*/server.*/host.name plus the
//! xrootd.server.* vendor keys) from the server incarnation identity.
void     otelResource(nlohmann::json& j, const std::string& src, int32_t stod,
                      const Server& srv);
//! Set the log-record envelope: instrumentation scope, event.name, severity,
//! and the @timestamp/timeUnixNano/observedTimeUnixNano fields. `tSecs` is the
//! event time in Unix seconds, possibly fractional for interpolated record
//! times (0 to omit the event-time fields); `error` picks ERROR vs INFO
//! severity.
void     otelBegin(nlohmann::json& j, const char* eventName, double tSecs,
                   bool error);
//! Emit a companion OTLP span document derived from an already-built log
//! document `src`, reusing its resource/attributes/traceId/spanId and replacing
//! the log envelope with the span fields (name/kind/start-end/status). `tBeg`/
//! `tEnd` are Unix seconds (fractional allowed); `parentSpanId` links to the
//! enclosing span (empty for a trace root). No-op unless span emission is
//! enabled.
void     emitSpan(const nlohmann::json& src, const char* name, double tBeg,
                  double tEnd, const std::string& parentSpanId);
//! Serialize one finished document and hand it to the sink, applying the
//! filter first: a matching rule may tag `j` in place or suppress it. The
//! single emission funnel for every document the decoder produces.
//! `srv` is the incarnation that produced the document; it feeds the
//! documents_total{cluster} counter kept here, so every document is counted
//! exactly once whatever produced it.
//! @return true when the document reached the sink. Callers guard the
//!         companion emitSpan() on this, so a suppressed log never leaves a
//!         parentless span behind.
bool     emitDoc(nlohmann::json& j, const Server& srv);
//! Fill the identity attributes (user.*, client.*, xrootd.*) into the
//! event `attributes` object from the user dictionary entry (and the token and
//! activity streams keyed by the same dictid). Returns the resolved VO (token
//! preferred, else the auth CGI of a VO-bearing method) for metric labels.
std::string otelIdentity(nlohmann::json& attrs, const Server& srv,
                         uint32_t userID);
//! Fold one finished file close into the user's session rollup (counters and a
//! capped recent-file list), keeping the entry's LRU weight in step. No-op when
//! the user dictid is unknown (e.g. user monitoring off or the 'u' record lost).
//! Times are not its business: NoteActive keeps those.
void     foldSession(Server& srv, uint32_t userID, const std::string& lfn,
                     int64_t rdBytes, int64_t rvBytes, int64_t wrBytes,
                     bool error);
//! Widen a session's observed activity bounds to cover one record naming its
//! user dictid, in the server's clock. `tBeg`/`tEnd` are the record's own
//! extent: a close passes its open and close times, everything else is an
//! instant and leaves `tEnd` at 0. No-op when the dictid is unknown.
void     NoteActive(Server& srv, uint32_t userID, double tBeg, double tEnd = 0);

//! Record a session's exact login time from a trace-stream disconnect: `tRec`
//! is the record's time and `csec` the connect duration it carries, both in the
//! server's clock. No-op when the dictid is unknown.
void     NoteLogin(Server& srv, uint32_t userID, double tRec, int32_t csec);

//! A session's resolved bounds, in the reporting server's clock, plus how the
//! begin was arrived at. `src` is one of "login" (exact, from the trace
//! stream's connect duration), "connect" (the login record's arrival,
//! translated out of this collector's clock), "first_activity" (the earliest
//! record naming the session) or "disconnect" (nothing else was available, so
//! the session is reported as an instant). It is emitted as
//! xrootd.session.start_time_source so a consumer can tell a measured start
//! from an estimated one.
struct SessionSpan
{
   double      beg = 0;
   double      end = 0;
   const char* src = "disconnect";
};

//! Resolve a session's bounds at its disconnect. Always returns a usable pair
//! with `beg <= end`: candidates are admitted only inside [stod, end], the
//! incarnation's own start time being a floor no session can predate, and the
//! disconnect itself is the last resort. The result also encloses the session's
//! observed activity (UserInfo::sFirst/sLast), so no record the session parents
//! falls outside the window reported for it. `u` may be null when the login
//! record was never seen (or has been evicted).
SessionSpan sessionSpanOf(int32_t stod, const Server& srv, const UserInfo* u,
                          double tRec) const;

//! Fill the xrootd.session.* attributes (totals plus the capped recent-file
//! list) into the event `attributes` object from a user's session rollup.
//! `sBeg`/`sEnd` come from sessionSpanOf and are always emitted, so the
//! session start, end and duration are present on every session document.
//! `u` may be null: the rollup then reads as zero rather than being omitted.
void     otelSession(nlohmann::json& attrs, const UserInfo* u, double sBeg,
                     double sEnd);

std::unordered_map<std::string, Server> servers;
// Previous cumulative values for g-stream providers that report running
// totals (oss), so they can be turned into Prometheus counter deltas. Keyed
// by "<server>|<provider>|<metric>".
std::unordered_map<std::string, uint64_t> gsPrev;
DocSink  doc;
RawSink  raw;
std::function<time_t()> nowFn;   // wall-clock source; empty = time(nullptr)
bool     dumpRaw;
bool     traces;
bool     gstream;
bool     redirects;
XrdMetrics::Subsystem* metrics;
const XrdMonFilter*    filter = nullptr;  // document filter (see SetFilter)

// Process-wide LRU index over all evictable correlation entries, bounding the
// resident state by an approximate byte budget (maxBytes; 0 = unbounded) with
// an optional secondary entry-count cap (maxEntries; 0 = off). serverTTL idle-
// reaps whole incarnations. lruBytes tracks the charged total.
//
std::list<LruNode> lru;            // front = least-recently-used, back = MRU
std::size_t maxBytes   = 0;        // byte budget (0 = unbounded)
std::size_t lruBytes   = 0;        // currently charged bytes
std::size_t maxEntries = 0;        // optional entry-count backstop (0 = off)
long        serverTTL  = 0;        // idle-incarnation reap age, secs (0 = off)
long        fileTTL    = 0;        // stale open-file entry age, secs (0 = off)
bool     resolveHosts = true;
bool     emitSessions = false;   // per-session rollup + session documents
bool     emitSpans    = false;   // companion OTLP span documents (--spans)

// Local FQDN substituted for a loopback (co-located) server, resolved at most
// once for the whole process (MyHostName is the same regardless of which
// server reports), then reused for every loopback incarnation. localIP4/6 are
// the first public (non-loopback) address of each family the FQDN resolves
// to, substituted for loopback literals that would otherwise be emitted.
void        resolveLocalHost();   // resolve localHost once, at construction
std::string publicFor(const std::string& ip) const;
std::string localHost;
// Clusters currently published in the servers{} gauge, so one whose last
// server went away can be parked at zero. The registry cannot remove a series,
// and a frozen nonzero value would read as a cluster that is still reporting.
std::unordered_set<std::string> mtrClusters;
std::string localIP4;
std::string localIP6;

// file.path/file.name/file.directory attributes from an LFN, plus the
// xrootd.dataset capture when a --dataset pattern is set.
void    setFile(nlohmann::json& a, const std::string& lfn) const;
std::string site;                 // --site, this collector's site (or empty)
regex_t datasetRe;                // compiled --dataset pattern
bool    hasDataset = false;       // datasetRe holds a compiled pattern

// SciTags registry (loaded from --scitags), mapping numeric flow-label ids to
// human names. sciExp: expId -> expName (doubles as a VO); sciAct: the packed
// key (expId<<32)|activityId -> activityName. Empty when no registry is loaded.
// Guarded by scitagsMtx so a background refresh thread can swap them in while
// the decode thread reads them in otelIdentity().
std::mutex                                 scitagsMtx;
std::unordered_map<int, std::string>       sciExp;
std::unordered_map<long long, std::string> sciAct;

Stats    stats;
};
#endif
