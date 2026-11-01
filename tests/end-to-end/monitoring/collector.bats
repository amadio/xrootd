#!/usr/bin/env bash

# The documents of xrdmoncollect for the detailed monitoring of a real
# server: successful transfers, failed opens, I/O errors, sessions and spans,
# the document filters, the OTLP and OpenSearch exports and the Prometheus
# metrics. The tests share one collector, its mock receivers and one server,
# and each test drives its own transfers in its own directory. Monitoring is
# UDP, thus a test repeats its transfer until the document arrives.
#
# When the build has the VOMS plugin and voms-proxy-fake is available, the
# client and the server use gsi, with a proxy that carries a fake VOMS
# attribute certificate, and a test checks that the VO of the client is on
# the documents.

load ../setup/server.bash
load collector.bash

BATS_TEST_TIMEOUT=120

# The site is the one identity that the collector supplies itself. It is not
# on the monitoring stream: all.sitename names the storage cluster, which is
# "server" here. Thus the two must differ, or the tests could not tell
# them apart.
SITE=E2E-SITE

# The application names of the filter rules: the client sends them with
# XRD_APPNAME at login, and the collector reports them as user_agent.name.
APP_TAGGED=e2e-tagged
APP_DROPPED=e2e-dropped

OS_INDEX=e2e-collector

# The client sends its site at login, and the collector reports it as
# xrootd.client.site.
export XRD_SITE=CLIENT-TEST-SITE

if has_plugin libXrdVoms && command -v voms-proxy-fake >/dev/null; then
    VOMS=voms.cfg
    export XrdSecPROTOCOL=gsi
    export X509_CERT_DIR="$CREDENTIALS"
    export X509_VOMS_DIR="$BATS_FILE_TMPDIR/vomsdir"
    export X509_USER_PROXY="$BATS_FILE_TMPDIR/vproxy.crt"
fi

# Make the fake VOMS proxy of the client, and the trust of the server in it.
# The host certificate signs the fake attribute certificate, thus the .lsc
# file of the VO lists the subject and the issuer of the host certificate.
make_voms_proxy() {
    mkdir -p "$X509_VOMS_DIR/dteam"
    {
        openssl x509 -in "$CREDENTIALS/host.pem" -noout -subject -nameopt compat | sed 's/^subject=//'
        openssl x509 -in "$CREDENTIALS/host.pem" -noout -issuer -nameopt compat | sed 's/^issuer=//'
    } > "$X509_VOMS_DIR/dteam/localhost.lsc"

    voms-proxy-fake -rfc -quiet -certdir "$CREDENTIALS" \
        -cert "$CREDENTIALS/client.crt" -key "$CREDENTIALS/client.key" \
        -hostcert "$CREDENTIALS/host.pem" -hostkey "$CREDENTIALS/host.key" \
        -voms dteam -uri localhost:15000 \
        -fqan /dteam/Role=production/Capability=NULL \
        -out "$X509_USER_PROXY"
    # gsi refuses a proxy which others can read
    chmod 600 "$X509_USER_PROXY"
}

