# fio I/O engine for XRootD

`fio-xrootd.so` is an external [fio](https://github.com/axboe/fio) I/O engine
that drives read/write workloads against an XRootD server through the
asynchronous `XrdCl::File` API. Unlike the built-in `http`/S3 engine (which is
synchronous, `iodepth=1`), this engine is genuinely asynchronous: it keeps up to
`iodepth` requests in flight and reaps completions delivered on XrdCl's own
worker threads.

## Requirements

fio ships no development headers (there is no `fio-devel`/`fio-dev` package), and
its engine ABI is version-locked via `FIO_IOOPS_VERSION`. The engine must
therefore be compiled against fio's **source headers at the same version** as the
`fio` binary that will load it; fio refuses an engine with a mismatched I/O-ops
version.

## Building

The engine is off by default. Enable it with `-DENABLE_FIO_ENGINE=ON`:

```sh
cmake -S xrootd -B build -DENABLE_FIO_ENGINE=ON
cmake --build build --target fio-xrootd
# -> build/lib/fio-xrootd.so
```

With no `FIO_SOURCE_DIR` given, CMake locates the installed `fio`, queries its
version, and fetches the matching fio source (a shallow `git clone`) to build
against. To build against an existing, already-configured fio checkout instead
(offline, or a custom version), pass it explicitly:

```sh
cd /path/to/fio && ./configure          # once, to generate config-host.h
cmake -S xrootd -B build -DENABLE_FIO_ENGINE=ON -DFIO_SOURCE_DIR=/path/to/fio
```

The engine is skipped (with a message, not an error) if `fio`/`git` are missing,
under sanitizer builds, or on non-Linux platforms.

## Running

```sh
export LD_LIBRARY_PATH=/path/to/build/lib:$LD_LIBRARY_PATH   # to find libXrdCl
export XRD_FIO_ENGINE=/path/to/build/lib/fio-xrootd.so

fio --name=randread --ioengine=external:$XRD_FIO_ENGINE \
    --filename='root\://host\:1094//path/to/file' \
    --rw=randread --bs=128k --size=1g --iodepth=32
```

**The target is a full `root://` URL given in `filename`.** Because fio splits
filenames on `:`, every colon in the URL must be escaped with a backslash:

```
root\://host\:1094//path/to/file      ==>  root://host:1094//path/to/file
```

List the engine options with:

```sh
fio --enghelp=/path/to/build/lib/fio-xrootd.so
```

## Using multiple files

The engine drives each fio file independently (its own `XrdCl::File`), so a
single job can read/write several targets concurrently — useful for simulating
I/O that spans more than one node in an XRootD cluster. There are two ways to
supply more than one file.

**Explicit list — `filename=`.** Separate URLs with `:`. Because fio itself
splits `filename` on `:`, every colon *inside* each URL must be backslash-escaped
(the list separators are the unescaped ones):

```
filename='root\://host1\:1094//data/a:root\://host2\:1094//data/b'
```

This targets two specific hosts. fio divides `size` across the files.

**Generated set — `filename_format=` + `nrfiles=`.** Give a template and a file
count; fio expands `$jobname`/`$jobnum`/`$filenum` to name each file:

```
filename_format=root://redirector:1094//data/f.$jobnum.$filenum
nrfiles=8
```

Point the template at a single **redirector/manager** URL: the cluster places
the generated files on different data servers, so one job produces I/O across
several nodes without naming them. This is the recommended way to simulate
cluster-wide activity.

Two caveats specific to `filename_format`:

* Unlike `filename=`, the format string is used **verbatim** — its colons must
  **not** be escaped. (The engine strips a `\` before a `:` either way, so an
  escaped format still works, but keep them bare.)
* The URL must live in `filename_format` (or `filename`), **not** in
  `directory=`. fio validates `directory=` with a local `stat()` at startup,
  which fails for a `root://` URL.

See `examples/multifile.fio`.

## Engine options

| Option              | Type | Default | Description                                                        |
|---------------------|------|---------|--------------------------------------------------------------------|
| `xrd_pgio`          | bool | 0       | Use `PgRead`/`PgWrite` (per-4KB-page CRC32C) instead of Read/Write |
| `xrd_readv`         | bool | 0       | Coalesce queued reads into a single `VectorRead` at commit         |
| `xrd_recreate`      | bool | 0       | Delete and recreate the file when opening for write                |
| `xrd_posc`          | bool | 0       | Enable Persist On Successful Close on write open                   |
| `xrd_makepath`      | bool | 1       | Create parent directories when opening for write                   |
| `xrd_timeout`       | int  | 0       | Per-operation XrdCl timeout in seconds (0 = environment default)   |
| `xrd_workerthreads` | int  | 0       | Number of XrdCl worker threads for completion dispatch (0 = XrdCl default) |

Standard XrdCl environment variables (`XRD_WORKERTHREADS`, `XRD_REQUESTTIMEOUT`,
credentials, etc.) apply as usual; `xrd_workerthreads` is a convenience that sets
`XRD_WORKERTHREADS` before XrdCl starts.

## Supported operations

* Sequential and random **read**/**write**, and **mixed** (`randrw`).
* **fsync**/**fdatasync** (`fsync=`, `fdatasync=`) → `XrdCl::File::Sync`.
* File **create/open/close/stat/size**, **truncate** and **unlink**
  (`unlink=1`) via XrdCl.
* **CRC32C page integrity** through `xrd_pgio=1` (`PgRead`/`PgWrite`).
* **Vectored reads** through `xrd_readv=1` (`VectorRead`).
* fio data verification (`verify=crc32c`, ...) works end-to-end.

**Trim** has no XRootD file-level equivalent and is accepted as an immediate
no-op success.

## Notes

* The engine forces `thread=1` (fio jobs run as threads in one process) because
  XrdCl's background threads and global runtime do not survive `fork()`.
* Writes report success/failure only (no byte count); a successful write is
  treated as a full-length write.
* See `examples/` for ready-to-edit job files.

## Implementation notes (for maintainers)

These are the non-obvious constraints behind the engine's design
(`XrdClFioEngine.cc`, `CMakeLists.txt`).

### Version lock — the engine is tied to one fio version

fio has no stable engine ABI. `FIO_IOOPS_VERSION` is bumped between releases
(e.g. fio 3.39 → 36, fio 3.42 → 39) and is checked when the engine is loaded, so
the engine only works with the fio it was built against. This is why the build
resolves the fio *source* from the installed *binary*'s version rather than from
a fixed pin, and why there is no packaged/installed engine: it would break on the
next fio update. If you bump the fio the tests run with, the engine is simply
rebuilt against the matching source — no code change is normally needed, as the
engine only touches long-stable `struct` fields.

### Compiling C++ against fio's C headers

fio's headers are C and are included inside `extern "C"` so the fio symbols the
engine calls (`log_err`, `__td_verror`, the inline/list helpers) keep C linkage
and resolve against the `-rdynamic` fio binary at `dlopen()` time. Two traps:

* `fio.h` → `arch/arch.h` includes C++ `<atomic>` under `__cplusplus`. It must be
  pulled in **before** the `extern "C"` block, otherwise its templates are parsed
  with C linkage and fail to compile. The engine `#include <atomic>` first for
  exactly this reason.
* `fio.h` → `minmax.h` defines `min`/`max` as macros; they are `#undef`'d after
  the fio includes so `std::min`/`std::max` work.
* The engine is registered through `extern "C" void get_ioengine(ioengine_ops **)`
  (fio's documented path for C++ engines) instead of exporting a plain global, to
  avoid name-mangling/static-init issues.
* `-Wno-invalid-offsetof`: fio's intrusive list (`container_of`/`offsetof`) is
  used on `xrd_iou`, which embeds a C++ `ResponseHandler`. The list node is the
  first member, so the offset is 0 and the computation is correct in practice.

### Asynchronous completion bridge (modeled on fio's `rados.c`)

`->queue()` submits an `XrdCl::File` op with a per-`io_u` `ResponseHandler` and
returns `FIO_Q_QUEUED`. XrdCl runs the handler on one of its **worker threads**
(not the submitting thread), so the handler pushes the finished `io_u` onto a
`pthread_mutex`/`pthread_cond`/intrusive-list completion queue that
`->getevents()` drains. The handler **owns and deletes** both the `XRootDStatus`
and the `AnyObject` response; for reads the data has already been read straight
into `io_u->xfer_buf`, and `ChunkInfo`/`PageInfo` lengths are used to set
`io_u->resid` for short reads. Errors map `XRootDStatus::errNo` → `io_u->error`
(falling back to `EIO`).

### File creation on write open

A bare `OpenFlags::Update` **fails with ENOENT** on a file that does not exist
yet — `Update` opens for read+write but does not create. So write opens try
`Update` and, on failure, retry with `OpenFlags::New` to create the file without
truncating an existing one; `xrd_recreate=1` uses `OpenFlags::Delete` to always
start fresh. (`OpenFlags::Read` is used for read-only jobs.)

### VectorRead mode (`xrd_readv=1`)

Reads are not submitted in `->queue()`; they are stashed and coalesced into a
single `VectorRead` per file in `->commit()` (each `ChunkInfo` points at that
`io_u`'s buffer, so data scatters directly into place). Batches are capped at
1024 chunks (XrdCl's per-request limit).

### CI and packaging

The engine is enabled for CI in `.ci/config.cmake` and skipped everywhere it
cannot run (no fio/git, sanitizer builds, non-Linux). `fio` and `git` are test
build-deps in `xrootd.spec` and `debian/control`. The `XRootD::fio` CTest runs
the workloads in `tests/XRootD/fio.sh` against a local server, using the exact
fio the engine was built against (passed as `$FIO`). The engine is intentionally
kept out of the installed packages (see the version-lock rationale above).

## Example job files

`examples/`: `seqwrite.fio`, `randread.fio`, `randrw.fio`, `write-verify.fio`,
`pgio.fio`, `readv.fio`, `multifile.fio`. Set `XRD_FIO_ENGINE` and adjust the
`filename`/`filename_format` URL, then run e.g. `fio examples/randread.fio`.
