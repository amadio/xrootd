# XrdHttpS3

XrdHttpS3 is an external handler of XrdHttp. It answers the S3 request
ListObjectsV2 for a bucket, which is a directory of the top directory of the
server. XrdHttp answers the other requests of an S3 client that this needs:
GET, PUT, HEAD and DELETE of an object.

With the handler, an XrdHttp server is an origin for the S3 client plugin
XrdClS3, for example under a cache (XCache) with `pss.origin s3://...`, which
lists directories and finds a directory with ListObjectsV2.

## Configuration

```
http.exthandler s3 libXrdHttpS3.so bucket1,bucket2
```

The parameter is the list of the buckets, with commas between the names. With
the option `+notls` after the name of the handler, the handler also answers
`http://` URLs.

The bucket `bucket1` is the directory `/bucket1`. The handler takes the
requests `GET /bucket1` and `GET /bucket1/`, thus a GET of a bucket does not
get the HTML listing of XrdHttp.

## Limits

- The handler does not check the SigV4 signature of a request. The file system
  authorizes the client of XrdHttp, as for the other requests.
- The handler answers only path-style requests (`https://host/bucket`), not
  virtual-host requests (`https://bucket.host`).
- The delimiter must be `/`. A listing without a delimiter fails with the
  status 501 (NotImplemented).
- A key cannot have an empty name, `.` or `..` between two `/`.
- For a prefix which ends with `/`, the listing has the key of the prefix
  when the directory exists. S3 has this key for the object which marks a
  directory. Thus an empty directory exists for an S3 client.
- The continuation token is the last key of the page.
- The make install does not install the handler. It is for the tests of
  XrdClS3, and a start of S3 support in the server.