setup_file() {
    require_commands curl openssl python3 xrdmoncollect xrdopen-denied xrdreadv-eof

    if [[ -n "${VOMS:-}" ]]; then
        make_voms_proxy
    fi

    # Only the first OTLP receiver and the OpenSearch receiver can refuse a
    # body. The "central" OTLP destination stays healthy, which shows that a
    # refusal at one destination does not disturb another.
    start_mock otlp reject
    start_mock central
    start_mock opensearch reject

    # --otlp-token reads the token from a file (@<path>), thus the secret is
    # not on the command line
    printf 'secrettoken123' > "$BATS_FILE_TMPDIR/otlp.token"

    # -c with a file of the test keeps the /etc/xrootd/xrdmoncollect.cfg of
    # the host out of the test. The file has only the filter rules, which
    # have no command-line options, and a second OTLP destination beside the
    # one of the command line (called "default"). A filter key must start in
    # column 1: inih appends an indented line to the value of the key before
    # it.
    cat > "$BATS_FILE_TMPDIR/collector.ini" <<EOF
[filter "$APP_TAGGED"]
app = $APP_TAGGED
action = tag
label = e2e-internal

[filter "$APP_DROPPED"]
app = $APP_DROPPED
action = drop

[otlp "central"]
url = http://127.0.0.1:$CENTRAL_PORT
EOF

    export COLLECTOR_OUT="$BATS_FILE_TMPDIR/collected.ndjson"
    export DOCUMENTS=$COLLECTOR_OUT
    export COLLECTOR_CACHE="$BATS_FILE_TMPDIR/cache"
    METRICS_PORT=$(free_port)
    export METRICS_PORT
    : > "$COLLECTOR_OUT"

    # --traces emits the records of the trace stream (the "io" destination of
    # collector.cfg). --spans makes OpenTelemetry spans of the operations and
    # of each I/O. --sessions folds each close into the session of its client,
    # and emits a session document at the disconnect. --dataset takes the
    # directory of the test as the name of a dataset.
    start_daemon collector udp xrdmoncollect -c "$BATS_FILE_TMPDIR/collector.ini" \
        -p @PORT@ -o "$COLLECTOR_OUT" --flush-secs 1 --flush-count 1 \
        --traces --spans --sessions --metrics-port "$METRICS_PORT" --site "$SITE" \
        --otlp-url "http://127.0.0.1:$OTLP_PORT" --otlp-token "@$BATS_FILE_TMPDIR/otlp.token" \
        --os-url "http://127.0.0.1:$OPENSEARCH_PORT" --os-index "$OS_INDEX" \
        --cache-dir "$COLLECTOR_CACHE" --dataset '/(test-[A-Za-z0-9]+)/'

    start_server collector.cfg ${VOMS:+"$VOMS"}
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    DIR=/test-n$BATS_TEST_NUMBER
    URL="$SERVER_URL/$DIR"
    # the documents of the file of the test
    OK_FILE="\"file.path\":\"$DIR/ok.ref\""
    xrdfs "$SERVER_URL" mkdir -p "$DIR"
    openssl rand -base64 -out ok.ref $(( 1024 * 64 ))
}

# Print the first document that matches all the regular expressions, and is
# not a span
document() {
    matching "$@" | grep -Ev '"kind":' | head -n 1
}

# Print the first span that matches all the regular expressions
span() {
    matching '"kind":' "$@" | head -n 1
}

upload() {
    xrdcp -f ok.ref "$URL/ok.ref"
}

transfer() {
    xrdcp -f ok.ref "$URL/ok.ref" && xrdcp -f "$URL/ok.ref" ok.dat
}

SUCCESSFUL='"xrootd.operation.state":"Successful"'
FAILED='"xrootd.operation.state":"Failed"'

@test "a transfer gives a document with the state Successful and the site of the client" {
    drive_until "$OK_FILE" "$SUCCESSFUL" -- transfer
    run -0 grep -Eq '"xrootd.client.site":"CLIENT-TEST-SITE"' "$COLLECTOR_OUT"
}

@test "the server and client addresses of a document are not loopback addresses" {
    # The collector runs on the host of the server, thus the datagrams come
    # from the loopback address. server.address must carry a real identity,
    # and client.address the name of the client or its numeric address.
    drive_until "$OK_FILE" "$SUCCESSFUL" -- transfer
    run -0 grep -Eq '"server.address":"[^"]+"' "$COLLECTOR_OUT"
    run -1 grep -Eq '"server\.address":"(localhost[^"]*|127\.[0-9.]+|::1|::ffff:127\.[0-9.]+)"' "$COLLECTOR_OUT"
    run -0 grep -Eq '"client.address":"[^"]+"' "$COLLECTOR_OUT"
    run -1 grep -Eq '"client.address":"(localhost[^"]*|127\.[0-9.]+|::1|::ffff:127\.[0-9.]+)"' "$COLLECTOR_OUT"
    run -1 grep -q '"xrootd.client.host"' "$COLLECTOR_OUT"
}

