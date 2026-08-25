#!/usr/bin/env bash
#
# A finished session must leave the correlation state on its own.
#
# The disconnect deliberately marks the user entry rather than erasing it, so a
# straggling close can still resolve an identity against it. Nothing bounded how
# long that lasted: only memory pressure, or --server-ttl reaping the whole
# incarnation a day later, ever reclaimed one, and a busy collector accumulated
# hundreds of thousands of spent sessions against a few thousand open files.
#
# This drives one login and one disconnect over the wire, then waits out the
# grace and the sweep, and asserts the entry went and was accounted for. The
# unit tests cover the sweep's arithmetic; what only a running daemon can show
# is that a real isDisc record on the real wire format reaches it at all, and
# that the per-kind gauge an operator would watch actually comes back down.
#
# Fails against a collector that keeps spent sessions until its incarnation dies.

set -eu

BIN="${1:?usage: user_ttl_test.sh <path-to-xrdmoncollect>}"

command -v python3 >/dev/null 2>&1 || { echo "python3 not found"; exit 77; }

exec python3 - "${BIN}" <<'EOF'
import atexit, shutil, signal, socket, struct, subprocess, sys, tempfile, time
import urllib.request

BIN = sys.argv[1]
STOD = 1700000000

def packet(code, payload):
    return struct.pack('>BBHI', ord(code), 0, 8 + len(payload), STOD) + payload

# One f-stream record: a 4-byte header (type, flags, size-including-header)
# then the body.
def frec(rtype, body):
    return struct.pack('>BBH', rtype, 0, 4 + len(body)) + body

# The isTime (type 2) marker every f-stream packet leads with: the count of
# records that follow, and the window their times are interpolated over.
def todrec(nrecs):
    now = int(time.time())
    body = struct.pack('>HHIIq', 0, nrecs, now - 1, now, 42)
    return frec(2, body)

tmp = tempfile.mkdtemp(prefix='xrdmon-userttl-')
atexit.register(shutil.rmtree, tmp, ignore_errors=True)

def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p

udp, mport = free_port(), free_port()
log = open(tmp + '/collector.log', 'w')

# user-ttl 2: a session is collectable two seconds after its disconnect. The
# sweep itself runs once a minute, so the run has to outlast that. The idle TTL
# is off, so anything expired here can only be the post-disconnect grace.
proc = subprocess.Popen(
    [BIN, '-p', str(udp), '-o', tmp + '/out.ndjson', '--user-ttl', '2',
     '--user-idle-ttl', '0', '--server-ttl', '0', '--sessions',
     '--metrics-port', str(mport), '--flush-secs', '1', '--flush-count', '1'],
    stdout=log, stderr=log, stdin=subprocess.DEVNULL)
atexit.register(lambda: proc.poll() is None and proc.kill())

def fail(msg):
    log.flush()
    sys.stderr.write('=== collector.log ===\n%s' % open(tmp + '/collector.log').read())
    sys.stderr.write('FAIL: %s\n' % msg)
    sys.exit(1)

def scrape():
    with urllib.request.urlopen('http://127.0.0.1:%d/metrics' % mport, timeout=5) as r:
        return r.read().decode()

def series(text, prefix):
    """The value of the first series whose name and labels start with `prefix`."""
    for line in text.splitlines():
        if line.startswith(prefix):
            return float(line.rsplit(' ', 1)[1])
    return None

for _ in range(50):
    try:
        scrape()
        break
    except Exception:
        if proc.poll() is not None:
            fail('collector exited early with %s' % proc.returncode)
        time.sleep(0.2)
else:
    fail('metrics endpoint never came up')

sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sender.sendto(packet('u', struct.pack('>I', 7) + b'xroot/alice.1:2@wn.example.org'),
              ('127.0.0.1', udp))

# The user entry must be there before the disconnect, or the test proves nothing
# about what removed it.
for _ in range(50):
    users = series(scrape(), 'xrootd_collector_state_entries{kind="users"}')
    if users:
        break
    time.sleep(0.2)
else:
    fail('the login never produced a users entry (got %s)' % users)

# Now disconnect it: an f-stream packet whose single record is an isDisc (4)
# naming the same dictid.
sender.sendto(packet('f', todrec(1) + frec(4, struct.pack('>I', 7))),
              ('127.0.0.1', udp))

# Two turns of the 60s sweep before giving up, as in idle_reap_test.sh. Nothing
# further is sent: only the wall clock can make this happen.
deadline = time.time() + 150
while time.time() < deadline:
    time.sleep(2)
    if proc.poll() is not None:
        fail('collector exited during the wait with %s' % proc.returncode)
    text = scrape()
    expired = series(text, 'xrootd_collector_expired_users_total{')
    users = series(text, 'xrootd_collector_state_entries{kind="users"}')
    if expired and expired >= 1:
        break
else:
    fail('the disconnected session was never expired '
         '(expired_users_total=%s, users still held=%s)' % (expired, users))

# The reason label is what tells an operator the mechanism is working rather
# than eating live sessions, so it has to be the right one.
if series(text, 'xrootd_collector_expired_users_total{'
                'cluster="unknown",server="127.0.0.1",reason="disconnected"}') is None \
   and 'reason="disconnected"' not in text:
    fail('expired_users_total carries no reason="disconnected" series:\n%s'
         % '\n'.join(l for l in text.splitlines() if 'expired_users' in l))

# And the gauge an operator would actually watch came back down.
if users:
    fail('users entries still held after the sweep: %s' % users)

proc.send_signal(signal.SIGTERM)
if proc.wait(timeout=15) != 0:
    fail('collector exited with %d' % proc.returncode)
print('ok: a disconnected session was expired by the grace and the gauge fell')
EOF
