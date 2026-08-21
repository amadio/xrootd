/******************************************************************************/
/*                                                                            */
/*                    X r d M o n C o l l e c t . c c                         */
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

// xrdmoncollect: read XRootD detailed-monitoring UDP packets (xrootd.monitor),
// correlate the "f" (file-stats) stream into one document per completed
// transfer, and write the documents as NDJSON (or OpenSearch _bulk) to stdout
// or a file. See xrootd-new-metrics.md, Phase 5.

#include <arpa/inet.h>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include "INIReader.h"

#include "XrdApps/XrdMonCollect/XrdMonDecode.hh"
#include "XrdApps/XrdMonCollect/XrdMonDiskCache.hh"
#include "XrdApps/XrdMonCollect/XrdMonFilter.hh"
#include "XrdApps/XrdMonCollect/XrdMonForward.hh"
#include "XrdApps/XrdMonCollect/XrdMonMemory.hh"
// For XrdMonBulkAdd(): the --bulk file sink writes _bulk NDJSON with no cluster
// involved, so the framing is needed even in a build without libcurl.
#include "XrdApps/XrdMonCollect/XrdMonOpenSearch.hh"
#include "XrdApps/XrdMonCollect/XrdMonPipe.hh"
#include "XrdApps/XrdMonCollect/XrdMonShovel.hh"
#include "XrdApps/XrdMonCollect/XrdMonShovelFrame.hh"
#include "XrdApps/XrdMonCollect/XrdMonSinkQueue.hh"
#include "XrdApps/XrdMonCollect/XrdMonTcpServer.hh"
#include "XrdMetrics/XrdMetricsRegistry.hh"
#include "XrdMetrics/XrdMetricsSerializer.hh"
#ifdef XRDMON_HAVE_CURL
#include "XrdApps/XrdMonCollect/XrdMonOtlp.hh"
#include <curl/curl.h>
#endif

namespace
{
volatile sig_atomic_t stopFlag = 0;
void onSignal(int) {stopFlag = 1;}

// systemd integration. With a companion .socket unit (socket activation) the
// listening sockets are created and held by systemd, so they survive service
// restarts: datagrams simply queue in the kernel receive buffer while the
// collector is down, giving zero-loss restarts up to that buffer.
// sdInheritedSocket returns the inherited fd matching the given socket type
// and configured port, or -1 (also when built without libsystemd or not
// socket-activated), in which case the caller binds the socket itself as
// usual. sdNotify forwards service state to systemd (Type=notify) and is a
// no-op outside of it.
//
int sdInheritedSocket(int type, int port)
{
#ifdef HAVE_SYSTEMD
   int n = sd_listen_fds(0);
   for (int i = 0; i < n; i++)
      {int fd = SD_LISTEN_FDS_START + i;
       if (sd_is_socket_inet(fd, AF_UNSPEC, type,
                             type == SOCK_STREAM ? 1 : -1,
                             (uint16_t)port) > 0)
          return fd;
      }
#else
   (void)type; (void)port;
#endif
   return -1;
}

void sdNotify(const char* state)
{
#ifdef HAVE_SYSTEMD
   sd_notify(0, state);
#else
   (void)state;
#endif
}

// INIReader keeps every parsed key in a protected std::map keyed by the
// lowercased "<section>=<name>" and exposes only the section names, so a loader
// cannot tell a mistyped key from an unset one. Filter rules need exactly that
// distinction — an unrecognised key there is a rule that silently matches
// nothing — so surface the keys of one section. Nothing else changes.
//
class MonCfg : public INIReader
{
public:
   using INIReader::INIReader;

   //! Lowercased names of the keys set in `section` (which may be given in any
   //! case, as INIReader folds it internally).
   std::vector<std::string> KeysIn(const std::string& section) const
   {
      std::string pfx = section + "=";
      std::transform(pfx.begin(), pfx.end(), pfx.begin(),
                     [](unsigned char c){return (char)std::tolower(c);});
      std::vector<std::string> keys;
      for (auto it = _values.lower_bound(pfx);
           it != _values.end() && !it->first.compare(0, pfx.size(), pfx); ++it)
          keys.push_back(it->first.substr(pfx.size()));
      return keys;
   }
};

// Recognise a "[<kind> "<name>"]" section header and extract its name. inih
// hands back whatever stands between the brackets, verbatim and unquoted, so
// both the git-config form and a bare "[<kind> <name>]" are accepted. Used for
// filter rules and for the two kinds of sink destination, which is why it takes
// the prefix rather than hardcoding one.
//
bool namedSection(const std::string& sec, const char* kPfx, std::string& name)
{
   const std::size_t  pLen = strlen(kPfx);

   if (sec.size() < pLen || strncasecmp(sec.c_str(), kPfx, pLen)) return false;
   std::string rest = sec.substr(pLen);
   if (!rest.empty() && !isspace((unsigned char)rest[0]) && rest[0] != '"'
                     && rest[0] != '.') return false;   // e.g. "[filters]"

   auto b = rest.find_first_not_of(" \t.");
   auto e = rest.find_last_not_of(" \t");
   name = (b == std::string::npos) ? std::string() : rest.substr(b, e - b + 1);
   if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
      name = name.substr(1, name.size() - 2);
   return true;
}

void usage(const char* prog)
{
   fprintf(stderr,
     "Usage: %s [-c <file>] -p <port> [-b <bindaddr>] [-o <file>]\n"
     "          [--bulk <index>]\n"
     "          [--os-url <url> [--os-index <name>] [--os-user <u>]\n"
     "           [--os-pass <p>] [--os-token <t>] [--os-insecure]\n"
     "           [--os-datastream]]\n"
     "          [--otlp-url <url> [--otlp-token <t>] [--otlp-insecure]]\n"
     "          [--cache-dir <dir>] [--forward <host:port>]\n"
     "          [--metrics-port <port>] [--site <name>]\n"
     "          [--tcp-port <port> [--tcp-token <t>]]\n"
     "          [--shovel <host:port> [--shovel-token <t>] [--spool-max <sz>]]\n"
     "          [--flush-count <n>] [--flush-secs <n>] [--rcvbuf <sz>]\n"
     "          [--queue-depth <n>] [--max-memory <sz>] [--max-entries <n>]\n"
     "          [--server-ttl <s>] [--file-ttl <s>]\n"
     "          [--state-file <file>] [--state-ttl <dur>]\n"
     "          [--scitags <src> [--scitags-refresh <s>]] [--dataset <re>]\n"
     "          [--no-resolve]\n"
     "          [--sessions] [--spans] [--traces] [--gstream] [--redirects]\n"
     "          [--debug] [-v]\n\n"
     "  -c <file>        load options from an INI config file (a [xrdmoncollect]\n"
     "                   section, optional [filter \"<name>\"] sections that tag\n"
     "                   or drop emitted documents, and optional\n"
     "                   [opensearch \"<name>\"] / [otlp \"<name>\"] sections, one\n"
     "                   per extra destination; default\n"
     "                   /etc/xrootd/xrdmoncollect.cfg if present;\n"
     "                   command-line options override file values)\n"
     "  -p <port>        UDP port to listen on (long form: --udp-port; required\n"
     "                   unless --tcp-port is given)\n"
     "  -b <bindaddr>    address to bind (default: all interfaces, dual-stack)\n"
     "  --tcp-port <p>   also accept UDP packets encapsulated over TCP from\n"
     "                   remote collectors running in shoveler mode\n"
     "  --tcp-token <t>  shared secret shovelers must present; @<file> reads it\n"
     "                   from a file (default: accept any connection)\n"
     "  --shovel <h:p>   shoveler mode: relay received UDP packets over TCP to\n"
     "                   a central collector's --tcp-port instead of decoding\n"
     "                   them (document sinks are inactive in this mode)\n"
     "  --shovel-token <t> shared secret for the --shovel connection; @<file>\n"
     "                   reads it from a file\n"
     "  --spool-max <sz> cap the shovel disk spool under --cache-dir, evicting\n"
     "                   the oldest buffers (K/M/G; default 1G; 0=unbounded)\n"
     "  -o <file>        append output to <file> (default: stdout unless --os-url)\n"
     "  --bulk <index>   write OpenSearch _bulk format to the file/stdout sink\n"
     "  --os-url <url>   POST documents to an OpenSearch cluster's _bulk API\n"
     "  --os-index <n>   index/data-stream name (default: xrootd-file-ops)\n"
     "  --os-user <u>    basic-auth user\n"
     "  --os-pass <p>    basic-auth password\n"
     "  --os-token <t>   bearer token (Authorization: Bearer); wins over basic\n"
     "                   auth; @<file> reads the token from a file\n"
     "  --os-insecure    skip TLS certificate verification\n"
     "  --os-datastream  target is a data stream (use the \"create\" action)\n"
     "  --otlp-url <url> POST OTLP/JSON to an OTel collector: logs to <url>/v1/logs\n"
     "                   and (with --spans) traces to <url>/v1/traces\n"
     "  --otlp-token <t> bearer token (Authorization: Bearer); @<file> reads it\n"
     "                   from a file\n"
     "  --otlp-insecure  skip TLS verification for the OTLP endpoint\n"
     "  --cache-dir <d>  cache _bulk bodies that fail to POST under <d> and retry\n"
     "                   (oldest-first, replayed on startup; default: off=drop)\n"
     "  --forward <h:p>  also stream documents as NDJSON over TCP to host:port\n"
     "                   (e.g. a logstash/fluentd/vector buffering frontend)\n"
     "  --flush-count <n> packets per receive batch / one batch -> one POST\n"
     "                   (default: 500)\n"
     "  --flush-secs <n>  hand off a partial batch after N seconds (default: 5)\n"
     "  --rcvbuf <sz>    kernel UDP receive buffer, SO_RCVBUF (K/M/G; default 16M)\n"
     "  --queue-depth <n> receive->serialize batches in flight (default: 64)\n"
     "  --metrics-port <p> serve aggregated metrics over HTTP on port <p>\n"
     "  --site <name>    tag everything this collector emits with a site: a\n"
     "                   site label on every metric series and xrootd.site on\n"
     "                   every document (default: none, and nothing is tagged)\n"
     "  --max-memory <sz> cap total process memory at ~<sz> bytes, evicting the\n"
     "                   least-recently-used correlation entries to meet it\n"
     "                   (K/M/G suffix; default 1G; 0=unbounded). Best-effort:\n"
     "                   only the correlation state can be evicted\n"
     "  --max-entries <n> optional hard cap on correlation entries (0=off)\n"
     "  --server-ttl <s> reclaim a server incarnation idle for >s seconds\n"
     "                   (default 86400; 0=never)\n"
     "  --file-ttl <s>   expire an open-file entry untouched for >s seconds\n"
     "                   (a leaked open whose close record was lost; default\n"
     "                   0=off). Only applied to servers reporting in-flight\n"
     "                   snapshots (\"xfr\" on xrootd.monitor fstat), which\n"
     "                   refresh live transfers every interval; set it to at\n"
     "                   least 3x the server's xfr reporting period\n"
     "  --state-file <f> save the correlation state to <f> on shutdown and\n"
     "                   reload it on startup (default: $STATE_DIRECTORY/\n"
     "                   xrdmoncollect-state.json under systemd, else off;\n"
     "                   an empty value disables)\n"
     "  --state-ttl <d>  discard a state snapshot older than this on reload\n"
     "                   (s/m/h/d suffix; default 15m)\n"
     "  --scitags <src>  SciTags registry mapping experiment/activity ids to\n"
     "                   names; a file path or an http(s):// URL.\n"
     "                   Numeric ids are kept either way\n"
     "  --scitags-refresh <s> re-fetch a URL registry every <s> seconds\n"
     "                   (default 3600; 0 disables; URL sources only)\n"
     "  --dataset <re>   POSIX extended regex; capture group 1, matched\n"
     "                   against each file path, is emitted as xrootd.dataset\n"
     "  --no-resolve     do not substitute the local FQDN for a loopback server\n"
     "  --sessions       correlate per-session activity and emit a session\n"
     "                   document per client disconnect (off by default)\n"
     "  --spans          also emit OpenTelemetry span documents (file-operation\n"
     "                   and session spans) alongside the logs (off by default)\n"
     "  --traces         emit a document per t-stream I/O record (high volume)\n"
     "  --gstream        emit a document per g-stream (plugin) record\n"
     "  --redirects      emit a document per r-stream redirect record\n"
     "  --debug          also emit one JSON object per decoded record\n"
     "  -v               print decoder statistics on exit\n", prog);
}

// Parse a byte size with an optional K/M/G/T suffix (1024-based) and optional
// trailing 'B'; a bare number is bytes. Returns 0 on a malformed value (which
// also means "unbounded" for --max-memory). Used for --max-memory.
//
std::size_t parseSize(const char* s)
{
   char* end = nullptr;
   double v = strtod(s, &end);
   if (end == s || v < 0) return 0;
   std::size_t mul = 1;
   switch (*end)
         {case 'k': case 'K': mul = 1ull << 10; end++; break;
          case 'm': case 'M': mul = 1ull << 20; end++; break;
          case 'g': case 'G': mul = 1ull << 30; end++; break;
          case 't': case 'T': mul = 1ull << 40; end++; break;
          default: break;
         }
   if (*end == 'b' || *end == 'B') end++;          // accept e.g. "256MB"
   if (*end != '\0') return 0;
   return (std::size_t)(v * (double)mul);
}

// How many bytes of datagram payload one receive batch may accumulate before
// it is handed on, derived from the memory budget so there is still a single
// memory knob.
//
// The receive queue is the largest consumer no eviction can reach: queueDepth
// batches of flushCount datagrams is 32000 packets at the defaults, ~256 MB at
// the 8 KiB an f stream can emit, and none of it is correlation state. Give the
// queue as a whole a sixteenth of the budget (floored at 8 MiB so a small
// budget cannot starve it, capped at 256 MiB so a huge one cannot make the
// bound meaningless) and divide by the depth to get the per-batch share.
//
// For ordinary small datagrams flushCount still binds first, so this only
// takes effect in the case it exists for.
//
std::size_t recvBatchBytes(std::size_t budget, int queueDepth)
{
   if (!budget) return 0;                          // unbounded
   std::size_t queue = std::min(std::max(budget/16, (std::size_t)(8u << 20)),
                                (std::size_t)(256u << 20));
   std::size_t depth = queueDepth > 0 ? (std::size_t)queueDepth : 1;
   return std::max((std::size_t)(64u << 10), queue / depth);
}

// Parse a duration with an optional s/m/h/d suffix (a bare number is seconds).
// Returns 0 on a malformed value. Used for --state-ttl.
//
long parseDuration(const char* s)
{
   char* end = nullptr;
   long v = strtol(s, &end, 10);
   if (end == s || v < 0) return 0;
   switch (*end)
         {case 'd': case 'D': v *= 24;  [[fallthrough]];
          case 'h': case 'H': v *= 60;  [[fallthrough]];
          case 'm': case 'M': v *= 60;  end++; break;
          case 's': case 'S': end++;    break;
          default: break;
         }
   if (*end != '\0') return 0;
   return v;
}

// Resolve a secret (e.g. a bearer token) that may be given inline or, with a
// leading '@', read from a file so it does not appear in argv/ps or the config.
// The file's contents are used verbatim minus trailing whitespace/newlines.
//
std::string loadSecret(const std::string& spec)
{
   if (spec.empty() || spec[0] != '@') return spec;
   FILE* f = fopen(spec.c_str() + 1, "r");
   if (!f)
      {fprintf(stderr, "xrdmoncollect: cannot read secret file '%s'\n",
               spec.c_str() + 1);
       return "";
      }
   std::string s; char buf[4096]; size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
   fclose(f);
   while (!s.empty() && (s.back()=='\n' || s.back()=='\r' ||
                         s.back()==' '  || s.back()=='\t')) s.pop_back();
   return s;
}

// Create and bind a UDP socket. With no bind address, an IPv6 dual-stack
// socket is used so packets from both IPv4 and IPv6 senders are received
// (servers commonly resolve "localhost" to ::1). With an explicit address the
// family is taken from it. Returns the fd, or -1 with a message on stderr.
//
int openUDP(int port, const char* bindStr)
{
// A socket inherited from systemd (socket activation) is already bound; it
// also outlives us, so datagrams keep queueing across a service restart.
//
   int sdFd = sdInheritedSocket(SOCK_DGRAM, port);
   if (sdFd >= 0)
      {fprintf(stderr, "xrdmoncollect: using systemd-provided UDP socket on "
               "port %d\n", port);
       return sdFd;
      }

   if (bindStr)
      {addrinfo hints; memset(&hints, 0, sizeof(hints));
       hints.ai_family   = AF_UNSPEC;
       hints.ai_socktype = SOCK_DGRAM;
       hints.ai_flags    = AI_NUMERICHOST | AI_PASSIVE;
       char portStr[16]; snprintf(portStr, sizeof(portStr), "%d", port);
       addrinfo* res = nullptr;
       if (getaddrinfo(bindStr, portStr, &hints, &res) != 0 || !res)
          {fprintf(stderr, "invalid bind address '%s'\n", bindStr); return -1;}
       int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
       if (fd >= 0 && bind(fd, res->ai_addr, res->ai_addrlen) < 0)
          {perror("bind"); close(fd); fd = -1;}
       freeaddrinfo(res);
       return fd;
      }

   int fd = socket(AF_INET6, SOCK_DGRAM, 0);
   if (fd < 0) {perror("socket"); return -1;}

   int off = 0;  // IPV6_V6ONLY off => also receive IPv4-mapped traffic
   setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

   sockaddr_in6 me; memset(&me, 0, sizeof(me));
   me.sin6_family = AF_INET6;
   me.sin6_port   = htons((uint16_t)port);
   me.sin6_addr   = in6addr_any;
   if (bind(fd, (sockaddr*)&me, sizeof(me)) < 0)
      {perror("bind"); close(fd); return -1;}
   return fd;
}

// A minimal HTTP exporter: serves the metrics registry to any GET request on
// the given TCP port. Prometheus scrapes /metrics; we answer every path.
//
// The collector's own metrics registry. The root Collector owns the "xrootd"
// prefix; its subsystems are "collector" (or "shoveler") plus "process", so a
// bare metric name such as "packets_total" is exposed as
// "xrootd_collector_packets_total".
//
static XrdMetrics::Collector& collectorRegistry()
{
   static XrdMetrics::Collector collector("xrootd");
   return collector;
}

// Process-level metrics, shared by both daemon modes and named by the same
// convention the server uses in XrdStats (xrootd_process_cpu_seconds_total).
//
// DEFINED AT THE BOTTOM OF THIS FILE, and that placement is not style:
// tests/XrdMonCollectTests/dashboards_test.py attributes each registration to
// the subsystem("...") literal that most recently precedes it in the file
// text, so a "process" literal anywhere above would relabel every collector
// metric that follows it. Keep it last, after every other registration.
//
static void registerProcessMetrics();

void serveMetrics(int port, std::atomic<bool>& stop)
{
   int ls = socket(AF_INET, SOCK_STREAM, 0);
   if (ls < 0) {perror("metrics socket"); return;}
   int one = 1;
   setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

   sockaddr_in a; memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY;
   a.sin_port = htons((uint16_t)port);
   if (bind(ls, (sockaddr*)&a, sizeof(a)) < 0 || listen(ls, 8) < 0)
      {perror("metrics bind/listen"); close(ls); return;}

   timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;        // wake to check stop
   setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

   while(!stop)
        {int c = accept(ls, nullptr, nullptr);
         if (c < 0)
            {if (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR) continue;
             break;
            }
         char req[2048];
         recv(c, req, sizeof(req), 0);              // read & ignore the request

         std::string body;
         XrdMetrics::PrometheusTextSerializer ser(body);
         collectorRegistry().serialize(ser);
         collectorRegistry().runTextCollectors(body);
         std::string resp = "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
         send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
         close(c);
        }
   close(ls);
}

// One received datagram (host:port + raw bytes) and batches of them are the
// XrdMonPacket/XrdMonBatch types shared with the shovel framing code.
//
using Packet = XrdMonPacket;
using Batch  = XrdMonBatch;

// A SciTags registry source given as an http(s) URL (vs. a local file path).
//
bool isUrl(const std::string& s)
{
   return s.compare(0, 7, "http://") == 0 || s.compare(0, 8, "https://") == 0;
}

#ifdef XRDMON_HAVE_CURL
size_t curlAppend(char* p, size_t sz, size_t n, void* ud)
{
   ((std::string*)ud)->append(p, sz * n);
   return sz * n;
}

// GET a URL into body via libcurl. Returns false with a message in err.
//
bool httpGet(const std::string& url, std::string& body, std::string& err)
{
   CURL* c = curl_easy_init();
   if (!c) {err = "curl init failed"; return false;}
   body.clear();
   curl_easy_setopt(c, CURLOPT_URL, url.c_str());
   curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlAppend);
   curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
   curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
   curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
   CURLcode rc = curl_easy_perform(c);
   curl_easy_cleanup(c);
   if (rc != CURLE_OK) {err = curl_easy_strerror(rc); return false;}
   return true;
}
#endif