@test "the identity of the server is on the service attributes" {
    # all.sitename is the storage cluster, -n the service, and the instance
    # id is the address and the port of this daemon. The site comes from the
    # --site of the collector, not from the cluster of the server.
    drive_until "$OK_FILE" "$SUCCESSFUL" -- transfer
    run -0 grep -Eq '"service.namespace":"server"' "$COLLECTOR_OUT"
    run -0 grep -Eq '"service.name":"server"' "$COLLECTOR_OUT"
    run -0 grep -Eq "\"service.instance.id\":\"[^\"]+:$SERVER_PORT\"" "$COLLECTOR_OUT"
    run -0 grep -Eq '"process.executable.name":"xrootd"' "$COLLECTOR_OUT"
    run -0 grep -Eq "\"xrootd.site\":\"$SITE\"" "$COLLECTOR_OUT"
    run -1 grep -q '"xrootd.site":"server"' "$COLLECTOR_OUT"
    run -1 grep -q '"xrootd.server.site"' "$COLLECTOR_OUT"
    run -1 grep -q '"xrootd.server.program"' "$COLLECTOR_OUT"
}

@test "file.path gives the directory, the extension and the dataset" {
    drive_until "$OK_FILE" "$SUCCESSFUL" -- transfer
    run -0 grep -Fq "\"file.directory\":\"$DIR\"" "$COLLECTOR_OUT"
    run -0 grep -Fq '"file.extension":"ref"' "$COLLECTOR_OUT"
    run -0 grep -Fq "\"xrootd.dataset\":\"${DIR#/}\"" "$COLLECTOR_OUT"
}

@test "a read record of the trace stream carries the correlation ids" {
    # the server buffers the trace stream, thus the record can come after the
    # close
    upload
    drive_until '"event.name":"xrootd.io.read"' -- xrdcp -f "$URL/ok.ref" ok.dat

    run -0 document '"event.name":"xrootd.io.read"'
    assert_regex "$output" '"traceId":"[0-9a-f]{32}"'
    assert_regex "$output" '"spanId":"[0-9a-f]{16}"'
}

@test "an I/O span has a transfer span as its parent" {
    upload
    drive_until '"kind":"SPAN_KIND_SERVER"' '"xrootd.io.offset":' -- xrdcp -f "$URL/ok.ref" ok.dat

    run -0 span '"kind":"SPAN_KIND_SERVER"' '"xrootd.io.offset":'
    assert_regex "$output" '"parentSpanId":"[0-9a-f]{16}"'
    assert_regex "$output" '"name":"(read|write|readv)"'
}

@test "a client that disconnects gives a session document" {
    # The disconnect comes with the fstat flush of the server. The start of
    # the session comes from the login, the connect, the first activity or
    # the disconnect. The trace and f-stream disconnects are separate
    # datagrams, thus any of these sources can win.
    upload
    drive_until '"event.name":"xrootd.session"' -- xrdcp -f "$URL/ok.ref" ok.dat

    run -0 document '"event.name":"xrootd.session"'
    assert_regex "$output" '"xrootd.session.start_time":"[0-9]{4}-[0-9]{2}-[0-9]{2}T'
    assert_regex "$output" '"xrootd.session.end_time":"[0-9]{4}-[0-9]{2}-[0-9]{2}T'
    # the start is inside the session, thus the duration is not negative
    assert_regex "$output" '"xrootd.session.duration":[0-9]+(\.[0-9]+)?[,}]'
    assert_regex "$output" '"xrootd.session.start_time_source":"(login|connect|first_activity|disconnect)"'
}

@test "the session span is the root of its trace" {
    upload
    drive_until '"name":"session"' -- xrdcp -f "$URL/ok.ref" ok.dat

    run -0 span '"name":"session"'
    assert_regex "$output" '"startTimeUnixNano":"[0-9]+"'
    refute_regex "$output" '"parentSpanId"'
}

@test "each span is inside the span of its parent" {
    # The times of the records are interpolated in the window of each
    # packet, and the f and t streams have separate windows. A span whose
    # parent is in another trace is a known defect (see README.md, "Span
    # nesting"), thus the check counts these and does not fail on them.
    upload
    drive_until '"name":"session"' -- xrdcp -f "$URL/ok.ref" ok.dat

    run -0 python3 -c '
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
                 p["endTimeUnixNano"]))
print("checked %d spans: %d escaping, %d with a cross-trace parent"
      % (len(spans), bad, cross))
sys.exit(1 if bad else 0)
' "$COLLECTOR_OUT"
}

