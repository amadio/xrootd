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

## Example job files

`examples/`: `seqwrite.fio`, `randread.fio`, `randrw.fio`, `write-verify.fio`,
`pgio.fio`, `readv.fio`. Set `XRD_FIO_ENGINE` and adjust the `filename` URL, then
run e.g. `fio examples/randread.fio`.
