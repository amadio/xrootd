# xrdmoncollect

`xrdmoncollect` reads XRootD detailed-monitoring UDP packets (the
`xrootd.monitor` streams), correlates the **`f` (file-stats) stream** against
the user dictionary, and writes **one JSON document per completed transfer**
(file close). The output is line-delimited JSON (NDJSON), the OpenSearch
`_bulk` format, or OTLP/JSON, suitable for ingestion into OpenSearch /
Elasticsearch, an OpenTelemetry collector, Loki, and so on.

This is the document-oriented half of the XRootD monitoring story. A companion
aggregate sink exposes bounded-cardinality Prometheus metrics over the same
decoded stream (see [Aggregated metrics](#aggregated-metrics-prometheus)); the
native server-side metrics live in the `XrdMetrics` component.

## Architecture

`xrdmoncollect` is a bounded, three-stage pipeline. The design goal is that a
slow or unreachable downstream sink never costs UDP monitoring packets: the
socket-draining stage is decoupled from decoding, and decoding is decoupled from
the (blocking) network POSTs by bounded, recycling hand-off queues.

```
 UDP :port
    │
    ▼
┌────────────────┐  recvPipe   ┌─────────────────────────┐
│ Receiver       │ ──────────▶ │ Serializer              │
│ (main thread)  │  (batches)  │ decode + correlate      │
└────────────────┘             │ (owns XrdMonDecode)     │
                               └───────────┬─────────────┘
                                 docSink fan-out
              ┌──────────────┬──────────────┴───────┬──────────────────┐
              ▼              ▼                       ▼                  ▼
        file / stdout   TCP forward           _bulk batch         OTLP batch
                                                   │ postPipe        │ otlpPipe
                                                   ▼                 ▼
                                          ┌──────────────┐   ┌──────────────┐
                                          │ OS output    │   │ OTLP output  │
                                          │ thread (POST)│   │ thread (POST)│
                                          └──────┬───────┘   └──────┬───────┘
                                            on failure           on failure
                                                 ▼                    ▼
                                            --cache-dir  ◀── replay oldest-first
```

### Pipeline stages

1. **Receiver** — the main thread. It does nothing but drain the UDP socket into
   pooled packet batches and hand them to the serializer through a bounded
   recycling queue. The kernel receive buffer is enlarged (`--rcvbuf`, default
   16M) and the queue is generously sized (`--queue-depth`, default 64). To
   prioritise *not losing packets* the receiver applies **backpressure** — it
   waits for a free batch rather than dropping — and combined with the large
   socket buffer it effectively never has to wait.
2. **Serializer** — a single thread that owns the `XrdMonDecode` instance
   exclusively. It decodes and correlates every packet, writes the file and TCP
   forward sinks inline, and accumulates one OpenSearch `_bulk` body and/or one
   OTLP batch per flush window (`--flush-count` packets or `--flush-secs`
   seconds, whichever comes first). Because it is the sole decoder, correlation
   state needs no locking.
3. **Output** — one dedicated thread per HTTP sink. Each performs the blocking
   POST (with retry) so that neither decoding nor reception ever waits on the
   network.

Work is handed between stages by `XrdMonPipe<T>` (`XrdMonPipe.hh`), a
single-producer/single-consumer bounded queue with buffer recycling
(`acquire`/`submit` on the producer side, `take`/`takeFor` on the consumer side,
`recycle` to return an emptied buffer). The receive queue holds `--queue-depth`
packet batches; the two POST queues hold `kPostQueueDepth` (16) bodies each. The
file and TCP-forward sinks are written synchronously in the serializer, so a
document they cannot absorb is **dropped and counted** (e.g. `fwd` drops while
the forward consumer is down); the HTTP sinks instead **cache-or-drop** (below).

### Correlation state

Decode state is kept **per server incarnation**, keyed by the sender address
plus the server start-of-day time (`src|stod`), so concurrent XRootD versions on
one host and restarts are separate incarnations. Each incarnation holds the
`u`/`d`/`i` (user, path, appinfo) dictionaries, the `T`/`U` (token, SciTags)
maps, and the **open-file table**. A file close is correlated by looking up its
`fileID` in that table to recover the LFN, open size, user, and open timestamp,
then joining the user dictionary to produce **one transfer document** per close
(see `XrdMonDecode.{hh,cc}`, the `Server` struct and `ServerFor`).

State is bounded so it cannot grow without limit when a close or disconnect is
lost (dropped datagram, crash, restart — the server never reuses a dictid within
an incarnation):

- `--max-memory` (default 256M; `0` = unbounded) caps state to an approximate
  byte budget, **LRU-evicting** cold entries first. Recency protects a genuine
  long-running transfer: each in-flight `xfr` snapshot and each reference of a
  session by a close promotes the entry, so a file left open for a day survives
  while memory allows.
- `--max-entries` adds an optional hard entry-count backstop (off by default).
- `--server-ttl` (default 86400s; `0` = never) reclaims whole incarnations idle
  past the TTL, so dead incarnations from restarts and rolling upgrades do not
  accumulate. Reaping the last incarnation of a sender also parks its
  `files_open`, `sessions_open` and `server_info` series at zero and recounts
  `servers{site}`, so a restarted server does not strand a nonzero series.
- A client disconnect sweeps that user's open-file entries whose close was
  never seen (the server reports a session's closes before its `isDisc`, so a
  leftover entry means the close record was lost). Swept entries are counted
  in `xrootd_collector_stale_opens_total{site,server}`.
- `--file-ttl` (default `0` = off) expires open-file entries untouched for the
  given period, covering leaks whose disconnect was also lost. It only applies
  to servers that report in-flight snapshots (`xfr` on `xrootd.monitor
  fstat`), where a live transfer refreshes its entry every interval — so a
  long-running transfer is never mistaken for a leak. Set it to at least 3× the
  server's xfr reporting period (`xfr count × flush interval`).

The consequences of eviction/loss for the *output* (orphan closes, documents
missing a field) are covered under [Limitations](#limitations).

### Serialization

Every document — the per-file close records, `session`,
`server_ident`, `frm`, `redirect`, the `t`-stream traces and `gstream` — shares
one OpenTelemetry-aligned schema: a process-level `resource` object and an
event-level `attributes` object, both keyed by dotted semantic-convention names
(with XRootD/WLCG-specific fields under the `xrootd.*`/`wlcg.*` vendor
namespaces). There is no top-level `type` field: the record kind is the
top-level `eventName` (the OTLP LogRecord EventName field, its semconv home
since the `event.name` attribute was deprecated), duplicated as
`attributes["event.name"]` because Loki only exposes *attributes* as
queryable structured metadata ([grafana/loki#19260](https://github.com/grafana/loki/issues/19260));
the attribute can be dropped once Loki surfaces the field. This one in-memory
shape is then framed differently per wire sink.

The event names the operation, not the stream it came from:

| `eventName` | Record |
|---|---|
| `xrootd.read` / `xrootd.write` | a file close, named by whether it carried write bytes |
| `xrootd.open`, `xrootd.close`, `xrootd.read`, `xrootd.write`, `xrootd.auth`, `xrootd.unknown` | an operation that failed without producing a close, named by the error category |
| `xrootd.redirect` | a redirect (`--redirects`) |
| `xrootd.session` | a client's session rollup (`--sessions`) |
| `xrootd.io.read`, `.write`, `.readv`, `.open`, `.close`, `.disconnect`, `.appid` | per-I/O trace detail (`--traces`) |
| `xrootd.gstream` | a plugin g-stream record (`--gstream`) |
| `xrootd.frm` | a File Residency Manager stage/migrate/purge record |
| `xrootd.server_ident` | a server identity announcement |

The first three families are the *concluded operations*, and they are the only
documents carrying `xrootd.operation.state` — so the presence of that attribute
selects them as a group, which is what a dashboard wants when it does not care
which operation concluded.

#### OpenSearch `_bulk`

The `_bulk` framing (`XrdMonOpenSearch::Add`) emits, per document, an
action/metadata line followed by the source line:

```
{"index":{"_index":"xrootd-transfers"}}
{"resource":{…},"attributes":{"event.name":"xrootd.read",…}}
```

A rolling index uses the `index` action (an upsert); a **data stream** uses the
`create` action instead (`--os-datastream`; data streams reject `index`). Bodies
are POSTed to `<url>/_bulk` with `Content-Type: application/x-ndjson`. A store
that expands dotted field names (such as OpenSearch) indexes each key as a nested
field (`attributes.file.path`, `resource.server.address`); the committed
[`opensearch-template.json`](opensearch-template.json) maps these for that sink.

#### OTLP / JSON

The OTLP encoder (`XrdMonOtlp.cc`) re-encodes the nested `resource`/`attributes`
objects into the strict OTLP `resourceLogs`/`resourceSpans` envelope with typed
`KeyValue` arrays (`toKeyValues` / `toAnyValue`: 64-bit integers as strings per
the proto3-JSON mapping, nested objects/arrays as `stringValue`). Records are
grouped by resource (one group per server incarnation), and classified as
**logs** (the default) or **spans** (documents that carry a `kind`, produced with
`--spans`). Logs POST to `<url>/v1/logs`, spans to `<url>/v1/traces`; the
log/span envelope fields (severity, times, trace/span ids, name/kind/status) pass
through since they are already OTLP-shaped, and the record's event name rides
in the LogRecord `eventName` field with a matching human-readable `body`:

```json
{"resourceLogs":[{
  "resource":{"attributes":[{"key":"service.name","value":{"stringValue":"xrootd"}}, …]},
  "scopeLogs":[{"scope":{"name":"xrdmoncollect"},
    "logRecords":[{"timeUnixNano":"…","severityNumber":9,
      "eventName":"xrootd.read",
      "body":{"stringValue":"xrootd.read"},
      "attributes":[{"key":"file.path","value":{"stringValue":"/store/…"}}, …]}]}]}]}
```

#### Sink comparison (OpenSearch vs OTLP)

Both sinks are fed the **same in-memory document** (`XrdMonDecode` builds one
`resource`/`attributes` object per event; `XrdMonOpenSearch::Add` frames it
verbatim, `XrdMonOtlpBatch::add` re-encodes that identical object). No
monitoring field reaches one sink but not the other — every stream field the
decoder captures (see [WLCG field mapping](#wlcg-field-mapping)) lands in a
single canonical document *before* either sink runs, so the choice of backend
(OpenSearch/Loki/Tempo) never changes which information is available. The
sinks differ only in wire encoding:

| Aspect | OpenSearch `_bulk` | OTLP / JSON |
| :-- | :-- | :-- |
| Field content | The full canonical document | The same fields, re-encoded (equivalent) |
| `resource` / `attributes` | Nested JSON objects with dotted keys | `KeyValue` arrays (`{"key","value"}`) |
| 64-bit integers | JSON numbers | Strings (proto3-JSON `intValue`) |
| Nested objects / mixed arrays (e.g. `xrootd.gstream.data`) | Preserved as nested JSON, queryable | Flattened to a JSON `stringValue` |
| Scalar arrays (e.g. `user.roles`) | JSON array | `arrayValue` of typed values |
| Envelope fields (`timeUnixNano`, `severityNumber`, `traceId`, `spanId`, `eventName`) | Top-level document keys | Native OTLP LogRecord/Span fields |
| `@timestamp` (ISO) / `scope` | Included (ISO convenience copy) | Omitted (redundant with `timeUnixNano`; scope is fixed to `xrdmoncollect`) |
| `eventName` | Document key + `attributes["event.name"]` | LogRecord `eventName` + `body` + `attributes["event.name"]` |
| Grouping | One document per event | Records grouped by resource (server incarnation) |
| Spans (`--spans`) | Emitted as ordinary documents (carry `kind`) | Split out to `/v1/traces` as OTLP spans |
| Endpoint / content-type | `<url>/_bulk`, `application/x-ndjson` | `<url>/v1/logs`+`/v1/traces`, `application/json` |

The one practical asymmetry is queryability of *nested* values: a structured
`xrootd.gstream.data` payload stays a nested object in OpenSearch but becomes a
JSON string under OTLP (a deliberate flat-export choice — the data is retained,
not dropped). All flat `xrootd.*`/`wlcg.*`/semconv fields are equally queryable
in both.

### Durability and offline caching

When an HTTP receiver is offline or returns errors, the output thread first
**retries in place**: transient failures (a network error, HTTP 429, or any 5xx)
are retried up to four times with exponential backoff (1s, 2s, 4s, 8s; capped at
16s). If a body still cannot be delivered:

- With `--cache-dir`, it is **spooled to disk** (`XrdMonDiskCache`): written to a
  `<name>.tmp` file and then atomically renamed to
  `<13-digit-epoch-ms>-<6-digit-seq>.ndjson`. Cached bodies are replayed
  **oldest-first** once the sink recovers, and any files left by a previous run
  are replayed on **startup** (init scans the directory in lexical — i.e.
  chronological — order and discards stale `.tmp` partials). The cache has no
  size limit.
- Without `--cache-dir`, the body is **dropped and counted**.

The OpenSearch `_bulk` bodies live flat under the cache directory; the OTLP logs
and traces cache **separately**, because they replay to different endpoints:

```
<cache-dir>/
├── 1751450432000-000000.ndjson      # OpenSearch _bulk bodies
├── 1751450432500-000001.ndjson
├── otlp-logs/
│   └── 1751450433000-000000.ndjson  # OTLP /v1/logs bodies
└── otlp-traces/
    └── 1751450433200-000000.ndjson  # OTLP /v1/traces bodies
```

Health signals to watch (with `--metrics-port`):
`xrootd_collector_cache_files`/`_bytes` (current backlog),
`xrootd_collector_cache_stored_total`/`_replayed_total`, and
`xrootd_collector_dropped_bulk_total`; the OTLP sink has the analogous
`otlp_cache_*` / `otlp_dropped_total` series.

### Shoveler mode (reliable TCP transport)

UDP is fine on the loopback or a quiet LAN, but when the collector sits far
from the packet sources (a central collector for a large or distributed
cluster), loss on the long-haul UDP leg becomes non-negligible — visible as a
climbing `xrootd_collector_packets_lost_total`. The fix, modeled on the OSG
shoveler, is to keep the UDP hop local and relay the datagrams over TCP: run
one `xrdmoncollect --shovel` next to every daemon and point the daemons'
`xrootd.monitor ... dest` at it; the shoveler encapsulates each datagram
(together with its original source address) and streams it to the central
collector's `--tcp-port`, which re-injects it into the normal pipeline as if
it had arrived by UDP:

```
 node A ─ xrootd/cmsd ─ UDP ─▶ xrdmoncollect --shovel ──┐(TCP, XSHV frames)
 node B ─ xrootd/cmsd ─ UDP ─▶ xrdmoncollect --shovel ──┤
 node C ─ xrootd/cmsd ─ UDP ─▶ xrdmoncollect --shovel ──┤
                                                        ▼
                                central xrdmoncollect --tcp-port [-p ...]
                                  (decode, correlate, sinks as usual)
```

Because every frame carries the datagram's **original source address**, the
central collector attributes packets to the emitting daemon — per-server
incarnation state, `server.address` resolution, and the `pseq`-gap loss
estimation all keep working, and now measure only the (local) UDP leg. In
shoveler mode there is no decoding, correlation, or document sink; the relay
is deliberately cheap. `--shovel` cannot be combined with `--tcp-port` (no
relay chaining), and options that only make sense with decoding active are
warned about and ignored, so one config file can serve a whole cluster.

While the central collector is unreachable, the shoveler spools framed
buffers to disk under `<cache-dir>/shovel/` (files named like the other
caches, with a `.frames` suffix) and replays them **oldest-first, ahead of
new traffic**, on reconnect — the same order-preserving choreography as the
HTTP sinks' caches. The spool is capped by `--spool-max` (default 1G; 0 =
unbounded), evicting the **oldest** buffers when full: during a long outage
fresh data is worth more than day-old data, and replay latency stays bounded.
Without `--cache-dir` the buffers are dropped and counted.

The wire protocol ("XSHV", version 1) is a simple length-prefixed framing;
all integers are network byte order, and any malformed field drops the
connection (the shoveler reconnects and replays from its spool). One hello,
then data frames:

| frame | offset | size | field |
| :-- | --: | --: | :-- |
| hello (once) | 0 | 4 | magic `"XSHV"` |
| | 4 | 1 | protocol version (1) |
| | 5 | 1 | flags (reserved, 0) |
| | 6 | 2 | auth token length T (max 4096) |
| | 8 | T | auth token bytes (may be empty) |
| hello reply | 0 | 1 | `0x01` accepted, `0x00` rejected (then close) |
| data (repeated) | 0 | 1 | frame type: `0x01` = data |
| | 1 | 1 | address family: 4 or 6 |
| | 2 | 2 | original UDP source port |
| | 4 | 4 or 16 | original source address |
| | — | 4 | payload length L (1 ≤ L ≤ 65536) |
| | — | L | one raw UDP monitoring datagram |

The hello carries an optional shared-secret token (`--shovel-token` on the
shoveler, `--tcp-token` on the collector; both accept `@<file>`), compared in
constant time. A rejected hello backs the shoveler off for 30s (it is a
configuration error, not a network blip; network failures retry after 5s).

One caveat: plain TCP has no application-level acknowledgement, so datagrams
already accepted by the kernel's socket buffer when the collector dies
abruptly are lost without being counted as drops (typically one flush window
per outage). The spool covers everything from the moment the failure is
detected.

## Quick start

```sh
# Collect to a file as NDJSON
xrdmoncollect -p 9930 -o /var/log/xrootd/transfers.ndjson -v

# Post directly to an OpenSearch data stream
xrdmoncollect -p 9930 --os-url https://opensearch:9200 \
              --os-index xrootd-transfers --os-datastream \
              --os-user admin --os-pass secret

# Export to an OpenTelemetry collector / Grafana Alloy (logs, plus spans)
xrdmoncollect -p 9930 --otlp-url http://alloy:4318 --spans

# Forward NDJSON to a Logstash/Fluentd TCP input for buffering
xrdmoncollect -p 9930 --forward logstash.example.org:5044

# Shoveler on every cluster node: relay the local UDP stream over TCP
xrdmoncollect -p 9930 --shovel collector.example.org:9931 \
              --shovel-token @/etc/xrootd/shovel.token \
              --cache-dir /var/spool/xrootd/moncollect

# Central collector for the shovelers (TCP only; add -p to also take UDP)
xrdmoncollect --tcp-port 9931 --tcp-token @/etc/xrootd/shovel.token \
              --os-url https://opensearch:9200 --os-datastream
```

## Streams and documents

By default the `f` (file-stats) stream produces a per-close document on each
file close and maintains the `xrootd_collector_files_open{site,server}` gauge
(from the `isXfr` snapshots and open/close records). A
file close is reported with `attributes["event.name"]` naming the direction:
`xrootd.read` when the close carried no write bytes, `xrootd.write` when it did.
That is the same distinction as `xrootd.operation.name`, which the document also
carries for consumers that key on attributes only.

XRootD serves both whole-file copies and partial or random data access, and the
close says which without the collector having to decide: `file.size` (the size
captured at open) sits next to `xrootd.read_bytes`, `xrootd.readv_bytes` and
`xrootd.write_bytes`, so a consumer applies whatever coverage rule it means —

```
read_bytes + readv_bytes >= file.size      # the whole file was read
```

— rather than inheriting one baked in here. `xrootd.open_seen` says whether the
open was joined at all, which is what decides whether `file.size` is available,
and `xrootd.forced_close` says whether the client concluded the operation or was
disconnected mid-way. Operations are counted in
`xrootd_collector_io_total{site,server,operation}` and the bytes they moved in
`xrootd_collector_io_bytes_total{site,server,operation}`.

### Streams

Several opt-in streams add finer-grained events:

- `--sessions` enables per-session activity correlation: every file close that
  named the user is folded into a per-session rollup, and a `session` document
  (`attributes["event.name"]` = `xrootd.session`) is emitted on each client
  disconnect (`isDisc`). The session `attributes` carry running totals
  (`xrootd.session.files`, `.reads`, `.writes`, `.read_bytes`, `.readv_bytes`,
  `.write_bytes`, `.errors`, `.start_time`/`.end_time`/`.duration` — see
  [Session times](#session-times) below; `read_bytes` and `readv_bytes` are kept
  apart so vectored access stays distinguishable from sequential) and a capped
  `xrootd.session.recent_files` list (the most recent closed files, each with
  `file.path`, `xrootd.operation.name`, `xrootd.bytes`). The
  totals cover every closed file; only the `recent_files` list is bounded, so a
  long session (a batch job opening many files in a dataset) stays
  memory-bounded. A client that hits an error and disconnects therefore yields
  one document with as much of its activity as the server reported. **Off by
  default** — when disabled no rollup is accumulated and no `session` document is
  produced, saving the per-session memory and receive-thread work for
  deployments that only consume the per-file documents.

  <a id="session-times"></a>
  **Session times.** `xrootd.session.start_time`, `.end_time` and `.duration`
  are **always present** on a session document, and the duration is never
  negative. The end is the disconnect. The start is harder: the wire carries no
  time on any dictionary record, so the login can only be timed by the
  collector, whose clock is not the reporting server's. The collector therefore
  estimates the offset between the two per server incarnation (from the window
  ends the `f` stream already carries) and resolves the start from the best
  evidence available, reporting which in
  `xrootd.session.start_time_source`:

  | `start_time_source` | Derived from | Accuracy |
  | :-- | :-- | :-- |
  | `login` | the `t`-stream disconnect's connect duration, subtracted from its time | exact; the server measured it |
  | `connect` | the `u` login record's arrival, translated into the server's clock | true login, within the receive batching interval |
  | `first_activity` | the earliest record naming the session (open, transfer snapshot, error, close) | exact, but misses the login and authentication |
  | `disconnect` | the disconnect itself | none: the session is reported as an instant |

  Candidates are admitted only within the session's own
  `[incarnation start, disconnect]` range, so a badly skewed clock degrades the
  start to a later rung rather than producing a wrong or missing one. The
  companion counter `xrootd_collector_session_starts_total{site,server,source}`
  makes the mix visible: a server reporting mostly `disconnect` is losing `u`
  records, or its `xrootd.monitor` destination lacks the `user` flag.
- `--spans` additionally emits an OpenTelemetry **span** document alongside each
  concluded-operation log: a file-operation span per close or failed operation
  (spanning open → close, with `status` `STATUS_CODE_OK`/`STATUS_CODE_ERROR`) and,
  with `--sessions`, a session span (the trace root) per disconnect. Every log
  already carries a deterministic `traceId`/`spanId` (the trace keyed by the
  client session `src|stod|user`, the span by the file id), so the span document
  simply re-frames the same identity with the OTLP span fields (`name`, `kind`,
  `startTimeUnixNano`/`endTimeUnixNano`, `status`, `parentSpanId`) for a tracing
  backend. The same session identity is also emitted as the semconv
  `session.id` attribute, so log stores without a tracing UI (Loki structured
  metadata, OpenSearch) can group a session's documents directly.
  **Off by default**; like the logs it can be high volume. **Spans are what
  create traces**: without `--spans` nothing is written to `/v1/traces`, so a
  logs-only export — however many `traceId`s the logs carry — shows no traces in
  Tempo/Grafana (logs correlate *to* traces, they do not create them).
- `--traces` turns each `t` (I/O trace) record into a document
  (`attributes["event.name"]` = `xrootd.io.read`/`xrootd.io.write` with
  `xrootd.io.offset`, `xrootd.io.length` and the resolved `file.path`, plus
  `xrootd.io.readv`, `xrootd.io.open`, `xrootd.io.close`,
  `xrootd.io.disconnect`, and `xrootd.io.appid`; the `xrootd.io.` prefix keeps
  the detail stream apart from the per-file documents). This
  is **high volume** (one record per I/O) — enable only when the detail is
  needed. Requires `io` in the server's monitor `dest` list and the path
  dictionary (`d` stream) to resolve file names. Every record carries the client
  session `traceId`. A true I/O op (`read`/`write`/`readv`) gets its **own**
  `spanId` and, with `--spans`, a companion child span parented on the file's
  transfer span — so the trace waterfall reads **session → file → I/O**. File
  markers (`open`/`close`) instead carry the file's transfer `spanId` (they are
  already represented by that span), and a `disconnect` the session span. The one
  exception is `appid`, which carries no dictionary id and so cannot be
  correlated. The opening user is resolved from the file id, so the file's
  `open` (`f` stream) must have been seen — otherwise the record falls back to
  the session-less trace, exactly as a close without a joined open does.
- `--gstream` forwards each `g` (plugin) record — from the `oss`, `pfc`,
  `throttle`, `tpc`, `http` g-streams — as a document tagged with its provider,
  embedding the plugin's JSON payload. Requires `xrootd.mongstream` on the
  server. Independently of document emission, when `--metrics-port` is set the
  `oss`, `pfc`, `tpc`, `throttle` and `http` providers are also parsed into
  aggregate metrics (see below); the cumulative providers (`oss`, `throttle`
  `io_total`, `http` counts) are converted to counter deltas. `ccm` and
  `tcpmon` are forwarded only.
- The `x` (FRM stage/migrate) and `p` (FRM purge) records are always decoded
  into an `frm` document (`attributes["event.name"]` = `xrootd.frm`;
  `xrootd.operation.name`, `user.name`, `file.path`, and — for purge — `file.size`)
  and counted in `xrootd_collector_frm_total{site,server,op}` /
  `xrootd_collector_frm_purge_bytes_total`. Emitted by a File Residency
  Manager.
- `--redirects` turns each `r` (redirect) record into a concluded-operation
  document: an `attributes["event.name"]` = `xrootd.redirect` report with
  `xrootd.operation.state` `"Redirected"`, the triggering
  `xrootd.operation.name`, the destination
  (`xrootd.redirect.kind`, `xrootd.redirect.target.address`,
  `xrootd.redirect.target.port`), the redirected `file.path`, and the joined
  user/client attributes. A redirect concludes the operation from the
  redirector's point of view (the data server that ultimately serves the file
  emits its own `Successful`/`Failed` close). Emitted mainly by
  redirectors/managers; requires `redir` in the monitor `dest` list. Redirects
  are also counted in `xrootd_collector_redirects_total{site,server,kind}`
  regardless of this flag.

The `u` (user), `d` (path) and `i` (appinfo) dictionaries are always consumed:
they resolve identities and paths for the other streams, and the appinfo (`i`)
is joined to each transfer document by session descriptor (adds `xrootd.app`
when the client set one and it differs from the login `&y=`, which is
`user_agent.original`).

The `=` (server identity), `T` (token) and `U` (user experiment/activity)
records are also always consumed:

- `=` (`MAPIDNT`) yields a one-off `server_ident` document
  (`attributes["event.name"]` = `xrootd.server_ident`) per server incarnation
  (its `resource`: `xrootd.server.site`, `service.instance.id`,
  `xrootd.server.program`, `service.version`, `server.address`,
  `server.port`) and its host/site/instance are joined into every transfer
  document's `resource`. Re-sent identically each `ident` interval; the
  collector emits the document only when it changes.
- `T` (`MAPTOKN`) carries the token identity (subject, VO, role, groups). Keyed
  by the user dictid, it joins onto each document as `user.id`, `wlcg.vo`,
  `user.roles` and `wlcg.groups`. There is no VO metric: the VO is not reliable
  enough across the monitoring stream to aggregate on. `wlcg.vo` comes from the
  token when present, else from the auth CGI `&o=` — but only for methods that
  can actually convey a VO (gsi with VOMS, sss, ztn, http/https); a `&o=` from
  unix/krb5/pwd/host auth is ignored rather than surfacing fake VO values.
  (For SciTokens the `T` record's own `&o=` is the token *issuer*.)
- `U` (`MAPUEAC`) carries the SciTags packet-marking flow labels (experiment
  and activity ids), joined onto transfers as
  `scitags.experiment_id`/`scitags.activity_id`.
  With `--scitags <src>` pointing at a SciTags registry (the scitags.org schema:
  a top-level `"experiments"` array of `{expId, expName, activities:[{activityId,
  activityName}]}`), those numeric ids are additionally mapped to human names —
  `scitags.experiment` and `scitags.activity`. These stand on their own (group
  by them in dashboards); they are deliberately not folded into `wlcg.vo`,
  which carries only genuine VO information. The numeric ids are always
  emitted, so the field is present with or without the registry; a
  missing/unparseable source is warned about at start-up and otherwise ignored.

  `<src>` is either a local file path or an `http(s)://` URL (e.g. the official
  `https://www.scitags.org/api.json`). A URL source is re-fetched in the
  background every `--scitags-refresh` seconds (default 3600; `0` disables) so a
  long-running collector tracks changes in the published registry; the swap is
  atomic with respect to the decode loop, and a failed re-fetch keeps the current
  registry. A URL source requires that the collector was built with libcurl.

### Dataset capture (`--dataset`)

Every document that carries a `file.path` also carries the OTel-semconv
`file.name`, `file.directory` (the path's directory, no trailing slash), and
`file.extension` (the last extension without the leading dot — `gz` for
`.tar.gz`; omitted for dotfiles and extension-less names). For dataset-level
popularity (see [Data popularity dashboards](#data-popularity-dashboards)),
`--dataset <re>` additionally emits `xrootd.dataset`: `<re>` is a POSIX
extended regular expression matched against each file path, and its first
capture group becomes the attribute value. For a CMS-style namespace
(`/store/<class>/<era>/<primary-dataset>/…`):

```sh
xrdmoncollect ... --dataset '^/store/[^/]+/[^/]+/([^/]+)/'
```

Pick the group per namespace (an EOS instance might capture the project
directory, ATLAS the dataset container). The pattern is compiled once at
start-up (an uncompilable one is rejected) and matched once per *emitted*
document on the serializer thread — never in the UDP receive path — so the
cost is negligible at realistic close rates; keep it anchored and simple. A
path the pattern does not match simply gets no `xrootd.dataset`.

### Document filtering (`[filter "…"]`)

A site's monitoring stream usually carries a good deal of activity that is
essential to its administrators but noise to everyone else: on an EOS instance,
FST-to-FST replication, draining, balancing and the namespace's own accesses.
Filter rules let the collector drop that traffic, or merely tag it so a
dashboard can exclude it while the records stay available.

Rules live in the collector's configuration file, one `[filter "<name>"]`
section per rule. There is no command-line equivalent:

```ini
[xrdmoncollect]
port = 9930

# Tag this instance's internal traffic, but keep it.
[filter "eos-internal"]
user     = daemon, root, ~^[0-9]+$
authprot = sss
action   = tag
label    = internal

# Drop the namespace's own bookkeeping accesses outright.
[filter "eos-proc"]
path   = /eos/*/proc/*
action = drop

# ... but never drop anything belonging to a real VO user.
[filter "keep-vo"]
vo     = ~.+
action = keep
```

> **Keys must start in column 1.** The INI parser treats an indented line as a
> continuation of the preceding key's value, so indenting a rule's keys folds
> everything after the first one into that first key. The collector rejects the
> recognisable cases at start-up rather than loading a rule that matches
> nothing, but the safe habit is simply not to indent.

**Reserved keys.** `action` is `tag` (the default), `drop` or `keep`; `label` is
the tag applied by a matching rule, defaulting to the rule's own name. Every
other key is a match condition, and an unrecognised one is a start-up error —
a rule that silently matches nothing is indistinguishable from one that works
until documents start going missing.

**Matching.** Conditions within a rule are ANDed. A value is a comma-separated
OR-list (a repeated key works too, and is equivalent), and each alternative is

* an **exact** string by default, so `daemon` never matches `mydaemon`;
* a **glob** when it contains `*` or `?` — `fnmatch(3)` with no flags, so `*`
  crosses `/` and `path = /eos/*/proc/*` reads as written;
* a **POSIX extended regular expression** when it starts with `~`, compiled
  once at start-up (an uncompilable one is rejected there) and matched
  unanchored, so anchor it yourself: `~^[0-9]+$` for a bare numeric uid.

A leading `!` on the whole value negates the condition.

A condition whose field the document does not carry **never matches, negated or
not**. That is what keeps `user = daemon` from firing on a server-identity
document, which has no user at all — so read `user = !daemon` as "has a user
and it is not daemon", not "has no daemon user". Both polarities failing open
is the safe default for a drop rule.

**Resolution is independent of rule order.** Every rule is evaluated; each
matching rule contributes its label to `xrootd.filter.labels` (a sorted,
de-duplicated string array under `attributes`); and the document is dropped only
when some matching rule says `drop` and none says `keep`. So `keep` beats
`drop` beats `tag`, however the rules happen to be written. A dropped document
is still labelled internally, so a `keep`-rescued one records why it matched.

**Condition keys.** Short names for the fields worth filtering on; anything else
in the document is reachable as a raw `attributes.<key>` or `resource.<key>`
path (for example `attributes.xrootd.session.files`).

| key | document field | key | document field |
|---|---|---|---|
| `user` | `user.name` | `path` | `file.path` |
| `userid` | `user.id` | `dir` | `file.directory` |
| `role` | `user.roles` *(array)* | `filename` | `file.name` |
| `vo` | `wlcg.vo` | `ext` | `file.extension` |
| `groups` | `wlcg.groups` | `dataset` | `xrootd.dataset` |
| `authprot` | `xrootd.auth.method` | `event` | `event.name` |
| `proto` | `network.protocol.name` | `op` | `xrootd.operation.name` |
| `scheme` | `url.scheme` | `state` | `xrootd.operation.state` |
| `client` | `client.address` | `error` | `error.type` |
| `clientip` | `network.peer.address` | `provider` | `xrootd.gstream.provider` |
| `clientsite` | `xrootd.client.site` | `target` | `xrootd.redirect.target.address` |
| `app` | `user_agent.name` | `session` | `session.id` |
| `appver` | `user_agent.version` | `severity` | `severityText` *(top level)* |
| `appinfo` | `user_agent.original` | `server` | `server.address` *(resource)* |
| `appid` | `xrootd.app` | `site` | `xrootd.server.site` *(resource)* |
| `experiment` | `scitags.experiment` | `instance` | `service.instance.id` *(resource)* |
| `activity` | `scitags.activity` | `program` | `xrootd.server.program` *(resource)* |
| | | `version` | `service.version` *(resource)* |

An array field (`role`) matches when any element does; numbers and booleans are
matched as their printed form (`attributes.xrootd.error.code = 3011`,
`attributes.xrootd.open_seen = true`); objects never match.

**What filtering does not touch.** Rules run on the finished document,
immediately before it reaches the sinks, and after everything else in the
pipeline. So a dropped document is still fully accounted for in the Prometheus
series ([Metrics](#metrics)) and still folded into its session rollup — the
aggregate view of the cluster stays complete while the document stream is
cleaned up. `filtered_documents_total` counts what was suppressed, and the
`-v` exit summary reports it as `filtered=`. Filtering also does not reduce
the collector's memory use: the correlation state is built either way, so
`--max-memory` should be sized as if no filter were configured.

A few consequences worth knowing:

* Rules apply to **all sinks at once** — the file/`--bulk` output, OpenSearch,
  OTLP and `--forward`. There is no per-sink filtering.
* A dropped log document takes its companion `--spans` span with it, and a
  tagged one passes the label on to its span, so a trace is never left with a
  parentless child.
* Identity attributes (`user`, `vo`, `authprot`, `client`, …) are attached to
  the transfer, error, redirect and session documents, but **not** to the
  per-I/O `--traces` records or to `xrootd.gstream` documents, which carry no
  identity. With `--traces` enabled, suppressing a session's I/O records needs a
  second rule keyed on something they do carry, such as `path`/`dir` or
  `event = ~^xrootd\.(read|write|readv)$`.
* Filtering on session-level identity keeps a whole trace together, since the
  same identity is present on the session document and on each of its file
  operations. A rule that matches only some of them will leave a partial trace.
* `--debug` raw record dumps bypass the filter entirely; they are a decoder
  dump, not a document stream.

At start-up the collector reports the rules it loaded, whether or not `-v` was
given:

```
xrdmoncollect: 3 filter rule(s) loaded (1 tag, 1 drop, 1 keep)
```

### Output document

The per-transfer document uses the OpenTelemetry-aligned schema described under
[Serialization](#serialization): a process-level `resource` object and an
event-level `attributes` object. One object per file close. The example below
shows a fully-populated successful read (server configured with
`lfn ops ssq xfr auth`, token and SciTags records present, `--dataset` and
`--scitags` set):

```json
{
  "resource": {
    "service.name": "xrootd",
    "service.instance.id": "srv1",
    "service.version": "5.6.1",
    "server.address": "srv1.example.org",
    "server.port": 1094,
    "xrootd.server.site": "SITE-A",
    "xrootd.server.program": "xrootd",
    "xrootd.server.id": 42,
    "xrootd.server.incarnation": 1700000000
  },
  "scope": { "name": "xrdmoncollect", "version": "v6.1.0" },
  "@timestamp": "2026-07-02T10:00:32.000Z",
  "timeUnixNano": "1751450432000000000",
  "observedTimeUnixNano": "1751450432100000000",
  "severityNumber": 9, "severityText": "INFO",
  "traceId": "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371",
  "spanId": "3ab4c1d2e3f40516",
  "eventName": "xrootd.read",
  "attributes": {
    "event.name": "xrootd.read",
    "file.path": "/store/data/Run2026A-PromptReco/file.root",
    "file.name": "file.root",
    "file.directory": "/store/data/Run2026A-PromptReco",
    "file.extension": "root",
    "file.size": 1073741824,
    "xrootd.dataset": "Run2026A-PromptReco",
    "xrootd.file.read_write": false,
    "client.address": "wn.example.org",
    "network.peer.address": "192.0.2.17",
    "network.type": "ipv4",
    "network.transport": "tcp",
    "network.protocol.name": "xroot",
    "session.id": "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371",
    "user.name": "alice",
    "user.id": "https://issuer/sub42",
    "user.roles": ["production"],
    "wlcg.vo": "atlas", "wlcg.groups": "/atlas/prod",
    "xrootd.auth.method": "gsi",
    "user_agent.name": "xrdcp", "user_agent.version": "v5.6.1",
    "user_agent.original": "task=prod-copy",
    "xrootd.client.site": "client-site",
    "xrootd.app": "job=1234",
    "scitags.experiment_id": 8, "scitags.experiment": "cms",
    "scitags.activity_id": 3, "scitags.activity": "analysis",
    "xrootd.operation.name": "read",
    "xrootd.operation.state": "Successful",
    "xrootd.open_seen": true,
    "xrootd.forced_close": false,
    "xrootd.operation.start_time": "2026-07-02T09:55:32.000Z",
    "xrootd.operation.duration": 300.25,
    "xrootd.read_bytes": 805306368,
    "xrootd.readv_bytes": 268435456,
    "xrootd.write_bytes": 0,
    "xrootd.read_ops": 320,
    "xrootd.readv_ops": 16,
    "xrootd.write_ops": 0,
    "xrootd.readv_segs": 4096,
    "xrootd.read_min": 4096,
    "xrootd.read_max": 8388608,
    "xrootd.readv_min": 1024,
    "xrootd.readv_max": 4194304,
    "xrootd.readv_segs_min": 4,
    "xrootd.readv_segs_max": 512,
    "xrootd.write_min": 0,
    "xrootd.write_max": 0,
    "xrootd.read_sumsq": 8.1e18,
    "xrootd.readv_sumsq": 2.9e17,
    "xrootd.rsegs_sumsq": 1.2e9,
    "xrootd.write_sumsq": 0.0,
    "xrootd.is_local": true
  }
}
```

A few transfer-document fields cannot appear in this (successful, xroot)
example because they are situational: a failed operation replaces
`"Successful"` with `"Failed"` and adds `error.type` (the server's verbatim
reason) and `xrootd.error.code`; a session over the HTTP bridge carries
`url.scheme` (`http`/`https`, with `network.protocol.name` `"http"`); a
redirect report (`--redirects`) carries `xrootd.operation.state`
`"Redirected"` plus `xrootd.redirect.kind` and
`xrootd.redirect.target.{address,port}`. The `*_sumsq` fields appear only with
`ssq` in the server config.

The request-size extremes (`*_min`/`*_max`, and `readv_segs_min`/`_max` for the
segment count per `readv()`) arrive with the `ops` block, and their presence
carries information:

- **`0`/`0`** — the operation never ran. The server zeroes the pair in that
  case, so a pure read reports `write_min`/`write_max` as `0`.
- **absent** — the operation ran but the extremes were not measured. XRootD
  maintains them only for a file tracked at `XrdXrootdFileStats::monOps` or
  above; at the lower level the op *counts* are real while the extremes keep
  their unset sentinel. Reporting that sentinel would look like a 2 GiB minimum
  request, so the collector omits the pair instead. A close can therefore carry
  `xrootd.read_ops` with no `xrootd.read_min`.

Failures come in two shapes (both logged at `severityText` `ERROR`,
`severityNumber` 17; see [WLCG field mapping](#wlcg-field-mapping) for the
semantics). A **failed open** never produced a close, so the server reports it
as a dedicated terminal record: the document carries the identity and `file.*`
attributes but no byte totals, and the failed operation *is* the event name and
the operation name. Abbreviated to the fields that differ from the
successful example (`resource` and the remaining identity attributes are as
above):

```json
{
  "resource": { "service.name": "xrootd", "server.address": "srv1.example.org" },
  "scope": { "name": "xrdmoncollect", "version": "v6.1.0" },
  "@timestamp": "2026-07-02T10:02:07.000Z",
  "timeUnixNano": "1751450527000000000",
  "observedTimeUnixNano": "1751450527100000000",
  "severityNumber": 17, "severityText": "ERROR",
  "traceId": "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371",
  "spanId": "5c6d7e8f90a1b2c3",
  "eventName": "xrootd.open",
  "attributes": {
    "event.name": "xrootd.open",
    "file.path": "/store/data/missing.root",
    "file.name": "missing.root",
    "file.directory": "/store/data",
    "file.extension": "root",
    "client.address": "wn.example.org",
    "session.id": "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371",
    "user.name": "alice",
    "xrootd.operation.name": "open",
    "xrootd.operation.state": "Failed",
    "xrootd.error.code": 3011,
    "error.type": "Unable to open /store/data/missing.root; no such file or directory"
  }
}
```

A **failed transfer** (a terminal read/write error recorded during the
session, or a failed close) is reported on the close record itself, so the
document keeps the full close shape — partial byte totals, `ops`/`ssq`
detail, `open_seen`, the byte-derived `read`/`write` direction — and adds
the error fields (the error-category byte, `read` here, feeds the
`errors_total{category}` metric label instead):

```json
{
  "resource": { "service.name": "xrootd", "server.address": "srv1.example.org" },
  "scope": { "name": "xrdmoncollect", "version": "v6.1.0" },
  "@timestamp": "2026-07-02T10:03:12.000Z",
  "timeUnixNano": "1751450592000000000",
  "observedTimeUnixNano": "1751450592100000000",
  "severityNumber": 17, "severityText": "ERROR",
  "traceId": "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371",
  "spanId": "7a8b9c0d1e2f3041",
  "eventName": "xrootd.read",
  "attributes": {
    "event.name": "xrootd.read",
    "file.path": "/store/data/Run2026A-PromptReco/file.root",
    "file.name": "file.root",
    "file.directory": "/store/data/Run2026A-PromptReco",
    "file.extension": "root",
    "file.size": 1073741824,
    "client.address": "wn.example.org",
    "session.id": "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371",
    "user.name": "alice",
    "xrootd.operation.name": "read",
    "xrootd.operation.state": "Failed",
    "xrootd.error.code": 3005,
    "error.type": "Unable to readv /store/data/Run2026A-PromptReco/file.root; illegal seek",
    "xrootd.open_seen": true,
    "xrootd.forced_close": false,
    "xrootd.operation.start_time": "2026-07-02T10:02:52.000Z",
    "xrootd.operation.duration": 20.5,
    "xrootd.read_bytes": 4096,
    "xrootd.readv_bytes": 0,
    "xrootd.write_bytes": 0
  }
}
```

Note that the failure does not change the event name: the close still reports
the direction it had, with `xrootd.operation.state` carrying the outcome. The
byte totals are what the operation managed before it aborted — 4 KiB of a 1 GiB
file here — so a consumer comparing them against `file.size` sees the shortfall
directly.

`xrootd.open_seen` is `false` (and the `file.*`, `user.*`, `client.*`
attributes are absent) for a close whose open record was lost or predates the
collector — the byte totals are still reported. Empty/zero
fields are omitted, so a given document only carries what the server actually
reported.

And a `session` document (`--sessions`), emitted when the client disconnects,
rolling up everything it did — the root of its trace:

```json
{
  "@timestamp": "2026-07-02T10:14:07.000Z",
  "eventName": "xrootd.session",
  "severityText": "INFO",
  "traceId": "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371",
  "spanId": "3c1a8b0d4e2a6f37",
  "resource": {
    "service.name": "xrootd",
    "server.address": "eos-node-12.example.org"
  },
  "attributes": {
    "event.name": "xrootd.session",
    "session.id": "9f1c8b0d4e2a6f37c1a8b0d4e2a6f371",
    "user.name": "alice",
    "client.address": "wn.example.org",
    "wlcg.vo": "cms",
    "xrootd.session.start_time": "2026-07-02T09:55:30.000Z",
    "xrootd.session.end_time": "2026-07-02T10:14:07.000Z",
    "xrootd.session.duration": 1117.0,
    "xrootd.session.start_time_source": "login",
    "xrootd.session.files": 2,
    "xrootd.session.reads": 2,
    "xrootd.session.writes": 0,
    "xrootd.session.read_bytes": 805310464,
    "xrootd.session.readv_bytes": 268435456,
    "xrootd.session.recent_files": [
      { "file.path": "/store/data/Run2026A-PromptReco/file.root",
        "xrootd.operation.name": "read",
        "xrootd.bytes": 1073741824 },
      { "file.path": "/store/data/Run2026A-PromptReco/other.root",
        "xrootd.operation.name": "read",
        "xrootd.bytes": 4096 }
    ]
  }
}
```

The three time fields and `start_time_source` are always present (see
[Session times](#session-times)); the counters are always present but the byte
totals, like everywhere else, are omitted when zero.

### WLCG field mapping

The schema covers the WLCG transfer-monitoring fields that XRootD currently puts
on the wire. Mapping (and the server config each needs):

| WLCG field | XRootD field | Source / requires |
| :-- | :-- | :-- |
| file_name | `file.path` (+ `file.name`/`file.directory`/`file.extension`) | `fstat … lfn` |
| dataset | `xrootd.dataset` | `--dataset <regex>` capture on `file.path` |
| operation_type | `xrootd.operation.name` (`read`/`write`) | `fstat … xfr` |
| operation_state | `xrootd.operation.state` (`Successful`/`Failed`/`Redirected`) | `fstat` (terminal report); `Redirected` from `r` with `--redirects` |
| error_message | `error.type` | `fstat` (failed open / I/O / close) |
| error_category | `xrootd.operation.name` + `xrootd.error.code` | `fstat` (failed open / I/O / close) |
| server_name/site | `server.address` / `xrootd.server.site` | `=` ident (`all.sitename`/`XRDSITE` for site) |
| server_ip / hostname | `server.address` | `=` ident, else UDP source (loopback → public address / local FQDN) |
| client_ip / hostname | `client.address` (name first, from `@host` or `&h=`) / `network.peer.address` (IP) | `u` descriptor / auth `&h=` (server DNS config) / login CGI `&a=` (numeric IP, 6.x+) |
| client_app / version | `user_agent.name` (`&x=` executable, else `xrootd`) / `user_agent.version` / `user_agent.original` (`&y=`) | login appinfo (`&x=`/`&R=`/`&y=`) |
| ip_version | `network.type` (`ipv4`/`ipv6`) | login appinfo (`&I=`) |
| client_site | `xrootd.client.site` | login appinfo (`&S=`, client `XRDSITE`/`XRD_SITE`) |
| auth_method | `xrootd.auth.method` | **`… auth`** |
| user | `user.name` (token `&n=` preferred over descriptor) / `user.id` (token `&s=`, else login DN `&n=`) | `u` / `T` token |
| vo | `wlcg.vo` | `T` token, else `… auth` (`&o=` from a VO-bearing method: gsi/sss/ztn/http(s)) |
| activity | `scitags.experiment`/`scitags.activity` (names), `scitags.*_id` (numeric), `user.roles` | `U` SciTags + `--scitags` registry; `T` token for role |
| start_time / end_time | `xrootd.operation.start_time` / the record's `@timestamp` | f-stream `FileTOD` window, interpolated per record |
| bytes | `xrootd.{read,readv,write}_bytes` | `fstat … xfr` |
| is_local (LAN/WAN) | `xrootd.is_local` | derived: client vs server domain (needs `=` ident) |

`client.address` carries the client's resolved name when one is known, with
the IP address only as a fallback (per the OTel semantic conventions). The
name is taken from the first real hostname among the `u` descriptor's `@host`
and the auth-reported `&h=` — both reverse-resolved by the *server* at login
time depending on its DNS configuration; the collector itself never does DNS
in the receive path. When the name wins, the numeric client IP the
server puts in the login CGI (`&a=`, XRootD 6.x+) is kept as
`network.peer.address` (the direct peer). When no name is available,
`client.address` is that numeric IP (or the descriptor's IP literal against
an older server). The client's port is never reported: it is not on the
monitoring wire (the `&a=` CGI strips it by construction, and the
descriptor's `:<n>` field is a server-side file descriptor, not a port).

The client's access protocol (the descriptor's `<prot>` field) travels as
`network.protocol.name` (`xroot`, `http`); a session that came in over the
HTTP bridge additionally carries `url.scheme` (`http`/`https`). Per-request
HTTP detail (method, status code) is not on the monitoring wire — the `http`
g-stream reports only cumulative per-method/status counters, which feed the
aggregated `xrootd_collector_http_requests_total` metric.

`server.address` is the single canonical server-name field. Its precedence
is: the host advertised on the `=` ident stream (when it is a real name, not
an IP literal; a `localhost*` name is replaced by the collector's own FQDN),
else — for a server reporting from the loopback address (the common
co-located collector + server setup, where the UDP source is
`::1`/`127.0.0.1`) — the collector's own local FQDN, since the reporting
server runs on the same host, else the numeric source IP.

Loopback literals (`127.0.0.0/8`, `::1`) that would otherwise be emitted in
`client.address`/`server.address` are replaced with this host's public
address: at startup (never in the receive loop) the collector resolves its own
FQDN once and caches the first non-loopback IPv4 and IPv6 address; when name
resolution yields nothing usable (e.g. `/etc/hosts` pinning the host name to
`127.0.1.1`), the first public — else private — interface address of each
family is used instead. A loopback of one family is replaced by the cached
address of the same family, falling back to the other family, then to the
FQDN. `--no-resolve` disables all of these substitutions (leaving the numeric
addresses and names as received). A *remote* server is never reverse-resolved
here — that hostname comes from its `=` ident, and a blocking reverse-DNS
lookup of an arbitrary source IP would stall the UDP receive loop.

`xrootd.is_local` is a heuristic: it is `true` when the client
(`client.address`, which is name-first) and the reporting
server share a registered domain (the part after the first host label),
`false` when they differ, and **omitted** when either side is an IP literal or
the server host is unknown (no `=` ident yet). It has no metric of its own —
derive the split from the documents.

`xrootd.operation.state` is the authoritative success/failure of the
operation: a plain close reports `Successful`, while a failed open, a
mid-transfer read/write error, or a failed close reports `Failed` together
with `xrootd.operation.name` (the operation that failed:
`open`/`read`/`write`/`close`/`auth`; named after semconv's
`nfs.operation.name`), `xrootd.error.code` (the XRootD error code), and
`error.type` (the server's error message). On a failed *close* the record
keeps its byte-derived `read`/`write` operation name — the error-category
byte still drives the `xrootd_collector_errors_total{site,server,category}`
metric label. A
failed open never produced any close record before; the server now emits a
terminal `isError` f-stream record, and sets `hasERR` on the close for a failed
close or a terminal `read`/`readv`/`pgread`/`write`/`writev`/`pgwrite` error
recorded during the session (the common "readv past EOF" surfaces as a `read`
failure). The `isError` record covers open failures reported synchronously
(`fsError`), asynchronously (the deferred-open callback), and the early `do_Open`
denials that bypass `fsError` — notably a second writer rejected with
`kXR_FileLocked`. This requires only
the existing `fstat` setup — no extra directive. A disconnect-driven
(`xrootd.forced_close`) close is **not** a failure unless an error was
actually recorded. The `error.type` is the server's own SFS reason verbatim
(e.g. `Unable to open …; no such file or directory` for a missing file); the
`XRootD::moncollect` test asserts that specific reason per-document for both a
failed open and the readv-past-EOF case.

A third terminal state, `"Redirected"`, is reported for `r`-stream redirect
records (with `--redirects`): from the redirector's point of view the operation
concluded by sending the client elsewhere. The redirect destination travels as
`xrootd.redirect.kind`, `xrootd.redirect.target.address`, and
`xrootd.redirect.target.port`; the data server that ultimately serves the file
emits its own `Successful`/`Failed` close.

`xrootd.client.site` is the site the *client* advertises for itself: an XRootD
client that has `XRDSITE` (or `XRD_SITE`, which takes precedence) set in its
environment sends it in the login CGI (`xrd.site`), the server folds it into the
user-map appinfo (`&S=`), and the collector surfaces it as `xrootd.client.site`.
It is absent when the client does not advertise one. (This is distinct from
`xrootd.server.site`, which is the *reporting server's* site — `all.sitename`
in its config, exported as `XRDSITE` — from the `=` ident record. A site that
is only dots — `XrdOucSiteName`'s sanitization of an entirely invalid name,
e.g. a stray `XRDSITE` env var inherited by a daemon with no `all.sitename`
directive — is dropped as carrying no information.)

### Monitoring stream field mapping

These tables give the concrete wire-field → JSON-key mapping for every XRootD
monitoring stream. Both sinks serialize the **same** canonical document (see
[Sink comparison](#sink-comparison-opensearch-vs-otlp)), so each key below is
identical in OpenSearch and OTLP — OpenSearch nests it as a JSON path
(`resource.<key>` / `attributes.<key>`), OTLP emits the same key as a Resource
or LogRecord `KeyValue`; the envelope keys become native OTLP LogRecord/Span
fields. Field meanings follow the [XRootD monitoring
spec](https://xrootd.web.cern.ch/doc/dev6/xrd_monitoring.htm).

**Resource** (process-level, from the `=` ident record and the UDP source):

| Wire field | Canonical key |
| :-- | :-- |
| `&inst=` | `resource.service.instance.id` |
| `&ver=` | `resource.service.version` |
| `@host` / resolved / source IP | `resource.server.address` |
| `&port=` | `resource.server.port` |
| `&site=` | `resource.xrootd.server.site` |
| `&pgm=` | `resource.xrootd.server.program` |
| f-stream `sID` | `resource.xrootd.server.id` |
| header `stod` | `resource.xrootd.server.incarnation` |
| — | `resource.service.name` = `xrootd` (constant) |

**Envelope** (per record, set by `otelBegin`):

| Source | Canonical key |
| :-- | :-- |
| record time (interpolated, see below) | `@timestamp`, `timeUnixNano` |
| receipt time | `observedTimeUnixNano` |
| error flag | `severityNumber` / `severityText` |
| record kind | `eventName` (+ `attributes.event.name`) |
| correlation | `traceId`, `spanId` (+ `attributes.session.id`) |

**Record times are interpolated within their reporting window.** The wire
carries only window boundaries (the f-stream `isTime` record's `tBeg`/`tEnd`
plus its record count; the t-stream's `WINDOW` marks), but records are
appended to the server's buffer in time order, so each record's time is
estimated by linear interpolation over its position in the packet. Event
times, `xrootd.operation.start_time`, span start/end and
`xrootd.operation.duration` (a fractional-seconds number) therefore carry
sub-window, millisecond-formatted *estimates* — accurate to well under the
flush interval, rather than collapsing onto the window boundary (which used
to make every open/close pair reported in one window compute a zero
duration). The window endpoints themselves have one-second wire granularity.

**`u` — user login (`MAPUSER`), descriptor `<prot>/<user>.<pid>:<sfd>@<host>` + CGI:**

| Wire field | Canonical key |
| :-- | :-- |
| `<prot>` | `attributes.network.protocol.name` (+ `url.scheme` if `http`/`https`) |
| `<user>` | `attributes.user.name` (token `&n=` preferred when present) |
| `@host` / `&h=` | `attributes.client.address` (first real hostname) |
| `&a=` (numeric IP) | `attributes.network.peer.address` (or `client.address` if no name) |
| `&h=` | auth-reported client host — name candidate for `client.address` |
| `&n=` | `attributes.user.id` (login DN; token `&s=` wins) |
| `&p=` | `attributes.xrootd.auth.method` |
| `&o=` | `attributes.wlcg.vo` (only if the auth method conveys a VO) |
| `&r=` | `attributes.user.roles[]` |
| `&g=` | `attributes.wlcg.groups` |
| `&x=` | `attributes.user_agent.name` (executable; else `xrootd`) |
| `&R=` | `attributes.user_agent.version` (client release) |
| `&y=` | `attributes.user_agent.original` (XRD_MONINFO) |
| `&S=` | `attributes.xrootd.client.site` |
| `&I=` | `attributes.network.type` (`ipv4`/`ipv6`) |
| — | `attributes.network.transport` = `tcp` (constant) |

**`d` / `i` — path & appinfo dictionaries:**

| Stream | Wire field | Canonical key |
| :-- | :-- | :-- |
| `d` (`MAPPATH`) | `<lfn>` | `attributes.file.path` (+ `file.name`/`file.directory`/`file.extension`) |
| `i` (`MAPINFO`) | `<appinfo>` | `attributes.xrootd.app` (only when it differs from `&y=`) |

**`T` — token (`MAPTOKN`):**

| Wire field | Canonical key |
| :-- | :-- |
| `&s=` | `attributes.user.id` (subject; preferred over login `&n=`) |
| `&n=` | `attributes.user.name` (mapped username; preferred over descriptor) |
| `&o=` | `attributes.wlcg.vo` (preferred over the `u` auth `&o=`) |
| `&r=` | `attributes.user.roles[]` |
| `&g=` | `attributes.wlcg.groups` |

**`U` — SciTags experiment/activity (`MAPUEAC`):**

| Wire field | Canonical key |
| :-- | :-- |
| `&Ec=` | `attributes.scitags.experiment_id` (+ `scitags.experiment` name via registry) |
| `&Ac=` | `attributes.scitags.activity_id` (+ `scitags.activity` name via registry) |

**`f` — file stream (`MAPFSTA`) → `xrootd.read`/`xrootd.write` / `xrootd.session` docs:**

| Record | Wire field | Canonical key |
| :-- | :-- | :-- |
| `isTime` | tBeg / tEnd / nRecs / sID | per-record time interpolation / `resource.xrootd.server.id` |
| `isOpen` | fsz / RW / lfn / user | `attributes.file.size`, `.xrootd.file.read_write`, `.file.*`, → identity |
| `isClose` (`xfr`) | read/readv/write bytes | `attributes.xrootd.{read,readv,write}_bytes` |
| `isClose` (`ops`) | op counts, readv segs, min/max | `attributes.xrootd.{read,readv,write}_{ops,min,max}`, `.readv_segs`, `.readv_segs_{min,max}` |
| `isClose` (`ssq`) | Σx² | `attributes.xrootd.{read,readv,rsegs,write}_sumsq` |
| `isClose` | derived | `event.name`, `.operation.name`, `.operation.state`, `.open_seen`, `.forced_close`, `.operation.duration`, `.is_local` |
| `isClose`/`isError` | error text / code | `attributes.error.type`, `.xrootd.error.code` |
| `isDisc` | session rollup | `attributes.xrootd.session.{files,reads,writes,errors,read_bytes,readv_bytes,write_bytes,start_time,end_time,duration,start_time_source,recent_files}` |

**`t` — trace stream (`MAPTRCE`, `--traces`):**

| Record | Wire field | Canonical key / `eventName` |
| :-- | :-- | :-- |
| read / write | offset, length | `attributes.xrootd.io.offset`, `.xrootd.io.length`; `xrootd.io.read`/`xrootd.io.write` |
| readv / readu | file dictid | `attributes.file.*`, `.xrootd.file.id`; `xrootd.io.readv` |
| open | fsz | `attributes.file.size`; `xrootd.io.open` |
| close | read/write bytes | `attributes.xrootd.{read,write}_bytes`; `xrootd.io.close` |
| disc | duration, user | `attributes.xrootd.session.{start_time,end_time,duration}` + identity; `xrootd.io.disconnect`. Also dates the session document's login exactly (see [Session times](#session-times)) |
| appid | 12-byte app id | `attributes.xrootd.app`; `xrootd.io.appid` |
| window | time | (sets envelope time; no document) |

**`g` — g-stream (`MAPGSTA`, `--gstream`) → `xrootd.gstream`:**

| Wire field | Canonical key |
| :-- | :-- |
| provider (top byte of sID) | `attributes.xrootd.gstream.provider` |
| record body (JSON/CGI line) | `attributes.xrootd.gstream.data` (nested object; a string under OTLP if unparseable) |

The g-stream reaches the collector in one of two on-wire shapes. The built-in
providers (`ccm`, `oss`, `http`, `pfc`, `TcpMon`, `Throttle`, `Tpc`) default to
the **binary** `XrdXrootdMonGS` framing (`dest ... <collector>` on the
`xrootd.monitor` line) — the native form. A separate `xrootd.mongstream ... send
json` (or `cgi`) directive instead emits **newline-delimited text**: a header
object (`{"code":"g",...,"gs":{"type":T,"tbeg":..,"tend":..}}`, or none with
`send json nohdr`) followed by each plugin's raw record, one per line. The
collector auto-detects the text form by its leading `{` and decodes it into the
same `xrootd.gstream` events and provider metrics; the provider comes from the
header's `gs.type` (or `unknown` under `nohdr`). Point a `send json` destination
only at a JSON consumer or at this collector — never mix it into a binary sink.

**`r` — redirect (`MAPREDR`, `--redirects`) → `xrootd.redirect`:**

| Wire field | Canonical key |
| :-- | :-- |
| type low nibble | `attributes.xrootd.operation.name` |
| type high nibble | `attributes.xrootd.redirect.kind` (`local`/`remote`) |
| port | `attributes.xrootd.redirect.target.port` |
| host | `attributes.xrootd.redirect.target.address` |
| path | `attributes.file.*` |
| dictid | → identity; `attributes.xrootd.operation.state` = `Redirected` |

**`x` / `p` — FRM stage/purge (`MAPXFER`/`MAPPURG`) → `xrootd.frm`:**

| Wire field | Canonical key |
| :-- | :-- |
| code | `attributes.xrootd.operation.name` (`transfer`/`purge`) |
| `<path>` | `attributes.file.*` |
| `<who>` descriptor | `attributes.network.protocol.name`, `.user.name`, `.client.address` |
| `&sz=` | `attributes.file.size` |
| `&tod=` | envelope time |

Two spec fields are intentionally not emitted: the `=` record's own daemon
login user (internal change-detection only) and the login `&h=` when a
descriptor hostname already won `client.address`.

## Sinks

Documents fan out to any combination of sinks; stdout is used only as the
fallback when no other sink is configured (`-o` always adds a file too):

- **File / stdout** (`-o`, `--bulk`): NDJSON, or the OpenSearch `_bulk` framing
  with `--bulk <index>`. Ship it with an external agent (Filebeat) or `curl`.
- **OpenSearch** (`--os-url`): available when the binary is built with libcurl
  (the build links `CURL::libcurl` if found). Documents are batched and posted
  via the `_bulk` API on a dedicated output thread; transient failures (network,
  HTTP 429/5xx) are retried with exponential backoff. With `--cache-dir` a body
  that still fails is written to disk and retried later (see
  [Durability and offline caching](#durability-and-offline-caching)).
- **OTLP/HTTP** (`--otlp-url`): posts the documents to an OpenTelemetry endpoint
  as OTLP/JSON — logs to `<url>/v1/logs` and, with `--spans`, spans to
  `<url>/v1/traces` — so xrdmoncollect feeds an **OpenTelemetry Collector** or
  **Grafana Alloy** natively; the collector then routes to Loki, Tempo,
  Elasticsearch, Kafka, and so on. The nested `resource`/`attributes` objects are
  re-encoded into the strict OTLP `resourceLogs`/`resourceSpans` envelope with
  typed `KeyValue` attributes (see [Serialization](#serialization)). Batched per
  flush on a dedicated output thread with retry/backoff. With `--cache-dir` a body
  that still fails is written to disk and retried later (logs and traces cache
  separately under `otlp-logs`/`otlp-traces` subdirectories, since they replay to
  different endpoints); without it a terminal failure drops the body (counted).
  Requires libcurl; `--otlp-insecure` skips TLS verification. This is the
  log/trace analogue of the metrics OTLP push in `XrdHttpMetricsExporter`.
- **TCP forward** (`--forward host:port`): streams the same NDJSON over a plain
  TCP connection to a buffering/forwarding frontend — Logstash (`tcp` input),
  Fluentd (`in_tcp`), Vector (`socket` source), or a message-broker bridge. The
  connection is lazily (re)established with a short cool-down; documents
  produced while the consumer is down are dropped (durable buffering is the
  downstream's job). Dependency-free, so it is built even without libcurl.

### Authentication

Both HTTP sinks authenticate to their endpoint:

- **Basic auth** (OpenSearch only): `--os-user`/`--os-pass`, sent as
  `Authorization: Basic`.
- **Bearer token** (both sinks): `--os-token` / `--otlp-token`, sent as
  `Authorization: Bearer <token>` on every request. This is the usual scheme for
  OTLP collectors (Grafana Alloy, a gateway OTel Collector) and for token-secured
  OpenSearch. For OpenSearch a bearer token takes precedence over basic auth if
  both are configured (they share the `Authorization` header).

A token given as `@<file>` is read from that file (trailing whitespace stripped)
instead of being taken literally, so the secret stays out of `ps`/argv; a
config-file value (a `[xrdmoncollect]` `os-token`/`otlp-token` key in a
mode-`0600` file) works the same way. The TCP `--forward` sink is a plain socket
with no application-layer auth — put it behind a trusted network or a TLS proxy.

The **shovel channel** authenticates with its own shared secret: the shoveler
sends `--shovel-token` in the XSHV hello and the collector, when `--tcp-token`
is set, requires a constant-time match before accepting any frames (rejections
are counted in `xrootd_collector_tcp_auth_failures_total`). Both accept
`@<file>`. Without `--tcp-token` the collector accepts any well-formed hello —
acceptable inside a trusted network, but set the token when the port is
reachable from outside the cluster. The channel itself is plain TCP (no TLS);
route it through a trusted network or a TLS tunnel if it must cross one.

## Configuration

### Command-line options

```
xrdmoncollect -p <port> [-b <bindaddr>] [-o <file>] [--bulk <index>]
              [--os-url <url> [--os-index <name>] [--os-user <u>]
               [--os-pass <p>] [--os-token <t>] [--os-insecure] [--os-datastream]]
              [--otlp-url <url> [--otlp-token <t>] [--otlp-insecure]]
              [--forward <host:port>]
              [--tcp-port <p> [--tcp-token <t>]]
              [--shovel <host:port> [--shovel-token <t>] [--spool-max <sz>]]
              [--flush-count <n>] [--flush-secs <n>] [--debug] [-v]

  -c <file>        load options from an INI config file, plus any
                   [filter "<name>"] document rules (see Configuration)
  -p <port>        UDP port to listen on (long form: --udp-port; required
                   unless --tcp-port is given)
  -b <bindaddr>    address to bind (default: all interfaces, dual-stack)
  --tcp-port <p>   also accept UDP packets encapsulated over TCP from remote
                   collectors running in shoveler mode
  --tcp-token <t>  shared secret shovelers must present; @<file> reads it from
                   a file (default: accept any connection)
  --shovel <h:p>   shoveler mode: relay received UDP packets over TCP to a
                   central collector's --tcp-port instead of decoding them
  --shovel-token <t> shared secret for the --shovel connection; @<file> reads
                   it from a file
  --spool-max <sz> cap the shovel disk spool under --cache-dir, evicting the
                   oldest buffers (K/M/G; default 1G; 0=unbounded)
  -o <file>        append output to <file> (default: stdout unless a network sink)
  --bulk <index>   write OpenSearch _bulk format to the file/stdout sink
  --os-url <url>   POST documents to an OpenSearch cluster's _bulk API
  --os-index <n>   index/data-stream name (default: xrootd-transfers)
  --os-user <u>    basic-auth user
  --os-pass <p>    basic-auth password
  --os-token <t>   bearer token (Authorization: Bearer); wins over basic auth;
                   @<file> reads the token from a file
  --os-insecure    skip TLS certificate verification
  --os-datastream  target is a data stream (use the "create" bulk action)
  --otlp-url <url> POST OTLP/JSON to an OTel collector (logs -> /v1/logs,
                   spans -> /v1/traces with --spans)
  --otlp-token <t> bearer token (Authorization: Bearer); @<file> reads it from
                   a file
  --otlp-insecure  skip TLS verification for the OTLP endpoint
  --cache-dir <d>  cache _bulk bodies that fail to POST under <d> and retry them
                   (oldest-first, replayed on startup; default: off = drop)
  --forward <h:p>  also stream documents as NDJSON over TCP to host:port
  --flush-count <n> packets per receive batch / one batch -> one POST (def 500)
  --flush-secs <n>  hand off a partial batch after N seconds (default: 5)
  --rcvbuf <sz>     kernel UDP receive buffer, SO_RCVBUF (K/M/G; default 16M)
  --queue-depth <n> receive->serialize batches in flight (default: 64)
  --metrics-port <p> serve aggregated metrics over HTTP on port <p>
  --max-memory <sz>  bound correlation state to ~<sz> bytes, LRU-evicting
                     (K/M/G suffix; default 256M; 0=unbounded)
  --max-entries <n>  optional hard cap on correlation entries (0=off)
  --server-ttl <s>   reclaim a server incarnation idle for >s seconds
                     (default 86400; 0=never)
  --file-ttl <s>     expire an open-file entry untouched for >s seconds (a
                     leaked open whose close record was lost; default 0=off).
                     Only applied to servers reporting in-flight snapshots
                     ("xfr" on xrootd.monitor fstat); set it to at least 3x
                     the server's xfr reporting period
  --scitags <src>  SciTags registry (file path or http(s):// URL) mapping
                   experiment/activity ids to names
  --scitags-refresh <s> re-fetch a URL registry every <s> seconds (default 3600)
  --no-resolve     do not substitute the local FQDN / public address for
                   loopback addresses and localhost names
  --sessions       correlate per-session activity and emit a session document
                   per client disconnect (off by default)
  --spans          also emit OpenTelemetry span documents alongside the logs
                   (required for any traces to appear in Tempo/Grafana)
  --traces         emit a document per t-stream I/O record (high volume); with
                   --spans each read/write is a child span under the file span
  --gstream        emit a document per g-stream (plugin) record
  --redirects      emit a document per r-stream redirect record
  --debug          also emit one JSON object per decoded record (debugging)
  -v               print decoder statistics on exit (SIGINT/SIGTERM)
```

### Configuration file

Options may be set in an INI configuration file instead of (or in addition to)
the command line. The file is loaded from the path given with `-c`/`--config`,
or automatically from `/etc/xrootd/xrdmoncollect.cfg` when that file exists (a
missing default path is silently ignored). All keys live in a single
`[xrdmoncollect]` section and mirror the long-option names; **command-line
options override values from the file**. A named-but-unreadable or malformed
file is a fatal error. See `xrdmoncollect.cfg.example` for the full key list.

```ini
[xrdmoncollect]
port = 9930
os-url = https://opensearch.example.org:9200
os-index = xrootd-transfers
forward = logstash.example.org:5044
metrics-port = 9931
max-memory = 256M
```

The file may additionally carry any number of `[filter "<name>"]` sections,
which drop or tag emitted documents and have no command-line equivalent — see
[Document filtering](#document-filtering-filter-). Any other section is
ignored with a warning.

### Server configuration

Point the server's file-stats stream at the collector. **The `xfr` option is
required to get close records** (and therefore transfer documents): without it
the server registers opens but never emits the per-file `isClose` record
(`XrdXrootdMonFile.cc` only assigns the monitor entry when I/O stats are kept).
The `lfn` option adds the path to the open record and `ops`/`ssq` add the
operation counts and sum-of-squares to the close record. The `auth` option
enriches the user dictionary with the authentication method and VO (see the
field table above); without it those fields are simply absent.

The VO path (gsi → VOMS attribute certificate → `XrdSecEntity.vorg` → MAPUSER
`&o=` → `wlcg.vo`) is exercised end-to-end by the `XRootD::moncollect`
integration test when the VOMS plug-in is built: it mints a fake VOMS proxy with
`voms-proxy-fake` and asserts `wlcg.vo` appears on the transfer document.

```
xrootd.monitor all flush 30 fstat 30 lfn ops ssq xfr 1 auth ident 300 \
               dest fstat info user <collector-host>:9930
```

`ops` is what makes `io_total`'s `read`/`readv`/`write` counts move, so without
it the collector reports opens, closes and byte volumes but no IOPS.

`ident 300` is worth setting: the `=` identity record is what tells the
collector a server's site and host name, and it defaults to hourly. Until it
arrives every metric from that server is labelled `site="unknown"` with the
numeric address as `server` (see [Site and server
labels](#site-and-server-labels)).

### Tuning `xrootd.monitor` for pipeline resilience

The monitor streams are UDP: once a datagram is larger than the path MTU it is
fragmented at the IP layer, and the loss of any *one* fragment silently drops
the *whole* datagram. Keeping every stream's buffer at or below the MTU is the
single most effective resilience knob on the server side.

**Size every buffer, not just `mbuff`.** Four independent buffer sizes become
UDP datagrams:

| Buffer | Directive option | Default | Streams |
| :-- | :-- | :-- | :-- |
| trace buffer | `mbuff <sz>` | 16K | `t` (I/O traces) |
| fstat buffer | `fbsz <sz>` | **65472** | `f` (open/close/xfr) |
| redirect buffer | `rbuff <sz>` | 32K | `r` (redirects) |
| g-stream buffer | `gbuff <sz>` | 32K | `g` (pfc/tpc/oss/... plugin records) |

Until XRootD 6.1 `fbsz` does **not** follow `mbuff`: a cluster that sets
`mbuff 1472` (to fit a 1500-byte MTU) but leaves `fbsz` alone still emits
fstat datagrams of up to 65472 bytes — ~44 IP fragments each, so even a 0.1%
fragment loss rate kills a few percent of the fstat stream, which is exactly
the stream the transfer documents are built from. Newer servers default `fbsz`
to the `mbuff` value when only `mbuff` is given; on older servers set it
explicitly:

```
xrootd.monitor all auth flush io 60s fstat 60s lfn ops ssq xfr 10 \
               mbuff 1400 fbsz 1400 rbuff 1400 gbuff 1400 window 15s \
               dest files fstat io info redir user <collector-host>:9930
```

For a 1500-byte MTU the payload limit is 1472 bytes over IPv4 (28 bytes of
headers) but only **1452 over IPv6** (40+8) — so the once-popular `1472`
fragments *every full packet* on an IPv6 path, and IPv6 fragments are widely
dropped by middleboxes. Use 1400 for dual-stack safety. Do not go far below
that: a single fstat record must fit the buffer, and a close record carrying
a long LFN plus an error message can reach several hundred bytes — records
that do not fit are dropped by the server.

**`flush`/`window` bound latency, not loss.** `flush <t>` forces the trace
buffer out even when not full, `fstat <t>` is the file-stats reporting
interval, and `window <t>` is the timestamp granularity inside trace buffers.
Smaller buffers simply flush more often (more, smaller datagrams — fine);
short windows spend buffer slots on window marks, so with a small `mbuff`
prefer `window 15s`-ish over very short windows. None of these cause loss by
themselves; fragmentation and receive-buffer overflow do.

**Reading the collector's loss/malformed metrics.** The server does *not*
stamp one packet sequence number per destination: the `f` stream and each
g-stream provider run their own independent `pseq` counters, while the
trace/redirect/map streams share one. The collector therefore tracks loss per
stream class, and the metrics tell them apart:

- `packets_total{site,server,stream}` — packets received, labeled the same
  `{site,server,stream}` as `packets_lost_total` so the two divide into a loss
  percentage per source (and per stream class):
  `100 * sum by(server)(rate(packets_lost_total[5m]))
  / sum by(server)(rate(packets_total[5m]))`. Malformed packets rejected before
  a stream class is known are not counted here (they land in `malformed_total`),
  so this is the well-formed denominator. `sum(packets_total)` still gives the
  global receive rate.
- `packets_lost_total{site,server,stream}` — pseq gaps, per stream class (`main`,
  `f`, `g:<provider>`). Loss concentrated on `f` with `mbuff`-sized `t`
  packets arriving fine is the fragmentation signature above. Loss across
  *all* streams points at the network or at the collector's receive buffer
  (`rcvbuf`, and `ReceiveBuffer=` in the socket unit — see
  [Running as a service](#running-as-a-service)).
- `malformed_total{server,stream,reason}` — structurally invalid packets:
  `bad_plen`/`short_packet` (truncation or stray traffic on the port),
  `bad_record` (`f`-stream record inconsistencies), `trailing_bytes`
  (`t`-stream payload not a whole number of records), `truncated_string`
  (`r`-stream host:path running past the packet). Run with `--debug` to log a
  diagnostic line (server, stream, reason) for each rejected packet. A
  `xrootd.mongstream ... send json`/`cgi` g-stream (leading `{`) is *not* counted
  here — it is decoded as text g-stream (see above), so it lands in
  `packets_total{stream="g:<provider>"}` like the binary form.
- `unknown_packets_total` — packets with an unhandled stream code (usually
  stray traffic, e.g. a scanner hitting the port).

## Deployment and tuning

### Running as a service

A systemd unit, `xrdmoncollect.service`, runs the collector as the `xrootd`
user reading `/etc/xrootd/xrdmoncollect.cfg`:

```sh
# edit /etc/xrootd/xrdmoncollect.cfg, then:
systemctl enable --now xrdmoncollect
journalctl -u xrdmoncollect -f
```

The unit is non-templated (one collector per host). Extra environment can be
supplied via `/etc/default/xrdmoncollect` or `/etc/sysconfig/xrdmoncollect`. The
default UDP port (9930) is unprivileged; for a port below 1024 add
`CAP_NET_BIND_SERVICE` to the unit's `CapabilityBoundingSet`/`AmbientCapabilities`.

The collector can run **co-located** with a server (the common case: the server
reports from the loopback address and the collector substitutes its own FQDN for
`server.address`, see [WLCG field mapping](#wlcg-field-mapping)) or as a **central**
receiver for many servers, each pointed at `<collector-host>:9930`. For a
central receiver, prefer the [shoveler chain](#shoveler-mode-reliable-tcp-transport)
over long-haul UDP: the same unit file works for a shoveler — its config just
sets `shovel = <collector-host>:<tcp-port>` (plus `cache-dir`) instead of the
sink options, and the central host's config sets `tcp-port`.

### Capacity and tuning

Enable `--metrics-port` and watch the collector's own metrics to size the knobs.
The defaults suit a busy single server; a central collector for many servers may
need larger buffers and a bigger memory budget.

| Symptom (metric) | Knob(s) | Action |
| :-- | :-- | :-- |
| `recv_queue_batches` rides at `--queue-depth`; `packets_lost_total` climbs | `--rcvbuf`, `--queue-depth` | Enlarge the kernel socket buffer and/or the in-flight batch queue so bursts are absorbed instead of dropped. |
| Too many small POSTs, or POST latency too high | `--flush-count`, `--flush-secs` | Larger/longer flush windows trade freshness for fewer, bigger requests; smaller windows lower end-to-end latency. |
| `state_bytes` near `--max-memory`; `evicted_total` climbing | `--max-memory`, `--max-entries`, `--server-ttl` | Raise the budget to cover the working set (≈ concurrent open files × incarnations); shorten the TTL to reclaim dead incarnations sooner. |
| `post_failures_total` / `otlp_failures_total`, growing `cache_files` | `--cache-dir` (+ downstream) | Ensure a cache dir is set so failures spool to disk instead of dropping; investigate the sink. Watch `dropped_bulk_total` for actual loss. |

Backpressure is intentional: if a sink stalls, the POST queue fills, the
serializer slows, and finally the receiver relies on `--rcvbuf` to ride out the
gap. Size `--rcvbuf` and `--cache-dir` for the longest sink outage you must
survive without loss.

## Consuming the data

### OpenSearch index / data stream

`--os-index` names either a rolling index or a **data stream**. A data stream is
the recommended shape for this append-only, time-series data: pass
`--os-datastream` so the collector uses the `_bulk` `create` action (data
streams reject `index`) and relies on the `@timestamp` every document carries.

A composable index template with an explicit mapping for the dotted
semantic-convention field names (the `resource.*`, `attributes.*`,
`xrootd.*`, and `wlcg.*` keys) is provided in
[`opensearch-template.json`](opensearch-template.json)
(IPs as `ip`, byte counters as `long`, identifiers as `keyword`, strings mapped
to `keyword` by default rather than analyzed `text`). Apply it once before
ingesting; it also creates the data stream backing the `xrootd-transfers` name:

```sh
curl -s -H 'Content-Type: application/json' \
     -XPUT https://opensearch:9200/_index_template/xrootd-transfers \
     --data-binary @opensearch-template.json
```

Drop the `data_stream` block from the template (and omit `--os-datastream`) to
use a plain rolling index with an ISM rollover policy instead.

### OpenSearch Dashboards

A ready-to-import OpenSearch Dashboards saved-objects file is provided in
[`opensearch-dashboards.ndjson`](opensearch-dashboards.ndjson): an
`xrootd-transfers*` index pattern plus a *XRootD Transfers (xrdmoncollect)*
dashboard built on the log records — throughput over time, read/write
rates, VO / auth-method / locality breakdowns, error categories, transfer
duration distribution, and top files/users/sites. Import it under **Dashboards
Management → Saved Objects → Import** (or via the API):

```sh
curl -s -u admin:secret -H 'osd-xsrf: true' \
     -XPOST 'https://dashboards:5601/api/saved_objects/_import?overwrite=true' \
     --form file=@opensearch-dashboards.ndjson
```

Traces (the `--spans` OTLP stream) are not shown here: their natural home is the
tracing backend the OTLP collector feeds (e.g. Tempo or Jaeger), which render
the `traceId`/`spanId` correlation as trace waterfalls.

### Loki / Grafana

The same log records can drive a Grafana dashboard backed by
[Grafana Loki](https://grafana.com/oss/loki/) instead of OpenSearch. Point the
OTLP sink at Loki's OTLP endpoint (optionally through an OpenTelemetry Collector
or Grafana Alloy in between):

```sh
xrdmoncollect -p 9930 --otlp-url http://loki:3100/otlp
```

`xrdmoncollect` appends `/v1/logs` to the OTLP URL, matching Loki's OTLP ingest
path. This requires **Loki ≥ 3.0** with structured metadata and OTLP ingestion
enabled (both on by default in 3.x). Loki promotes only a small set of *resource*
attributes to stream labels — for our records `service.name` (always `xrootd`)
and `service.instance.id` — and stores everything else, including all event
attributes, as **structured metadata** with dots rewritten to underscores. So the
OpenSearch field `attributes.xrootd.operation.name` becomes the queryable label
`xrootd_operation_name`, and a typical query reads:

```logql
sum by (xrootd_operation_name) (
  count_over_time({service_name="xrootd"} | xrootd_operation_state=~".+" [$__auto])
)
```

`xrootd_operation_state` is set by exactly the close, error and redirect
documents, which makes its presence the way to select concluded operations as a
family now that each names its own operation.

A ready-to-import dashboard is provided in
[`grafana-loki-dashboard.json`](grafana-loki-dashboard.json) — the same panels as
the OpenSearch dashboard (throughput, VO / auth / locality / state
breakdowns, error categories, top files/users/sites, sessions). Import it under
**Grafana → Dashboards → New → Import** and select your Loki data source for the
`DS_LOKI` input. Two panels are approximations, because LogQL lacks the matching
aggregation: *Distinct clients* (no native count-distinct) and *Transfer duration
quantiles* (p50/p90/p99, standing in for OpenSearch's fixed-bucket duration
histogram).

### Data popularity dashboards

Dataset popularity — which datasets are read, how much, by whom — has been a
long-standing request from the experiments (CMS in particular, for dynamic
data placement and cache/cleanup decisions). Two ready-to-import dashboards
present the same visualizations on each stack, built entirely from the
transfer log records:

- [`opensearch-popularity.ndjson`](opensearch-popularity.ndjson) — *XRootD
  Data Popularity (xrdmoncollect)* for OpenSearch Dashboards; import exactly
  like the transfers dashboard above (it reuses the same `xrootd-transfers`
  index pattern, so importing both files with `overwrite=true` is fine).
- [`grafana-loki-popularity-dashboard.json`](grafana-loki-popularity-dashboard.json)
  — *XRootD Data Popularity (Loki)* for Grafana on Loki; import and select
  the Loki data source for `DS_LOKI`.

Both follow the same three-zone layout: top-level stats (total bytes read,
active users, distinct datasets/directories), stacked read-volume time series
(by SciTags experiment, by VO, by client site, and transfers vs accesses),
and top-10 leaderboards (datasets by bytes and by accesses, users by bytes,
client sites) with a dataset-activity-over-time panel to catch datasets
suddenly becoming popular.

The dataset dimension comes from the collector's
[`--dataset` capture](#dataset-capture---dataset); without it the
dataset panels stay empty while the directory/user/VO panels still work. The
Loki variant groups by structured metadata, which is comparatively expensive
over long ranges — prefer hours-to-days ranges there (OpenSearch aggregations
handle longer windows better). Collector-health and aggregate-rate panels
stay in the Prometheus dashboard ([below](#aggregated-metrics-prometheus));
the Prometheus metrics deliberately carry no per-dataset labels (unbounded
cardinality), so popularity is a log-analytics concern.

### Aggregated metrics (Prometheus)

With `--metrics-port <p>` the collector also runs a small HTTP exporter that
serves Prometheus metrics aggregated from the decoded records. Unlike the
per-file documents (which belong in a document store), these are bounded in
cardinality and suitable for a time-series database.

Every per-server series carries `{site, server}`. Both come from the `=`
identity record, so both are provisional until it arrives — see
[Site and server labels](#site-and-server-labels) below.

I/O, per site and server:

```
xrootd_collector_io_total{site,server,operation="open|close|read|readv|write"}
xrootd_collector_io_bytes_total{site,server,operation="read|readv|write"}
xrootd_collector_errors_total{site,server,category="open|read|write|close|auth|unknown"}
xrootd_collector_app_io_bytes_total{site,app,operation="read|readv|write"}
```

`rate(io_bytes_total)` is bandwidth and `rate(io_total)` is IOPS, both
`sum by (site)` for a whole cluster. The two are not equally available: the
bytes come from the fstat `xfr` block and are always present, while
`io_total`'s `read`/`readv`/`write` counts come from the optional `ops` block
and only move when the server config includes it. `open` and `close` always
tick. `app_io_bytes_total` attributes the same volumes to the client
application (the `i`-stream appid when the site sets one, else the login's
`&x=` executable) and deliberately carries no `server` label — `app × server`
is the one label product here with real cardinality risk.

Live state and identity:

```
xrootd_collector_files_open{site,server}       (gauge)
xrootd_collector_sessions_open{site,server}    (gauge)
xrootd_collector_servers{site}                 (gauge: servers reporting)
xrootd_collector_server_info{site,server,ip,instance_name,program,version}  (gauge, always 1)
```

`servers{site}` counts distinct servers, not incarnations, so a server that
restarted is one server while its old incarnation waits out the TTL; a site
whose last server went away is parked at zero. `server_info` carries the
identity that would otherwise have to be a label on every series — which EOS
instance a node belongs to (`instance_name`, from `&inst=`; not `instance`,
which Prometheus stamps itself at scrape time) and which XRootD release it
runs. Retired incarnations are parked at zero there too, so
`count by (site) (server_info)` can outrun `servers{site}` after an upgrade:
`servers{site}` is the live count.

Sessions:

```
xrootd_collector_sessions_total{site,server}    (sessions ended)
xrootd_collector_session_starts_total{site,server,source="login|connect|first_activity|disconnect"}
xrootd_collector_session_duration_seconds{site} (histogram)
```

Read the duration histogram next to `session_starts_total`: a guessed
`disconnect` start makes the duration zero, so the two together say how much
of the distribution is real. `sessions_open` counts live sessions from the
user dictionary, so it stays at zero unless the server's monitor config
includes a `user` destination.

Collector health:

```
xrootd_collector_documents_total{site}      (documents emitted, after filtering)
xrootd_collector_filtered_documents_total   (documents suppressed by a [filter] rule)
xrootd_collector_stale_opens_total{site,server}  (opens dropped: close lost)
xrootd_collector_orphan_closes_total{site,server} (closes without an open)
xrootd_collector_packets_total{site,server,stream}
xrootd_collector_packets_lost_total{site,server,stream}
xrootd_collector_malformed_total{site,server,stream,reason}
xrootd_collector_unknown_packets_total      (packets with an unhandled stream code)
xrootd_collector_disconnects_total          (f-stream session disconnect records)
xrootd_collector_evicted_total              (entries evicted by the memory budget)
xrootd_collector_reaped_servers_total       (incarnations reclaimed by --server-ttl)
xrootd_collector_state_bytes                (gauge: resident correlation state)
xrootd_collector_recv_queue_batches         (gauge: receiver->serializer depth)
```

Records decoded, one counter per stream, all unlabelled:

```
xrootd_collector_trace_records_total        (t-stream)
xrootd_collector_gstream_records_total      (g-stream)
xrootd_collector_redirect_records_total     (r-stream)
xrootd_collector_frm_records_total          (x/p streams)
xrootd_collector_token_records_total        (T-stream)
xrootd_collector_ident_records_total        (=-stream)
```

Sink health. The OpenSearch sink:

```
xrootd_collector_post_queue_bodies          (gauge: bodies awaiting the POST)
xrootd_collector_post_failures_total        (OpenSearch _bulk POST failures)
xrootd_collector_cache_files                (gauge: cached bodies awaiting replay)
xrootd_collector_cache_bytes                (gauge: bytes of cached bodies)
xrootd_collector_cache_stored_total         (bodies written to the disk cache)
xrootd_collector_cache_replayed_total       (cached bodies replayed)
xrootd_collector_dropped_bulk_total         (bodies dropped: no/failed cache)
```

and the OTLP sink, the same shape under its own names:

```
xrootd_collector_otlp_queue_bodies          (gauge: bodies awaiting the export)
xrootd_collector_otlp_failures_total        (OTLP POST failures)
xrootd_collector_otlp_cache_files           (gauge: cached bodies awaiting replay)
xrootd_collector_otlp_cache_bytes           (gauge: bytes of cached bodies)
xrootd_collector_otlp_cache_stored_total    (bodies written to the disk cache)
xrootd_collector_otlp_cache_replayed_total  (cached bodies replayed)
xrootd_collector_otlp_dropped_total         (bodies dropped: no/failed cache)
```

With `--tcp-port`, the shovel listener adds:

```
xrootd_collector_tcp_connections_total      (shovel connections accepted)
xrootd_collector_tcp_connections_active     (gauge: currently open)
xrootd_collector_tcp_auth_failures_total    (rejected by the token check)
xrootd_collector_tcp_frames_total           (shoveled datagrams received)
xrootd_collector_tcp_malformed_total        (connections dropped for protocol violations)
xrootd_collector_tcp_bytes_total            (bytes received on shovel connections)
```

A shoveler (`--shovel`) exposes its own, smaller set under the
`xrootd_shoveler_` prefix instead:

```
xrootd_shoveler_packets_total               (UDP datagrams received)
xrootd_shoveler_frames_sent_total           (datagrams relayed to the collector)
xrootd_shoveler_frames_spooled_total        (datagrams written to the disk spool)
xrootd_shoveler_frames_dropped_total        (datagrams dropped: no usable spool)
xrootd_shoveler_connects_total              (successful connect+handshakes)
xrootd_shoveler_connected                   (gauge: collector connection up)
xrootd_shoveler_recv_queue_batches          (gauge: receiver->sender depth)
xrootd_shoveler_spool_files                 (gauge: buffers awaiting replay)
xrootd_shoveler_spool_bytes                 (gauge: spool bytes on disk)
xrootd_shoveler_spool_stored_total          (buffers written to the spool)
xrootd_shoveler_spool_replayed_total        (buffers replayed)
xrootd_shoveler_spool_dropped_total         (oldest buffers evicted by --spool-max)
```

From the `g` (plugin) streams (when `--gstream` data is flowing):

```
xrootd_collector_oss_ops_total{site,server,op="..."}
xrootd_collector_oss_slow_ops_total{site,server,op="..."}
xrootd_collector_pfc_files_total{site,server}
xrootd_collector_pfc_bytes_total{site,server,source="hit|miss|bypass|disk|prefetch"}
xrootd_collector_tpc_total{site,server,type="push|pull",result="ok|error"}
xrootd_collector_tpc_bytes_total{site,server,type="push|pull"}
xrootd_collector_tpc_size_bytes{site,server}   (histogram)
xrootd_collector_throttle_io_total{site,server}
xrootd_collector_throttle_io_active{site,server}   (gauge)
xrootd_collector_http_requests_total{site,server,method="...",status="..."}
```

From the `x`/`p` (FRM) streams:

```
xrootd_collector_frm_total{site,server,op="transfer|purge"}
xrootd_collector_frm_purge_bytes_total{site,server}
```

`frm_total{op="transfer"}` and the `tpc_*` family are the only places the word
"transfer" survives, because they are the only whole-file transfers the
collector sees. Everything else counts individual I/O operations or file
open→close lifecycles, and is named for what it counts.

#### Site and server labels

`site` is the server's `all.sitename`, and `server` is the same name the
documents carry in `resource.server.address`: the `=` identity record's
advertised host, or the local FQDN for a co-located server, falling back to
the numeric source address with the UDP port stripped. Naming a server
identically in both places is what lets a Grafana panel and an OpenSearch
query talk about the same machine.

Both come from the `=` record, so both flip together when it arrives: before
it a server reads `{site="unknown", server="<ip>"}` and after it
`{site="CERN-PROD", server="fst-096.cern.ch"}`. Nothing merges the two — the
earlier series simply stops advancing, so `rate()` heals itself while
`increase()` across the flip under-reports.

The window is the server's `xrootd.monitor ... ident` interval, **an hour by
default**. Two things shorten it: `--state-file` persists the identity across
a collector restart, and setting `ident 300` in the server's monitor
directive (see [Server configuration](#server-configuration)) brings it down
to five minutes. A site of only dots — `XrdOucSiteName`'s sanitization of an
all-invalid name — carries no information and reads as `unknown`.

Note that when several storage instances share one `all.sitename` (a CERN EOS
deployment puts `eosalice`, `eosatlas`, `eoscms` and `eoslhcb` all under
`CERN-PROD`), `sum by (site)` merges them. Separate them either by giving each
instance its own sitename, or by joining on `server_info`:

```
sum by (instance_name) (
  rate(xrootd_collector_io_bytes_total[5m])
  * on(site, server) group_left(instance_name) xrootd_collector_server_info
)
```

Point a Prometheus scrape job at `http://<collector-host>:<p>/metrics`.

[`grafana-dashboard.json`](grafana-dashboard.json) (next to this README) is a
ready-to-import dashboard covering the metrics above: collector health (ingest/decode rates, correlation
memory, queue depth), sink health (POST failures, drops, queue and disk-cache
backlog for both the OpenSearch and OTLP sinks), shovel transport (collector
TCP connections and shoveled-datagram rates, plus per-shoveler pipeline, spool
backlog, and connectivity — scrape the shovelers' `--metrics-port` with the
same Prometheus), I/O activity per site and server
(throughput, IOPS, open files and sessions, errors by category), and the
`g`/`x`/`p`-stream backends (redirects, TPC, PFC, OSS, HTTP, throttle, FRM).
Import it in Grafana (*Dashboards → New → Import*), then pick the Prometheus
data source that scrapes the collector; **Site** and **Server** variables
multi-select what to show.

## Limitations

- Correlation state is bounded by `--max-memory` / `--max-entries` (see
  [Correlation state](#correlation-state)). A dropped entry merely yields a
  document missing that field, or an orphan close. Evictions are counted in
  `xrootd_collector_evicted_total` and the live budget utilisation is the
  `xrootd_collector_state_bytes` gauge; reclaimed incarnations are counted in
  `xrootd_collector_reaped_servers_total`.
- UDP is lossy: a lost open record yields an orphan close
  (`xrootd_collector_orphan_closes_total{site,server}`); a lost close leaves a
  stale open, reclaimed at that user's disconnect or by `--file-ttl` and
  counted in `xrootd_collector_stale_opens_total{site,server}`; a lost dictionary
  record yields a document without identity/path. The server stamps every
  datagram to one destination with a single sequence number (header `pseq`), so
  the collector estimates loss from forward gaps in it —
  `xrootd_collector_packets_lost_total{site,server}` and the `-v` `lost=` count.
  (Reordering, a small backward step, is not counted as loss. A disconnect
  overtaking its session's final closes can make the sweep drop opens whose
  closes arrive right after, turning them into orphan closes — already their
  fate had the reorder hit the closes directly.) A frequent root cause of
  systematic loss is IP fragmentation of full monitoring packets — buffer
  sizes above 1452 bytes fragment on 1500-MTU IPv6 paths (see
  [NETWORK_TUNING.md](NETWORK_TUNING.md), section 4). The
  [shoveler chain](#shoveler-mode-reliable-tcp-transport) confines this to the
  local hop; its own TCP leg is lossless except for the un-acked kernel-buffer
  window when the collector dies abruptly (see the caveat there).
- Sub-window record times (event times, transfer start/duration, span
  start/end) are linear interpolation estimates over each record's position in
  its reporting window, not measured values; only the window boundaries are on
  the wire (with one-second granularity). Durations of transfers much shorter
  than the flush interval are therefore approximate, but no longer collapse to
  zero.
- The `f` stream drives the transfer correlation state. The `t` (per-I/O trace)
  records reuse that state read-only to stamp each record with its file's
  `traceId`/`spanId` (see `--traces`), but do not themselves build correlation
  entries; the `g` (plugin) stream is decoded enough to be counted and
  optionally emitted, but is not joined into the transfer correlation.