// Load the SciTags registry into the decoder from a file path or an http(s)
// URL. Thread-safe with respect to the decode loop (LoadScitags*/otelIdentity
// share a mutex), so it is also called from the periodic refresh thread.
//
bool loadScitags(XrdMonDecode& dec, const std::string& src, std::string& err)
{
   if (isUrl(src))
      {
#ifdef XRDMON_HAVE_CURL
       std::string body;
       if (!httpGet(src, body, err)) return false;
       if (!dec.LoadScitagsJson(body))
          {err = "fetched document is not a valid SciTags registry"; return false;}
       return true;
#else
       err = "a URL SciTags registry requires building with libcurl";
       return false;
#endif
      }
   if (!dec.LoadScitags(src)) {err = "cannot read or parse the file"; return false;}
   return true;
}

/******************************************************************************/
/*                         S h o v e l e r   m o d e                          */
/******************************************************************************/

// In shoveler mode there is no decoding, correlation, or document sink:
// received UDP datagrams are XSHV-framed and relayed over TCP to a central
// collector's --tcp-port, with an optional disk spool bridging collector
// outages. Deployed next to the daemons, it turns the lossy long-haul UDP leg
// into a local one plus a reliable TCP hop.
//
struct ShovelerOpts
{
   int         udpPort;
   const char* bindStr;
   std::string host;         // central collector
   int         port;         // central collector TCP port
   std::string token;        // shared secret for the hello (may be empty)
   std::string cacheDir;     // spool root (no spool if empty)
   std::size_t spoolMax;     // spool byte cap (0 = unbounded)
   std::size_t flushCount;
   long        flushSecs;
   std::size_t rcvbuf;
   int         queueDepth;
   std::size_t maxMemory;    // process memory budget, for the queue byte bound
   int         metricsPort;
   bool        verbose;
};

int runShoveler(const char* prog, const ShovelerOpts& o)
{
   XrdMonShovel shovel(o.host, o.port, o.token);

   XrdMonDiskCache* spool = nullptr;
   if (!o.cacheDir.empty())
      {mkdir(o.cacheDir.c_str(), 0750);   // parent (Init's mkdir is single-level)
       spool = new XrdMonDiskCache(o.cacheDir + "/shovel", ".frames");
       spool->SetMaxBytes(o.spoolMax);
       std::string e;
       if (!spool->Init(e))
          {fprintf(stderr, "%s: %s\n", prog, e.c_str()); return 4;}
      }

   int fd = openUDP(o.udpPort, o.bindStr);
   if (fd < 0) return 4;
   if (o.rcvbuf > 0)
      {int want = (int)o.rcvbuf;
#ifdef SO_RCVBUFFORCE
       if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &want, sizeof(want)) != 0)
#endif
          setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
      }

   XrdMonPipe<Batch> recvPipe((size_t)(o.queueDepth > 0 ? o.queueDepth : 1));

   std::atomic<std::uint64_t> packets{0},       framesSent{0},
                              framesSpooled{0}, framesDropped{0};

// The shoveler's own metrics, under the "shoveler" subsystem (so a Prometheus
// scrape distinguishes a relay from a full collector on the same dashboard).
//
   std::atomic<bool> exporterStop{false};
   std::thread       exporter;
   if (o.metricsPort > 0)
      {XrdMetrics::Subsystem& sub = collectorRegistry().subsystem("shoveler");
       sub.observeCounter<std::uint64_t>("packets_total", "UDP monitor packets received")
          .add({}, [&]{return packets.load();});
       sub.observeCounter<std::uint64_t>("frames_sent_total", "datagrams relayed to the central collector")
          .add({}, [&]{return framesSent.load();});
       sub.observeCounter<std::uint64_t>("frames_spooled_total", "datagrams written to the disk spool while the collector was unreachable")
          .add({}, [&]{return framesSpooled.load();});
       sub.observeCounter<std::uint64_t>("frames_dropped_total", "datagrams dropped (collector unreachable and no usable spool)")
          .add({}, [&]{return framesDropped.load();});
       sub.observeCounter<std::uint64_t>("connects_total", "successful connect+handshakes to the collector")
          .add({}, [&]{return shovel.Connects();});
       sub.observeGauge<std::int64_t>("connected", "whether the collector connection is currently up")
          .add({}, [&]{return (int64_t)(shovel.Connected() ? 1 : 0);});
       sub.observeGauge<std::int64_t>("recv_queue_batches", "packet batches queued from the receiver to the sender")
          .add({}, [&]{return (int64_t)recvPipe.readyDepth();});
       if (spool)
          {sub.observeGauge<std::int64_t>("spool_files", "spooled frame buffers awaiting replay")
              .add({}, [&]{return (int64_t)spool->Files();});
           sub.observeGauge<std::int64_t>("spool_bytes", "bytes of spooled frame buffers on disk")
              .add({}, [&]{return (int64_t)spool->Bytes();});
           sub.observeCounter<std::uint64_t>("spool_stored_total", "frame buffers written to the disk spool")
              .add({}, [&]{return spool->Stored();});
           sub.observeCounter<std::uint64_t>("spool_replayed_total", "spooled frame buffers successfully replayed")
              .add({}, [&]{return spool->Replayed();});
           sub.observeCounter<std::uint64_t>("spool_dropped_total", "oldest spooled buffers evicted by --spool-max")
              .add({}, [&]{return spool->Dropped();});
          }
       registerProcessMetrics();     // must follow every sub.* call above
       exporter = std::thread(serveMetrics, o.metricsPort, std::ref(exporterStop));
      }

   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_handler = onSignal;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;
   sigaction(SIGINT,  &sa, nullptr);
   sigaction(SIGTERM, &sa, nullptr);

   {struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));}

// Sender thread: frames each batch into one contiguous buffer and ships it.
// While the collector is unreachable, buffers go to the disk spool (and are
// replayed oldest-first on idle wake-ups and ahead of new buffers, preserving
// order); without a spool they are dropped and counted.
//
   std::thread sender([&]()
      {const int kDrainPerIter = 64;   // cap spool replays per wake-up
       std::size_t dropTally = 0;      // total drops, for the rate-limited warn
       time_t      warnAt    = 0;

       auto ship = [&](const std::string& b)->bool
          {std::string e;
           return shovel.Send(b, e);
          };
       auto drain = [&]()
          {if (!spool) return;
           std::string e;
           for (int i = 0; i < kDrainPerIter; i++)
              {int r = spool->ReplayOldest(ship, e);
               if (!e.empty())
                  {fprintf(stderr, "xrdmoncollect: %s\n", e.c_str()); e.clear();}
               if (r != 1) break;       // 0 = empty, -1 = collector still down
              }
          };
       auto complain = [&](std::size_t n, const std::string& why)
          {framesDropped += n;
           dropTally     += n;
           time_t now = time(0);
           if (now - warnAt >= 10)      // rate-limit to once per 10 seconds
              {warnAt = now;
               fprintf(stderr, "xrdmoncollect: shovel dropped %zu datagram(s): "
                       "%s\n", dropTally, why.c_str());
              }
          };

       Batch       b;
       std::string buf;
       for (;;)
          {if (recvPipe.takeFor(b, 1000))
              {XrdMonRecycleBody(buf);   // reused every batch; do not keep a peak
               std::size_t n = 0;
               for (auto& p : b)
                   if (XrdMonShovelEncode(buf, (const sockaddr*)&p.addr,
                                          p.data.data(), p.data.size())) n++;
               std::string e;
               if (!n) {}                             // nothing encodable
                  else if (spool && !spool->Empty())
                  {// Preserve order: queue behind the backlog, then drain.
                   if (spool->Store(buf, e)) framesSpooled += n;
                      else complain(n, e);
                   drain();
                  }
                  else if (shovel.Send(buf, e)) framesSent += n;
                  else if (spool)
                  {std::string se;
                   if (spool->Store(buf, se)) framesSpooled += n;
                      else complain(n, se);
                  }
                  else complain(n, e);
               b.clear();
               recvPipe.recycle(std::move(b));
              }
              else
              {if (recvPipe.closedDrained()) break;
               drain();                 // idle: make progress on the backlog
              }
          }
      });

