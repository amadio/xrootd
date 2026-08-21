#!/usr/bin/env bash

# End-to-end test for the xrdmoncollect terminal operation reports
# (xrootd.operation.state / xrootd.operation.name / error.type). A real
# xrootd server is
# pointed at a local xrdmoncollect instance; we drive a successful transfer and
# a failed open, then assert the collector emits the matching documents.
#
# When the VOMS plug-in is built (MONCOLLECT_VOMS=1, set by CMake), the session
# is additionally driven over gsi with a proxy carrying a fake VOMS attribute
# certificate, and we assert the VO is surfaced on the monitoring stream
# (XrdSecEntity.vorg -> u-record "&o=" -> collector user.vo). The fake AC is
# minted with voms-proxy-fake and signed by the test host cert, which the
# server's voms library trusts via a generated vomsdir/.lsc file.

COLLECTOR_PORT=8096
COLLECTOR_OUT="${PWD}/${NAME}/collected.ndjson"
COLLECTOR_PID="${PWD}/${NAME}/collector.pid"
COLLECTOR_CFG="${PWD}/${NAME}/collector.cfg"
COLLECTOR_LOG="${PWD}/${NAME}/collector.log"

# Application names the filter rules key on (see setup_moncollect): the client
# advertises them via XRD_APPNAME at login, so they reach the collector as the
# u-record's "&x=" and surface as user_agent.original's companion
# user_agent.name -- the same shape as an EOS instance tagging its internal
# traffic.
APP_TAGGED=e2e-tagged
APP_DROPPED=e2e-dropped

# Mock OTLP/HTTP receiver: when python3 is available the collector additionally
# exports OTLP logs/traces here, and the test asserts they arrive. Optional so
# the e2e still runs on hosts without python3.
OTLP_PORT=8097
METRICS_PORT=8098
# A second OTLP receiver, configured through an [otlp "central"] section rather
# than the command line: the shape a site uses to keep its own monitoring and
# feed a central collector at the same time.
OTLP2_PORT=8099
# The same mock again, standing in for an OpenSearch cluster: it ignores the
# path and its "{}" reply carries no "errors":true, which is all _bulk needs.
OS_PORT=8100
OS_INDEX=e2e-moncollect

# The site is the one identity the collector supplies itself: it is not on the
# monitoring wire (all.sitename names the storage cluster, which this test sets
# to "moncollect"), so the two must differ here or the assertions below could
# not tell them apart.
SITE=E2E-SITE
OTLP_OUT="${PWD}/${NAME}/otlp.captured"
OTLP_PID="${PWD}/${NAME}/otlp.pid"
OTLP2_OUT="${PWD}/${NAME}/otlp2.captured"
OTLP2_PID="${PWD}/${NAME}/otlp2.pid"
OS_OUT="${PWD}/${NAME}/os.captured"
OS_PID="${PWD}/${NAME}/os.pid"
OTLP_CACHE="${PWD}/${NAME}/otlp-cache"
# While one of these files exists, the corresponding mock refuses its next POST
# with a 400 (see otlp_mock.py): the e2e uses them to prove that a body a sink
# permanently refuses is quarantined instead of wedging the export behind it.
OTLP_REJECT="${PWD}/${NAME}/otlp.reject-next"
OS_REJECT="${PWD}/${NAME}/os.reject-next"

# Security fragment that moncollect.cfg continues into (see the cfg). Generated
# at setup time so its contents can depend on whether VOMS is built. The name has
# no underscore because XRootD's inline $var substitution stops at non-alnum.
MONCOLLECTSEC="${MONCOLLECTSEC:-${PWD}/${NAME}/security.cfg}"
export MONCOLLECTSEC

# gsi/VOMS environment, shared by the server (inherited from the setup
# invocation) and the client (run invocation). Set at script scope so it applies
# to every test.sh phase, mirroring gsi.sh.
if [ "${MONCOLLECT_VOMS}" = 1 ]; then
	TLS_DIR="${BINARY_DIR}/tests/tls"
	VOMS_DIR="${PWD}/${NAME}/vomsdir"
	export XrdSecPROTOCOL=gsi
	export X509_CERT_DIR="${TLS_DIR}"
	export X509_VOMS_DIR="${VOMS_DIR}"
	export X509_USER_PROXY="${PWD}/${NAME}/vproxy.crt"
fi

# Write the security fragment moncollect.cfg continues into. XRootD only allows a
# single level of "continue", so this fragment is the leaf: with VOMS it prepends
# the gsi + VOMS extraction directives (mirrors gsi.cfg) to a verbatim copy of
# common.cfg; without it the fragment is just common.cfg, so the test is
# unchanged on non-VOMS builds.
function write_security_fragment() {
	{
		if [ "${MONCOLLECT_VOMS}" = 1 ]; then
			cat <<-EOF
			xrootd.seclib libXrdSec.so
			sec.protparm gsi -ca:verify -certdir:${TLS_DIR}
			sec.protparm gsi -crl:require -crldir:${TLS_DIR}
			sec.protparm gsi -key:${TLS_DIR}/host.key
			sec.protparm gsi -cert:${TLS_DIR}/host.pem
			sec.protparm gsi -gridmap:${PWD}/${NAME}/gridmap
			sec.protparm gsi -gmapopt:trymap,usedn
			sec.protparm gsi -vomsat:extract -vomsfun:libXrdVoms.so -vomsfunparms:dbg
			sec.protparm gsi -md:sha512:sha256 -d:1 -trustdns:false
			sec.protocol gsi
			sec.protbind * only gsi
			ofs.authorize 1
			acc.authdb ${PWD}/${NAME}/authdb
			EOF
		fi
		cat "${SOURCE_DIR}/common.cfg"
	} >| "${MONCOLLECTSEC}"
}

