#!/usr/bin/env bash
#
# Housekeeping must run on the wall clock, not on packet arrival.
#
# The serializer thread used to block indefinitely in recvPipe.take(), so a
# collector whose servers had gone quiet never called ReapServers() again --
# the one state in which idle incarnations most need reclaiming, and the one
# where nothing would ever wake the thread to notice. This sends a single
# packet, then goes silent for well past --server-ttl, and asserts the
# incarnation was reclaimed anyway.
#
# Fails against a collector that waits without a timeout.

set -eu

BIN="${1:?usage: idle_reap_test.sh <path-to-xrdmoncollect>}"

command -v python3 >/dev/null 2>&1 || { echo "python3 not found"; exit 77; }
command -v curl    >/dev/null 2>&1 || { echo "curl not found";    exit 77; }

exec python3 - "${BIN}" <<'EOF'
import atexit, re, shutil, signal, socket, struct, subprocess, sys, tempfile, time
import urllib.request

BIN = sys.argv[1]
STOD = 1700000000

def packet(code, payload):
    return struct.pack('>BBHI', ord(code), 0, 8 + len(payload), STOD) + payload

tmp = tempfile.mkdtemp(prefix='xrdmon-reap-')
atexit.register(shutil.rmtree, tmp, ignore_errors=True)

# Bind the ports the collector will use, then release them: picking them this
# way avoids a fixed port colliding with a parallel ctest job. The metrics
# endpoint is TCP and the monitoring stream is UDP. Another process can take a
# port before the collector binds it, thus a failed bind starts the collector
# again on other ports, up to 5 times.
def free_port(kind):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p

def fail(msg):
    sys.stderr.write('=== collector.log ===\n%s' % open(tmp + '/collector.log').read())
    sys.stderr.write('FAIL: %s\n' % msg)
    sys.exit(1)

def scrape():
    with urllib.request.urlopen('http://127.0.0.1:%d/metrics' % mport, timeout=5) as r:
        return r.read().decode()

# server-ttl 2: an incarnation is stale two seconds after its last packet. The
# reap sweep itself runs once a minute, so the run has to outlast that.
def start():
    with open(tmp + '/collector.log', 'w') as log:
        return subprocess.Popen(
            [BIN, '-p', str(udp), '-o', tmp + '/out.ndjson', '--server-ttl', '2',
             '--metrics-port', str(mport), '--flush-secs', '1', '--flush-count', '1'],
            stdout=log, stderr=log, stdin=subprocess.DEVNULL)

# The perror() lines of a failed bind. Their text after the colon differs
# between C libraries. A failed bind of the metrics port does not stop the
# collector, thus the test looks at the log.
def bind_failed():
    return re.search(r'^(metrics )?bind(/listen)?:',
                     open(tmp + '/collector.log').read(), re.M) is not None

# Wait for the exporter to answer before sending anything. Return False when
# the collector could not bind its ports.
def wait_up():
    for _ in range(50):
        try:
            scrape()
            return True
        except Exception:
            if bind_failed():
                proc.kill()
                proc.wait()
                return False
            if proc.poll() is not None:
                fail('collector exited early with %s' % proc.returncode)
            time.sleep(0.2)
    fail('metrics endpoint never came up')

proc = None
atexit.register(lambda: proc is not None and proc.poll() is None and proc.kill())
for _ in range(5):
    udp, mport = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM)
    proc = start()
    if wait_up():
        break
else:
    fail('collector could not bind its ports')

# One packet creates one incarnation, and then the collector goes silent.
sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sender.sendto(packet('u', struct.pack('>I', 7) + b'xroot/alice.1:2@wn.example.org'),
              ('127.0.0.1', udp))

def reaped(text):
    for line in text.splitlines():
        if line.startswith('xrootd_collector_reaped_servers_total'):
            return float(line.rsplit(' ', 1)[1])
    return None

# The sweep is gated at 60s, so allow it two turns before giving up. Nothing
# is sent during this window: only a wall-clock wake-up can make it happen.
deadline = time.time() + 150
got = None
while time.time() < deadline:
    time.sleep(2)
    if proc.poll() is not None:
        fail('collector exited during the idle window with %s' % proc.returncode)
    got = reaped(scrape())
    if got is None:
        fail('reaped_servers_total missing from the exposition')
    if got >= 1:
        break
else:
    fail('idle incarnation was never reaped (reaped_servers_total=%s): '
         'housekeeping is still gated on packet arrival' % got)

proc.send_signal(signal.SIGTERM)
if proc.wait(timeout=15) != 0:
    fail('collector exited with %d' % proc.returncode)
print('ok: idle incarnation reaped without any further traffic')
EOF