// Receiver loop: same shape as the collector's, but records only the raw
// source address (the sender re-encodes the sockaddr on the wire; no per-
// packet host:port formatting is needed).
//
   sdNotify("READY=1");
   Batch  cur;
   recvPipe.acquire(cur);
   time_t batchStart = time(0);
   size_t curBytes   = 0;
   const size_t batchBytes = recvBatchBytes(o.maxMemory, o.queueDepth);
   char   buff[64*1024];
   while (!stopFlag)
        {sockaddr_storage from;
         socklen_t fromLen = sizeof(from);
         ssize_t n = recvfrom(fd, buff, sizeof(buff), 0,
                              (sockaddr*)&from, &fromLen);
         if (n < 0)
            {if (errno == EINTR) continue;
             if (errno == EAGAIN || errno == EWOULDBLOCK)
                {if (!cur.empty() && time(0)-batchStart >= o.flushSecs)
                    {recvPipe.submit(std::move(cur));
                     if (!recvPipe.acquire(cur)) break;
                     batchStart = time(0); curBytes = 0;
                    }
                 continue;
                }
             perror("recvfrom"); break;
            }

         packets++;
         Packet pkt;
         pkt.data.assign(buff, (size_t)n);
         memcpy(&pkt.addr, &from, fromLen);
         pkt.addrLen = fromLen;
         cur.push_back(std::move(pkt));
         curBytes += (size_t)n;
         if (cur.size() >= o.flushCount || time(0)-batchStart >= o.flushSecs
         || (batchBytes && curBytes >= batchBytes))
            {recvPipe.submit(std::move(cur));
             if (!recvPipe.acquire(cur)) break;
             batchStart = time(0); curBytes = 0;
            }
        }

   sdNotify("STOPPING=1");
   if (!cur.empty()) recvPipe.submit(std::move(cur));
   recvPipe.close();
   sender.join();
   if (exporter.joinable()) {exporterStop = true; exporter.join();}

   if (o.verbose)
      fprintf(stderr, "xrdmoncollect: shoveler packets=%llu sent=%llu "
              "spooled=%llu dropped=%llu\n",
              (unsigned long long)packets.load(),
              (unsigned long long)framesSent.load(),
              (unsigned long long)framesSpooled.load(),
              (unsigned long long)framesDropped.load());

   close(fd);
   delete spool;
   return 0;
}

/******************************************************************************/
/*                     S i n k   d e s t i n a t i o n s                      */
/******************************************************************************/

// One configured OpenSearch destination. The flat --os-* options define the
// one named "default"; [opensearch "<name>"] config sections add more.
struct OsDest
{
   std::string name = "default";
   std::string url, index = "xrootd-file-ops", user, pass, token;
   bool        insecure = false, dataStream = false;
};

// One configured OTLP destination, likewise.
struct OtlpDest
{
   std::string name = "default";
   std::string url, token;
   bool        insecure = false;
};

// The disk cache directory for a destination. "default" keeps the legacy paths
// so an upgrade does not orphan whatever is spooled there.
std::string sinkCacheDir(const std::string& root, const char* kind,
                         const std::string& name, const char* signal = "")
{
   if (name == "default")
      return *signal ? root + "/" + kind + signal : root;
   return root + "/" + kind + "-" + name + signal;
}

// A destination name reaches the filesystem (its disk-cache directory) and the
// Prometheus label, so it is restricted rather than merely non-empty. No '/',
// so it cannot escape --cache-dir; every named destination is also prefixed
// with its kind, so "." and ".." can only ever name a literal directory.
bool sinkName(const std::string& n, std::string& err)
{
   if (n.empty()) {err = "needs a name"; return false;}
   if (!isalnum((unsigned char)n[0]))
      {err = "name '" + n + "' must start with a letter or digit"; return false;}
   for (char c : n)
       if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.')
          {err = "name '" + n + "' may only contain letters, digits, '_', '-' "
                 "and '.'";
           return false;
          }
   return true;
}

// Read one key, rejecting a value inih mangled. Repeated keys arrive
// newline-joined and an indented line is appended to the previous key's value,
// so a '\n' here always means the file is not saying what its author thinks.
bool sinkValue(const MonCfg& cfg, const std::string& sec, const char* key,
               const std::string& dflt, std::string& out, std::string& err)
{
   std::string v = cfg.Get(sec, key, dflt);
   if (v.find('\n') != std::string::npos)
      {err = std::string("'") + key + "' has more than one value: a repeated "
             "key, or a continuation line that must start in column 1";
       return false;
      }
   out = v;
   return true;
}

bool knownKey(const std::string& k, const char* const* allowed)
{
   for (const char* const* p = allowed; *p; p++) if (k == *p) return true;
   return false;
}

// Parse [opensearch "<name>"]. Nothing is inherited from the flat os-* keys --
// not the index, and emphatically not the credentials. Inheriting would make
// the result depend on whether a value came from the config file or the command
// line (the two are read in that order), and would silently POST the site's own
// OpenSearch credentials to a central endpoint.
//
bool parseOsSection(const MonCfg& cfg, const std::string& sec,
                    const std::string& name, OsDest& d, std::string& err)
{
   static const char* const kKeys[] = {"url", "index", "user", "pass", "token",
                                       "insecure", "datastream", nullptr};
   for (const std::string& k : cfg.KeysIn(sec))
       if (!knownKey(k, kKeys)) {err = "unknown key '" + k + "'"; return false;}

   d = OsDest();
   d.name = name;
   if (!sinkValue(cfg, sec, "url",   "",      d.url,   err)) return false;
   if (!sinkValue(cfg, sec, "index", d.index, d.index, err)) return false;
   if (!sinkValue(cfg, sec, "user",  "",      d.user,  err)) return false;
   if (!sinkValue(cfg, sec, "pass",  "",      d.pass,  err)) return false;
   if (!sinkValue(cfg, sec, "token", "",      d.token, err)) return false;
   d.insecure   = cfg.GetBoolean(sec, "insecure", false);
   d.dataStream = cfg.GetBoolean(sec, "datastream", false);
   if (d.url.empty()) {err = "needs a 'url'"; return false;}
   return true;
}

// Parse [otlp "<name>"]; see parseOsSection on inheritance.
//
bool parseOtlpSection(const MonCfg& cfg, const std::string& sec,
                      const std::string& name, OtlpDest& d, std::string& err)
{
   static const char* const kKeys[] = {"url", "token", "insecure", nullptr};
   for (const std::string& k : cfg.KeysIn(sec))
       if (!knownKey(k, kKeys)) {err = "unknown key '" + k + "'"; return false;}

   d = OtlpDest();
   d.name = name;
   if (!sinkValue(cfg, sec, "url",   "", d.url,   err)) return false;
   if (!sinkValue(cfg, sec, "token", "", d.token, err)) return false;
   d.insecure = cfg.GetBoolean(sec, "insecure", false);
   if (d.url.empty()) {err = "needs a 'url'"; return false;}
   return true;
}

#ifdef XRDMON_HAVE_CURL

// Retries a live POST makes when there is nowhere to spill to. With a disk
// cache the live attempt uses 0 and the replay path uses this, so a failing
// destination degrades to "cache it, retry on the replay schedule" instead of
// holding the body (and the queue behind it) for the whole ladder.
const int kSinkRetries = 4;

// Bodies one destination may have queued for it. The byte bound below is the
// operative one; this just keeps a stream of tiny bodies from growing the deque
// without limit.
const std::size_t kSinkQueueBodies = 16;

// Both sink clients report the same three outcomes under their own nested enum
// (neither header can name a shared one without pulling in the other's, or the
// cache's). This is where they meet the cache's view of the same distinction.
template<class R>
XrdMonDiskCache::Verdict verdictOf(R r)
{
   return r == R::Ok       ? XrdMonDiskCache::Verdict::Delivered
        : r == R::Rejected ? XrdMonDiskCache::Verdict::Rejected
                           : XrdMonDiskCache::Verdict::Unavailable;
}

// A destination's live state: its client, its queue, its thread, its disk cache
// and its own counters. Everything a failing destination touches is private to
// it, which is the point -- a central collector going down must not stall the
// site's own. Non-copyable and non-movable (the thread, the queue and the
// metric readers all pin the address), hence vector<unique_ptr<>>.
struct OsSink
{
   OsSink(const OsDest& d, std::size_t qBytes)
         : cfg(d), q(kSinkQueueBodies, qBytes) {}
  ~OsSink() {delete cli; delete cache;}

   OsDest            cfg;
   XrdMonOpenSearch* cli   = nullptr;
   XrdMonSinkQueue   q;
   std::thread       th;
   XrdMonDiskCache*  cache = nullptr;
   XrdMetrics::Counter<std::uint64_t>* failures = nullptr;
   XrdMetrics::Counter<std::uint64_t>* dropped  = nullptr;
   // Set while the thread is posting or spilling. An empty queue is not an
   // idle destination: the body it is working on has already been taken.
   std::atomic<bool> busy{false};
   std::atomic<bool> done{false};   // thread has left its loop
   time_t            warnAt = 0;    // per-sink, so one dead endpoint cannot
};                                  // silence another's diagnostics

struct OtlpSink
{
   OtlpSink(const OtlpDest& d, std::size_t qBytes)
           : cfg(d), logs(kSinkQueueBodies, qBytes),
                     traces(kSinkQueueBodies, qBytes) {}
  ~OtlpSink() {delete cli; delete logCache; delete traceCache;}

   OtlpDest          cfg;
   XrdMonOtlp*       cli = nullptr;
   XrdMonSinkQueue   logs, traces;
   std::thread       th;
   XrdMonDiskCache*  logCache   = nullptr;
   XrdMonDiskCache*  traceCache = nullptr;
   XrdMetrics::Counter<std::uint64_t>* failures = nullptr;
   XrdMetrics::Counter<std::uint64_t>* dropped  = nullptr;
   std::atomic<bool> busy{false};   // see OsSink::busy
   std::atomic<bool> done{false};
   time_t            warnAt = 0;
};

// Documents are accumulated once per distinct (index, action) pair rather than
// once per destination, so two clusters written with the same index share one
// body and one serialization. The common case is a single group.
struct OsGroup
{
   std::string          index;
   bool                 create = false;
   std::string          body;
   std::size_t          count  = 0;
   time_t               lastShip = 0;   // own coalescing deadline: a shared one
   std::vector<OsSink*> dests;          // lets a fast group starve a slow one
};
#endif
}