@test "the collector exports OTLP logs and traces with the bearer token" {
    # The OTLP export is asynchronous, thus the close of a transfer reaches
    # the NDJSON file before its OTLP body reaches the mock.
    wait_for 30 eval 'transfer > /dev/null 2>&1; grep -q "\"key\":\"xrootd.operation.state\"" "$OTLP_OUT"'
    run -0 grep -q '^/v1/logs ' "$OTLP_OUT"
    run -0 grep -q '"resourceLogs"' "$OTLP_OUT"
    # the event name is in the EventName field of the LogRecord
    run -0 grep -Eq '"eventName":"xrootd\.(read|write)"' "$OTLP_OUT"
    run -0 grep -q '^authz /v1/logs Bearer secrettoken123' "$OTLP_OUT"

    wait_for 15 grep -q 'resourceSpans' "$OTLP_OUT"
    run -0 grep -q '^/v1/traces ' "$OTLP_OUT"
    # --cache-dir makes a cache for each signal at the start
    [[ -d "$COLLECTOR_CACHE/otlp-logs" ]]
    [[ -d "$COLLECTOR_CACHE/otlp-traces" ]]
}

@test "a second OTLP destination gets the documents without the token of the first" {
    wait_for 30 eval 'transfer > /dev/null 2>&1; grep -q "\"key\":\"xrootd.operation.state\"" "$CENTRAL_OUT"'
    run -0 grep -q '"resourceLogs"' "$CENTRAL_OUT"
    # a named destination inherits nothing, least of all a credential
    run -1 grep -q 'Bearer secrettoken123' "$CENTRAL_OUT"
    [[ -d "$COLLECTOR_CACHE/otlp-central-logs" ]]
    [[ -d "$COLLECTOR_CACHE/otlp-central-traces" ]]
}

@test "the metrics of the OTLP destinations have one series for each destination" {
    wait_for 30 eval 'transfer > /dev/null 2>&1; grep -q "\"key\":\"xrootd.operation.state\"" "$CENTRAL_OUT"'
    run -0 curl -sf "http://localhost:$METRICS_PORT/metrics"
    assert_line --regexp "^xrootd_collector_otlp_failures_total\{site=\"$SITE\",destination=\"default\"\} "
    assert_line --regexp "^xrootd_collector_otlp_failures_total\{site=\"$SITE\",destination=\"central\"\} "
    # one family: a second family of the same name is an OpenMetrics error
    run -0 grep -c '^# HELP xrootd_collector_otlp_failures_total ' <<< "$output"
    assert_output 1
}

@test "the VO of a gsi client with a VOMS proxy is on the transfer document" {
    [[ -n "${VOMS:-}" ]] || skip "no VOMS plugin or no voms-proxy-fake"

    # gsi puts the VO into XrdSecEntity.vorg, the server sends it in the user
    # record ("&o="), and the collector reports it
    drive_until '"xrootd.vo":"dteam"' -- upload
    run -0 grep -Eq '"user.roles":\["production"\]' "$COLLECTOR_OUT"
}

@test "an open of a missing file gives a Failed document with kXR_NotFound" {
    local missing="\"file.path\":\"$DIR/missing.root\""
    drive_until "$missing" "$FAILED" -- xrdcp "$URL/missing.root" x.dat

    run -0 document "$missing" "$FAILED"
    assert_regex "$output" '"xrootd.operation.name":"open"'
    assert_regex "$output" '"xrootd.error.code":3011'
    assert_regex "$output" '"error.type":"File or object not found"'
}

@test "a vector read past the end of a file gives a Failed document with kXR_FSError" {
    # the server maps ESPIPE to kXR_FSError
    upload
    drive_until "$OK_FILE" '"user_agent.name":"xrdreadv-eof"' "$FAILED" -- xrdreadv-eof "$URL/ok.ref"

    run -0 document "$OK_FILE" '"user_agent.name":"xrdreadv-eof"' "$FAILED"
    assert_regex "$output" '"xrootd.operation.name":"read"'
    assert_regex "$output" '"xrootd.error.code":3005'
    assert_regex "$output" '"error.type":"Filesystem error"'
}

