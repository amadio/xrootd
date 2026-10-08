# Application storage client extraction

This fork branch is based on XRootD PR #2952 at
`377d5a11131ec24f4ca057b98ecbf728ffe2c476`. It extracts the application-facing
storage functionality from the older `rucio-webdav-client`/PR #2868 series.
The Rucio reference snapshot is `a787fb1435b7e5f4bad861670ffdd01671cdb0dc`, whose
optional XRootD dependency is pinned to `c26d41a0695ca962b08a07dde08e72c511988b46`.
This branch does not change that consumer dependency or publish upstream work.

## Selected functionality

- Object-scoped ROOT and HTTP credentials, owned token-file lifetime, TLS
  options and environment snapshots without process-global configuration changes.
- Distinct ROOT channel identities and explicit ZTN credential precedence.
- Resource-scoped WebDAV capability and property parsing from PR #2906.
- Native WebDAV MOVE, recursive MKCOL, quota queries and partial DELETE errors.
- StorageClient API version 2: `from_environment`, `probe`, `stat`, `info`,
  `exists`, `get`, `put`, `delete`, `move`, `listdir`, `mkdir_p`, `space`, `close`.
- Structured checksum negotiation and additional native error categories.

The authentication baseline comes from `725100f84338`; WebDAV operations from
`0c8c684f91e9`; and the application API from `e5924fa4fce5` and its helper
dependencies. These are adapted extractions, not wholesale file replacements.
CLI parsing, output compatibility, token issuance and tape workflows are excluded.
The PUT deadline fix from PR #2901 is already in the base and is not duplicated.

## Compatibility choices

The Pythonic helpers, streams, asyncio and optional fsspec from #2952 remain
available. `FileSystem.checksum` still returns `(algorithm, value)` or raises
OSError. The additive `checksum_info` method returns a native status and
ChecksumInfo for application-level negotiation. StorageClient retains the
native XRootD exception contract expected by the inspected Rucio adapter.

Opaque query parameters are passed unchanged with the remote path, including
ROOT paths with double slashes. Explicit credentials take precedence over
ambient values. Scoped HTTP clients suppress ambient Authorization headers and
X.509 selection; client-only parameters are stripped from wire URLs. File
operations retain their context when following redirects and issuing later reads.

MOVE creates requested destination parents before mutation and propagates
conflicts without retrying them as missing-parent failures. It is restricted to
one endpoint and rejects query-bearing operands. Query-bearing uploads require
`create_parents=False`; signed file URLs are not implicitly repurposed for MKCOL.

Metadata workflows share an operation budget. Transfers check that budget after
preparation and request cooperative cancellation through the native progress
callback. Native timeout granularity and blocked requests/cleanup can exceed the
budget; no hard wall-clock deadline is promised. Closing a client waits for its
active operations before releasing owned credentials.

## Validation boundary

Python contracts include the complete existing #2952 suite and the new auth and
storage tests. Isolated loopback tests exercise the real HTTP plugin with
different bearer contexts, anonymous access, opaque queries, metadata/checksums,
authorization/not-found distinction, namespace operations, quotas, partial DELETE
and upload/download with redirect credential preservation. Native tests cover
WebDAV parsing/configuration and ROOT channel identity separation.

The Rucio adapter's mock tests verify the consumer call/error contract. They are
not a full Rucio installation or production endpoint acceptance test. Credentialed
ROOT/X.509 interoperability, third-party copy and tape remain separate validation
gates. Generic behavior belongs in XRootD; GFAL can wrap this client for small
compatibility choices and its CLI without duplicating protocol implementations.