int main(int argc, char* argv[])
{
// Before anything allocates and, crucially, before any thread starts: the
// arena count is fixed at first use per thread, and the mmap threshold has to
// be pinned before glibc starts ratcheting it upward. Covers the shoveler too,
// which runs out of this same main().
//
   XrdMonTuneAllocator();

   int         port    = 0;
   int         tcpPort = 0;         // TCP listener for shoveled packets (off)
   std::string tcpToken;            // shared secret for shovel hellos (off)
   std::string shovelHost;          // shoveler mode destination (off if empty)
   int         shovelPort = 0;
   std::string shovelToken;         // shared secret for the shovel hello
   size_t      spoolMax = 1ull << 30;  // shovel disk spool cap (~1 GiB)
   const char* bindStr = nullptr;
   const char* outFile = nullptr;
   std::string bulkIdx;
   bool        debug   = false;
   bool        verbose = false;
   bool        traces  = false;
   bool        gstream = false;
   bool        redirects = false;
   int         metricsPort = 0;
   size_t      maxMemory  = 1ull << 30;     // ~1 GiB total process memory cap
   size_t      maxEntries = 0;              // optional hard entry cap (off)
   long        serverTtl  = 86400;          // reap incarnations idle > 24h
   long        fileTtl    = 0;              // expire stale open-file entries
                                            // (0 = off; needs "xfr" reporting)
   std::string stateFile;                   // state snapshot path (off if empty)
   long        stateTtl   = 900;            // max snapshot age to reload (15m)
   std::string osUrl, osUser, osPass, osToken;
   std::string osIndex = "xrootd-file-ops";
   bool        osInsecure = false;
   bool        osDataStream = false;
   std::string cacheDir;            // disk cache for failed POSTs (off if empty)
   std::string fwdHost; int fwdPort = 0;
   std::string otlpUrl;             // OTLP/HTTP endpoint (off if empty)
   std::string otlpToken;           // OTLP bearer token (off if empty)
   bool        otlpInsecure = false;
   size_t      flushCount = 500;     // packets per receive batch (one batch->POST)
   long        flushSecs  = 5;       // max age of a partial receive batch
   size_t      rcvbuf     = 16ull << 20;  // kernel UDP receive buffer (SO_RCVBUF)
   int         queueDepth = 64;      // receive->serialize batches in flight
   std::string scitags;
   long        scitagsRefresh = 3600;
   std::string dataset;              // --dataset capture regex (off if empty)
   std::string site;                 // --site, this collector's site (or empty)
   bool        resolve = true;
   bool        sessions = false;      // per-session rollup + session documents
   bool        spans    = false;      // companion OTLP span documents
   std::string bindStore, outStore;   // backing storage for config bind/output
   std::vector<OsDest>   osNamed;     // [opensearch "<name>"] config sections
   std::vector<OtlpDest> otlpNamed;   // [otlp "<name>"] config sections
   XrdMonFilter filter;               // [filter "<name>"] document rules
                                      // (declared here so it outlives decoder)

// Under systemd (StateDirectory=xrdmoncollect) default the state snapshot into
// the provided state directory; a config/command-line --state-file overrides
// this, and an explicitly empty value disables persistence. The variable may
// hold several ':'-separated directories; the first one is ours.
//
   if (const char* sd = getenv("STATE_DIRECTORY"))
      {std::string dir(sd);
       auto colon = dir.find(':');
       if (colon != std::string::npos) dir.resize(colon);
       if (!dir.empty()) stateFile = dir + "/xrdmoncollect-state.json";
      }

// Load a configuration file before parsing the command line, so command-line
// options override file values. The path is taken from -c/--config if given,
// otherwise the default /etc/xrootd/xrdmoncollect.cfg is loaded when present
// (a missing default is silently ignored). Keys live in a single
// [xrdmoncollect] section and mirror the long-option names.
//
   const char* cfgPath = nullptr;
   for (int i = 1; i < argc; i++)
       {if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config"))
           {if (i+1 < argc) cfgPath = argv[++i];}
        else if (!strncmp(argv[i], "--config=", 9)) cfgPath = argv[i] + 9;
       }
   std::string cfgFile;
   if (cfgPath) cfgFile = cfgPath;
   else
      {const char* def = "/etc/xrootd/xrdmoncollect.cfg";
       if (access(def, R_OK) == 0) cfgFile = def;
      }
   if (!cfgFile.empty())
      {MonCfg cfg(cfgFile);
       int perr = cfg.ParseError();
       if (perr == -1)
          {fprintf(stderr, "%s: cannot open config file '%s'\n", argv[0],
                   cfgFile.c_str());
           return 2;}
       if (perr > 0)
          {fprintf(stderr, "%s: parse error in '%s' at line %d\n", argv[0],
                   cfgFile.c_str(), perr);
           return 2;}
       const char* sec = "xrdmoncollect";
       port       = (int)cfg.GetInteger(sec, "port", port);
       port       = (int)cfg.GetInteger(sec, "udp-port", port);  // alias
       tcpPort    = (int)cfg.GetInteger(sec, "tcp-port", tcpPort);
       tcpToken   = cfg.Get(sec, "tcp-token", tcpToken);
       std::string shv = cfg.Get(sec, "shovel", "");
       if (!shv.empty())
          {auto c = shv.rfind(':');
           if (c == std::string::npos || c == 0 || c+1 >= shv.size())
              {fprintf(stderr, "%s: config 'shovel' needs host:port\n", argv[0]);
               return 2;}
           shovelHost = shv.substr(0, c);
           shovelPort = atoi(shv.c_str() + c + 1);
          }
       shovelToken = cfg.Get(sec, "shovel-token", shovelToken);
       std::string sm = cfg.Get(sec, "spool-max", "");
       if (!sm.empty()) spoolMax = parseSize(sm.c_str());
       bindStore  = cfg.Get(sec, "bind", "");
       if (!bindStore.empty()) bindStr = bindStore.c_str();
       outStore   = cfg.Get(sec, "output", "");
       if (!outStore.empty()) outFile = outStore.c_str();
       bulkIdx      = cfg.Get(sec, "bulk", bulkIdx);
       osUrl        = cfg.Get(sec, "os-url", osUrl);
       osIndex      = cfg.Get(sec, "os-index", osIndex);
       osUser       = cfg.Get(sec, "os-user", osUser);
       osPass       = cfg.Get(sec, "os-pass", osPass);
       osToken      = cfg.Get(sec, "os-token", osToken);
       osInsecure   = cfg.GetBoolean(sec, "os-insecure", osInsecure);
       osDataStream = cfg.GetBoolean(sec, "os-datastream", osDataStream);
       otlpUrl      = cfg.Get(sec, "otlp-url", otlpUrl);
       otlpToken    = cfg.Get(sec, "otlp-token", otlpToken);
       otlpInsecure = cfg.GetBoolean(sec, "otlp-insecure", otlpInsecure);
       cacheDir     = cfg.Get(sec, "cache-dir", cacheDir);
       std::string fwd = cfg.Get(sec, "forward", "");
       if (!fwd.empty())
          {auto c = fwd.rfind(':');
           if (c == std::string::npos || c == 0 || c+1 >= fwd.size())
              {fprintf(stderr, "%s: config 'forward' needs host:port\n", argv[0]);
               return 2;}
           fwdHost = fwd.substr(0, c);
           fwdPort = atoi(fwd.c_str() + c + 1);
          }
       flushCount  = (size_t)cfg.GetInteger(sec, "flush-count", (long)flushCount);
       flushSecs   = cfg.GetInteger(sec, "flush-secs", flushSecs);
       std::string rb = cfg.Get(sec, "rcvbuf", "");
       if (!rb.empty()) rcvbuf = parseSize(rb.c_str());
       queueDepth  = (int)cfg.GetInteger(sec, "queue-depth", queueDepth);
       metricsPort = (int)cfg.GetInteger(sec, "metrics-port", metricsPort);
       std::string mm = cfg.Get(sec, "max-memory", "");
       if (!mm.empty()) maxMemory = parseSize(mm.c_str());
       maxEntries  = (size_t)cfg.GetInteger(sec, "max-entries", (long)maxEntries);
       serverTtl   = cfg.GetInteger(sec, "server-ttl", serverTtl);
       fileTtl     = cfg.GetInteger(sec, "file-ttl", fileTtl);
       stateFile   = cfg.Get(sec, "state-file", stateFile);
       std::string sttl = cfg.Get(sec, "state-ttl", "");
       if (!sttl.empty()) stateTtl = parseDuration(sttl.c_str());
       scitags     = cfg.Get(sec, "scitags", scitags);
       scitagsRefresh = cfg.GetInteger(sec, "scitags-refresh", scitagsRefresh);
       dataset     = cfg.Get(sec, "dataset", dataset);
       site        = cfg.Get(sec, "site", site);
       resolve     = !cfg.GetBoolean(sec, "no-resolve", !resolve);
       sessions    = cfg.GetBoolean(sec, "sessions", sessions);
       spans       = cfg.GetBoolean(sec, "spans", spans);
       traces      = cfg.GetBoolean(sec, "traces", traces);
       gstream     = cfg.GetBoolean(sec, "gstream", gstream);
       redirects   = cfg.GetBoolean(sec, "redirects", redirects);
       debug       = cfg.GetBoolean(sec, "debug", debug);
       verbose     = cfg.GetBoolean(sec, "verbose", verbose);

// Document filter rules: one [filter "<name>"] section per rule, whose keys
// (bar the reserved action/label) are match conditions. An unknown key is
// fatal rather than ignored, since a rule that silently matches nothing is
// indistinguishable from one that works until documents go missing.
//
       std::set<std::string> ruleNames, osNames, otlpNames;
       for (const std::string& fsec : cfg.Sections())
           {std::string name, serr;
            if (!strcasecmp(fsec.c_str(), sec)) continue;

// A sink destination: one [opensearch "<name>"] or [otlp "<name>"] section per
// endpoint, alongside (or instead of) the flat os-*/otlp-* keys, which define
// the one called "default". Malformed is fatal, never skipped: dropping a
// destination silently can leave no network sink at all, and the file sink then
// takes over and floods stdout with NDJSON at line rate.
//
            auto dupName = [&](std::set<std::string>& seen, const char* kind)
               {std::string l = name;
                std::transform(l.begin(), l.end(), l.begin(),
                               [](unsigned char c){return (char)std::tolower(c);});
                if (seen.insert(l).second) return false;
                fprintf(stderr, "%s: duplicate %s destination '%s'\n",
                        argv[0], kind, name.c_str());
                return true;
               };

            if (namedSection(fsec, "opensearch", name))
               {OsDest d;
                if (!sinkName(name, serr)
                ||  !parseOsSection(cfg, fsec, name, d, serr))
                   {fprintf(stderr, "%s: [opensearch \"%s\"]: %s\n", argv[0],
                            name.c_str(), serr.c_str());
                    return 2;
                   }
                if (dupName(osNames, "OpenSearch")) return 2;
                osNamed.push_back(std::move(d));
                continue;
               }
            if (namedSection(fsec, "otlp", name))
               {OtlpDest d;
                if (!sinkName(name, serr)
                ||  !parseOtlpSection(cfg, fsec, name, d, serr))
                   {fprintf(stderr, "%s: [otlp \"%s\"]: %s\n", argv[0],
                            name.c_str(), serr.c_str());
                    return 2;
                   }
                if (dupName(otlpNames, "OTLP")) return 2;
                otlpNamed.push_back(std::move(d));
                continue;
               }

            if (!namedSection(fsec, "filter", name))
               {fprintf(stderr, "%s: warning: ignoring unrecognised config "
                        "section '[%s]'\n", argv[0], fsec.c_str());
                continue;
               }
            if (name.empty())
               {fprintf(stderr, "%s: a filter section needs a name: "
                        "[filter \"<name>\"]\n", argv[0]);
                return 2;
               }
            // INIReader folds "<section>=<key>" to lower case but keeps the
            // section name verbatim, so two rules whose names differ only in
            // case would silently share one set of values.
            std::string lname = name;
            std::transform(lname.begin(), lname.end(), lname.begin(),
                           [](unsigned char c){return (char)std::tolower(c);});
            if (!ruleNames.insert(lname).second)
               {fprintf(stderr, "%s: duplicate filter rule '%s'\n", argv[0],
                        name.c_str());
                return 2;
               }

            std::size_t r = filter.AddRule(name);
            for (const std::string& key : cfg.KeysIn(fsec))
                {std::string val = cfg.Get(fsec, key, ""), err;
                 bool ok;
                 if      (key == "label") {filter.SetLabel(r, val); continue;}
                 else if (key == "action") ok = filter.SetAction(r, val, err);
                 else    ok = filter.AddCondition(r, key, val, err);
                 if (!ok)
                    {fprintf(stderr, "%s: [filter \"%s\"]: %s\n", argv[0],
                             name.c_str(), err.c_str());
                     return 2;
                    }
                }
           }
       if (!filter.Empty())
          {std::string err;
           if (!filter.Validate(err))
              {fprintf(stderr, "%s: %s\n", argv[0], err.c_str());
               return 2;
              }
          }
      }

// Parse arguments. Short options (-p/-b/-o/-v/-h) and long options are handled
// uniformly by getopt_long; long-only options use synthetic values above the
// byte range. getopt_long reports unknown options / missing arguments itself
// (returning '?'), to which we add the usage text.
//
   enum
   {  OPT_BULK = 256, OPT_OS_URL, OPT_OS_INDEX, OPT_OS_USER, OPT_OS_PASS,
      OPT_OS_TOKEN, OPT_OS_INSECURE, OPT_OS_DATASTREAM,
      OPT_OTLP_URL, OPT_OTLP_TOKEN, OPT_OTLP_INSECURE,
      OPT_CACHE_DIR, OPT_FORWARD, OPT_TCP_PORT, OPT_TCP_TOKEN,
      OPT_SHOVEL, OPT_SHOVEL_TOKEN, OPT_SPOOL_MAX,
      OPT_FLUSH_COUNT,
      OPT_FLUSH_SECS, OPT_RCVBUF, OPT_QUEUE_DEPTH,
      OPT_METRICS_PORT, OPT_MAX_MEMORY, OPT_MAX_ENTRIES,
      OPT_SERVER_TTL, OPT_FILE_TTL, OPT_STATE_FILE, OPT_STATE_TTL,
      OPT_SCITAGS, OPT_SCITAGS_REFRESH, OPT_DATASET, OPT_SITE,
      OPT_NO_RESOLVE,
      OPT_SESSIONS, OPT_SPANS, OPT_TRACES, OPT_GSTREAM, OPT_REDIRECTS, OPT_DEBUG
   };
   static const struct option longOpts[] =
   {  {"config",          required_argument, nullptr, 'c'},
      {"udp-port",        required_argument, nullptr, 'p'},
      {"tcp-port",        required_argument, nullptr, OPT_TCP_PORT},
      {"tcp-token",       required_argument, nullptr, OPT_TCP_TOKEN},
      {"shovel",          required_argument, nullptr, OPT_SHOVEL},
      {"shovel-token",    required_argument, nullptr, OPT_SHOVEL_TOKEN},
      {"spool-max",       required_argument, nullptr, OPT_SPOOL_MAX},
      {"bulk",            required_argument, nullptr, OPT_BULK},
      {"os-url",          required_argument, nullptr, OPT_OS_URL},
      {"os-index",        required_argument, nullptr, OPT_OS_INDEX},
      {"os-user",         required_argument, nullptr, OPT_OS_USER},
      {"os-pass",         required_argument, nullptr, OPT_OS_PASS},
      {"os-token",        required_argument, nullptr, OPT_OS_TOKEN},
      {"os-insecure",     no_argument,       nullptr, OPT_OS_INSECURE},
      {"os-datastream",   no_argument,       nullptr, OPT_OS_DATASTREAM},
      {"otlp-url",        required_argument, nullptr, OPT_OTLP_URL},
      {"otlp-token",      required_argument, nullptr, OPT_OTLP_TOKEN},
      {"otlp-insecure",   no_argument,       nullptr, OPT_OTLP_INSECURE},
      {"cache-dir",       required_argument, nullptr, OPT_CACHE_DIR},
      {"forward",         required_argument, nullptr, OPT_FORWARD},
      {"flush-count",     required_argument, nullptr, OPT_FLUSH_COUNT},
      {"flush-secs",      required_argument, nullptr, OPT_FLUSH_SECS},
      {"rcvbuf",          required_argument, nullptr, OPT_RCVBUF},
      {"queue-depth",     required_argument, nullptr, OPT_QUEUE_DEPTH},
      {"metrics-port",    required_argument, nullptr, OPT_METRICS_PORT},
      {"max-memory",      required_argument, nullptr, OPT_MAX_MEMORY},
      {"max-entries",     required_argument, nullptr, OPT_MAX_ENTRIES},
      {"server-ttl",      required_argument, nullptr, OPT_SERVER_TTL},
      {"file-ttl",        required_argument, nullptr, OPT_FILE_TTL},
      {"state-file",      required_argument, nullptr, OPT_STATE_FILE},
      {"state-ttl",       required_argument, nullptr, OPT_STATE_TTL},
      {"scitags",         required_argument, nullptr, OPT_SCITAGS},
      {"scitags-refresh", required_argument, nullptr, OPT_SCITAGS_REFRESH},
      {"dataset",         required_argument, nullptr, OPT_DATASET},
      {"site",            required_argument, nullptr, OPT_SITE},
      {"no-resolve",      no_argument,       nullptr, OPT_NO_RESOLVE},
      {"sessions",        no_argument,       nullptr, OPT_SESSIONS},
      {"spans",           no_argument,       nullptr, OPT_SPANS},
      {"traces",          no_argument,       nullptr, OPT_TRACES},
      {"gstream",         no_argument,       nullptr, OPT_GSTREAM},
      {"redirects",       no_argument,       nullptr, OPT_REDIRECTS},
      {"debug",           no_argument,       nullptr, OPT_DEBUG},
      {"help",            no_argument,       nullptr, 'h'},
      {nullptr, 0, nullptr, 0}
   };

   int opt;
   while ((opt = getopt_long(argc, argv, "c:p:b:o:vh", longOpts, nullptr)) != -1)
       {switch(opt)
        {case 'c': break;   // config file: already loaded in the pre-scan above
         case 'p': port    = atoi(optarg); break;
         case 'b': bindStr = optarg;       break;
         case 'o': outFile = optarg;       break;
         case 'v': verbose = true;         break;
         case 'h': usage(argv[0]);         return 0;
         case OPT_BULK:          bulkIdx      = optarg;             break;
         case OPT_OS_URL:        osUrl        = optarg;             break;
         case OPT_OS_INDEX:      osIndex      = optarg;             break;
         case OPT_OS_USER:       osUser       = optarg;             break;
         case OPT_OS_PASS:       osPass       = optarg;             break;
         case OPT_OS_TOKEN:      osToken      = optarg;             break;
         case OPT_OS_INSECURE:   osInsecure   = true;               break;
         case OPT_OS_DATASTREAM: osDataStream = true;               break;
         case OPT_OTLP_URL:      otlpUrl      = optarg;             break;
         case OPT_OTLP_TOKEN:    otlpToken    = optarg;             break;
         case OPT_OTLP_INSECURE: otlpInsecure = true;               break;
         case OPT_CACHE_DIR:     cacheDir     = optarg;             break;
         case OPT_TCP_PORT:      tcpPort      = atoi(optarg);       break;
         case OPT_TCP_TOKEN:     tcpToken     = optarg;             break;
         case OPT_SHOVEL:
             {std::string hp = optarg;
              auto c = hp.rfind(':');
              if (c == std::string::npos || c == 0 || c+1 >= hp.size())
                 {fprintf(stderr, "%s: --shovel needs host:port\n", argv[0]);
                  return 2;}
              shovelHost = hp.substr(0, c);
              shovelPort = atoi(hp.c_str() + c + 1);
             }
             break;
         case OPT_SHOVEL_TOKEN:  shovelToken  = optarg;             break;
         case OPT_SPOOL_MAX:     spoolMax     = parseSize(optarg);  break;
         case OPT_FORWARD:
             {std::string hp = optarg;
              auto c = hp.rfind(':');
              if (c == std::string::npos || c == 0 || c+1 >= hp.size())
                 {fprintf(stderr, "%s: --forward needs host:port\n", argv[0]);
                  return 2;}
              fwdHost = hp.substr(0, c);
              fwdPort = atoi(hp.c_str() + c + 1);
             }
             break;
         case OPT_FLUSH_COUNT:   flushCount   = (size_t)atol(optarg); break;
         case OPT_FLUSH_SECS:    flushSecs    = atol(optarg);         break;
         case OPT_RCVBUF:        rcvbuf       = parseSize(optarg);    break;
         case OPT_QUEUE_DEPTH:   queueDepth   = atoi(optarg);         break;
         case OPT_METRICS_PORT:  metricsPort  = atoi(optarg);         break;
         case OPT_MAX_MEMORY:    maxMemory    = parseSize(optarg);    break;
         case OPT_MAX_ENTRIES:   maxEntries   = (size_t)atol(optarg); break;
         case OPT_SERVER_TTL:    serverTtl    = atol(optarg);         break;
         case OPT_FILE_TTL:      fileTtl      = atol(optarg);         break;
         case OPT_STATE_FILE:    stateFile    = optarg;               break;
         case OPT_STATE_TTL:     stateTtl     = parseDuration(optarg); break;
         case OPT_SCITAGS:       scitags      = optarg;               break;
         case OPT_SCITAGS_REFRESH: scitagsRefresh = atol(optarg);     break;
         case OPT_DATASET:       dataset      = optarg;               break;
         case OPT_SITE:          site         = optarg;               break;
         case OPT_NO_RESOLVE:    resolve      = false;               break;
         case OPT_SESSIONS:      sessions     = true;                break;
         case OPT_SPANS:         spans        = true;                break;
         case OPT_TRACES:        traces       = true;                break;
         case OPT_GSTREAM:       gstream      = true;                break;
         case OPT_REDIRECTS:     redirects    = true;                break;
         case OPT_DEBUG:         debug        = true;                break;
         default: usage(argv[0]); return 2;   // '?': getopt already complained
        }
       }
   if (optind < argc)
      {fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], argv[optind]);
       usage(argv[0]); return 2;}

   if ((port <= 0 && tcpPort <= 0) || port > 65535 || tcpPort > 65535)
      {fprintf(stderr, "%s: a valid -p/--udp-port or --tcp-port is required\n",
               argv[0]);
       usage(argv[0]); return 2;}