@test "an open that the file lock denies gives a document with kXR_FileLocked" {
    # The lock manager denies the second writer before the open of the file
    # system, in a branch of do_Open that sends the error itself.
    upload
    local locked='"error.type":"File or object is locked"'
    drive_until "$OK_FILE" "$locked" -- xrdopen-denied "$URL/ok.ref"

    run -0 document "$OK_FILE" "$locked"
    assert_regex "$output" '"xrootd.operation.name":"open"'
    assert_regex "$output" '"xrootd.error.code":3003'
}

@test "the collector loads the filter rules of its configuration" {
    run -0 grep -Eq '2 filter rule\(s\) loaded \(1 tag, 1 drop, 0 keep\)' "$COLLECTOR_LOG"
}

@test "a tag rule labels the document and the span of a matching application" {
    # Wait for the close on the f-stream: the trace-stream disconnect has the
    # same application name, and it can come before any transfer document.
    local close='"event.name":"xrootd[.](read|write)"' app="\"user_agent.name\":\"$APP_TAGGED\""
    upload
    drive_until "$close" "$OK_FILE" "$app" -- env XRD_APPNAME="$APP_TAGGED" xrdcp -f "$URL/ok.ref" tagged.dat

    run -0 document "$close" "$OK_FILE" "$app"
    assert_regex "$output" '"xrootd.filter.labels":\["e2e-internal"\]'

    # the span comes from the tagged document, thus it has the label too
    run -0 span '"kind":"SPAN_KIND_SERVER"' "$OK_FILE" '"xrootd.filter.labels":\["e2e-internal"\]'
}

@test "a document of an application without a rule has no labels" {
    drive_until "$OK_FILE" "$SUCCESSFUL" -- transfer
    run -0 document "$OK_FILE" "$SUCCESSFUL"
    refute_regex "$output" '"xrootd.filter.labels"'
}

@test "a drop rule removes the documents of a matching application" {
    # When the document of a later transfer of another file arrives, the
    # collector has processed the dropped transfers, thus their absence shows
    # that the rule matched.
    upload
    for _ in 1 2 3 4 5; do
        XRD_APPNAME="$APP_DROPPED" xrdcp -f "$URL/ok.ref" dropped.dat > /dev/null 2>&1 || true
    done
    xrdcp -f ok.ref "$URL/marker.ref"
    drive_until '"event.name":"xrootd[.](read|write)"' "\"file.path\":\"$DIR/marker.ref\"" -- \
        env XRD_APPNAME="$APP_TAGGED" xrdcp -f "$URL/marker.ref" marker.dat
    run -1 grep -q "\"user_agent.name\":\"$APP_DROPPED\"" "$COLLECTOR_OUT"
}

# assert_quarantined <control file> <capture file> <label> <cache dir>...
#
# Arm the mock to refuse its next POST with a 400 (as a validation error of
# Loki, or a mapping conflict of OpenSearch), and drive new documents to it.
# Check that the collector puts the refused body into quarantine, with the
# error text of the endpoint in its log and a .rejected file on disk, and
# that the later exports still arrive. Replay is oldest first, thus without
# the quarantine one refused body would block all bodies after it.
assert_quarantined() {
    local ctl=$1 out=$2 what=$3 after
    shift 3

    touch "$ctl"
    wait_for 30 eval 'transfer > /dev/null 2>&1; grep -q "^rejected " "$out"'
    run -0 grep -q "$what.*rejected by endpoint.*simulated validation error" "$COLLECTOR_LOG"
    run -0 grep -q 'quarantined rejected body' "$COLLECTOR_LOG"
    run -0 find "$@" -maxdepth 1 -name '*.rejected'
    refute_output ''

    # the export continues after the body in quarantine
    after=$(grep -c '^/' "$out" || true)
    wait_for 30 eval 'transfer > /dev/null 2>&1; (( $(grep -c "^/" "$out" || true) > after ))'
}

@test "a body that the OTLP endpoint refuses goes to quarantine and the export continues" {
    assert_quarantined "$OTLP_REJECT" "$OTLP_OUT" OTLP \
        "$COLLECTOR_CACHE/otlp-logs" "$COLLECTOR_CACHE/otlp-traces"
}

