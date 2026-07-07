# Network Tuning for XRootD Monitoring

Guidance for system administrators on tuning hosts to **minimize UDP packet loss**
in the XRootD monitoring pipeline. XRootD emits monitoring records as UDP
datagrams, and UDP has no retransmission: any datagram dropped at the sender, on
the wire, or at the receiver is lost permanently. The settings below reduce the
probability of loss at each stage of the receive path.

> **Scope.** This document targets the host running the **monitoring collector**
> (the receiver), which is where the overwhelming majority of loss occurs. A
> shorter section at the end covers the **XRootD server** (the sender). Examples
> assume a RHEL-family system (AlmaLinux / Rocky / RHEL) using `systemd`,
> `sysctl.d`, and NetworkManager, but the kernel parameters are distribution-independent.

---

## 1. Where UDP monitoring packets are lost

Tuning the wrong layer wastes effort, so start by understanding the path a
datagram takes and where each drop is counted:

```
 wire ──▶ NIC RX ring ──▶ per-CPU backlog ──▶ socket receive buffer ──▶ collector recv()
             (1)               (2)                    (3)
```

| # | Layer | Drops when… | Counter to watch |
|---|-------|-------------|------------------|
| 1 | NIC RX ring buffer | packets arrive faster than softirq/NAPI drains the ring | `ethtool -S`: `rx_dropped`, `rx_missed_errors`, `rx_no_buffer_count` |
| 2 | Per-CPU backlog queue | the input queue between softirq and the socket overflows | `/proc/net/softnet_stat` col 2 (drops), col 3 (budget squeeze) |
| 3 | Socket receive buffer | the collector cannot `recv()` fast enough and `SO_RCVBUF` fills | `nstat`: `UdpRcvbufErrors`; `/proc/net/udp` drops column |

**The socket receive buffer (layer 3) is the most common loss point for a
monitoring collector.** Diagnose which layer is actually dropping (Section 7)
before changing anything.

---

## 2. Socket receive buffer — the primary lever

A crucial property: **UDP does not auto-tune its receive buffer.** TCP grows
`net.ipv4.tcp_rmem` dynamically; UDP uses a fixed `SO_RCVBUF` bounded by
`net.core.rmem_max`. Tuning `tcp_rmem` does **nothing** for the monitoring stream.

The XRootD collector requests a large `SO_RCVBUF` on its listening socket. The
kernel **silently caps that request at `net.core.rmem_max`**, so the
administrator's job is to raise the ceiling high enough to grant what the
collector asks for. If `rmem_max` is left at its default (often ~208 KiB), the
collector's request is truncated and bursts are dropped no matter what the
collector does.

```ini
# Ceiling for what a socket may obtain via setsockopt(SO_RCVBUF)
net.core.rmem_max = 134217728        # 128 MiB
net.core.rmem_default = 33554432     # 32 MiB

# Minimum receive buffer guaranteed per UDP socket even under global memory pressure
net.ipv4.udp_rmem_min = 8192

# Global UDP memory budget in pages: min / pressure / max.
# Normally auto-sized to RAM; verify it is not the constraint on a busy collector.
net.ipv4.udp_mem = 12148128 16197504 24296256
```

Two behaviours to keep in mind:

- **`rmem_max` is a ceiling, not an allocation.** Raising it does not consume
  memory; it only permits the collector to obtain a larger buffer.
- **The kernel doubles the request.** `setsockopt(SO_RCVBUF, N)` yields an
  effective buffer of `2N` (the extra half is accounting overhead), capped at
  `2 × rmem_max`. Size `rmem_max` to at least the real buffer you intend to grant.

**Sizing.** The buffer must absorb the worst-case burst that arrives while the
collector is momentarily not draining (scheduling delay, GC pause, disk flush).
XRootD buffers records on the server and flushes them on an interval, so many
servers can flush almost simultaneously. As a rule of thumb, provide headroom for
several flush intervals' worth of `N_servers × mbuff`. In practice, set 32–128 MiB
and let the drop counters (Section 7) tell you whether it is enough — do not
over-engineer the exact figure.

**Verify the buffer actually took effect** on the live socket:

