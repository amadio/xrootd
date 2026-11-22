#!/usr/bin/env bash

# Helpers of the tests of XrdClS3, the S3 client plugin, as XCache uses it: a
# proxy or a cache serves the objects of an S3 origin, which it reads and
# writes with XrdClS3. The origin of the tests is a server with XrdHttp over
# HTTPS. Its data directory has the bucket $BUCKET, as a path-style S3 service
# has. XrdHttp does not check the SigV4 signature which XrdClS3 sends, and it
# answers no request which only S3 has, such as a listing. Thus a listing, and
# a stat of a missing object, which XrdClS3 checks with a listing, fail.

load ../setup/server.bash

readonly BUCKET=test-bucket

# The servers read the origin with XrdClS3, over HTTPS with XrdClHttp, which
# trusts the CA of the tests
export XRD_PLUGINCONFDIR="$SETUP_DIR/client-plugins"
export X509_CERT_FILE="$CREDENTIALS/ca.pem"
export XRDCLS3_URLSTYLE=path

# start_s3_origin <name> [snippet.cfg...]
#
# Start an HTTPS origin with XrdHttp, and make its bucket. Export S3_ORIGIN,
# the s3:// URL of the origin. XrdClHttp presents a grid proxy of the user,
# which the test CA cannot verify, thus the servers get the path of a proxy
# which cannot exist.
start_s3_origin() {
    local name=$1 data port
    shift

    require_plugin libXrdHttp
    X509_USER_PROXY=/dev/null/x509 start_server "$name" http.cfg tls.cfg "$@" || return 1
    data="$(_setup_var "$name")_DATA"
    port="$(_setup_var "$name")_PORT"
    mkdir -p "${!data}/$BUCKET"
    export S3_ORIGIN="s3://localhost:${!port}"
}

# start_s3_server <name> <role> [snippet.cfg...]
#
# Start a proxy or a cache, with the role proxy or cache, in front of the
# origin at $S3_ORIGIN. XrdClS3 signs its requests with the keys of two files.
start_s3_server() {
    local keys="$BATS_FILE_TMPDIR/s3-keys"

    if [[ ! -d "$keys" ]]; then
        mkdir -p "$keys"
        printf 'access-key\n' > "$keys/access"
        printf 'secret-key\n' > "$keys/secret"
    fi

    ORIGIN=$S3_ORIGIN X509_USER_PROXY=/dev/null/x509 \
        XRDCLS3_ACCESSKEYLOCATION="$keys/access" \
        XRDCLS3_SECRETKEYLOCATION="$keys/secret" \
        start_server "$@"
}