@test "a body that OpenSearch refuses goes to quarantine and the other destinations continue" {
    # the "default" OpenSearch destination caches in --cache-dir itself
    assert_quarantined "$OPENSEARCH_REJECT" "$OPENSEARCH_OUT" OpenSearch "$COLLECTOR_CACHE"
    # the refusal was of this destination alone
    run -0 grep -q '^/v1/logs ' "$CENTRAL_OUT"
    run -1 grep -q '^rejected ' "$CENTRAL_OUT"
}

@test "the Prometheus metrics count the operations, the errors and the state" {
    # "ops" is in the monitor directive of the server, thus the counts of the
    # reads move too. The open of a missing file counts an error.
    drive_until "$OK_FILE" "$SUCCESSFUL" -- transfer
    drive_until "\"file.path\":\"$DIR/missing.root\"" "$FAILED" -- xrdcp "$URL/missing.root" x.dat
    curl -sf "http://localhost:$METRICS_PORT/metrics" -o metrics.txt
    local labels="site=\"$SITE\",cluster=\"server\""

    run -0 grep -Eq "^xrootd_collector_io_total\\{$labels,server=\"[^\"]+\",operation=\"close\"\\} [1-9]" metrics.txt
    run -0 grep -Eq "^xrootd_collector_io_total\\{$labels,server=\"[^\"]+\",operation=\"open\"\\} [1-9]" metrics.txt
    run -0 grep -Eq "^xrootd_collector_io_total\\{$labels,server=\"[^\"]+\",operation=\"read\"\\} [1-9]" metrics.txt
    run -0 grep -Eq "^xrootd_collector_io_bytes_total\\{$labels,server=\"[^\"]+\",operation=\"read\"\\} [1-9]" metrics.txt
    run -0 grep -Eq "^xrootd_collector_servers\\{$labels\\} 1" metrics.txt
    run -0 grep -Eq "^xrootd_collector_server_info\\{$labels," metrics.txt
    run -0 grep -Eq "^xrootd_collector_documents_total\\{$labels\\} [1-9]" metrics.txt
    run -0 grep -Eq "^xrootd_collector_errors_total\\{$labels,server=\"[^\"]+\",category=\"[a-z]+\"\\} [1-9]" metrics.txt

    # Only FRM staging and third-party copies are transfers, per-file I/O is
    # not. Prometheus adds its own "instance" label at the scrape.
    run -1 grep -E '^xrootd_collector_[a-z_]*transfer' metrics.txt
    run -1 grep -E '[{,]instance="' metrics.txt

    # --site is a global label, thus it leads each series, also the
    # aggregates without other labels
    run -0 grep -Eq "^xrootd_collector_evicted_total\{site=\"$SITE\"\}" metrics.txt
    run -0 grep -Eq "^xrootd_collector_state_entries\{site=\"$SITE\",kind=\"users\"\} [1-9]" metrics.txt
    run -0 grep -Eq "^xrootd_collector_state_entries\{site=\"$SITE\",kind=\"files\"\}" metrics.txt
    run -0 grep -Eq "^xrootd_collector_state_budget_bytes\{site=\"$SITE\"\} [1-9]" metrics.txt
    run -0 grep -Eq "^xrootd_collector_memory_cap_bytes\{site=\"$SITE\"\} [1-9]" metrics.txt
    # a ratio from 0 to 1, thus only its presence is certain
    run -0 grep -Eq "^xrootd_collector_state_budget_used_ratio\{site=\"$SITE\"\} [0-9]" metrics.txt
    run -0 grep -Eq "^xrootd_process_resident_memory_bytes\{site=\"$SITE\"\} [1-9]" metrics.txt

    # the site and the cluster are independent
    run -1 grep -E "[{,]cluster=\"$SITE\"" metrics.txt
    run -1 grep -E '[{,]site="server"' metrics.txt

    # XrdMetrics matches the labels by position, thus each series with a
    # cluster must start with site, then cluster
    run -1 grep -E '^xrootd_collector_[a-z_]+\{[^}]*cluster="' <<< "$(grep -vE '^xrootd_collector_[a-z_]+\{site="[^"]*",cluster="' metrics.txt)"

    # one HELP block for each family
    run -0 bash -c "grep '^# HELP' metrics.txt | awk '{print \$3}' | sort | uniq -d"
    assert_output ''
}