# Mint the fake VOMS proxy and the trust material the server uses to verify it.
function setup_moncollect_voms() {
	require_commands voms-proxy-fake openssl

	# gsi maps the client DN to a username and authorizes it for all of /.
	cat >| "${PWD}/${NAME}/authdb" <<-EOF
	u client / a
	EOF
	cat >| "${PWD}/${NAME}/gridmap" <<-EOF
	"/CN=client" client
	EOF

	# Trust the fake AC: it is signed by the test host cert, so the .lsc lists
	# that cert's subject and issuer DNs in OpenSSL slash (compat) form.
	mkdir -p "${VOMS_DIR}/dteam"
	{
		openssl x509 -in "${TLS_DIR}/host.pem" -noout -subject -nameopt compat | sed 's/^subject=//'
		openssl x509 -in "${TLS_DIR}/host.pem" -noout -issuer  -nameopt compat | sed 's/^issuer=//'
	} >| "${VOMS_DIR}/dteam/localhost.lsc"

	# Create a proxy from the client cert carrying a fake VOMS AC for VO dteam.
	voms-proxy-fake -rfc -quiet \
		-certdir "${TLS_DIR}" \
		-cert "${TLS_DIR}/client.crt" -key "${TLS_DIR}/client.key" \
		-hostcert "${TLS_DIR}/host.pem" -hostkey "${TLS_DIR}/host.key" \
		-voms dteam -uri localhost:15000 \
		-fqan /dteam/Role=production/Capability=NULL \
		-out "${X509_USER_PROXY}"
	# gsi rejects proxies with loose permissions.
	chmod 600 "${X509_USER_PROXY}"
}

function setup_moncollect() {
	require_commands xrdmoncollect xrdcp xrdfs xrdreadv-eof xrdopen-denied

	write_security_fragment
	if [ "${MONCOLLECT_VOMS}" = 1 ]; then
		setup_moncollect_voms
	fi

	# When python3 is available, start the mock receivers and point the
	# collector at them (the traces export is exercised via --spans below).
	# Only the first OTLP receiver and the OpenSearch one get a control file:
	# the "central" OTLP destination stays healthy throughout, which is what
	# shows that one destination refusing a body does not disturb another.
	OTLP_ARGS=""
	if command -v python3 >/dev/null 2>&1; then
		: > "${OTLP_OUT}"
		python3 "${SOURCE_DIR}/otlp_mock.py" "${OTLP_PORT}" "${OTLP_OUT}" \
		        "${OTLP_REJECT}" \
		        > "${PWD}/${NAME}/otlp.log" 2>&1 < /dev/null &
		echo $! > "${OTLP_PID}"
		disown 2>/dev/null || true
		: > "${OTLP2_OUT}"
		python3 "${SOURCE_DIR}/otlp_mock.py" "${OTLP2_PORT}" "${OTLP2_OUT}" \
		        > "${PWD}/${NAME}/otlp2.log" 2>&1 < /dev/null &
		echo $! > "${OTLP2_PID}"
		disown 2>/dev/null || true
		: > "${OS_OUT}"
		python3 "${SOURCE_DIR}/otlp_mock.py" "${OS_PORT}" "${OS_OUT}" \
		        "${OS_REJECT}" \
		        > "${PWD}/${NAME}/os.log" 2>&1 < /dev/null &
		echo $! > "${OS_PID}"
		disown 2>/dev/null || true
		# --otlp-token exercises the bearer-auth path; the token is read from a
		# file (@<path>) so the secret is not passed on the command line.
		printf 'secrettoken123' > "${PWD}/${NAME}/otlp.token"
		OTLP_ARGS="--otlp-url http://127.0.0.1:${OTLP_PORT} \
		           --otlp-token @${PWD}/${NAME}/otlp.token \
		           --os-url http://127.0.0.1:${OS_PORT} \
		           --os-index ${OS_INDEX} \
		           --cache-dir ${OTLP_CACHE}"
		sleep 1   # let them bind before the first export
	fi

	# Start the collector before the server so the f-stream destination has a
	# listener. The PID file lands in ${NAME}/ so the harness teardown kills it.
	# Redirect all of its fds (and detach) so it does not inherit and hold open
	# the test runner's stdout pipe, which would block ctest.
	# --dataset captures the per-run mktemp leaf (test-XXXXXX) of the paths the
	# test transfers, standing in for an experiment dataset name.
	: > "${COLLECTOR_OUT}"
	# --traces emits the per-I/O trace-stream records (the server sends them via
	# the 'io' destination in moncollect.cfg); --spans turns concluded operations
	# and each I/O op into OpenTelemetry span documents, so the test can assert
	# both the correlation ids and the session -> file -> I/O span nesting.
	# -c pointed at a generated file keeps a host's /etc/xrootd/xrdmoncollect.cfg
	# (auto-loaded when present) from leaking admin settings into the test. The
	# file carries nothing but the document filter rules, which have no
	# command-line equivalent; with no [xrdmoncollect] section, every setting
	# still comes from the command line below.
	# NB: a filter key must start in column 1. inih appends an indented line to
	# the preceding key's value, so indenting these would fold "action" into
	# "app" and leave a rule that matches nothing.
	cat >| "${COLLECTOR_CFG}" <<-EOF
	[filter "${APP_TAGGED}"]
	app = ${APP_TAGGED}
	action = tag
	label = e2e-internal

	[filter "${APP_DROPPED}"]
	app = ${APP_DROPPED}
	action = drop
	EOF
	# A second OTLP destination alongside the command-line one (which is the
	# destination called "default"). Both receive the same documents; each has
	# its own thread, queue and disk-cache subdirectory.
	if [ -f "${OTLP2_PID}" ]; then
		cat >> "${COLLECTOR_CFG}" <<-EOF

		[otlp "central"]
		url = http://127.0.0.1:${OTLP2_PORT}
		EOF
	fi
	# --sessions folds each close into its client's session and emits a session
	# document (the trace root) when the client disconnects, which is where the
	# session start/end/duration are reported.
	xrdmoncollect -c "${COLLECTOR_CFG}" -p "${COLLECTOR_PORT}" -o "${COLLECTOR_OUT}" \
	              --flush-secs 1 --flush-count 1 --traces --spans --sessions \
	              --metrics-port "${METRICS_PORT}" \
	              --site "${SITE}" \
	              ${OTLP_ARGS} \
	              --dataset '/(test-[A-Za-z0-9]+)/' \
	              > "${COLLECTOR_LOG}" 2>&1 < /dev/null &
	echo $! > "${COLLECTOR_PID}"
	disown 2>/dev/null || true

	# Give it a moment to bind the socket.
	sleep 1
}