// --max-memory now caps the whole process, so a value below what the daemon
// occupies before it decodes anything can only ever hold the state budget at
// its floor. Warn rather than refuse: the setting is legal, just futile, and a
// site upgrading from the old correlation-state meaning is exactly who will
// hit this.
//
   if (maxMemory && maxMemory < (128ull << 20))
      fprintf(stderr, "%s: --max-memory %zuM is below this process's own "
              "footprint; it now caps total memory, not just correlation "
              "state, so the state budget will sit at its floor\n",
              argv[0], maxMemory >> 20);

// The site is a property of this collector, not of the servers reporting to it:
// it is not on the monitoring wire at all (all.sitename names the storage
// cluster), and the deployment model is one collector per site. As a global
// label it prepends `site` to every series without touching any call site --
// global labels sit outside the positional schema getOrAddLabeled freezes.
//
// This must run before the first subsystem is created, in either mode, or
// setGlobalLabels() declines the change: every family captures the label set by
// pointer and bakes its rendered prefix once. Hence here, ahead of the shoveler
// branch below.
//
   if (!site.empty() && !collectorRegistry().setGlobalLabels({{"site", site}}))
      {fprintf(stderr, "%s: internal error: --site applied too late to label "
               "the metrics\n", argv[0]);
       return 2;}

// Shoveler mode: relay only. It needs the UDP port to receive on and must not
// itself accept shoveled TCP traffic (no relay chaining — it would make
// forwarding loops too easy to configure by accident). Options that only make
// sense with decoding active are warned about and ignored, so one config file
// can be shared by the shovelers and the central collector.
//
   if (!shovelHost.empty())
      {if (tcpPort > 0)
          {fprintf(stderr, "%s: --shovel cannot be combined with --tcp-port\n",
                   argv[0]);
           return 2;}
       if (port <= 0)
          {fprintf(stderr, "%s: shoveler mode requires -p/--udp-port\n",
                   argv[0]);
           return 2;}
       if (shovelPort <= 0 || shovelPort > 65535)
          {fprintf(stderr, "%s: --shovel needs a valid port\n", argv[0]);
           return 2;}
       std::string secret = loadSecret(shovelToken);
       if (secret.size() > kShovelMaxToken)
          {fprintf(stderr, "%s: --shovel-token exceeds %zu bytes\n", argv[0],
                   kShovelMaxToken);
           return 2;}
       auto ignored = [&](const char* opt)
          {fprintf(stderr, "%s: warning: %s is ignored in shoveler mode\n",
                   argv[0], opt);};
       if (!osUrl.empty())   ignored("--os-url");
       if (!otlpUrl.empty()) ignored("--otlp-url");
       if (fwdPort > 0)      ignored("--forward");
       if (outFile)          ignored("-o");
       if (!bulkIdx.empty()) ignored("--bulk");
       if (!scitags.empty()) ignored("--scitags");
       if (!dataset.empty()) ignored("--dataset");
       if (sessions)         ignored("--sessions");
       if (spans)            ignored("--spans");
       if (traces)           ignored("--traces");
       if (gstream)          ignored("--gstream");
       if (redirects)        ignored("--redirects");
       if (debug)            ignored("--debug");
       if (!filter.Empty())  ignored("a [filter] section");
       if (!osNamed.empty())   ignored("an [opensearch] section");
       if (!otlpNamed.empty()) ignored("an [otlp] section");

       ShovelerOpts so;
       so.udpPort     = port;
       so.bindStr     = bindStr;
       so.host        = shovelHost;
       so.port        = shovelPort;
       so.token       = secret;
       so.cacheDir    = cacheDir;
       so.spoolMax    = spoolMax;
       so.flushCount  = flushCount;
       so.flushSecs   = flushSecs;
       so.rcvbuf      = rcvbuf;
       so.queueDepth  = queueDepth;
       so.maxMemory   = maxMemory;
       so.metricsPort = metricsPort;
       so.verbose     = verbose;
       return runShoveler(argv[0], so);
      }

// Set up the HTTP sinks. Each destination is independent -- its own client,
// queue, thread, retry state and disk cache -- so one that is slow or
// unreachable can neither stall the decoder nor take another destination down
// with it.
//
#ifndef XRDMON_HAVE_CURL
   if (!osUrl.empty() || !osNamed.empty())
      {fprintf(stderr, "%s: an OpenSearch destination requires building with "
               "libcurl\n", argv[0]);
       return 2;}
   if (!otlpUrl.empty() || !otlpNamed.empty())
      {fprintf(stderr, "%s: an OTLP destination requires building with "
               "libcurl\n", argv[0]);
       return 2;}
   const bool osEnabled = false, otlpEnabled = false;
#else
   std::vector<OsDest>   osDests;
   std::vector<OtlpDest> otlpDests;

// The flat options (and their command-line overrides) are the destination
// called "default"; the named sections follow it. A section that also calls
// itself "default" is a collision, not an override -- silently merging two
// endpoint definitions is how a document ends up somewhere nobody expected.
//
   if (!osUrl.empty())
      {OsDest d;
       d.url = osUrl; d.index = osIndex; d.user = osUser; d.pass = osPass;
       d.token = loadSecret(osToken);
       d.insecure = osInsecure; d.dataStream = osDataStream;
       osDests.push_back(std::move(d));
      }
   for (OsDest& d : osNamed)
      {if (d.name == "default" && !osDests.empty())
          {fprintf(stderr, "%s: [opensearch \"default\"] collides with the "
                   "os-url option; name it something else\n", argv[0]);
           return 2;
          }
       d.token = loadSecret(d.token);
       osDests.push_back(d);
      }

   if (!otlpUrl.empty())
      {OtlpDest d;
       d.url = otlpUrl; d.token = loadSecret(otlpToken); d.insecure = otlpInsecure;
       otlpDests.push_back(std::move(d));
      }
   for (OtlpDest& d : otlpNamed)
      {if (d.name == "default" && !otlpDests.empty())
          {fprintf(stderr, "%s: [otlp \"default\"] collides with the otlp-url "
                   "option; name it something else\n", argv[0]);
           return 2;
          }
       d.token = loadSecret(d.token);
       otlpDests.push_back(d);
      }

   const bool osEnabled   = !osDests.empty();
   const bool otlpEnabled = !otlpDests.empty();

// Queue budget. Bodies waiting on their destinations are part of the process
// --max-memory caps, and until now nothing bounded them in bytes at all: the
// old pipe held a fixed count of bodies of any size. Split one budget evenly
// across every queue (an OTLP destination has two, logs and traces) so adding
// a destination cannot silently double the footprint.
//
   const std::size_t nQueues = osDests.size() + 2 * otlpDests.size();
   const std::size_t qBudget = maxMemory ? maxMemory / 32 : (32u << 20);
   const std::size_t qBytes  = nQueues
                             ? std::max<std::size_t>(4u << 20, qBudget / nQueues)
                             : 0;

   std::vector<std::unique_ptr<OsSink>>   osSinks;
   std::vector<std::unique_ptr<OtlpSink>> otlpSinks;

   if (!cacheDir.empty() && (osEnabled || otlpEnabled))
      mkdir(cacheDir.c_str(), 0750);   // parent (Init's mkdir is single-level)

   for (const OsDest& d : osDests)
      {auto s = std::unique_ptr<OsSink>(new OsSink(d, qBytes));
       s->cli = new XrdMonOpenSearch(d.url, d.index, d.user, d.pass, d.token,
                                     d.insecure, d.dataStream);
       std::string e;
       if (!s->cli->Init(e))                  // curl_global_init: main thread
          {fprintf(stderr, "%s: OpenSearch[%s]: %s\n", argv[0],
                   d.name.c_str(), e.c_str()); return 4;}
       if (!cacheDir.empty())
          {s->cache = new XrdMonDiskCache(sinkCacheDir(cacheDir, "os", d.name));
           if (!s->cache->Init(e))
              {fprintf(stderr, "%s: OpenSearch[%s]: %s\n", argv[0],
                       d.name.c_str(), e.c_str()); return 4;}
          }
       osSinks.push_back(std::move(s));
      }

// OTLP/HTTP export (logs -> /v1/logs, spans -> /v1/traces on an OpenTelemetry
// Collector, Grafana Alloy, or any OTLP receiver). Logs and traces get separate
// caches: they replay to different endpoints.
//
   for (const OtlpDest& d : otlpDests)
      {auto s = std::unique_ptr<OtlpSink>(new OtlpSink(d, qBytes));
       s->cli = new XrdMonOtlp(d.url, d.token, d.insecure);
       std::string e;
       if (!s->cli->Init(e))
          {fprintf(stderr, "%s: OTLP[%s]: %s\n", argv[0],
                   d.name.c_str(), e.c_str()); return 4;}
       if (!cacheDir.empty())
          {s->logCache   = new XrdMonDiskCache(
                              sinkCacheDir(cacheDir, "otlp", d.name, "-logs"));
           s->traceCache = new XrdMonDiskCache(
                              sinkCacheDir(cacheDir, "otlp", d.name, "-traces"));
           if (!s->logCache->Init(e) || !s->traceCache->Init(e))
              {fprintf(stderr, "%s: OTLP[%s]: %s\n", argv[0],
                       d.name.c_str(), e.c_str()); return 4;}
          }
       otlpSinks.push_back(std::move(s));
      }
#endif

// Traces are spans, and spans are only produced with --spans. Exporting OTLP
// without it sends logs (which carry a traceId but do not, by themselves, create
// a trace) and nothing to /v1/traces, so Tempo/Grafana show no traces at all.
// Warn rather than fail: a logs-only OTLP export is a legitimate setup.
//
   if (otlpEnabled && !spans)
      fprintf(stderr, "%s: note: OTLP export without --spans emits logs only; "
                      "no spans are sent to /v1/traces (Tempo will show no "
                      "traces)\n", argv[0]);

// The file/stdout sink is the fallback when no network sink is configured.
// -o always enables a file in addition to any OpenSearch/OTLP/forward sink.
//
   bool  fileSink = outFile || (!osEnabled && !otlpEnabled && fwdPort <= 0);
   FILE* out      = stdout;
   if (outFile && !(out = fopen(outFile, "a")))
      {fprintf(stderr, "%s: cannot open '%s': %s\n", argv[0], outFile,
               strerror(errno)); return 4;}

// --bulk only frames the file sink; it has no effect on what the OpenSearch
// sink posts (that is --os-index). Saying so beats leaving an operator to
// wonder why their index name never appeared anywhere.
//
   if (!bulkIdx.empty() && !fileSink)
      fprintf(stderr, "%s: warning: --bulk frames the file sink and no file "
                      "sink is enabled; use --os-index for the OpenSearch "
                      "target index\n", argv[0]);

// Optional NDJSON-over-TCP forwarding sink (to a buffering frontend).
//
   XrdMonForward* fwd = fwdPort > 0 ? new XrdMonForward(fwdHost, fwdPort)
                                    : nullptr;
   size_t fwdDrops = 0;
   time_t fwdWarn  = 0;

// Create and bind the UDP socket (dual-stack by default). With --tcp-port the
// UDP port is optional: a TCP-only collector serves only shoveled packets.
//
   int fd = -1;
   if (port > 0)
      {fd = openUDP(port, bindStr);
       if (fd < 0) return 4;

// Enlarge the kernel UDP receive buffer so bursts (and the brief windows when
// the receiver waits on a full pipeline) are absorbed without the kernel
// dropping datagrams. SO_RCVBUFFORCE bypasses the rmem_max cap when privileged;
// fall back to SO_RCVBUF otherwise. Best-effort: a failure is not fatal.
//
       if (rcvbuf > 0)
          {int want = (int)rcvbuf;
#ifdef SO_RCVBUFFORCE
           if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &want,
                          sizeof(want)) != 0)
