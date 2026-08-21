# Network Tuning for XRootD Monitoring

Guidance for system administrators on tuning hosts to **minimize UDP packet loss**
in the XRootD monitoring pipeline. XRootD emits monitoring records as UDP
datagrams, and UDP has no retransmission: any datagram dropped at the sender, on
the wire, or at the receiver is lost permanently. The settings below reduce the
probability of loss at each stage of the receive path.

> **Scope.** This document targets the host running the **monitoring collector**
> (the receiver), which is where the overwhelming majority of loss occurs. A
> shorter section covers the **XRootD server** (the sender). Examples assume a
> RHEL-family system (AlmaLinux / Rocky / RHEL) using `systemd`, `sysctl.d`, and
> NetworkManager, but the kernel parameters are distribution-independent.

---

## 0. How to use this document

Three questions, in order. Answering them out of order wastes effort, because
the fixes live at different layers and only one of them is usually the problem.

1. **Am I losing packets at all?** The collector tells you, and it is the only
   thing that can: it compares the sequence numbers the *sender* stamped.
   Section 9.
2. **At which layer?** Kernel counters localize a loss to this host; collector
   counters cannot, but they see loss on the wire and at the sender that the
   kernel never will. You need both. Sections 1 and 10.
3. **Is tuning the right answer?** If the collector is a long way from the
   servers, no amount of buffer will fix a lossy WAN path, and the structural
   answer is to stop sending monitoring over it. Section 7.

**Where things are documented.** This file owns host, kernel, NIC and socket
tuning. [`DEPLOY.md`](DEPLOY.md) owns which configuration lines to write and how
to stand up the backend. [`README.md`](README.md) owns why the collector behaves
as it does — its threads, queues and memory budget. `man 8 xrdmoncollect` is the
option reference, and its *Tuning for pipeline resilience* section is the
authoritative statement of the server-side buffer rules summarized in Section 4.

---

## 1. Where UDP monitoring packets are lost

Tuning the wrong layer wastes effort, so start by understanding the path a
datagram takes and where each drop is counted:

```
 xrootd ─▶ wire ──▶ NIC RX ring ──▶ per-CPU backlog ──▶ socket buffer ──▶ recv() ─▶ decoder
   (0)                  (1)               (2)                (3)                      (4)
```

| # | Layer | Drops when… | Counter to watch |
|---|-------|-------------|------------------|
| 0 | Sender's record buffer | a record does not fit the stream buffer (`mbuff`, `fbsz`, `rbuff`, `gbuff`), or a full datagram is fragmented and one fragment is lost | collector: `packets_lost_total{…,stream}`; kernel on either side: `IpReasmFails` |
| 1 | NIC RX ring buffer | packets arrive faster than softirq/NAPI drains the ring | `ethtool -S`: `rx_dropped`, `rx_missed_errors`, `rx_no_buffer_count` |
| 2 | Per-CPU backlog queue | the input queue between softirq and the socket overflows | `/proc/net/softnet_stat` col 2 (drops), col 3 (budget squeeze) |
| 3 | Socket receive buffer | the collector cannot `recv()` fast enough and `SO_RCVBUF` fills | `nstat`: `UdpRcvbufErrors`; `/proc/net/udp` drops column |
| 4 | The collector's own receive queue | the *decoder* falls behind; the receiver waits for a free batch, stops draining the socket, and loss reappears at layer 3 | `xrootd_collector_recv_queue_batches` riding at `--queue-depth` |

**The socket receive buffer (layer 3) is the most common loss point for a
monitoring collector.** Layer 4 is the one that looks like layer 3 and is not:
the socket buffer fills, but enlarging it only buys time, because the collector
is CPU-bound rather than burst-limited. Diagnose which layer is actually
dropping (Section 10) before changing anything.

Note that layers 3 and 4 are the *only* ones a slow **sink** can reach, and it
cannot: everything downstream of the collector's decoder sheds its own oldest
data rather than applying backpressure, precisely so that a struggling
OpenSearch or a dead central collector cannot turn into UDP loss. See
[`README.md`](README.md) § *Flow control and queues*.