# Re-run an action each second (monitoring is UDP, so a single burst may be
# lost or race the server's monitor startup) until the collector output matches
# the given grep -E pattern, up to ~60s.
function drive_until() {
	local pattern=$1 desc=$2 action=$3 i
	for i in $(seq 1 60); do
		eval "${action}" >/dev/null 2>&1 || true
		if grep -Eq "${pattern}" "${COLLECTOR_OUT}" 2>/dev/null; then
			echo "found: ${desc}"
			return 0
		fi
		sleep 1
	done
	echo 1>&2 "=== collector output (${COLLECTOR_OUT}) ==="
	cat 1>&2 "${COLLECTOR_OUT}" || true
	error "timed out waiting for: ${desc}"
}

function test_moncollect() {
	echo "server: XRootD $(assert xrdfs "${HOST}" query config version 2>&1)"

	TMPDIR=$(mktemp -d "${PWD}/${NAME}/test-XXXXXX")
	assert xrdfs "${HOST}" mkdir -p "${TMPDIR}"
	assert openssl rand -base64 -out "${TMPDIR}/ok.ref" $((1024 * 64))

	# 1. A successful transfer: upload then download a file. The close produces
	#    a transfer document with operation state "Successful". Re-driven each
	#    second until the document is observed (tolerates UDP loss). XRD_SITE is
	#    advertised by the client at login and surfaces as client.site.
	export XRD_SITE=CLIENT-TEST-SITE
	drive_until '"xrootd.operation.state":"Successful"' "successful transfer document" \
		"xrdcp -f '${TMPDIR}/ok.ref' '${HOST}/${TMPDIR}/ok.ref' \
		 && xrdcp -f '${HOST}/${TMPDIR}/ok.ref' '${TMPDIR}/ok.dat'"

	# The client-advertised site must travel to the collector as client.site.
	assert grep -Eq '"xrootd.client.site":"CLIENT-TEST-SITE"' "${COLLECTOR_OUT}"

	# The collector runs co-located with the server, so the monitor datagrams
	# arrive from the loopback address. The resource's server.address (the one
	# canonical server-name field) must carry a real identity (the local FQDN
	# substituted for loopback, or the '=' ident host) and must never be a
	# loopback literal or name.
	assert grep -Eq '"server.address":"[^"]+"' "${COLLECTOR_OUT}"
	assert_failure grep -Eq '"server\.address":"(localhost[^"]*|127\.[0-9.]+|::1|::ffff:127\.[0-9.]+)"' "${COLLECTOR_OUT}"

	# client.address carries the server-resolved client name when one is known,
	# with the numeric "&a=" IP as the fallback (and as network.peer.address
	# when the name wins). It must never be a loopback literal or name (the
	# loopback client is renamed to this host's public identity). There is no
	# separate xrootd.client.host field.
	assert grep -Eq '"client.address":"[^"]+"' "${COLLECTOR_OUT}"
	assert_failure grep -Eq '"client.address":"(localhost[^"]*|127\.[0-9.]+|::1|::ffff:127\.[0-9.]+)"' "${COLLECTOR_OUT}"
	assert_failure grep -q '"xrootd.client.host"' "${COLLECTOR_OUT}"

	# The '=' ident's three identity fields land on the OTel service
	# attributes: all.sitename (common.cfg sets it to the instance name) is
	# the storage cluster, -n is the service, and the instance id is the
	# address and port of this one daemon rather than a name a whole fleet
	# would share. The cluster must be byte-identical to the metric label.
	assert grep -Eq '"service.namespace":"moncollect"' "${COLLECTOR_OUT}"
	assert grep -Eq '"service.name":"moncollect"' "${COLLECTOR_OUT}"
	assert grep -Eq "\"service.instance.id\":\"[^\"]+:${XRD_PORT}\"" "${COLLECTOR_OUT}"
	assert grep -Eq '"process.executable.name":"xrootd"' "${COLLECTOR_OUT}"
	# The site is the collector's own, from --site, and must not be confused
	# with the cluster the server advertises.
	assert grep -Eq "\"xrootd.site\":\"${SITE}\"" "${COLLECTOR_OUT}"
	assert_failure grep -q '"xrootd.site":"moncollect"' "${COLLECTOR_OUT}"
	assert_failure grep -q '"xrootd.server.site"' "${COLLECTOR_OUT}"
	assert_failure grep -q '"xrootd.server.program"' "${COLLECTOR_OUT}"

	# file.path is decomposed into the semconv file.directory and
	# file.extension, and the --dataset capture group surfaces as
	# xrootd.dataset.
	assert grep -Fq "\"file.directory\":\"${TMPDIR}\"" "${COLLECTOR_OUT}"
	assert grep -Fq '"file.extension":"ref"' "${COLLECTOR_OUT}"
	assert grep -Eq '"xrootd.dataset":"test-[A-Za-z0-9]+"' "${COLLECTOR_OUT}"

	# Trace-stream I/O records (--traces) must carry OTel correlation ids: a
	# 32-hex traceId and a 16-hex spanId. Re-drive until a read record lands
	# (the trace stream is buffered server-side and can lag the close).
	drive_until '"event.name":"xrootd.io.read"' "trace-stream read record" \
		"xrdcp -f '${HOST}/${TMPDIR}/ok.ref' '${TMPDIR}/ok.dat'"
	# The read log (no "kind") carries the ids.
	read_doc=$(grep -E '"event.name":"xrootd.io.read"' "${COLLECTOR_OUT}" \
		| grep -Ev '"kind":' | head -n1)
	assert grep -Eq '"traceId":"[0-9a-f]{32}"' <<<"${read_doc}"
	assert grep -Eq '"spanId":"[0-9a-f]{16}"' <<<"${read_doc}"

	# With --spans, each I/O op is also a child span nested under the file's
	# transfer span (session -> file -> I/O): a span document (has "kind") that
	# carries "xrootd.io.offset" must name the op and carry a 16-hex parentSpanId.
	io_span=$(grep -E '"kind":"SPAN_KIND_SERVER"' "${COLLECTOR_OUT}" \
		| grep -E '"xrootd.io.offset":' | head -n1)
	assert test -n "${io_span}"
	assert grep -Eq '"parentSpanId":"[0-9a-f]{16}"' <<<"${io_span}"
	assert grep -Eq '"name":"(read|write|readv)"' <<<"${io_span}"

	# With --sessions, a disconnecting client yields a session document. Its
	# start, end and duration are always present: the start is resolved from
	# the login (exactly, when the trace stream reports the connect duration),
	# else the login record's arrival, else the session's first activity, else
	# the disconnect. Re-drive until one lands: the disconnect is reported on
	# the server's fstat flush interval and monitoring is UDP.
	drive_until '"event.name":"xrootd.session"' "session document" \
		"xrdcp -f '${HOST}/${TMPDIR}/ok.ref' '${TMPDIR}/ok.dat'"
	sess_doc=$(grep -E '"event.name":"xrootd.session"' "${COLLECTOR_OUT}" \
		| grep -Ev '"kind":' | head -n1)
	assert test -n "${sess_doc}"
	assert grep -Eq '"xrootd.session.start_time":"[0-9]{4}-[0-9]{2}-[0-9]{2}T' <<<"${sess_doc}"
	assert grep -Eq '"xrootd.session.end_time":"[0-9]{4}-[0-9]{2}-[0-9]{2}T' <<<"${sess_doc}"
	# No leading '-' accepted: the resolver clamps the start into the session,
	# so the duration can never come out negative.
	assert grep -Eq '"xrootd.session.duration":[0-9]+(\.[0-9]+)?[,}]' <<<"${sess_doc}"
	# Which rung produced the start is reported, but not which one wins here:
	# the trace and f-stream disconnects are independent datagrams and either
	# may arrive first, so only membership of the enumeration is asserted.
	assert grep -Eq '"xrootd.session.start_time_source":"(login|connect|first_activity|disconnect)"' \
		<<<"${sess_doc}"

	# The session span is the trace root: no parent, and a start time (a span
	# without startTimeUnixNano is not valid OTLP).
	sess_span=$(grep -E '"name":"session"' "${COLLECTOR_OUT}" \
		| grep -E '"kind":' | head -n1)
	assert test -n "${sess_span}"
	assert grep -Eq '"startTimeUnixNano":"[0-9]+"' <<<"${sess_span}"
	assert_failure grep -q '"parentSpanId"' <<<"${sess_span}"

	# Every span lies inside the one that parents it, at both levels of the
	# waterfall: session -> file -> I/O. Record times are interpolated per packet
	# window and the f and t streams window independently, so this is the one
	# assertion a real server exercises that a unit test cannot -- the two
	# streams here are genuinely unsynchronised, and against an unfixed collector
	# every I/O span misses its file span by tens of seconds. Gated on python3
	# like the OTLP receiver above; the rest of the e2e runs without it.
	# A span whose parent is in a different trace is a separate, known defect
	# (an I/O record that overtook the open of its file cannot be correlated, so
	# it falls back to the session-less trace id while still naming the file's
	# span as its parent). Those are counted and reported, not asserted on --
	# see README.md, "Span nesting". Every span the decoder does place must
	# nest.
	if command -v python3 >/dev/null 2>&1; then
		assert python3 -c '
import json, sys
spans = [json.loads(l) for l in open(sys.argv[1]) if "\"kind\"" in l]
by_id = {s["spanId"]: s for s in spans}
bad = cross = 0
for s in spans:
    p = by_id.get(s.get("parentSpanId"))
    if p is None: continue            # root, or a parent not emitted here
    if p["traceId"] != s["traceId"]: cross += 1; continue
    if (int(s["startTimeUnixNano"]) < int(p["startTimeUnixNano"])
     or int(s["endTimeUnixNano"])   > int(p["endTimeUnixNano"])):
        bad += 1
        print("span %s escapes parent %s: [%s,%s] vs [%s,%s]"
              % (s.get("name"), p.get("name"), s["startTimeUnixNano"],
                 s["endTimeUnixNano"], p["startTimeUnixNano"],
                 p["endTimeUnixNano"]), file=sys.stderr)
print("checked %d spans: %d escaping, %d with a cross-trace parent"
      % (len(spans), bad, cross))
sys.exit(1 if bad else 0)
' "${COLLECTOR_OUT}"
	fi

	# OTLP export (when the mock receiver is running): the collector must POST an
	# OTLP logs export to /v1/logs (resourceLogs envelope with typed KeyValue
	# attributes) and, with --spans, a traces export to /v1/traces.
	if [ -f "${OTLP_PID}" ]; then
		# Wait for the transfer log itself to land, not merely the resourceLogs
		# envelope: the always-present xrootd.server_ident log satisfies
		# resourceLogs on its own, and the OTLP export is an async pipeline (the
		# NDJSON sink is synchronous), so a transfer close reaches the file well
		# before its OTLP body reaches the mock. Keying off operation.state (like
		# drive_until keys off the NDJSON) tolerates that latency and UDP loss.
		for _ in $(seq 1 30); do
			grep -q '"key":"xrootd.operation.state"' "${OTLP_OUT}" 2>/dev/null && break
			xrdcp -f "${TMPDIR}/ok.ref" "${HOST}/${TMPDIR}/ok.ref" >/dev/null 2>&1 || true
			sleep 1
		done
		assert grep -q '^/v1/logs ' "${OTLP_OUT}"
		assert grep -q '"resourceLogs"' "${OTLP_OUT}"
		assert grep -q '"key":"xrootd.operation.state"' "${OTLP_OUT}"
		# the event name must ride in the top-level LogRecord EventName field
		# (the event.name attribute stays as a duplicate for Loki)
		assert grep -Eq '"eventName":"xrootd\.(read|write)"' "${OTLP_OUT}"
		# the bearer token (read from @file) must reach the endpoint
		assert grep -q '^authz /v1/logs Bearer secrettoken123' "${OTLP_OUT}"
		for _ in $(seq 1 15); do
			grep -q 'resourceSpans' "${OTLP_OUT}" 2>/dev/null && break; sleep 1
		done
		assert grep -q '^/v1/traces ' "${OTLP_OUT}"
		assert grep -q '"resourceSpans"' "${OTLP_OUT}"
		# --cache-dir gives the OTLP sink on-failure durability: the per-signal
		# cache subdirectories are created at startup (logs and traces replay to
		# different endpoints, so they cache separately).
		assert test -d "${OTLP_CACHE}/otlp-logs"
		assert test -d "${OTLP_CACHE}/otlp-traces"
		echo "found: OTLP logs + traces export (with disk cache)"
	fi

	# A second destination, declared as [otlp "central"], receives the same
	# documents independently of the first: its own thread, queue and cache.
	if [ -f "${OTLP2_PID}" ]; then
		for _ in $(seq 1 30); do
			grep -q '"key":"xrootd.operation.state"' "${OTLP2_OUT}" 2>/dev/null && break
			xrdcp -f "${TMPDIR}/ok.ref" "${HOST}/${TMPDIR}/ok.ref" >/dev/null 2>&1 || true
			sleep 1
		done
		assert grep -q '"resourceLogs"' "${OTLP2_OUT}"
		assert grep -q '"key":"xrootd.operation.state"' "${OTLP2_OUT}"
		# The token belongs to the "default" destination only: a named section
		# inherits nothing, least of all a credential.
		assert_failure grep -q 'Bearer secrettoken123' "${OTLP2_OUT}"
		assert test -d "${OTLP_CACHE}/otlp-central-logs"
		assert test -d "${OTLP_CACHE}/otlp-central-traces"
		# Every sink series is broken out by destination, and each name is one
		# series of one family (a second family of the same name is an
		# OpenMetrics error, and observed families bypass the registry's dedup).
		metrics=$(curl -sf "http://localhost:${METRICS_PORT}/metrics" || true)
		for d in default central; do
			grep -q \
			  "xrootd_collector_otlp_failures_total{site=\"${SITE}\",destination=\"${d}\"}" \
			  <<<"${metrics}" \
			  || error "no otlp_failures_total series for destination ${d}"
		done
		test "$(grep -c '^# HELP xrootd_collector_otlp_failures_total ' <<<"${metrics}")" \
		     -eq 1 || error "otlp_failures_total emitted more than one family"
		echo "found: a second OTLP destination, exported independently"
	fi

	# With VOMS, the proxy's fake VOMS attribute certificate must surface on the
	# monitoring stream: gsi extracts it into XrdSecEntity.vorg, the server emits
	# it in the MAPUSER record ("&o="), and the collector reports it as user.vo.
	# Re-drive uploads until a transfer document carries the VO (tolerates the
	# u-record racing the close under UDP), then check the role too.
	if [ "${MONCOLLECT_VOMS}" = 1 ]; then
		drive_until '"xrootd.vo":"dteam"' "VO surfaced on transfer document" \
			"xrdcp -f '${TMPDIR}/ok.ref' '${HOST}/${TMPDIR}/ok.ref'"
		assert grep -Eq '"user.roles":\["production"\]' "${COLLECTOR_OUT}"
	fi

	# 2. A failed open: reading a nonexistent file fails before any close, so
	#    the server emits a terminal isError record -> operation state "Failed".
	drive_until '"xrootd.operation.state":"Failed"' "failed-open document" \
		"xrdcp '${HOST}/${TMPDIR}/does-not-exist.root' '${TMPDIR}/x.dat'"

	# The failed-open report must, on a SINGLE document, name the missing file
	# and carry operation name "open" with the *specific* server-side reason
	# (the real "no such file or directory", not merely a non-empty string) and
	# the kXR error code (kXR_NotFound = 3011). Pin the checks to one record so
	# they cannot pass by matching fields spread across different documents.
	open_doc=$(grep -E '"file.path":"[^"]*does-not-exist\.root"' "${COLLECTOR_OUT}" \
		| grep -E '"xrootd.operation.state":"Failed"' | head -n1)
	test -n "${open_doc}" || error "no failed-open transfer document found"
	assert grep -Eq '"xrootd.operation.name":"open"' <<<"${open_doc}"
	assert grep -Eq '"xrootd.error.code":3011' <<<"${open_doc}"
	assert grep -Eq '"error.type":"Unable to open[^"]*no such file or directory"' \
		<<<"${open_doc}"

	# 3. A mid-transfer read error: a vector read past EOF fails on the server,
	#    which records the terminal error on the file so its close reports
	#    operation state "Failed" with operation name "read". xrdreadv-eof opens
	#    the existing file and issues the failing readv, then closes.
	drive_until '"error.type":"Unable to readv' "failed-readv close document" \
		"xrdreadv-eof '${HOST}/${TMPDIR}/ok.ref'"

	# As with the open failure, verify the specific reason on a single document:
	# the readv-past-EOF close must report operation name "read" with the
	# server's ESPIPE reason (kXR_FSError = 3005), not just any error. The
	# strerror text differs by libc: "illegal seek" (glibc) vs "invalid seek"
	# (musl).
	readv_doc=$(grep -E '"user_agent.name":"xrdreadv-eof"' "${COLLECTOR_OUT}" \
		| grep -E '"xrootd.operation.state":"Failed"' | head -n1)
	test -n "${readv_doc}" || error "no failed-readv transfer document found"
	assert grep -Eq '"xrootd.operation.name":"read"' <<<"${readv_doc}"
	assert grep -Eq '"xrootd.error.code":3005' <<<"${readv_doc}"
	assert grep -Eq '"error.type":"Unable to readv[^"]*(illegal|invalid) seek"' \
		<<<"${readv_doc}"

	# 4. A lock-denied open: a second writer of an already-open file is rejected
	#    by the server's file-lock manager (kXR_FileLocked) *before* the
	#    filesystem open, a branch of do_Open that sends the error directly and
	#    so used to bypass the terminal-error report. xrdopen-denied opens the
	#    existing file for write twice; the second open must surface as a failed
	#    open with the lock reason.
	drive_until '"error.type":"[^"]*open denied' "lock-denied open document" \
		"xrdopen-denied '${HOST}/${TMPDIR}/ok.ref'"

	locked_doc=$(grep -E '"error.type":"[^"]*open denied' "${COLLECTOR_OUT}" \
		| head -n1)
	test -n "${locked_doc}" || error "no lock-denied open document found"
	assert grep -Eq '"xrootd.operation.name":"open"' <<<"${locked_doc}"
	assert grep -Eq '"xrootd.error.code":3003' <<<"${locked_doc}"
	assert grep -Eq '"error.type":"[^"]*is already opened by[^"]*open denied' \
		<<<"${locked_doc}"

	# 5. Document filtering. Two [filter] rules were written into the collector
	#    config at setup: one tags documents from XRD_APPNAME=${APP_TAGGED},
	#    the other drops documents from XRD_APPNAME=${APP_DROPPED}. This is the
	#    shape an EOS site uses to separate its own internal traffic from user
	#    activity.

	# The rules were actually loaded. This is the positive control for the
	# drop assertion below, which can only ever observe an absence.
	assert grep -Eq '2 filter rule\(s\) loaded \(1 tag, 1 drop, 0 keep\)' \
		"${COLLECTOR_LOG}"

	# A tag rule labels the matching document in place and changes nothing else.
	# Pin the wait to the f-stream close: the client's trace-stream disconnect
	# carries the same app name and is flushed on its own schedule, so waiting
	# for the app name alone can return before any transfer document exists
	# (and a disconnect record has no companion span to check below). Keys are
	# serialized in sorted order, so event.name always precedes user_agent.name.
	drive_until "\"event.name\":\"xrootd[.](read|write)\".*\"user_agent.name\":\"${APP_TAGGED}\"" \
		"tagged transfer document" \
		"XRD_APPNAME=${APP_TAGGED} xrdcp -f '${HOST}/${TMPDIR}/ok.ref' '${TMPDIR}/tagged.dat'"
	tagged_doc=$(grep -E "\"event.name\":\"xrootd[.](read|write)\".*\"user_agent.name\":\"${APP_TAGGED}\"" \
		"${COLLECTOR_OUT}" | grep -Ev '"kind":' | head -n1)
	test -n "${tagged_doc}" || error "no tagged transfer document found"
	assert grep -Fq '"xrootd.filter.labels":["e2e-internal"]' <<<"${tagged_doc}"

	# The companion span inherits the label, because it is built from the
	# already-tagged log document.
	tagged_span=$(grep -F '"xrootd.filter.labels":["e2e-internal"]' "${COLLECTOR_OUT}" \
		| grep -E '"kind":"SPAN_KIND_SERVER"' | head -n1)
	test -n "${tagged_span}" || error "no tagged span document found"

	# A document no rule matches is left untouched: the very first successful
	# transfer ran long before either app name was used.
	ok_doc=$(grep -E '"xrootd.operation.state":"Successful"' "${COLLECTOR_OUT}" | head -n1)
	test -n "${ok_doc}" || error "no successful transfer document found"
	assert_failure grep -q '"xrootd.filter.labels"' <<<"${ok_doc}"

	# A drop rule suppresses every document of the matching session. Drive the
	# same transfer the tag rule was just proven on, then wait for a *later*
	# tagged transfer of a distinctly named file to land: once that marker is
	# through, the collector has processed past the dropped ones, so their
	# absence means the rule fired rather than that monitoring stopped flowing.
	for _ in $(seq 1 5); do
		XRD_APPNAME="${APP_DROPPED}" \
			xrdcp -f "${HOST}/${TMPDIR}/ok.ref" "${TMPDIR}/dropped.dat" \
			>/dev/null 2>&1 || true
	done
	assert xrdcp -f "${TMPDIR}/ok.ref" "${HOST}/${TMPDIR}/marker.ref"
	drive_until '"event.name":"xrootd[.](read|write)".*"file.name":"marker.ref"' \
		"post-drop marker document" \
		"XRD_APPNAME=${APP_TAGGED} xrdcp -f '${HOST}/${TMPDIR}/marker.ref' '${TMPDIR}/marker.dat'"
	assert_failure grep -q "\"user_agent.name\":\"${APP_DROPPED}\"" "${COLLECTOR_OUT}"

	# 6. A body a sink permanently refuses must not wedge its export. Arm a mock
	#    to refuse its next POST with a 400 (the shape of a Loki validation
	#    error, or of an OpenSearch mapping conflict), drive fresh documents at
	#    it, then assert the refused body was quarantined -- the receiver's own
	#    error text in the collector log and a .rejected file on disk -- and that
	#    later exports still arrive. Before this behaviour existed, replay being
	#    strictly oldest-first meant one refused body blocked every one behind
	#    it, forever.
	#
	#    ${1} the control file, ${2} the receiver's capture file, ${3} where the
	#    .rejected file is expected, ${4} a label for the messages.
	assert_rejection_quarantined() {
		local ctl=$1 out=$2 dir=$3 what=$4 after i
		touch "${ctl}"
		for i in $(seq 1 30); do
			grep -q '^rejected ' "${out}" 2>/dev/null && break
			xrdcp -f "${HOST}/${TMPDIR}/ok.ref" "${TMPDIR}/reject.dat" \
				>/dev/null 2>&1 || true
			sleep 1
		done
		assert grep -q '^rejected ' "${out}"
		assert grep -q "${what}.*rejected by endpoint.*simulated validation error" \
			"${COLLECTOR_LOG}"
		assert grep -q 'quarantined rejected body' "${COLLECTOR_LOG}"
		assert test -n "$(find ${dir} -maxdepth 1 -name '*.rejected' -print -quit)"
		# The export keeps flowing past the quarantined body.
		after=$(grep -c '^/' "${out}" || true)
		for i in $(seq 1 30); do
			test "$(grep -c '^/' "${out}" || true)" -gt "${after}" && break
			xrdcp -f "${HOST}/${TMPDIR}/ok.ref" "${TMPDIR}/resumed.dat" \
				>/dev/null 2>&1 || true
			sleep 1
		done
		assert test "$(grep -c '^/' "${out}" || true)" -gt "${after}"
		echo "found: rejected ${what} body quarantined, export resumed"
	}

	if [ -f "${OTLP_PID}" ]; then
		assert_rejection_quarantined "${OTLP_REJECT}" "${OTLP_OUT}" \
			"${OTLP_CACHE}/otlp-logs ${OTLP_CACHE}/otlp-traces" OTLP
	fi

	# 7. The same for the OpenSearch sink, whose "default" destination caches
	#    directly under --cache-dir rather than in a subdirectory.
	if [ -f "${OS_PID}" ]; then
		assert_rejection_quarantined "${OS_REJECT}" "${OS_OUT}" \
			"${OTLP_CACHE}" OpenSearch
		# The refusal was this destination's alone: the second OTLP receiver was
		# never armed and must have kept receiving throughout.
		assert grep -q '^/v1/logs ' "${OTLP2_OUT}"
		assert_failure grep -q '^rejected ' "${OTLP2_OUT}"
	fi

	# The Prometheus exposition, which nothing else in the suite covers. The
	# server config sets "all.sitename moncollect" and "ident 2s", so by now
	# the identity has landed and the labels carry the real cluster and host
	# rather than the "unknown"/numeric-address pair a server gets before it.
	if command -v curl >/dev/null 2>&1; then
		metrics="${PWD}/${NAME}/metrics.txt"
		assert curl -sf "http://localhost:${METRICS_PORT}/metrics" -o "${metrics}"

		echo "collector metrics:"
		grep -E '^xrootd_collector_(io_|app_io_|errors_|files_open|sessions_open|servers|server_info|documents_)' \
			"${metrics}" || true

		assert grep -Eq "^xrootd_collector_io_total\\{site=\"${SITE}\",cluster=\"moncollect\",server=\"[^\"]+\",operation=\"close\"\\} [1-9]" \
			"${metrics}"
		assert grep -Eq "^xrootd_collector_io_total\\{site=\"${SITE}\",cluster=\"moncollect\",server=\"[^\"]+\",operation=\"open\"\\} [1-9]" \
			"${metrics}"
		# "ops" is in the server's monitor directive, so the per-request counts
		# are populated too -- without it only open/close would move.
		assert grep -Eq "^xrootd_collector_io_total\\{site=\"${SITE}\",cluster=\"moncollect\",server=\"[^\"]+\",operation=\"read\"\\} [1-9]" \
			"${metrics}"
		assert grep -Eq "^xrootd_collector_io_bytes_total\\{site=\"${SITE}\",cluster=\"moncollect\",server=\"[^\"]+\",operation=\"read\"\\} [1-9]" \
			"${metrics}"
		assert grep -Eq "^xrootd_collector_servers\\{site=\"${SITE}\",cluster=\"moncollect\"\\} 1" "${metrics}"
		assert grep -Eq "^xrootd_collector_server_info\\{site=\"${SITE}\",cluster=\"moncollect\"," "${metrics}"
		assert grep -Eq "^xrootd_collector_documents_total\\{site=\"${SITE}\",cluster=\"moncollect\"\\} [1-9]" \
			"${metrics}"
		# The test drove a denied open, so an error is counted by category.
		assert grep -Eq "^xrootd_collector_errors_total\\{site=\"${SITE}\",cluster=\"moncollect\",server=\"[^\"]+\",category=\"[a-z]+\"\\} [1-9]" \
			"${metrics}"

		# No metric may be named for a transfer unless it is one. FRM staging
		# and third-party copies are; per-file I/O is not.
		assert_failure grep -E '^xrootd_collector_[a-z_]*transfer' "${metrics}"
		# Prometheus stamps its own `instance` label at scrape time and renames
		# a collision to exported_instance, so ours must not be called that.
		assert_failure grep -E '[{,]instance="' "${metrics}"
		# --site is a global label, so it leads every series -- including the
		# aggregates that carry no other label at all.
		assert grep -Eq "^xrootd_collector_evicted_total\{site=\"${SITE}\"\}" \
			"${metrics}"
		assert grep -Eq "^xrootd_collector_state_entries\{site=\"${SITE}\"\} [1-9]" \
			"${metrics}"
		# The memory cap's setpoint. Non-zero because --max-memory defaults on;
		# without it the control loop would be silently inert.
		assert grep -Eq "^xrootd_collector_state_budget_bytes\{site=\"${SITE}\"\} [1-9]" \
			"${metrics}"
		# The "process" subsystem: a second subsystem on the same registry, so
		# this also proves the global site label reaches beyond "collector".
		assert grep -Eq "^xrootd_process_resident_memory_bytes\{site=\"${SITE}\"\} [1-9]" \
			"${metrics}"
		# Site and cluster are independent: the site comes from the collector's
		# own config, the cluster from the server's all.sitename.
		assert_failure grep -E "[{,]cluster=\"${SITE}\"" "${metrics}"
		assert_failure grep -E "[{,]site=\"moncollect\"" "${metrics}"
		# Every series carrying a cluster must render {site=...,cluster=...} in
		# that order. This is what catches a schema frozen the wrong way round:
		# XrdMetrics matches labels positionally and would otherwise silently
		# render cluster="<hostname>" with no error anywhere.
		stray=$(grep -E '^xrootd_collector_[a-z_]+\{[^}]*cluster="' "${metrics}" \
			| grep -vE '^xrootd_collector_[a-z_]+\{site="[^"]*",cluster="' || true)
		test -z "${stray}" || error "series not led by site,cluster: ${stray}"
		# One family, one HELP block: a name registered through both the
		# labelled and the observed path would emit two.
		dupes=$(grep '^# HELP' "${metrics}" | awk '{print $3}' | sort | uniq -d)
		test -z "${dupes}" || error "duplicate metric families: ${dupes}"
	fi

	echo "collector documents:"
	cat "${COLLECTOR_OUT}"
}