#endif
              setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
          }
      }

// Batch state for the OpenSearch sink and a flush helper.
//
#ifdef XRDMON_HAVE_CURL
   // One accumulator per distinct (index, action) pair, not one per
   // destination: two clusters written with the same index share a body, so
   // the documents are framed once however many destinations consume them.
   std::vector<OsGroup> osGroups;
   for (auto& up : osSinks)
      {OsSink* s = up.get();
       auto it = std::find_if(osGroups.begin(), osGroups.end(),
                              [&](const OsGroup& g)
                                 {return g.index  == s->cfg.index
                                      && g.create == s->cfg.dataStream;});
       if (it == osGroups.end())
          {OsGroup g;
           g.index = s->cfg.index; g.create = s->cfg.dataStream;
           osGroups.push_back(std::move(g));
           it = osGroups.end() - 1;
          }
       it->dests.push_back(s);
      }

   // Documents are accumulated into an OTLP batch (grouped by resource) and, at
   // each flush, one logs body and one traces body are produced -- once, and
   // shared by every OTLP destination.
   XrdMonOtlpBatch otlpBatch;
#endif
   // Hand the accumulated bodies to the sink threads (one POST per body, per
   // destination). Never blocks: a queue that is full drops its oldest body
   // rather than letting one destination throttle the decoder.
   //
   // Coalesce while every destination is behind. The bounded pipe used to
   // throttle this loop by blocking the serializer, which had the side effect
   // of batching: a consumer slower than the input received fewer, larger
   // bodies. Without that, it would receive a stream of small ones of which its
   // queue drops all but the last few -- more POSTs, less data delivered. So
   // hold the accumulator until some destination has drained what it already
   // has, and ship regardless after kCoalesceSecs so nothing is held
   // indefinitely (which is also what bounds the accumulator's own size). A
   // destination that is keeping up always has an empty queue, so nothing
   // changes for a healthy collector.
   //
#ifdef XRDMON_HAVE_CURL
   const long kCoalesceSecs = 2;
   // ...and never hold more than a queue slot's worth, whatever the clock says.
   // Time alone bounds nothing: a burst can build tens of megabytes inside two
   // seconds, and this is memory nothing else in the process can reclaim -- it
   // is in neither the correlation state nor the output queues. Both
   // accumulators measure themselves exactly, so this bounds real bytes.
   const std::size_t kCoalesceBytes = qBytes ? qBytes : (4u << 20);
   time_t     otlpLastShip  = time(0);
   for (OsGroup& g : osGroups) g.lastShip = otlpLastShip;

   auto anyIdle = [](const std::vector<OsSink*>& dests)
      {for (OsSink* s : dests)
           if (s->q.bodies() == 0 && !s->busy.load()) return true;
       return dests.empty();
      };
   auto anyOtlpIdle = [&]()
      {for (auto& s : otlpSinks)
           if (s->logs.bodies() == 0 && s->traces.bodies() == 0
           && !s->busy.load()) return true;
       return otlpSinks.empty();
      };
#endif
   auto flush = [&](bool force = false)
      {
#ifdef XRDMON_HAVE_CURL
       const time_t now = time(0);

       for (OsGroup& g : osGroups)
          {if (!g.count) continue;
           if (!force && now - g.lastShip < kCoalesceSecs && !anyIdle(g.dests)
           &&  g.body.size() < kCoalesceBytes)
              continue;
           g.lastShip = now;
           const std::size_t was = g.body.size();
           XrdMonBody body = std::make_shared<const std::string>(std::move(g.body));
           // The move leaves an unspecified string; reset it and re-reserve
           // roughly what it just held, which is what the pipe's buffer
           // recycling used to do. Bodies above kBodyKeepBytes are mmap'd
           // (XrdMonTuneAllocator pins the threshold there) and returned to the
           // OS on free, so there is nothing to gain from keeping them warm.
           g.body = std::string();
           g.body.reserve(std::min(was, kBodyKeepBytes));
           g.count = 0;
           for (OsSink* s : g.dests) s->q.push(body);
          }
       if ((otlpBatch.haveLogs() || otlpBatch.haveTraces())
       &&  (force || now - otlpLastShip >= kCoalesceSecs || anyOtlpIdle()
            || otlpBatch.logBytes()   >= kCoalesceBytes
            || otlpBatch.traceBytes() >= kCoalesceBytes))
          {otlpLastShip = now;
           if (otlpBatch.haveLogs())
              {XrdMonBody b =
                  std::make_shared<const std::string>(otlpBatch.takeLogsBody());
               for (auto& s : otlpSinks) s->logs.push(b);
              }
           if (otlpBatch.haveTraces())
              {XrdMonBody b =
                  std::make_shared<const std::string>(otlpBatch.takeTracesBody());
               for (auto& s : otlpSinks) s->traces.push(b);
              }
          }
#endif
      };

// Wire up the sinks. With --bulk the file sink is written in OpenSearch _bulk
// format; the OpenSearch sink batches documents and posts them via _bulk.
//
// Two sinks, because they want two different things. The file, OpenSearch and
// forward sinks all consume the document as text, so they share one
// serialization. The OTLP sink re-encodes the document, so it takes the tree
// the decoder already built rather than parsing the text back into one. The
// text sink is installed only when a text consumer exists, which is what makes
// an OTLP-only collector serialize nothing per document at all.
//
   const bool wantText = fileSink || osEnabled || fwd != nullptr;

   XrdMonDecode::DocSink docSink;
   if (wantText) docSink =
      [&](const std::string& d)
         {if (fileSink)
             {if (bulkIdx.empty()) fprintf(out, "%s\n", d.c_str());
                 else
                 {// Same framing the network sink uses, so a --bulk file can be
                  // curl'd into a data stream (which rejects "index") as well.
                  std::string line;
                  XrdMonBulkAdd(line, bulkIdx, osDataStream, d);
                  fwrite(line.data(), 1, line.size(), out);
                 }
             }
#ifdef XRDMON_HAVE_CURL
          for (OsGroup& g : osGroups)
              {XrdMonBulkAdd(g.body, g.index, g.create, d); g.count++;}
#endif
          if (fwd)
             {std::string e;
              if (!fwd->Send(d, e))
                 {fwdDrops++;
                  // Rate-limit the warning to at most once every 10 seconds.
                  time_t now = time(0);
                  if (now - fwdWarn >= 10)
                     {fwdWarn = now;
                      fprintf(stderr, "xrdmoncollect: forward dropped %zu doc(s): "
                              "%s\n", fwdDrops, e.c_str());
                     }
                 }
             }
         };

#ifdef XRDMON_HAVE_CURL
   XrdMonDecode::TreeSink treeSink;
   if (otlpEnabled) treeSink = [&](const nlohmann::json& j){otlpBatch.add(j);};
#endif

   XrdMonDecode::RawSink rawSink;
   if (debug) rawSink = [&](const std::string& r){fprintf(out, "%s\n", r.c_str());};

// When a metrics port is given, aggregate transfers into the registry and
// serve it over HTTP. The decoder-level statistics are exposed too.
//
   XrdMetrics::Subsystem* subsystem =
            metricsPort > 0 ? &collectorRegistry().subsystem("collector") : nullptr;

// The receiver thread fills packet batches and hands them, through this bounded
// recycling pipe, to the serializer thread that owns the decoder. A generous
// depth plus the enlarged SO_RCVBUF means the receiver effectively never has to
// wait (which would risk kernel-buffer drops).
//
   XrdMonPipe<Batch> recvPipe((size_t)(queueDepth > 0 ? queueDepth : 1));

// Byte bound on a batch, so a stream of large datagrams cannot fill the queue
// with far more memory than flushCount alone implies. Shared by every producer
// into this pipe.
//
   const size_t batchBytes = recvBatchBytes(maxMemory, queueDepth);

// The TCP listener for shoveled packets produces into the same pipe as the
// UDP receiver (the pipe is mutex-protected on the producer side), so the
// serializer cannot tell the transports apart.
//
   XrdMonTcpServer* tcpSrv = nullptr;
   if (tcpPort > 0)
      {tcpSrv = new XrdMonTcpServer(tcpPort, bindStr, loadSecret(tcpToken),
                                    recvPipe, flushCount, flushSecs, batchBytes);
       int sdFd = sdInheritedSocket(SOCK_STREAM, tcpPort);
       if (sdFd >= 0)
          {fprintf(stderr, "xrdmoncollect: using systemd-provided TCP socket "
                   "on port %d\n", tcpPort);
           tcpSrv->SetInheritedFd(sdFd);
          }
       std::string e;
       if (!tcpSrv->Start(e))
          {fprintf(stderr, "%s: %s\n", argv[0], e.c_str()); return 4;}
      }

   XrdMonDecode decoder(docSink, rawSink, debug, traces, gstream, redirects, subsystem);
#ifdef XRDMON_HAVE_CURL
   if (treeSink) decoder.SetTreeSink(treeSink);
#endif
// --max-memory caps the whole process, and the decoder meets it by steering
// its own state budget from a real RSS reading. Where there is no reader, fall
// back to the old behaviour -- the knob bounds the charged state estimate -- so
// the safeguard degrades rather than disappearing.
//
   if (XrdMonProcessRss())
      {decoder.SetMaxRss(maxMemory);
       decoder.SetMemoryHooks({XrdMonProcessRss, XrdMonReleaseMemory});
      }
      else
      {decoder.SetMaxBytes(maxMemory);
       if (maxMemory)
          fprintf(stderr, "xrdmoncollect: no resident-memory reader on this "
                  "platform; --max-memory bounds the correlation-state "
                  "estimate only\n");
      }
   decoder.SetMaxEntries(maxEntries);
   decoder.SetServerTTL(serverTtl);
   decoder.SetFileTTL(fileTtl);
   decoder.SetResolveHosts(resolve);
   decoder.SetSite(site);
   decoder.SetEmitSessions(sessions);
   decoder.SetEmitSpans(spans);
   if (!filter.Empty())
      {decoder.SetFilter(&filter);
       // Silently dropping documents is exactly the kind of thing an operator
       // must see confirmed at start-up, so this is not gated on --verbose.
       fprintf(stderr, "xrdmoncollect: %zu filter rule(s) loaded "
               "(%zu tag, %zu drop, %zu keep)\n", filter.Size(),
               filter.Count(XrdMonFilter::Action::Tag),
               filter.Count(XrdMonFilter::Action::Drop),
               filter.Count(XrdMonFilter::Action::Keep));
      }
   if (!dataset.empty() && !decoder.SetDatasetRegex(dataset))
      {fprintf(stderr, "%s: --dataset '%s' is not a valid POSIX extended "
               "regular expression\n", argv[0], dataset.c_str());
       return 2;
      }

// Reload the correlation state saved by the previous incarnation, so a clean
// restart does not open a blind window while dictionaries repopulate. Runs
// before any packet is decoded (the decoder is still single-threaded here).
//
   if (!stateFile.empty())
      {std::string note;
       decoder.LoadState(stateFile, stateTtl, note);
       if (!note.empty())
          fprintf(stderr, "xrdmoncollect: %s\n", note.c_str());

// LoadState holds the stream buffer, its string copy and the parsed json tree
// all at once, so a large snapshot leaves a peak several times the state it
// restored. Give it back now, while this is still single-threaded and free,
// rather than letting it stand as the process's high-water mark for the rest
// of the run -- and before the memory cap takes its first sample.
//
       XrdMonReleaseMemory();
      }
#ifdef XRDMON_HAVE_CURL
   // Every sink's Init() calls curl_global_init; do it here too when a URL
   // registry is the only curl user, before the first (main-thread) fetch and
   // the refresh thread that follows. It has to happen on this thread either
   // way: curl_global_init was not thread-safe before libcurl 7.84.
   if (!scitags.empty() && isUrl(scitags) && !osEnabled && !otlpEnabled)
      curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
   if (!scitags.empty())
      {std::string e;
       if (!loadScitags(decoder, scitags, e))
          fprintf(stderr, "%s: warning: SciTags registry '%s': %s; "
                          "experiment/activity ids stay numeric\n",
                  argv[0], scitags.c_str(), e.c_str());
      }