---

## 2. Socket receive buffer — the primary lever

A crucial property: **UDP does not auto-tune its receive buffer.** TCP grows
`net.ipv4.tcp_rmem` dynamically; UDP uses a fixed `SO_RCVBUF` bounded by
`net.core.rmem_max`. Tuning `tcp_rmem` does **nothing** for the monitoring
stream (it does matter for the shoveler's TCP leg — Section 8).

### 2.1 `rmem_max` is a ceiling, not an allocation

```ini
# Ceiling for what an unprivileged socket may obtain via setsockopt(SO_RCVBUF)
net.core.rmem_max = 134217728        # 128 MiB
net.core.rmem_default = 33554432     # 32 MiB

# Minimum receive buffer guaranteed per UDP socket even under global memory pressure
net.ipv4.udp_rmem_min = 8192

# Global UDP memory budget in pages: min / pressure / max.
# Normally auto-sized to RAM; verify it is not the constraint on a busy collector.
net.ipv4.udp_mem = 12148128 16197504 24296256
```

Raising `rmem_max` does not consume memory; it only permits a socket to obtain a
larger buffer. Left at its default (often around 208 KiB) it truncates the
collector's request and bursts are dropped no matter what the collector does.

### 2.2 What the collector actually requests

`--rcvbuf` (default `16M`) is attempted twice:

1. **`SO_RCVBUFFORCE`**, which bypasses `rmem_max` — but requires
   `CAP_NET_ADMIN`. The bundled `xrdmoncollect.service` runs as the `xrootd`
   user with an empty `CapabilityBoundingSet=`, so this call fails there by
   design.
2. **`SO_RCVBUF`**, which succeeds and is **silently capped at `rmem_max`**.

So in the shipped configuration the ceiling does apply, and an administrator who
sets `rcvbuf = 64M` against a default `rmem_max` gets 208 KiB and no warning.
Either raise `rmem_max` (§2.1) or use socket activation (§2.3).

**The kernel doubles the request.** `setsockopt(SO_RCVBUF, N)` yields an
effective buffer of `2N` — the extra half is accounting overhead — capped at
`2 × rmem_max`, and it is the doubled figure that `ss` reports. Do not read a
`rb32000000` next to a `rcvbuf = 16M` as a mistake.

### 2.3 Socket activation and `ReceiveBuffer=`

The bundled `xrdmoncollect.socket` is the supported way past `rmem_max` without
raising it system-wide:

```ini
# /usr/lib/systemd/system/xrdmoncollect.socket  (excerpt)
ListenDatagram=9930
#ListenStream=9931        # uncomment on a central collector accepting shovelers
ReceiveBuffer=16M
BindIPv6Only=both
```

```sh
systemctl enable --now xrdmoncollect.socket
```

systemd creates the sockets **as root**, before dropping privileges, so it uses
`SO_RCVBUFFORCE` and `ReceiveBuffer=` is **not** capped by `rmem_max`. The
daemon then inherits the ready socket through the `LISTEN_FDS` protocol and
never calls `setsockopt` on it at all.

Raise it with a drop-in rather than editing the shipped unit:

```sh
systemctl edit xrdmoncollect.socket
# [Socket]
# ReceiveBuffer=64M
systemctl restart xrdmoncollect.socket xrdmoncollect.service
```

There is a second, independent benefit that has nothing to do with buffer size:
**systemd holds the sockets across a restart**. Datagrams queue in the kernel
receive buffer and shoveler connections queue in the listen backlog while the
daemon is not running, so a `systemctl restart`, an upgrade, or a crash-restart
loses nothing that fits in the buffer. This is a different failure mode from
everything else in this document — it is not a rate problem — and it is the only
mitigation for it. (It is the same technique HAProxy uses for seamless reloads,
simplified because systemd can own the sockets outright.)

The ports in the socket unit must match `udp-port`/`tcp-port` in
`/etc/xrootd/xrdmoncollect.cfg`. On a mismatch the daemon simply binds its own
sockets and only the zero-loss property is quietly lost.

> **Containers have no socket activation.** Under podman, docker or Kubernetes
> the daemon binds its own socket, so the **host's** `net.core.rmem_max` is the
> real ceiling unless the container is granted `CAP_NET_ADMIN`. A `rcvbuf = 16M`
> in a container config on an untuned host is silently truncated. Apply §11.1 to
> the host, not to the image. See [`DEPLOY.md`](DEPLOY.md) §4.2 for the
> package-track commands.

### 2.4 Sizing, and verifying it took effect

The buffer must absorb the worst-case burst that arrives while the collector is
momentarily not draining (scheduling delay, a control tick, a disk flush).
XRootD buffers records on the server and flushes them on an interval, so many
servers can flush almost simultaneously. As a rule of thumb, provide headroom for
several flush intervals' worth of `N_servers × mbuff`. In practice, set 32–128 MiB
and let the drop counters (Section 10) tell you whether it is enough — do not
over-engineer the exact figure.

```bash
ss -uanmp
# In the skmem field: r<bytes-used> and rb<buffer-size>.
# rb should be roughly twice what was asked for (§2.2). If it is ~416 KiB
# instead, rmem_max capped the request: either the socket unit is not in use,
# or rmem_max is still at its default.
```

---

## 3. Backlog queue and softirq budget

At high packet-per-second rates (many servers, or detailed `io` monitoring
enabled), the bottleneck can move upstream of the socket into the kernel's
receive processing:

```ini
net.core.netdev_max_backlog = 30000   # default 1000 — far too small for 10G-class NICs
net.core.netdev_budget = 600          # packets processed per NAPI poll (default 300)
net.core.netdev_budget_usecs = 8000
```

Interpret `/proc/net/softnet_stat` (values are hex, one row per CPU):

- **Column 2** rising → backlog overflow → raise `netdev_max_backlog`.
- **Column 3** (`time_squeeze`) rising → NAPI exhausted its budget before draining
  the ring → raise `netdev_budget` / `netdev_budget_usecs`.

---

## 4. MTU and fragmentation

This is the point where host tuning and XRootD configuration interact, and it is
the subtlest lever for minimizing loss specifically.

XRootD sends each monitoring report as **a single UDP datagram**, whose payload
size is set per stream by `mbuff` (trace), `fbsz` (fstat), `rbuff` (redirect)
and `gbuff` (g-stream) on the `xrootd.monitor` directive. On a standard
1500-byte MTU, the usable UDP payload is only **1472 bytes** over IPv4
(1500 − 20 IP − 8 UDP) and **1452 bytes over IPv6** (1500 − 40 IPv6 − 8 UDP). A
larger buffer therefore forces the datagram to be **IP-fragmented** into
multiple frames.

> **The `fbsz` trap.** Before XRootD 6.1, `fbsz` does **not** follow `mbuff`:
> setting `mbuff 1472` alone still emits fstat datagrams at the `fbsz` default of
> **65472 bytes** — roughly 44 fragments each, of which losing any one drops the
> whole datagram. That is precisely the stream the transfer documents are built
> from. Set `fbsz` explicitly on such servers.

> **IPv6 pitfall:** buffer sizes chosen against the IPv4 limit — `1472` is a
> popular choice — exceed the IPv6 payload limit by 20 bytes, so on an IPv6
> path *every full packet* is fragmented into a 1452-byte fragment plus a tiny
> trailer fragment. IPv6 routers never fragment in-network and middleboxes
> widely drop IPv6 fragments, so this configuration silently loses a large
> share of the busiest (fullest) monitoring packets. Use ≤ 1400 for dual-stack
> safety.

Do not go far *below* the MTU either: a single fstat record — a close with a long
LFN and an error message — must fit the buffer, or the server drops it at
layer 0.

**The signature in the collector.** Fragmentation loss is not uniform across
streams, and that is what makes it identifiable:

- `packets_lost_total{stream="f"}` climbing while `stream="main"` stays clean —
  the fstat datagrams are the big ones. This is the fragmentation signature.
- `orphan_closes_total` climbing (an open record was lost) and
  `stale_opens_total` climbing (a close was lost, reclaimed later at the
  client's disconnect), with `files_open` drifting up in between.
- Kernel `IpReasmFails` rising on either host, which is the same event seen from
  the other side and the strongest possible confirmation.

Fragmentation hurts a lossy transport in two compounding ways:

- **Loss amplification.** If *any single fragment* is dropped, IP reassembly
  fails and the *entire* datagram is discarded. A modest fragment-loss rate
  translates into a much higher datagram-loss rate.
- **Reassembly pressure.** Incomplete datagrams occupy the reassembly cache
  (`net.ipv4.ipfrag_high_thresh`) until they complete or the `ipfrag_time`
  timeout (30 s) expires, which can itself cause drops under load.

Two coherent strategies — choose based on the network you control:

**Strategy A — no fragmentation (recommended for a pure loss-minimization goal
on standard networks).** Keep each datagram within one frame by setting every
stream buffer below the payload limit (≈1400 on 1500 MTU, leaving headroom and
staying under the 1452-byte IPv6 limit on dual-stack paths). Cost: more packets
per second and higher softirq load, but every lost packet loses only one small
record buffer, with zero fragment amplification.

**Strategy B — jumbo frames end-to-end.** With MTU 9000, an 8 KiB `mbuff`
datagram fits in a single frame, giving both packing efficiency and no
fragmentation. This requires jumbo support on **every** hop (NICs, switches,
routers) between server and collector. A single non-jumbo hop with the
Don't-Fragment bit set causes PMTUD blackholes; without DF you are back to
fragmentation. Only viable on a fully controlled, same-datacenter path.

If you must run large stream buffers on a 1500-MTU network (a common default),
raise the reassembly thresholds so fragments survive bursts:

```ini
net.ipv4.ipfrag_high_thresh = 16777216   # 16 MiB
net.ipv4.ipfrag_low_thresh  = 12582912   # 12 MiB
```

Watch `nstat -az | grep -i Reasm` for `IpReasmFails`; a rising count is a direct
signal that fragmentation is costing you datagrams.

---

## 5. NIC-level tuning

```bash
# Inspect current vs. maximum RX ring, then raise the RX ring toward its max
# to absorb short bursts in hardware
ethtool -g <iface>
ethtool -G <iface> rx 4096

# Ground truth for ring overflow
ethtool -S <iface> | grep -E 'drop|miss|fifo|no_buf'
```

**Single-flow steering caveat.** If one XRootD server sends to one collector port,
receive-side scaling (RSS) may hash that entire flow onto a **single RX queue and
a single CPU**, so one core performs all softirq work regardless of core count. If
`ethtool -S` shows drops while most CPUs sit idle, enable **Receive Packet
Steering (RPS)** to spread softirq processing in software:

```bash
# Spread rx-queue 0 across a set of CPUs (hex mask; example: CPUs 0–7 = ff)
echo ff > /sys/class/net/<iface>/queues/rx-0/rps_cpus
```

Additionally, pin NIC IRQs to cores on the same NUMA node as the collector
process, and keep the collector on nearby cores to preserve cache and memory
locality.

---

## 6. Sender side (XRootD server host)

Little is required here — a sender rarely drops its own monitoring — but for
completeness ensure the send path is not the constraint:

```ini
net.core.wmem_max = 33554432
net.core.wmem_default = 16777216
```

On the `xrootd.monitor` directive, `flush`, `window`, and the per-stream buffer
sizes govern how bursty emission is. Longer `flush` / `window` intervals with
matched buffers produce fewer, fuller packets (lower pps, but each lost packet
costs more records); shorter intervals produce more, smaller packets. If
near-real-time reporting is not required, larger intervals reduce traffic — but
weigh that against the fragmentation trade-off in Section 4.

`man 8 xrdmoncollect`, *Tuning for pipeline resilience*, is the authoritative
statement of the buffer rules and is kept in step with the server. See also
[`DEPLOY.md`](DEPLOY.md) §2.1 for a complete worked `xrootd.monitor` stanza.

---

## 7. When tuning runs out: shoveler mode

Everything above assumes the loss is on this host and is a rate problem. If the
collector is not on the same LAN as the servers, it very likely is not. A WAN
path has loss you cannot buffer your way out of, and monitoring UDP is exactly
the traffic class a congested link discards first.

The structural fix, modeled on the OSG shoveler, is to **stop sending monitoring
over the long path**:

```
 node A ─ xrootd ─ UDP(loopback) ─▶ xrdmoncollect --shovel ──┐
 node B ─ xrootd ─ UDP(loopback) ─▶ xrdmoncollect --shovel ──┤ XSHV over TCP
 node C ─ xrootd ─ UDP(loopback) ─▶ xrdmoncollect --shovel ──┤
                                                             ▼
                             central xrdmoncollect --tcp-port [-p …]
```

Run one `xrdmoncollect --shovel` next to every daemon and point that daemon's
`xrootd.monitor … dest` at `localhost`. The shoveler does not decode: it
encapsulates each datagram together with its **original source address** and
streams it to the central collector's `--tcp-port`, which re-injects it into the
normal pipeline as though it had arrived by UDP. Per-server incarnation state,
`server.address` resolution and `pseq`-gap loss estimation all keep working —
and now they measure only the local hop.

While the central collector is unreachable, the shoveler spools frames to disk
under `--cache-dir` (bounded by `--spool-max`, default `1G`, evicting oldest)
and replays them oldest-first on reconnect. That is durability the UDP path
cannot offer at all.

**What it does not fix.** The loopback hop is still UDP, so a busy node still
needs §2 — a shoveler that cannot drain its socket drops exactly as a collector
would. And if the shoveler itself is killed abruptly, whatever was in its kernel
receive buffer is gone; socket activation (§2.3) is what narrows that window.

Deployment details, tokens and unit files are in [`DEPLOY.md`](DEPLOY.md) §1 and
§3.1; the frame layout is in [`README.md`](README.md) § *Shoveler mode*.

---

## 8. The TCP paths: XSHV and the sink POSTs

Once shovelers are in play, part of the pipeline is TCP and obeys entirely
different rules. Unlike UDP, TCP **does** auto-tune its receive buffer, so
`net.ipv4.tcp_rmem` matters here and `net.core.rmem_max` does not cap it.

**Inbound (the `--tcp-port` listener).**

```ini
net.core.somaxconn = 1024              # ceiling for the listen() backlog
net.ipv4.tcp_rmem = 4096 131072 16777216
```

The listener uses a backlog of 64 and enforces a hard cap of **512 concurrent
connections**; beyond that, a new connection is accepted and immediately closed,
so a site with more than 512 shovelers per collector needs a second collector
rather than a larger backlog. Shoveler connections are long-lived and can be
idle for long stretches on a quiet node, so enable keepalives on any path that
crosses a NAT or a stateful firewall — otherwise the middlebox drops the
conntrack entry and the next frame goes into a black hole until the 5 s
reconnect cool-down notices:

```ini
net.ipv4.tcp_keepalive_time = 600
net.ipv4.tcp_keepalive_intvl = 60
net.ipv4.tcp_keepalive_probes = 5
```

**Outbound (the sink POSTs).** Each destination has its own thread and its own
curl handle. With many destinations, or a destination that fails often enough to
churn connections, watch the ephemeral port range and `tcp_fin_timeout`:

```bash
sysctl net.ipv4.ip_local_port_range net.ipv4.tcp_fin_timeout
ss -tan state time-wait | wc -l
```

Note the timing: a POST has a 30 s timeout and up to five attempts, so a body
that is going to fail takes about 165 s to say so. That is why `TimeoutStopSec`
in the unit is 45 s and why the daemon cancels transfers in flight at shutdown —
an endpoint that black-holes packets would otherwise hold the process across its
whole ladder. A destination with `--cache-dir` configured does not run the ladder
on a live body at all: it fails fast and spills.

**The metrics port** is TCP too, and is the one listener with no tuning story:
it binds IPv4 only, ignores `--bind`, serves one connection at a time, and
serializes the whole registry per request. Scrape it at 15–60 s, not at 1 s, and
firewall it (Section 11 and [`DEPLOY.md`](DEPLOY.md) §15).

---

## 9. Collector-side counters

Kernel counters and collector counters answer different questions, and neither
alone is enough:

- **Kernel counters say "this host dropped it."** They are authoritative about
  where, and blind to anything that happened before the packet arrived.
- **Collector counters say "the sender's sequence numbers show a gap."** They
  see loss on the wire and at the sender, and cannot tell you where it happened.

Read them together. Enable `--metrics-port` and scrape it; every series below is
labelled `{cluster, server}` at minimum, so loss is attributable to one source
rather than averaged across the fleet.

| Counter | Says | Read next to |
|---|---|---|
| `packets_lost_total{cluster,server,stream}` | pseq gaps per stream class (`main`, `f`, `g:<provider>`) | `packets_total{…,stream}`, for a per-source loss percentage |
| …concentrated on `stream="f"` | fragmentation of the large fstat datagrams | §4, and `fbsz` on that server |
| …across *all* streams | the network, or this host's receive buffer | `nstat UdpRcvbufErrors`, §2 |
| `malformed_total{cluster,server,stream,reason}` | truncation or stray traffic on the port (`bad_plen`, `short_packet`, `bad_record`, `trailing_bytes`, `truncated_string`) | `--debug`, which logs a line per rejected packet |
| `unknown_packets_total` | unhandled stream codes — usually a scanner hitting the port | — |
| `recv_queue_batches` riding at `--queue-depth` | the **decoder** is the bottleneck, not the network (layer 4) | serializer-thread CPU; `UdpRcvbufErrors` will start rising too |
| `orphan_closes_total`, `stale_opens_total` | the correlation consequence of loss: an open lost, or a close lost | `packets_lost_total`; these are effects, not independent evidence |
| `invalid_utf8_total` | files named in a non-UTF-8 encoding | not packet loss and not a collector fault |

A per-source loss percentage:

```promql
100 * sum by (server) (rate(xrootd_collector_packets_lost_total[5m]))
    / sum by (server) (rate(xrootd_collector_packets_total[5m]))
```

Malformed packets are rejected before their stream class is known, so they are
not in `packets_total` — which makes it the well-formed denominator this
expression wants.

---

## 10. Diagnosis runbook — localize before tuning

Run these on the collector host and read the counters as **rates over time**, not
absolutes (a flat non-zero value is likely from a past incident, not the current
state).

```bash
nstat -az | grep -i udp        # UdpInDatagrams, UdpInErrors, UdpRcvbufErrors, UdpNoPorts
ss -uanmp                      # per-socket: skmem shows buffer used (r…) vs size (rb…)
cat /proc/net/udp              # per-socket drops column and rx_queue
netstat -su                    # "receive buffer errors", "packet receive errors"
cat /proc/net/softnet_stat     # col2 = backlog drops, col3 = budget squeeze
ethtool -S <iface>             # hardware ring drops
nstat -az | grep -i Reasm      # IpReasmFails — fragmentation losses
ip -s -s link show <iface>     # interface RX errors/dropped
curl -s localhost:9932 | grep -E 'packets_(lost_)?total|recv_queue'   # the collector's view
```

Decision tree:

| Symptom | Layer | Action |
|---------|-------|--------|
| `UdpRcvbufErrors` rising | Socket buffer full (3) | Raise `rmem_max` or adopt socket activation (§2.3); confirm via `ss -uanmp` that the buffer took effect. If it is already large, the collector is draining too slowly — check the next row |
| `recv_queue_batches` at `--queue-depth`, with `UdpRcvbufErrors` following | The decoder (4) | The collector is CPU-bound, not burst-limited. Raise `--queue-depth` for headroom, but the real fixes are fewer document streams (`--traces`, `--gstream`) or a second collector |
| `softnet_stat` col 2 rising | Backlog overflow (2) | Raise `netdev_max_backlog` |
| `softnet_stat` col 3 rising | Softirq budget exhausted (2) | Raise `netdev_budget` / `netdev_budget_usecs` |
| `ethtool` `rx_dropped`/`missed` rising, CPUs idle | NIC ring / single-CPU softirq (1) | Raise RX ring; enable RPS; fix IRQ affinity |
| `IpReasmFails` rising, loss concentrated on `stream="f"` | Fragmentation (0) | Set `fbsz` explicitly (Strategy A) or adopt jumbo frames (Strategy B); raise `ipfrag_*_thresh` as a stopgap |
| `packets_lost_total` rising with every kernel counter flat | Not this host | The loss is on the wire or at the sender. If the path is a WAN, go to §7 |

---

## 11. Recommended baseline configuration

### 11.1 Kernel parameters (persistent)

Create a drop-in so the settings persist and version cleanly in the deploy repo:

```ini
# /etc/sysctl.d/99-xrootd-udp.conf   — collector host
net.core.rmem_max = 134217728
net.core.rmem_default = 33554432
net.core.netdev_max_backlog = 30000
net.core.netdev_budget = 600
net.core.netdev_budget_usecs = 8000
net.ipv4.udp_rmem_min = 8192
net.ipv4.ipfrag_high_thresh = 16777216
net.ipv4.ipfrag_low_thresh  = 12582912

# only if shovelers feed this host (§8)
net.core.somaxconn = 1024
net.ipv4.tcp_keepalive_time = 600
```

Apply without reboot:

```bash
sudo sysctl --system
```

This drop-in is required on the **container host** for any containerized
collector, since socket activation is unavailable there (§2.3).

### 11.2 Socket unit drop-in (native installs)

```bash
sudo systemctl edit xrdmoncollect.socket
```

```ini
[Socket]
ReceiveBuffer=64M
# ListenStream=9931        # a central collector accepting shovelers
```

```bash
sudo systemctl restart xrdmoncollect.socket xrdmoncollect.service
```

With this in place `rmem_max` no longer bounds the collector, though §11.1 is
still worth applying for the backlog and fragmentation settings.

### 11.3 NIC ring buffer (persistent)

The `ethtool -G` setting does not survive a reboot on its own. On
NetworkManager-managed systems, persist it as a connection property:

```bash
nmcli connection modify <conn-name> ethtool.ring-rx 4096
nmcli connection up <conn-name>
```

### 11.4 RPS (persistent, only if single-CPU softirq saturation is observed)

RPS masks reset on reboot and on link events, so set them from a small `systemd`
oneshot unit:

```ini
# /etc/systemd/system/xrootd-rps.service
[Unit]
Description=Configure RPS for XRootD monitoring NIC
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo ff > /sys/class/net/<iface>/queues/rx-0/rps_cpus'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now xrootd-rps.service
```

---

## 12. Validation checklist

1. `sudo sysctl --system` applied; re-read values with `sysctl net.core.rmem_max`.
2. `ss -uanmp` on the live collector socket shows the expected large `rb` buffer —
   remembering that the kernel doubles the request (§2.2). If it shows ~416 KiB,
   the request was capped: check whether the socket unit is actually in use with
   `systemctl status xrdmoncollect.socket`.
3. Generate representative load (real traffic or a UDP generator at expected pps).
4. Watch `nstat`, `ss -uanmp`, `ethtool -S`, and `softnet_stat` as **rates**,
   alongside `packets_lost_total` and `recv_queue_batches` from the collector.
5. Raise only the specific parameter tied to the counter that is moving; re-test.
6. If `IpReasmFails` appears, resolve the MTU/buffer relationship (Section 4)
   rather than masking it with larger reassembly buffers.
7. **Restart the collector under load** and confirm `packets_lost_total` does not
   step. Without socket activation it will; with it, anything that fits in the
   buffer survives. This is the one check that exercises §2.3 rather than
   assuming it.

> The numeric values here are a sane starting point, not a target. The correct
> values depend on server count, per-server rate, whether detailed `io`
> monitoring is enabled, and the path MTU. The drop counters are the source of
> truth — tune against them.