```bash
ss -uanmp
# In the skmem field: r<bytes-used> and rb<buffer-size>.
# rb should reflect the large buffer; if it is small, rmem_max is capping the
# collector's request, or the collector requested less than expected.
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
size is set by `mbuff` on the `xrootd.monitor` directive. On a standard 1500-byte
MTU, the usable UDP payload is only **1472 bytes** (1500 − 20 IP − 8 UDP). A
larger `mbuff` (e.g. the common `mbuff 8k`) therefore forces the datagram to be
**IP-fragmented** into multiple frames.

Fragmentation hurts a lossy transport in two compounding ways:

- **Loss amplification.** If *any single fragment* is dropped, IP reassembly
  fails and the *entire* datagram is discarded. A modest fragment-loss rate
  translates into a much higher datagram-loss rate.
- **Reassembly pressure.** Incomplete datagrams occupy the reassembly cache
  (`net.ipv4.ipfrag_high_thresh`) until they complete or the `ipfrag_time`
  timeout (30 s) expires, which can itself cause drops under load.

Two coherent strategies — choose based on the network you control:

**Strategy A — no fragmentation (recommended for a pure loss-minimization goal
on standard networks).** Keep each datagram within one frame by setting `mbuff`
below the payload limit (≈1400 on 1500 MTU, leaving headroom). Cost: more packets
per second and higher softirq load, but every lost packet loses only one small
record buffer, with zero fragment amplification.

**Strategy B — jumbo frames end-to-end.** With MTU 9000, an 8 KiB `mbuff`
datagram fits in a single frame, giving both packing efficiency and no
fragmentation. This requires jumbo support on **every** hop (NICs, switches,
routers) between server and collector. A single non-jumbo hop with the
Don't-Fragment bit set causes PMTUD blackholes; without DF you are back to
fragmentation. Only viable on a fully controlled, same-datacenter path.

If you must run a large `mbuff` on a 1500-MTU network (a common default), raise
the reassembly thresholds so fragments survive bursts:

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

On the `xrootd.monitor` directive, `flush`, `window`, and `mbuff` govern how
bursty emission is. Longer `flush` / `window` intervals with a matched `mbuff`
produce fewer, fuller packets (lower pps, but each lost packet costs more
records); shorter intervals produce more, smaller packets. If near-real-time
reporting is not required, larger `flush` and `mbuff` reduce traffic — but weigh
that against the fragmentation trade-off in Section 4.

---

## 7. Diagnosis runbook — localize before tuning

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
```

Decision tree:

| Symptom | Layer | Action |
|---------|-------|--------|
| `UdpRcvbufErrors` rising | Socket buffer full | Raise `rmem_max`; confirm collector's `SO_RCVBUF` took effect via `ss -uanmp`; if buffer is already large, the collector is draining too slowly |
| `softnet_stat` col 2 rising | Backlog overflow | Raise `netdev_max_backlog` |
| `softnet_stat` col 3 rising | Softirq budget exhausted | Raise `netdev_budget` / `netdev_budget_usecs` |
| `ethtool` `rx_dropped`/`missed` rising, CPUs idle | NIC ring / single-CPU softirq | Raise RX ring; enable RPS; fix IRQ affinity |
| `IpReasmFails` rising | Fragmentation | Shrink `mbuff` (Strategy A) or adopt jumbo frames (Strategy B); raise `ipfrag_*_thresh` as a stopgap |

---

## 8. Recommended baseline configuration

### 8.1 Kernel parameters (persistent)

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
```

Apply without reboot:

```bash
sudo sysctl --system
```

### 8.2 NIC ring buffer (persistent)

The `ethtool -G` setting does not survive a reboot on its own. On
NetworkManager-managed systems, persist it as a connection property:

```bash
nmcli connection modify <conn-name> ethtool.ring-rx 4096
nmcli connection up <conn-name>
```

### 8.3 RPS (persistent, only if single-CPU softirq saturation is observed)

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

## 9. Validation checklist

1. `sudo sysctl --system` applied; re-read values with `sysctl net.core.rmem_max`.
2. `ss -uanmp` on the live collector socket shows the expected large `rb` buffer —
   confirming `rmem_max` is granting the collector's `SO_RCVBUF` request.
3. Generate representative load (real traffic or a UDP generator at expected pps).
4. Watch `nstat`, `ss -uanmp`, `ethtool -S`, and `softnet_stat` as **rates**.
5. Raise only the specific parameter tied to the counter that is moving; re-test.
6. If `IpReasmFails` appears, resolve the MTU/`mbuff` relationship (Section 4)
   rather than masking it with larger reassembly buffers.

> The numeric values here are a sane starting point, not a target. The correct
> values depend on server count, per-server rate, whether detailed `io`
> monitoring is enabled, and the path MTU. The drop counters are the source of
> truth — tune against them.