// For a URL registry, refresh it in the background so a long-running collector
// tracks changes in the published registry. The swap is mutex-guarded against
// the decode loop; a failed re-fetch keeps the current registry.
//
   std::atomic<bool> scitagsStop{false};
   std::thread       scitagsThread;
   if (!scitags.empty() && isUrl(scitags) && scitagsRefresh > 0)
      scitagsThread = std::thread([&]()
         {while (!scitagsStop)
             {for (long s = 0; s < scitagsRefresh && !scitagsStop; s++) sleep(1);
              if (scitagsStop) break;
              std::string e;
              if (!loadScitags(decoder, scitags, e))
                 fprintf(stderr, "xrdmoncollect: SciTags refresh failed: %s\n",
                         e.c_str());
             }
         });

   std::atomic<bool> exporterStop{false};
   std::thread       exporter;
   if (subsystem)
      {auto& s = decoder.GetStats();
#define OBS(name, help, fld) \
       subsystem->observeCounter<std::uint64_t>(name, help).add({}, [&]{return (uint64_t)s.fld;})
       // packets_total, malformed_total, orphan_closes_total, stale_opens_total
       // and documents_total are emitted by the decoder itself, labeled by
       // {site, ...}; only the unlabeled aggregates live here. Registering a
       // name through both paths does not merge them — observeCounter bypasses
       // the registry's byName_ dedup — so it would put two families of the
       // same name in the exposition, which strict OpenMetrics parsers reject.
       OBS("unknown_packets_total",
           "packets with an unhandled stream code", unknown);
       OBS("invalid_utf8_total",
           "wire strings repaired because their bytes were not valid UTF-8",
           badUtf8);
       OBS("evicted_total",
           "dictionary/open-file entries evicted by the memory budget", evicted);
       OBS("reaped_servers_total",
           "idle server incarnations reclaimed by the server TTL", reaped);
       OBS("memory_floored_total",
           "control ticks over the memory cap with the state budget already "
           "at its floor", memFloored);
       OBS("filtered_documents_total",
           "documents suppressed before emission by a [filter] rule", filtered);
       OBS("disconnects_total",
           "f-stream session disconnect records", discs);
       OBS("trace_records_total", "t-stream records decoded", traces);
       OBS("gstream_records_total", "g-stream records decoded", gevents);
       OBS("redirect_records_total",
           "r-stream redirect records decoded", redirs);
       OBS("frm_records_total",
           "x/p FRM stage/purge records decoded", frmEvents);
       OBS("token_records_total",
           "T-stream token records decoded", mapTokn);
       OBS("ident_records_total",
           "=-stream server-identity records decoded", mapIdnt);
#undef OBS
       subsystem->observeGauge<std::int64_t>("state_entries", "correlation entries held (dictionaries, tokens, activity, open files)")
          .add({}, [&]{return (int64_t)decoder.StateEntries();});
       subsystem->observeGauge<std::int64_t>("state_budget_bytes", "charged-byte budget the memory cap is currently enforcing")
          .add({}, [&]{return (int64_t)decoder.MaxBytes();});
       subsystem->observeGauge<std::int64_t>("recv_queue_batches", "packet batches queued from the receiver to the serializer")
          .add({}, [&]{return (int64_t)recvPipe.readyDepth();});
       if (tcpSrv)
          {subsystem->observeCounter<std::uint64_t>("tcp_connections_total", "shovel TCP connections accepted")
              .add({}, [&]{return tcpSrv->Connections();});
           subsystem->observeGauge<std::int64_t>("tcp_connections_active", "shovel TCP connections currently open")
              .add({}, [&]{return tcpSrv->Active();});
           subsystem->observeCounter<std::uint64_t>("tcp_auth_failures_total", "shovel TCP connections rejected by the token check")
              .add({}, [&]{return tcpSrv->AuthFailures();});
           subsystem->observeCounter<std::uint64_t>("tcp_frames_total", "shoveled datagrams received over TCP")
              .add({}, [&]{return tcpSrv->Frames();});
           subsystem->observeCounter<std::uint64_t>("tcp_malformed_total", "shovel TCP connections dropped for protocol violations")
              .add({}, [&]{return tcpSrv->Malformed();});
           subsystem->observeCounter<std::uint64_t>("tcp_bytes_total", "bytes received on shovel TCP connections")
              .add({}, [&]{return tcpSrv->BytesIn();});
          }
#ifdef XRDMON_HAVE_CURL
    // Every sink series carries the destination it belongs to. Each family is
    // created once and gains one series per destination -- an observed family
    // bypasses the registry's name deduplication, so creating it inside the
    // loop would emit as many families of one name as there are destinations.
    // All of it stays here, after subsystem("collector"), because
    // dashboards_test.py attributes a registration to the nearest preceding
    // subsystem() literal.
    //
       {static const std::vector<std::string> kDest{"destination"};
        auto& postQ = subsystem->observeGauge<std::int64_t>("post_queue_bodies", "serialized _bulk bodies queued for the OpenSearch POST", {}, kDest);
        auto& postQB = subsystem->observeGauge<std::int64_t>("post_queue_bytes", "bytes of serialized _bulk bodies queued for the OpenSearch POST", {}, kDest);
        auto& postOver = subsystem->observeCounter<std::uint64_t>("post_overflow_total", "_bulk bodies dropped because the destination queue was full", {}, kDest);
        auto& postFail = subsystem->counterFamily<std::uint64_t>("post_failures_total", "OpenSearch _bulk POSTs that failed after retries", kDest);
        auto& postDrop = subsystem->counterFamily<std::uint64_t>("dropped_bulk_total", "_bulk bodies dropped after a failed POST (no/failed disk cache)", kDest);
        for (auto& up : osSinks)
           {OsSink* s = up.get();
            postQ.add({s->cfg.name},  [s]{return (int64_t)s->q.bodies();});
            postQB.add({s->cfg.name}, [s]{return (int64_t)s->q.bytes();});
            postOver.add({s->cfg.name}, [s]{return s->q.overflowed();});
            s->failures = &postFail.labels({s->cfg.name});
            s->dropped  = &postDrop.labels({s->cfg.name});
           }

        bool anyOsCache = false;
        for (auto& up : osSinks) anyOsCache = anyOsCache || up->cache;
        if (anyOsCache)
           {auto& cf = subsystem->observeGauge<std::int64_t>("cache_files", "cached _bulk bodies awaiting replay", {}, kDest);
            auto& cb = subsystem->observeGauge<std::int64_t>("cache_bytes", "bytes of cached _bulk bodies on disk", {}, kDest);
            auto& cs = subsystem->observeCounter<std::uint64_t>("cache_stored_total", "_bulk bodies written to the disk cache", {}, kDest);
            auto& cr = subsystem->observeCounter<std::uint64_t>("cache_replayed_total", "cached _bulk bodies successfully replayed", {}, kDest);
            auto& cj = subsystem->observeCounter<std::uint64_t>("cache_rejected_total", "_bulk bodies permanently refused by the cluster and quarantined as .rejected files", {}, kDest);
            for (auto& up : osSinks)
               {OsSink* s = up.get();
                if (!s->cache) continue;
                cf.add({s->cfg.name}, [s]{return (int64_t)s->cache->Files();});
                cb.add({s->cfg.name}, [s]{return (int64_t)s->cache->Bytes();});
                cs.add({s->cfg.name}, [s]{return (uint64_t)s->cache->Stored();});
                cr.add({s->cfg.name}, [s]{return (uint64_t)s->cache->Replayed();});
                cj.add({s->cfg.name}, [s]{return (uint64_t)s->cache->Rejected();});
               }
           }

        auto& otQ = subsystem->observeGauge<std::int64_t>("otlp_queue_bodies", "OTLP bodies queued for the export POST (logs + traces)", {}, kDest);
        auto& otQB = subsystem->observeGauge<std::int64_t>("otlp_queue_bytes", "bytes of OTLP bodies queued for the export POST", {}, kDest);
        auto& otOver = subsystem->observeCounter<std::uint64_t>("otlp_overflow_total", "OTLP bodies dropped because the destination queue was full", {}, kDest);
        auto& otFail = subsystem->counterFamily<std::uint64_t>("otlp_failures_total", "OTLP POSTs that failed after retries", kDest);
        auto& otDrop = subsystem->counterFamily<std::uint64_t>("otlp_dropped_total", "OTLP bodies dropped after a failed POST (no/failed disk cache)", kDest);
        for (auto& up : otlpSinks)
           {OtlpSink* s = up.get();
            otQ.add({s->cfg.name},  [s]{return (int64_t)(s->logs.bodies() + s->traces.bodies());});
            otQB.add({s->cfg.name}, [s]{return (int64_t)(s->logs.bytes()  + s->traces.bytes());});
            otOver.add({s->cfg.name}, [s]{return s->logs.overflowed() + s->traces.overflowed();});
            s->failures = &otFail.labels({s->cfg.name});
            s->dropped  = &otDrop.labels({s->cfg.name});
           }

        bool anyOtlpCache = false;
        for (auto& up : otlpSinks) anyOtlpCache = anyOtlpCache || up->logCache;
        if (anyOtlpCache)
           {auto& cf = subsystem->observeGauge<std::int64_t>("otlp_cache_files", "cached OTLP bodies awaiting replay (logs + traces)", {}, kDest);
            auto& cb = subsystem->observeGauge<std::int64_t>("otlp_cache_bytes", "bytes of cached OTLP bodies on disk", {}, kDest);
            auto& cs = subsystem->observeCounter<std::uint64_t>("otlp_cache_stored_total", "OTLP bodies written to the disk cache", {}, kDest);
            auto& cr = subsystem->observeCounter<std::uint64_t>("otlp_cache_replayed_total", "cached OTLP bodies successfully replayed", {}, kDest);
            auto& cj = subsystem->observeCounter<std::uint64_t>("otlp_cache_rejected_total", "OTLP bodies permanently refused by the endpoint and quarantined as .rejected files", {}, kDest);
            for (auto& up : otlpSinks)
               {OtlpSink* s = up.get();
                if (!s->logCache) continue;
                cf.add({s->cfg.name}, [s]{return (int64_t)(s->logCache->Files() + s->traceCache->Files());});
                cb.add({s->cfg.name}, [s]{return (int64_t)(s->logCache->Bytes() + s->traceCache->Bytes());});
                cs.add({s->cfg.name}, [s]{return (uint64_t)(s->logCache->Stored() + s->traceCache->Stored());});
                cr.add({s->cfg.name}, [s]{return (uint64_t)(s->logCache->Replayed() + s->traceCache->Replayed());});
                cj.add({s->cfg.name}, [s]{return (uint64_t)(s->logCache->Rejected() + s->traceCache->Rejected());});
               }
           }

        // Held by the accumulator between flushes, so it belongs to no
        // destination and appears on no queue -- the one part of the output
        // path that used to be invisible. Now bounded, and released outright
        // when the memory cap says so, but worth watching: a collector that
        // sits high here is one whose endpoints are all behind at once.
        if (otlpEnabled)
           subsystem->observeGauge<std::int64_t>("otlp_batch_bytes",
                 "bytes of OTLP output accumulated but not yet queued")
              .add({}, [&]{return (int64_t)(otlpBatch.logBytes()
                                          + otlpBatch.traceBytes());});
       }
#endif
       registerProcessMetrics();  // must follow every subsystem-> call above
       exporter = std::thread(serveMetrics, metricsPort, std::ref(exporterStop));
      }

// Install signal handlers without SA_RESTART so a signal interrupts the blocking
// recvfrom() on the receiver (main) thread; the loop then sees stopFlag, and the
// 1s SO_RCVTIMEO below bounds shutdown latency regardless of which thread caught
// the signal.
//
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_handler = onSignal;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;
   sigaction(SIGINT,  &sa, nullptr);
   sigaction(SIGTERM, &sa, nullptr);

   if (fd >= 0)
      {struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
       setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      }

// One thread per destination: it performs the (blocking) POST off the
// serializer thread, so a slow or unreachable endpoint holds up neither
// decoding nor reception -- and, with several destinations, holds up none of
// the others either. Each owns its client, its queue, its retry state and its
// disk cache; nothing is shared but the bodies, which are const.
//
#ifdef XRDMON_HAVE_CURL
   const int kDrainPerIter = 64;   // cap cache replays per wake-up

   for (auto& up : osSinks)
      {OsSink* s = up.get();
       s->th = std::thread([s, kDrainPerIter]()
          {
           // POST one body. Drives both live bodies and cache replays, so a
           // failure counts once regardless of the source. A transient failure
           // is rate-limited to one warning per 10s (a down cluster would log
           // per body); a permanent rejection is always logged in full -- it
           // names the body being lost and the cluster's reason, and it does
           // not recur per retry cycle because the body leaves the replay path.
           auto post = [s](const std::string& b, int retries)
              {s->cli->SetMaxRetry(retries);
               std::string e;
               XrdMonOpenSearch::PostResult r = s->cli->Bulk(b, e);
               if (r != XrdMonOpenSearch::PostResult::Ok)
                  {if (s->failures) ++*s->failures;
                   time_t now = time(0);
                   if (r == XrdMonOpenSearch::PostResult::Rejected
                       || now - s->warnAt >= 10)
                      {s->warnAt = now;
                       fprintf(stderr, "xrdmoncollect: OpenSearch[%s] bulk post "
                               "%s: %s\n", s->cfg.name.c_str(),
                               r == XrdMonOpenSearch::PostResult::Rejected
                                  ? "rejected by endpoint" : "failed",
                               e.c_str());}
                  }
                  else if (!e.empty())
                     fprintf(stderr, "xrdmoncollect: OpenSearch[%s]: %s\n",
                             s->cfg.name.c_str(), e.c_str());
               return r;
              };
           // Replay up to kDrainPerIter cached bodies, oldest first, stopping
           // as soon as the sink is down again. The replay keeps the full retry
           // ladder, whose backoff is also what paces this loop. A rejected
           // body is quarantined by the cache (counted as progress), so a
           // poison body cannot pin the backlog behind it.
           auto drain = [&]()
              {if (!s->cache) return;
               std::string e;
               for (int i = 0; i < kDrainPerIter; i++)
                  {int r = s->cache->ReplayOldest(
                              [&](const std::string& b)
                                 {return verdictOf(post(b, kSinkRetries));}, e);
                   if (!e.empty())
                      {fprintf(stderr, "xrdmoncollect: OpenSearch[%s]: %s\n",
                               s->cfg.name.c_str(), e.c_str()); e.clear();}
                   if (r != 1) break;      // 0 = empty, -1 = sink still down
                  }
              };

           // Queue overflow is silent apart from its counter, and it is data
           // loss, so say so at least once. Rate-limited, and reported by the
           // sink thread rather than the producer so the serializer stays lean.
           std::uint64_t seenOver = 0;
           time_t        overWarn = 0;
           auto noteOverflow = [&]()
              {std::uint64_t n = s->q.overflowed();
               if (n == seenOver) return;
               time_t now = time(0);
               if (now - overWarn >= 30)
                  {overWarn = now;
                   fprintf(stderr, "xrdmoncollect: OpenSearch[%s] dropped %llu "
                           "_bulk bod%s: the destination queue is full\n",
                           s->cfg.name.c_str(),
                           (unsigned long long)(n - seenOver),
                           n - seenOver == 1 ? "y" : "ies");
                   seenOver = n;
                  }
              };

           XrdMonBody body;
           for (;;)
              {noteOverflow();
               if (s->q.takeFor(body, 1000))
                  {s->busy.store(true);
                   std::string e;
                   if (s->cache && !s->cache->Empty())
                      {// Preserve order: queue behind the backlog, then drain.
                       if (!s->cache->Store(*body, e))
                          {if (s->dropped) ++*s->dropped;
                           fprintf(stderr, "xrdmoncollect: OpenSearch[%s] cache "
                                   "store failed: %s\n", s->cfg.name.c_str(),
                                   e.c_str());}
                       drain();
                      }
                      // With a cache to fall back on the live attempt does not
                      // retry: holding this body for the whole ladder while the
                      // queue behind it overflows loses more than it saves.
                      else
                      {switch (post(*body, s->cache ? 0 : kSinkRetries))
                          {case XrdMonOpenSearch::PostResult::Ok:
                                break;
                           case XrdMonOpenSearch::PostResult::Rejected:
                                // Retrying can only fail identically: keep the
                                // body as evidence, never as backlog.
                                if (s->dropped) ++*s->dropped;
                                if (s->cache)
                                   {if (s->cache->Quarantine(*body, e))
                                         fprintf(stderr, "xrdmoncollect: "
                                                 "OpenSearch[%s]: %s\n",
                                                 s->cfg.name.c_str(), e.c_str());
                                    else fprintf(stderr, "xrdmoncollect: "
                                                 "OpenSearch[%s] quarantine "
                                                 "failed: %s\n",
                                                 s->cfg.name.c_str(), e.c_str());
                                   }
                                break;
                           default:      // transient: cache for replay
                                if (s->cache)
                                   {if (!s->cache->Store(*body, e))
                                       {if (s->dropped) ++*s->dropped;
                                        fprintf(stderr, "xrdmoncollect: "
                                                "OpenSearch[%s] cache store "
                                                "failed: %s\n",
                                                s->cfg.name.c_str(), e.c_str());}
                                   }
                                   else if (s->dropped) ++*s->dropped;
                                break;
                          }
                      }
                   body.reset();
                   s->busy.store(false);
                  }
                  else
                  {if (s->q.closedDrained()) break;
                   drain();                 // idle: make progress on the backlog
                  }
              }
           s->done.store(true);
          });
      }

// OTLP destinations: one thread each, draining two queues (logs -> /v1/logs and
// traces -> /v1/traces) that share the endpoint's one curl handle.
//
   for (auto& up : otlpSinks)
      {OtlpSink* s = up.get();
       s->th = std::thread([s, kDrainPerIter]()
          {
           // See the OpenSearch sink above for why a rejection is logged
           // unconditionally where a transient failure is rate-limited.
           auto post = [s](bool traces, const std::string& b, int retries)
              {s->cli->SetMaxRetry(retries);
               std::string e;
               XrdMonOtlp::PostResult r = traces ? s->cli->PostTraces(b, e)
                                                 : s->cli->PostLogs(b, e);
               if (r != XrdMonOtlp::PostResult::Ok)
                  {if (s->failures) ++*s->failures;
                   time_t now = time(0);
                   if (r == XrdMonOtlp::PostResult::Rejected
                       || now - s->warnAt >= 10)
                      {s->warnAt = now;
                       fprintf(stderr, "xrdmoncollect: OTLP[%s] %s post %s: "
                               "%s\n", s->cfg.name.c_str(),
                               traces ? "traces" : "logs",
                               r == XrdMonOtlp::PostResult::Rejected
                                  ? "rejected by endpoint" : "failed",
                               e.c_str());
                      }
                  }
               return r;
              };
           auto drain = [&](XrdMonDiskCache* c, bool traces)
              {if (!c) return;
               std::string e;
               for (int i = 0; i < kDrainPerIter; i++)
                  {int r = c->ReplayOldest([&](const std::string& b)
                              {return verdictOf(post(traces, b, kSinkRetries));}, e);
                   if (!e.empty())
                      {fprintf(stderr, "xrdmoncollect: OTLP[%s]: %s\n",
                               s->cfg.name.c_str(), e.c_str()); e.clear();}
                   if (r != 1) break;      // 0 = empty, -1 = endpoint still down
                  }
              };
           // One body from whichever queue has one; both are polled each wake-up
           // so neither signal can starve the other.
           auto serve = [&](XrdMonSinkQueue& q, XrdMonDiskCache* c, bool traces)
              {XrdMonBody body;
               if (!q.takeFor(body, 500)) return;
               s->busy.store(true);
               std::string e;
               if (c && !c->Empty())
                  {if (!c->Store(*body, e))
                      {if (s->dropped) ++*s->dropped;
                       fprintf(stderr, "xrdmoncollect: OTLP[%s] cache store "
                               "failed: %s\n", s->cfg.name.c_str(), e.c_str());}
                   drain(c, traces);
                  }
                  else
                  {switch (post(traces, *body, c ? 0 : kSinkRetries))
                      {case XrdMonOtlp::PostResult::Ok:
                            break;
                       case XrdMonOtlp::PostResult::Rejected:
                            // Retrying can only fail identically: keep the body
                            // as evidence, never as backlog.
                            if (s->dropped) ++*s->dropped;
                            if (c)
                               {if (c->Quarantine(*body, e))
                                     fprintf(stderr, "xrdmoncollect: OTLP[%s]: "
                                             "%s\n", s->cfg.name.c_str(),
                                             e.c_str());
                                else fprintf(stderr, "xrdmoncollect: OTLP[%s] "
                                             "quarantine failed: %s\n",
                                             s->cfg.name.c_str(), e.c_str());
                               }
                            break;
                       default:         // transient: cache for replay
                            if (c)
                               {if (!c->Store(*body, e))
                                   {if (s->dropped) ++*s->dropped;
                                    fprintf(stderr, "xrdmoncollect: OTLP[%s] "
                                            "cache store failed: %s\n",
                                            s->cfg.name.c_str(), e.c_str());}
                               }
                               else if (s->dropped) ++*s->dropped;
                            break;
                      }
                  }
               s->busy.store(false);
              };

           // See the OpenSearch sink: overflow is data loss and deserves more
           // than a counter nobody is watching yet.
           std::uint64_t seenOver = 0;
           time_t        overWarn = 0;
           auto noteOverflow = [&]()
              {std::uint64_t n = s->logs.overflowed() + s->traces.overflowed();
               if (n == seenOver) return;
               time_t now = time(0);
               if (now - overWarn >= 30)
                  {overWarn = now;
                   fprintf(stderr, "xrdmoncollect: OTLP[%s] dropped %llu "
                           "bod%s: the destination queue is full\n",
                           s->cfg.name.c_str(),
                           (unsigned long long)(n - seenOver),
                           n - seenOver == 1 ? "y" : "ies");
                   seenOver = n;
                  }
              };

           for (;;)
              {noteOverflow();
               serve(s->logs,   s->logCache,   false);
               serve(s->traces, s->traceCache, true);
               if (s->logs.closedDrained() && s->traces.closedDrained()) break;
               if (s->logs.bodies() == 0 && s->traces.bodies() == 0)
                  {drain(s->logCache,   false);   // idle: work on the backlogs
                   drain(s->traceCache, true);
                  }
              }
           s->done.store(true);
          });
      }
#endif

// Serializer thread: owns the decoder exclusively (preserving its single-thread
// contract). It drains packet batches from the receiver, decodes each packet
// (the DocSink writes the file/forward sinks inline and appends to the
// OpenSearch _bulk body), periodically reaps idle incarnations, and hands one
// _bulk body per batch to the output thread.
//
// It waits with a timeout rather than blocking indefinitely so that
// housekeeping runs on the wall clock instead of on packet arrival. Blocking
// meant a collector whose servers had gone quiet never reaped them -- exactly
// the state in which idle incarnations most need reclaiming -- and left a
// residual _bulk body unsent until the next packet. takeFor + closedDrained()
// is the same idiom the two output threads and the shoveler's sender use;
// close() does notify_all(), so shutdown is not delayed by the timeout.
//
   std::thread serializer([&]()
      {time_t     lastReap  = time(0);
       const long reapEvery = 60;   // sweep idle server incarnations once a minute
       const int  idleWakeMs = 1000;
       Batch b;
       for (;;)
          {bool got = recvPipe.takeFor(b, idleWakeMs);
           if (got)
              {for (auto& p : b)
                  decoder.Process(p.src, p.data.data(), (int)p.data.size());
               if (fileSink) fflush(out);
               b.clear();
               recvPipe.recycle(std::move(b));
              }
              else if (recvPipe.closedDrained()) break;
           time_t now = time(0);
           if (now - lastReap >= reapEvery)
              {decoder.ReapServers(now); lastReap = now;}
           decoder.MemoryTick(now);       // self-rate-limiting; see MemoryTick
           // Over the RSS cap, ship what is accumulated rather than holding it
           // for coalescing: it is memory the decoder cannot evict, so this is
           // the only way the cap reaches it. At most once per control tick.
           flush(decoder.TakeMemoryRelease());
          }
       flush(true);                       // hand off anything after the pipe closed
#ifdef XRDMON_HAVE_CURL
       // Let every sink thread drain what it has and exit.
       for (auto& s : osSinks)   s->q.close();
       for (auto& s : otlpSinks) {s->logs.close(); s->traces.close();}
#endif
      });

// Receiver loop (main thread): drain the socket as fast as possible into pooled
// batches and hand them to the serializer. It touches neither the decoder nor
// the sinks, so a slow POST can never stall packet reception; under sustained
// backlog it waits on the pipe (backpressure) rather than dropping, with the
// enlarged SO_RCVBUF absorbing the gap. Without a UDP port (a TCP-only
// collector) the main thread just waits for a signal while the TCP connection
// threads produce.
//
   sdNotify("READY=1");
   if (fd >= 0)
      {Batch   cur;
       recvPipe.acquire(cur);
       time_t  batchStart = time(0);
       size_t  curBytes   = 0;
       char    buff[64*1024];
       while(!stopFlag)
            {sockaddr_storage from;
             socklen_t fromLen = sizeof(from);
             ssize_t n = recvfrom(fd, buff, sizeof(buff), 0,
                                  (sockaddr*)&from, &fromLen);
             if (n < 0)
                {if (errno == EINTR) continue;
                 if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {if (!cur.empty() && time(0)-batchStart >= flushSecs)
                        {recvPipe.submit(std::move(cur));
                         if (!recvPipe.acquire(cur)) break;
                         batchStart = time(0); curBytes = 0;
                        }
                     continue;
                    }
                 perror("recvfrom"); break;
                }

             Packet pkt;
             pkt.src = XrdMonSenderName((sockaddr*)&from, fromLen);
             pkt.data.assign(buff, (size_t)n);
             memcpy(&pkt.addr, &from, fromLen);
             pkt.addrLen = fromLen;
             cur.push_back(std::move(pkt));
             curBytes += (size_t)n;
             if (cur.size() >= flushCount || time(0)-batchStart >= flushSecs
             || (batchBytes && curBytes >= batchBytes))
                {recvPipe.submit(std::move(cur));
                 if (!recvPipe.acquire(cur)) break;
                 batchStart = time(0); curBytes = 0;
                }
            }
       if (!cur.empty()) recvPipe.submit(std::move(cur));
      }
      else
      while (!stopFlag) sleep(1);

// Shutdown: stop the TCP listener first (its connection threads are pipe
// producers, so they must be joined before the pipe closes), then close the
// pipe (the serializer drains the remaining batches, does a final flush, and
// returns) and join.
//
   sdNotify("STOPPING=1");
   if (tcpSrv) tcpSrv->Stop();
   recvPipe.close();
   serializer.join();          // drains the decode pipe, then closes postPipe

// The serializer has stopped, so the correlation state is quiescent: snapshot
// it for the next incarnation to reload.
//
   if (!stateFile.empty())
      {if (decoder.SaveState(stateFile))
          {if (verbose)
              fprintf(stderr, "xrdmoncollect: correlation state saved to %s\n",
                      stateFile.c_str());
          }
          else
          fprintf(stderr, "xrdmoncollect: cannot save correlation state to "
                  "'%s': %s\n", stateFile.c_str(), strerror(errno));
      }
#ifdef XRDMON_HAVE_CURL
   // Every destination drains in parallel, so the wait is the slowest one
   // rather than their sum. Give them a grace period to deliver what they hold
   // and then cut whatever is still in flight: an endpoint that black-holes
   // packets would otherwise hold the process for its whole retry ladder (five
   // 30s timeouts) and be SIGKILLed part way through.
   {const int kSinkGraceSecs = 20;
    for (int i = 0; i < kSinkGraceSecs * 10; i++)
        {bool all = true;
         for (auto& s : osSinks)   all = all && s->done.load();
         for (auto& s : otlpSinks) all = all && s->done.load();
         if (all) break;
         usleep(100000);
        }
    for (auto& s : osSinks)
        if (!s->done.load())
           {fprintf(stderr, "xrdmoncollect: OpenSearch[%s] did not finish in "
                    "%ds; abandoning the POST in flight\n",
                    s->cfg.name.c_str(), kSinkGraceSecs);
            s->cli->Cancel();
           }
    for (auto& s : otlpSinks)
        if (!s->done.load())
           {fprintf(stderr, "xrdmoncollect: OTLP[%s] did not finish in %ds; "
                    "abandoning the POST in flight\n",
                    s->cfg.name.c_str(), kSinkGraceSecs);
            s->cli->Cancel();
           }
   }
   for (auto& s : osSinks)   if (s->th.joinable()) s->th.join();
   for (auto& s : otlpSinks) if (s->th.joinable()) s->th.join();
#endif

   if (exporter.joinable()) {exporterStop = true; exporter.join();}
   if (scitagsThread.joinable()) {scitagsStop = true; scitagsThread.join();}

   if (verbose)
      {const XrdMonDecode::Stats& s = decoder.GetStats();
       fprintf(stderr,
         "xrdmoncollect: packets=%llu malformed=%llu records=%llu "
         "mapUser=%llu mapTokn=%llu mapUeac=%llu mapIdnt=%llu "
         "opens=%llu closes=%llu xfrs=%llu discs=%llu docs=%llu "
         "filtered=%llu orphanCloses=%llu lost=%llu evicted=%llu "
         "traces=%llu gevents=%llu redirs=%llu spans=%llu frm=%llu "
         "memFloored=%llu unknown=%llu\n",
         (unsigned long long)s.packets, (unsigned long long)s.malformed,
         (unsigned long long)s.records, (unsigned long long)s.mapUser,
         (unsigned long long)s.mapTokn, (unsigned long long)s.mapUeac,
         (unsigned long long)s.mapIdnt,
         (unsigned long long)s.opens, (unsigned long long)s.closes,
         (unsigned long long)s.xfrs, (unsigned long long)s.discs,
         (unsigned long long)s.docs, (unsigned long long)s.filtered,
         (unsigned long long)s.orphanCls,
         (unsigned long long)s.lost, (unsigned long long)s.evicted,
         (unsigned long long)s.traces, (unsigned long long)s.gevents,
         (unsigned long long)s.redirs, (unsigned long long)s.spans,
         (unsigned long long)s.frmEvents, (unsigned long long)s.memFloored,
         (unsigned long long)s.unknown);
      }

   if (out != stdout) fclose(out);
   if (fd >= 0) close(fd);
   delete tcpSrv;
   delete fwd;
#ifdef XRDMON_HAVE_CURL
   osSinks.clear();      // each sink owns its client and its caches
   otlpSinks.clear();
#endif
   return 0;
}

/******************************************************************************/
/*              r e g i s t e r P r o c e s s M e t r i c s                   */
/******************************************************************************/

// Deliberately the last thing in this file. See the forward declaration near
// collectorRegistry() for why: dashboards_test.py resolves a metric's
// subsystem by scanning backwards for the nearest subsystem("...") literal,
// so anything registered below this point would be labelled "process".
//
// Called once from each daemon mode, before any thread starts. The guard is
// not defensive tidiness: observeGauge bypasses the registry's byName_ dedup,
// so a second call would put two families of the same name in the exposition,
// which strict OpenMetrics parsers reject.
//
// The anonymous namespace is reopened because the declaration sits inside the
// one that closes above main().
//
namespace
{
void registerProcessMetrics()
{
   static bool done = false;
   if (done) return;
   done = true;

// Registered only where there is a reader, so a platform without one shows no
// series at all rather than a flat and very convincing zero.
//
   if (!XrdMonProcessRss()) return;

   collectorRegistry().subsystem("process")
      .observeGauge<std::int64_t>("resident_memory_bytes",
                                  "resident set size of the collector process")
      .add({}, []{return (int64_t)XrdMonProcessRss();});
}
}
