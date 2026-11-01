//------------------------------------------------------------------------------
// fio external I/O engine for XRootD, built on the asynchronous XrdCl::File API.
//
// Engine name: "xrootd"          Shared object: fio-xrootd.so
// Usage:       ioengine=external:<path>/fio-xrootd.so
//              filename=root://host:1094//path/to/file
//
// The engine is truly asynchronous: ->queue() submits an XrdCl::File operation
// with a ResponseHandler and returns FIO_Q_QUEUED. XrdCl invokes the handler on
// one of its own worker threads; the handler records the result and pushes the
// io_u onto a completion queue guarded by a mutex/condvar. ->getevents() drains
// that queue. This mirrors the librados engine (engines/rados.c), whose async
// model is structurally identical.
//
// Because the fio headers are C, they are included with C linkage so the fio
// symbols we reference (generic helpers, logging, list ops) resolve against the
// fio binary at dlopen() time. C++ engines are registered through the
// get_ioengine() entry point (see fio ioengines.c).
//------------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <unordered_map>

// fio's arch.h includes <atomic> under C++. Pull it (and the other C++ standard
// headers) in here, before the extern "C" block below, so their include guards
// make the re-includes inside extern "C" no-ops. Otherwise <atomic>'s templates
// would be parsed with C linkage and fail to compile.
#include <atomic>

#include "XProtocol/XProtocol.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClEnv.hh"
#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdCl/XrdClURL.hh"
#include "XrdOuc/XrdOucECMsg.hh"
#include "XrdPosix/XrdPosixMap.hh"
#include "XrdSys/XrdSysPthread.hh"

// fio's C headers. Wrapped in extern "C" so the referenced fio symbols
// (log_err, __td_verror, ...) keep C linkage and resolve against the fio binary
// at dlopen() time. minmax.h (pulled in by fio.h) defines min()/max() as macros,
// which would shadow std::min/std::max, so undef them afterwards.
extern "C" {
#include "fio.h"
#include "optgroup.h"
}
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

//------------------------------------------------------------------------------
// Engine options (parsed by fio into td->eo). First member must be a pad: fio
// treats an option whose off1 == 0 as "unset".
//------------------------------------------------------------------------------
struct xrd_options {
	void         *pad;
	unsigned int  pgio;           // use PgRead/PgWrite (per-page CRC32C)
	unsigned int  readv;          // coalesce queued reads into VectorRead
	unsigned int  recreate;       // delete+recreate file on write open
	unsigned int  posc;           // Persist On Successful Close
	unsigned int  timeout;        // per-op XrdCl timeout in seconds (0 = default)
	unsigned int  workerthreads;  // XrdCl worker threads (0 = XrdCl default)
};

static struct fio_option options[] = {
	{
		.name     = "xrd_pgio",
		.lname    = "XRootD PgRead/PgWrite",
		.type     = FIO_OPT_BOOL,
		.off1     = offsetof(struct xrd_options, pgio),
		.help     = "Use PgRead/PgWrite (per-page CRC32C) instead of Read/Write",
		.def      = "0",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_INVALID,
	},
	{
		.name     = "xrd_readv",
		.lname    = "XRootD VectorRead",
		.type     = FIO_OPT_BOOL,
		.off1     = offsetof(struct xrd_options, readv),
		.help     = "Coalesce queued reads into a single VectorRead at commit",
		.def      = "0",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_INVALID,
	},
	{
		.name     = "xrd_recreate",
		.lname    = "XRootD recreate on write",
		.type     = FIO_OPT_BOOL,
		.off1     = offsetof(struct xrd_options, recreate),
		.help     = "Delete and recreate the file when opening for write",
		.def      = "0",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_INVALID,
	},
	{
		.name     = "xrd_posc",
		.lname    = "XRootD Persist On Successful Close",
		.type     = FIO_OPT_BOOL,
		.off1     = offsetof(struct xrd_options, posc),
		.help     = "Enable Persist On Successful Close on write open",
		.def      = "0",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_INVALID,
	},
	{
		.name     = "xrd_timeout",
		.lname    = "XRootD operation timeout",
		.type     = FIO_OPT_INT,
		.off1     = offsetof(struct xrd_options, timeout),
		.maxval   = 86400,
		.minval   = 0,
		.help     = "Per-operation XrdCl timeout in seconds (0 = environment default)",
		.def      = "0",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_INVALID,
	},
	{
		.name     = "xrd_workerthreads",
		.lname    = "XRootD worker threads",
		.type     = FIO_OPT_INT,
		.off1     = offsetof(struct xrd_options, workerthreads),
		.maxval   = 256,
		.minval   = 0,
		.help     = "Number of XrdCl worker threads for completion dispatch (0 = default)",
		.def      = "0",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_INVALID,
	},
	{
		.name = NULL,
	},
};

//------------------------------------------------------------------------------
// Runtime state
//------------------------------------------------------------------------------
// The limits of a readv request, for a server which does not report them:
// readv_iov_max is the number of segments, readv_ior_max the size of one
// segment, as in XProtocol. The server also limits the total of one readv to
// 2 GiB, with the header of each segment included.
static const uint32_t XRD_READV_IOV_MAX   = XrdProto::maxRvecsz;
static const uint32_t XRD_READV_IOR_MAX   = XrdProto::maxRVdsz;
static const uint64_t XRD_READV_TOTAL_MAX = 0x80000000ULL;
static const uint64_t XRD_READV_SEG_HDR   = XrdProto::rlItemLen;

struct xrd_iou;   // forward declaration

// Per-file engine state (FILE_ENG_DATA).
struct xrd_file {
	XrdCl::File  file;
	uint32_t     readv_iov_max = XRD_READV_IOV_MAX;
	uint32_t     readv_ior_max = XRD_READV_IOR_MAX;
	unsigned int inflight      = 0;   // requests in XrdCl, under xrd_data::lock
};

// Per-thread engine state (td->io_ops_data).
struct xrd_data {
	pthread_mutex_t   lock;
	pthread_cond_t    cond;
	struct flist_head completed;    // finished xrd_iou's awaiting ->getevents()
	struct flist_head pending;      // reads awaiting VectorRead coalescing (readv)
	struct io_u     **events;       // scratch returned by ->event(), sized to iodepth
	unsigned int      nr_completed;
	unsigned int      nr_inflight;  // requests in XrdCl of all files of the job
};

// Count n requests of a file which go to XrdCl, or which come back (n < 0).
// fio can close a file, and end the job, while requests of it are still in
// XrdCl: at the end of an iolog, td_io_getevents() reaps no more events. The
// close and the cleanup wait for these requests (xrd_wait_inflight).
static void xrd_count_inflight(struct xrd_data *d, struct xrd_file *xf, int n)
{
	pthread_mutex_lock(&d->lock);
	xf->inflight += n;
	d->nr_inflight += n;
	pthread_mutex_unlock(&d->lock);
}

// Response handler for a single-operation submission, embedded in and reused by
// its owning xrd_iou across successive I/Os.
class IoHandler : public XrdCl::ResponseHandler
{
public:
	struct xrd_data *d   = nullptr;
	struct xrd_iou  *iou = nullptr;
	struct xrd_file *xf  = nullptr;
	// For sync=1: the file to sync after the write, and the timeout.
	XrdCl::File     *sync_file = nullptr;
	time_t           timeout   = 0;

	void HandleResponse(XrdCl::XRootDStatus *status,
	                    XrdCl::AnyObject    *response) override;
};

// Per-io_u context (io_u->engine_data), allocated in ->io_u_init().
struct xrd_iou {
	struct flist_head     list;
	struct io_u          *io_u;
	IoHandler             handler;
	std::vector<uint32_t> cksums;   // per-page CRC32C for PgWrite
	std::string           errmsg;   // the XRootD error, for ->errdetails()
};

// Normalize a fio file name into an XRootD URL. fio's filename= list is split
// on ':' and the '\' of an escaped "\:" is stripped, so those names arrive
// already clean. filename_format=, by contrast, is stored verbatim (never split
// or unescaped), so a user who escapes its colons the same way leaves literal
// backslashes behind. Drop a '\' that precedes a ':' so both directives accept
// the same URL syntax; on an already-clean name this is a no-op.
static std::string xrd_url(const char *name)
{
	std::string url;
	url.reserve(strlen(name));
	for (const char *p = name; *p; ++p) {
		if (p[0] == '\\' && p[1] == ':')
			continue;
		url += *p;
	}
	return url;
}

// Translate an XRootDStatus into a POSIX errno for fio. For an error response
// of the server, errNo holds a kXR_* code, not an errno, thus fio would print
// "Unknown error 3011" and ignore_error= would never match. XrdPosixMap maps
// both the server codes and the client codes.
static inline int xrd_errno(const XrdCl::XRootDStatus &st)
{
	if (st.IsOK())
		return 0;
	XrdOucECMsg ecm;
	int err = -XrdPosixMap::Result(st, ecm);
	return err > 0 ? err : EIO;
}

void IoHandler::HandleResponse(XrdCl::XRootDStatus *status,
                               XrdCl::AnyObject    *response)
{
	struct io_u *io_u = iou->io_u;

	// sync=1 (O_SYNC) has no equivalent in XRootD: sync the file after the
	// write, and complete the io_u when the sync completes.
	if (sync_file && status->IsOK()) {
		XrdCl::File *f = sync_file;
		sync_file = nullptr;
		delete status;
		delete response;
		XrdCl::XRootDStatus st = f->Sync(this, timeout);
		if (st.IsOK())
			return;
		status   = new XrdCl::XRootDStatus(st);
		response = nullptr;
	}

	if (status->IsOK()) {
		io_u->error = 0;
		io_u->resid = 0;
		if (io_u->ddir == DDIR_READ) {
			uint32_t got = (uint32_t) io_u->xfer_buflen;
			if (response) {
				XrdCl::ChunkInfo *ci = nullptr;
				XrdCl::PageInfo  *pi = nullptr;
				response->Get(ci);
				if (ci)
					got = ci->length;
				else {
					response->Get(pi);
					if (pi)
						got = pi->GetLength();
				}
			}
			if (got < io_u->xfer_buflen)
				io_u->resid = io_u->xfer_buflen - got;
		}
	} else {
		io_u->error = xrd_errno(*status);
		io_u->resid = io_u->xfer_buflen;
		iou->errmsg = status->ToStr();
	}

	delete status;
	delete response;

	// Once the io_u is on the queue, the fio thread can reap it, end the
	// job, and free both this handler (in its xrd_iou) and d. Thus signal
	// before the unlock, and use only local copies. A close can wait on the
	// condition too, thus wake all waiters.
	struct xrd_data *data = d;
	struct xrd_file *file = xf;
	pthread_mutex_lock(&data->lock);
	file->inflight--;
	data->nr_inflight--;
	flist_add_tail(&iou->list, &data->completed);
	data->nr_completed++;
	pthread_cond_broadcast(&data->cond);
	pthread_mutex_unlock(&data->lock);
	// handler is embedded in xrd_iou and reused: do not delete this.
}

// Response handler for a coalesced VectorRead: completes every io_u in the
// batch at once. Allocated per commit-batch and self-deleting.
class VReadHandler : public XrdCl::ResponseHandler
{
public:
	struct xrd_data           *d;
	struct xrd_file           *xf;
	std::vector<struct xrd_iou *> ious;

	VReadHandler(struct xrd_data *data, struct xrd_file *file,
	             std::vector<struct xrd_iou *> &&batch)
		: d(data), xf(file), ious(std::move(batch)) {}

	void HandleResponse(XrdCl::XRootDStatus *status,
	                    XrdCl::AnyObject    *response) override
	{
		int err = xrd_errno(*status);
		std::string msg = err ? status->ToStr() : std::string();

		delete status;
		delete response;

		// See IoHandler: d can be gone after the unlock.
		struct xrd_data *data = d;
		pthread_mutex_lock(&data->lock);
		xf->inflight -= ious.size();
		data->nr_inflight -= ious.size();
		for (struct xrd_iou *iou : ious) {
			iou->io_u->error = err;
			iou->io_u->resid = err ? iou->io_u->xfer_buflen : 0;
			iou->errmsg = msg;
			flist_add_tail(&iou->list, &data->completed);
			data->nr_completed++;
		}
		pthread_cond_broadcast(&data->cond);
		pthread_mutex_unlock(&data->lock);

		delete this;
	}
};

//------------------------------------------------------------------------------
// Engine entry points
//------------------------------------------------------------------------------
static int fio_xrd_get_file_size(struct thread_data *td, struct fio_file *f);
static int xrd_stat_files(struct thread_data *td);

// fio does the submission accounting only for an engine without a commit hook.
// This engine has one, thus it sets the issue time of each io_u that it hands
// to XrdCl, records the submission latency (slat), and counts the io_us of
// each submission. FIO_ASYNCIO_SETS_ISSUE_TIME tells fio to leave the issue
// time to the engine.
static void xrd_set_issue_time(struct thread_data *td, struct io_u *io_u)
{
	if (!fio_fill_issue_time(td))
		return;
	fio_gettime(&io_u->issue_time, NULL);
	// only used for iolog
	if (td->o.read_iolog_file)
		memcpy(&td->last_issue, &io_u->issue_time, sizeof(io_u->issue_time));
}

static void xrd_mark_queued(struct thread_data *td, struct io_u *io_u)
{
	xrd_set_issue_time(td, io_u);
	io_u_queued(td, io_u);
}

static int fio_xrd_setup(struct thread_data *td)
{
	struct xrd_options *o = (struct xrd_options *) td->eo;

	// XrdCl's background threads and global environment do not survive fork(),
	// so run all fio jobs as threads sharing a single XrdCl runtime.
	td->o.use_thread = 1;

	// XrdCl reads the XRD_* variables when fio loads the engine, thus a
	// setenv() here has no effect. Put the value in the XrdCl environment
	// before the first request below starts the PostMaster. The value of the
	// first job applies to all jobs, and XRD_WORKERTHREADS has precedence.
	if (o && o->workerthreads)
		XrdCl::DefaultEnv::GetEnv()->PutInt("WorkerThreads",
		                                    (int) o->workerthreads);

	// An engine with a setup hook must set the size of each file itself:
	// fio then does not call get_file_sizes(), the only caller of
	// ->get_file_size(). Without the size, a read job without size= fails,
	// and fio cannot place the I/O of file_append= or offset=N%.
	return xrd_stat_files(td);
}

// The mode of a file which the engine creates.
static const XrdCl::Access::Mode XRD_FILE_MODE =
	XrdCl::Access::UR | XrdCl::Access::UW |
	XrdCl::Access::GR | XrdCl::Access::OR;

// The size of the requests with which the engine lays out and pre-reads files.
static const uint64_t XRD_SETUP_BS = 1024 * 1024;

// Open a file for the layout: create it, and its directories, if it is missing.
// A File whose open failed cannot open again, thus the retry uses a new one.
static XrdCl::XRootDStatus xrd_open_layout(std::unique_ptr<XrdCl::File> &file,
                                           const std::string &url,
                                           time_t timeout)
{
	auto flags = XrdCl::OpenFlags::Update | XrdCl::OpenFlags::MakePath;
	file.reset(new XrdCl::File());
	XrdCl::XRootDStatus st = file->Open(url, flags, XRD_FILE_MODE, timeout);
	if (!st.IsOK() && xrd_errno(st) == ENOENT) {
		file.reset(new XrdCl::File());
		st = file->Open(url, flags | XrdCl::OpenFlags::New, XRD_FILE_MODE,
		                timeout);
	}
	return st;
}

// Write the data of fio to the range [from, end) of a file.
static int xrd_layout_file(struct thread_data *td, struct fio_file *f,
                           uint64_t from, uint64_t end)
{
	struct xrd_options *o = (struct xrd_options *) td->eo;
	const time_t to = (time_t) o->timeout;

	std::unique_ptr<XrdCl::File> file;
	XrdCl::XRootDStatus st = xrd_open_layout(file, xrd_url(f->file_name), to);

	std::vector<char> buf(XRD_SETUP_BS);
	for (uint64_t off = from; st.IsOK() && off < end; ) {
		uint64_t len = std::min<uint64_t>(buf.size(), end - off);
		fill_io_buffer(td, buf.data(), len, len);
		st = file->Write(off, (uint32_t) len, buf.data(), to);
		off += len;
	}

	if (file->IsOpen()) {
		XrdCl::XRootDStatus cst = file->Close(to);
		if (st.IsOK())
			st = cst;
	}

	if (!st.IsOK()) {
		log_err("fio: xrootd: layout of %s failed: %s\n",
		        f->file_name, st.ToStr().c_str());
		td_verror(td, xrd_errno(st), "xrd_layout");
		return 1;
	}
	return 0;
}

// Make each file of a job which reads at least as large as the range of the
// job, as fio does for a local file. fio writes the missing part of a local
// file before a read job; for this engine, fio leaves the layout to the
// engine. fio gets the sizes of all files before the first job starts, and
// an earlier job, for example a write job before a stonewall, can extend a
// file later. Thus get the size again, under a lock: all jobs share one
// process, and several jobs can name the same file.
static int xrd_layout_files(struct thread_data *td)
{
	static std::mutex mutex;
	std::lock_guard<std::mutex> lock(mutex);

	struct fio_file *f;
	unsigned int i, files = 0;
	unsigned long long bytes = 0;
	for_each_file(td, f, i) {
		uint64_t end = f->file_offset + f->io_size;
		if (f->real_file_size >= end)
			continue;
		fio_file_clear_size_known(f);
		if (fio_xrd_get_file_size(td, f))
			return 1;
		if (f->real_file_size < end) {
			files++;
			bytes += end - f->real_file_size;
		}
	}

	if (!files)
		return 0;

	if (read_only) {
		log_err("fio: refusing extend of file due to read-only\n");
		return 0;
	}

	// The message of fio for the layout of local files
	if (output_format & FIO_OUTPUT_NORMAL)
		log_info("%s: Laying out IO file%s (%u file%s / %s%lluMiB)\n",
		         td->o.name, files > 1 ? "s" : "", files,
		         files > 1 ? "s" : "", files > 1 ? "total " : "",
		         bytes >> 20);

	for_each_file(td, f, i) {
		uint64_t end = f->file_offset + f->io_size;
		if (f->real_file_size >= end)
			continue;
		if (!td->o.allow_create && !fio_file_size_known(f)) {
			log_err("fio: file creation disallowed by allow_file_create=0\n");
			td_verror(td, ENOENT, "xrd_layout");
			return 1;
		}
		if (xrd_layout_file(td, f, f->real_file_size, end))
			return 1;
		f->real_file_size = end;
		fio_file_set_size_known(f);
	}
	return 0;
}

// Read the range of each file once, before the job starts. fio itself reads
// the file descriptor for pre_read=1, which a file of this engine does not
// have, thus the engine reads the files and turns the option off. Through a
// cache (XCache), this fills the cache before the job.
static int xrd_pre_read_files(struct thread_data *td)
{
	struct xrd_options *o = (struct xrd_options *) td->eo;
	const time_t to = (time_t) o->timeout;
	std::vector<char> buf(XRD_SETUP_BS);

	struct fio_file *f;
	unsigned int i;
	for_each_file(td, f, i) {
		XrdCl::File file;
		uint64_t end = std::min<uint64_t>(f->file_offset + f->io_size,
		                                  f->real_file_size);
		// A write job can have a missing file, with no data to read
		if (end <= f->file_offset)
			continue;
		XrdCl::XRootDStatus st = file.Open(xrd_url(f->file_name),
		                                   XrdCl::OpenFlags::Read,
		                                   XrdCl::Access::None, to);
		for (uint64_t off = f->file_offset; st.IsOK() && off < end; ) {
			uint32_t got = 0;
			uint64_t len = std::min<uint64_t>(buf.size(), end - off);
			st = file.Read(off, (uint32_t) len, buf.data(), got, to);
			if (st.IsOK() && !got)
				break;
			off += got;
		}
		if (file.IsOpen()) {
			XrdCl::XRootDStatus cst = file.Close(to);
			if (st.IsOK())
				st = cst;
		}
		if (!st.IsOK()) {
			log_err("fio: xrootd: pre-read of %s failed: %s\n",
			        f->file_name, st.ToStr().c_str());
			td_verror(td, xrd_errno(st), "xrd_pre_read");
			return 1;
		}
	}

	td->o.pre_read = 0;
	return 0;
}

// fio calls this hook after it computes the range of each file. The real size
// limits the offsets that fio generates: a read stays within the data of the
// file, but a write can extend the file up to the end of the range of the job.
// A job which reads gets the missing data of its files first.
static int fio_xrd_post_init(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	// fio adds the files of read_iolog= after the setup hook, thus their
	// size is unknown (-1). fio then repeats a short read at the end of the
	// file, and counts no byte of it.
	for_each_file(td, f, i) {
		if (f->real_file_size == -1ULL && fio_xrd_get_file_size(td, f))
			return 1;
	}

	if (td_read(td) && !td->o.read_iolog_file && xrd_layout_files(td))
		return 1;

	if (td->o.pre_read && xrd_pre_read_files(td))
		return 1;

	if (!td_write(td))
		return 0;

	for_each_file(td, f, i) {
		uint64_t end = f->file_offset + f->io_size;
		if (f->real_file_size < end)
			f->real_file_size = end;
	}
	return 0;
}

static int fio_xrd_init(struct thread_data *td)
{
	// A trim which does nothing would count as done in the results.
	if (td_trim(td)) {
		log_err("fio: xrootd: trim is not supported, XRootD has no trim\n");
		td_verror(td, EOPNOTSUPP, "xrd_init");
		return 1;
	}
	if (td->o.odirect)
		log_info("fio: xrootd: direct=1 has no effect, XRootD has no direct I/O\n");

	struct xrd_data *d = new xrd_data();

	pthread_mutex_init(&d->lock, NULL);
	pthread_cond_init(&d->cond, NULL);
	INIT_FLIST_HEAD(&d->completed);
	INIT_FLIST_HEAD(&d->pending);
	d->nr_completed = 0;
	d->nr_inflight = 0;

	unsigned int depth = td->o.iodepth ? td->o.iodepth : 1;
	d->events = (struct io_u **) calloc(depth, sizeof(struct io_u *));
	if (!d->events) {
		delete d;
		return 1;
	}

	td->io_ops_data = d;
	return 0;
}

static void fio_xrd_cleanup(struct thread_data *td)
{
	struct xrd_data *d = (struct xrd_data *) td->io_ops_data;

	if (!d)
		return;

	// No request of the job may come back after d is gone
	pthread_mutex_lock(&d->lock);
	while (d->nr_inflight)
		pthread_cond_wait(&d->cond, &d->lock);
	pthread_mutex_unlock(&d->lock);

	free(d->events);
	pthread_cond_destroy(&d->cond);
	pthread_mutex_destroy(&d->lock);
	delete d;
}

static int fio_xrd_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct xrd_iou *iou = new xrd_iou();

	INIT_FLIST_HEAD(&iou->list);
	iou->io_u = io_u;
	io_u->engine_data = iou;
	return 0;
}

static void fio_xrd_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct xrd_iou *iou = (struct xrd_iou *) io_u->engine_data;

	if (iou) {
		io_u->engine_data = NULL;
		delete iou;
	}
}

static enum fio_q_status fio_xrd_queue(struct thread_data *td,
                                       struct io_u *io_u)
{
	fio_ro_check(td, io_u);

	struct xrd_data    *d   = (struct xrd_data *) td->io_ops_data;
	struct xrd_options *o   = (struct xrd_options *) td->eo;
	struct xrd_iou     *iou = (struct xrd_iou *) io_u->engine_data;
	struct xrd_file    *xf  = (struct xrd_file *) FILE_ENG_DATA(io_u->file);
	XrdCl::File        *f   = &xf->file;
	time_t              to  = (time_t) o->timeout;

	iou->io_u = io_u;
	iou->handler.d = d;
	iou->handler.iou = iou;
	iou->handler.xf = xf;
	iou->handler.sync_file = nullptr;
	iou->handler.timeout = to;
	iou->errmsg.clear();

	XrdCl::XRootDStatus st;

	switch (io_u->ddir) {
	case DDIR_READ:
		if (o->readv && io_u->xfer_buflen <= xf->readv_ior_max &&
		    io_u->offset + io_u->xfer_buflen <= io_u->file->real_file_size) {
			// Defer submission; ->commit() issues a single VectorRead.
			// A read larger than one readv segment goes out alone. So
			// does a read past the end of the file: the server fails the
			// whole readv for it, where a read returns less data.
			flist_add_tail(&iou->list, &d->pending);
			return FIO_Q_QUEUED;
		}
		xrd_count_inflight(d, xf, 1);
		if (o->pgio)
			st = f->PgRead(io_u->offset, io_u->xfer_buflen,
			               io_u->xfer_buf, &iou->handler, to);
		else
			st = f->Read(io_u->offset, io_u->xfer_buflen,
			             io_u->xfer_buf, &iou->handler, to);
		break;
	case DDIR_WRITE:
		xrd_count_inflight(d, xf, 1);
		if (td->o.sync_io)
			iou->handler.sync_file = f;
		if (o->pgio) {
			iou->cksums.clear();
			st = f->PgWrite(io_u->offset, io_u->xfer_buflen,
			                io_u->xfer_buf, iou->cksums,
			                &iou->handler, to);
		} else {
			st = f->Write(io_u->offset, io_u->xfer_buflen,
			              io_u->xfer_buf, &iou->handler, to);
		}
		break;
	case DDIR_SYNC:
	case DDIR_DATASYNC:
		xrd_count_inflight(d, xf, 1);
		st = f->Sync(&iou->handler, to);
		break;
	default:
		// XRootD has no trim: fio_xrd_init() refuses a trim job, and a
		// trim of verify (trim_percentage=) fails here.
		xrd_set_issue_time(td, io_u);
		io_u->error = EOPNOTSUPP;
		return FIO_Q_COMPLETED;
	}

	if (!st.IsOK()) {
		xrd_count_inflight(d, xf, -1);
		xrd_set_issue_time(td, io_u);
		io_u->error = xrd_errno(st);
		iou->errmsg = st.ToStr();
		return FIO_Q_COMPLETED;
	}

	xrd_mark_queued(td, io_u);
	io_u_mark_submit(td, 1);
	return FIO_Q_QUEUED;
}

static int fio_xrd_commit(struct thread_data *td)
{
	struct xrd_data    *d = (struct xrd_data *) td->io_ops_data;
	struct xrd_options *o = (struct xrd_options *) td->eo;

	if (!o->readv || flist_empty(&d->pending))
		return 0;

	time_t to = (time_t) o->timeout;

	// Group deferred reads by their file so each file gets its own
	// VectorRead request(s).
	std::unordered_map<struct xrd_file *, std::vector<struct xrd_iou *> > groups;
	struct flist_head *n, *tmp;

	flist_for_each_safe(n, tmp, &d->pending) {
		struct xrd_iou *iou = flist_entry(n, struct xrd_iou, list);
		flist_del(n);
		struct xrd_file *xf = (struct xrd_file *) FILE_ENG_DATA(iou->io_u->file);
		groups[xf].push_back(iou);
	}

	for (auto &kv : groups) {
		struct xrd_file               *xf   = kv.first;
		XrdCl::File                   *f    = &xf->file;
		std::vector<struct xrd_iou *> &ious = kv.second;

		for (size_t i = 0; i < ious.size(); ) {
			// Fill a batch up to the number of segments and the total
			// size that the server accepts in one readv.
			XrdCl::ChunkList chunks;
			std::vector<struct xrd_iou *> batch;
			uint64_t total = 0;

			while (i < ious.size() && chunks.size() < xf->readv_iov_max) {
				struct io_u *io_u = ious[i]->io_u;
				uint64_t size = io_u->xfer_buflen + XRD_READV_SEG_HDR;
				if (!chunks.empty() && total + size > XRD_READV_TOTAL_MAX)
					break;
				chunks.push_back(XrdCl::ChunkInfo(io_u->offset,
				                                  io_u->xfer_buflen,
				                                  io_u->xfer_buf));
				batch.push_back(ious[i]);
				total += size;
				++i;
			}

			// The handler can complete and delete itself before
			// VectorRead() returns, thus give it a copy of the batch.
			VReadHandler *h = new VReadHandler(d, xf, std::vector<struct xrd_iou *>(batch));
			xrd_count_inflight(d, xf, (int) batch.size());
			XrdCl::XRootDStatus st = f->VectorRead(chunks, 0, h, to);
			if (st.IsOK()) {
				for (struct xrd_iou *iou : batch)
					xrd_mark_queued(td, iou->io_u);
				io_u_mark_submit(td, batch.size());
			} else {
				// XrdCl does not call the handler: complete the batch
				// with the error, as a response would
				for (struct xrd_iou *iou : batch)
					xrd_set_issue_time(td, iou->io_u);
				h->HandleResponse(new XrdCl::XRootDStatus(st), nullptr);
			}
		}
	}
	return 0;
}

static int fio_xrd_getevents(struct thread_data *td, unsigned int min,
                             unsigned int max, const struct timespec *t)
{
	struct xrd_data *d = (struct xrd_data *) td->io_ops_data;
	unsigned int events = 0;

	pthread_mutex_lock(&d->lock);
	while (events < min || (events < max && d->nr_completed > 0)) {
		if (d->nr_completed == 0) {
			pthread_cond_wait(&d->cond, &d->lock);
			continue;
		}
		struct xrd_iou *iou = flist_first_entry(&d->completed,
		                                        struct xrd_iou, list);
		flist_del(&iou->list);
		d->nr_completed--;
		d->events[events++] = iou->io_u;
	}
	pthread_mutex_unlock(&d->lock);

	return events;
}

// fio prints the text of this hook after the error of a failed io_u, and frees
// it. The text of XRootD has the error of the server, for example its path.
// fio 3.38 added the thread to the arguments of the hook.
static char *xrd_errdetails(struct io_u *io_u)
{
	struct xrd_iou *iou = (struct xrd_iou *) io_u->engine_data;

	if (!iou || iou->errmsg.empty())
		return NULL;
	return strdup(iou->errmsg.c_str());
}

#if XRD_FIO_VERSION >= 338
static char *fio_xrd_errdetails(struct thread_data *td, struct io_u *io_u)
{
	return xrd_errdetails(io_u);
}
#else
static char *fio_xrd_errdetails(struct io_u *io_u)
{
	return xrd_errdetails(io_u);
}
#endif

static struct io_u *fio_xrd_event(struct thread_data *td, int event)
{
	struct xrd_data *d = (struct xrd_data *) td->io_ops_data;

	return d->events[event];
}

// Ask the data server of the file for its readv limits. The answer of each
// server is kept, thus a job with many files asks each server once. A server
// which does not answer gets the defaults.
static void xrd_readv_limits(struct xrd_file *xf, time_t timeout)
{
	static std::mutex mutex;
	static std::unordered_map<std::string, std::pair<uint32_t, uint32_t> > known;

	std::string host;
	if (!xf->file.GetProperty("DataServer", host))
		return;

	{
		std::lock_guard<std::mutex> lock(mutex);
		auto it = known.find(host);
		if (it != known.end()) {
			xf->readv_iov_max = it->second.first;
			xf->readv_ior_max = it->second.second;
			return;
		}
	}

	XrdCl::FileSystem fs(XrdCl::URL("root://" + host));
	XrdCl::Buffer arg, *resp = nullptr;
	arg.FromString("readv_iov_max readv_ior_max");
	XrdCl::XRootDStatus st = fs.Query(XrdCl::QueryCode::Config, arg, resp,
	                                  timeout);
	std::unique_ptr<XrdCl::Buffer> answer(resp);

	// The server answers one line for each key, with the key itself for
	// an unknown key, thus parse only numbers.
	if (st.IsOK() && answer) {
		unsigned long iov = 0, ior = 0;
		std::string text = answer->ToString();
		if (sscanf(text.c_str(), "%lu %lu", &iov, &ior) == 2 &&
		    iov > 0 && ior > 0 && iov <= UINT32_MAX && ior <= UINT32_MAX) {
			xf->readv_iov_max = (uint32_t) iov;
			xf->readv_ior_max = (uint32_t) ior;
		}
	}

	std::lock_guard<std::mutex> lock(mutex);
	known[host] = { xf->readv_iov_max, xf->readv_ior_max };
}

static int fio_xrd_open(struct thread_data *td, struct fio_file *f)
{
	struct xrd_options *o = (struct xrd_options *) td->eo;
	XrdCl::OpenFlags::Flags flags;
	XrdCl::Access::Mode     mode = XrdCl::Access::None;
	const time_t            to   = (time_t) o->timeout;

	// fio opens the files of a write job for read only with verify_only=1,
	// and all files with readonly=1.
	const bool writing = (td_write(td) || td_trim(td)) &&
	                     !td->o.verify_only && !read_only;
	// allow_file_create=0 forbids the creation of a file, and thus of its
	// directories. fio creates the directories of a local file, too.
	const bool create = td->o.allow_create;

	if (writing) {
		// Update opens an existing file for read+write. A bare Update fails
		// if the file does not exist yet, so recreate/create handling is
		// applied below.
		flags = XrdCl::OpenFlags::Update;
		if (create)
			flags |= XrdCl::OpenFlags::MakePath;
		// Delete creates a missing file, thus without create the file is
		// truncated after the open, as fio does with O_TRUNC for a local
		// file.
		if (o->recreate && create)
			flags |= XrdCl::OpenFlags::Delete;   // always start from a fresh file
		if (o->posc)
			flags |= XrdCl::OpenFlags::POSC;
		mode = XRD_FILE_MODE;
	} else {
		flags = XrdCl::OpenFlags::Read;
	}

	const std::string url = xrd_url(f->file_name);

	struct xrd_file *xf = new xrd_file();
	XrdCl::XRootDStatus st = xf->file.Open(url, flags, mode, to);

	// For write workloads without recreate, the file may simply not exist yet:
	// retry with New so it is created (New alone would fail if it existed).
	// Retry only for a missing file: after another error, such as a denied
	// access, the retry fails with "file exists" and hides the real cause.
	if (!st.IsOK() && writing && create && !o->recreate &&
	    xrd_errno(st) == ENOENT) {
		delete xf;
		xf = new xrd_file();
		st = xf->file.Open(url, flags | XrdCl::OpenFlags::New, mode, to);
	}

	if (!st.IsOK()) {
		if (writing && !create && xrd_errno(st) == ENOENT)
			log_err("fio: file creation disallowed by allow_file_create=0\n");
		log_err("fio: xrootd: open %s failed: %s\n",
		        f->file_name, st.ToStr().c_str());
		delete xf;
		td_verror(td, xrd_errno(st), "xrd_open");
		return 1;
	}

	if (o->recreate && writing && !create)
		st = xf->file.Truncate(0, to);

	if (!st.IsOK()) {
		log_err("fio: xrootd: truncate %s failed: %s\n",
		        f->file_name, st.ToStr().c_str());
		XrdCl::XRootDStatus cst = xf->file.Close(to);
		(void) cst;   // the error of the truncate is the one to report
		delete xf;
		td_verror(td, xrd_errno(st), "xrd_open");
		return 1;
	}

	if (o->readv)
		xrd_readv_limits(xf, to);

	FILE_SET_ENG_DATA(f, xf);
	return 0;
}

static int fio_xrd_close(struct thread_data *td, struct fio_file *f)
{
	struct xrd_options *o = (struct xrd_options *) td->eo;
	struct xrd_data *d = (struct xrd_data *) td->io_ops_data;
	struct xrd_file *xf = (struct xrd_file *) FILE_ENG_DATA(f);
	int ret = 0;

	// The requests of the file must come back first: XrdCl refuses the close
	// of a file with requests, and they write to the File and to the io_us.
	if (xf && d) {
		pthread_mutex_lock(&d->lock);
		while (xf->inflight)
			pthread_cond_wait(&d->cond, &d->lock);
		pthread_mutex_unlock(&d->lock);
	}

	// fio ignores the result of ->close_file(), thus a failed close, for
	// example of a file with POSC or with a deferred error of a write,
	// reports itself and sets the error of the job.
	if (xf) {
		XrdCl::XRootDStatus st = xf->file.Close((time_t) o->timeout);
		if (!st.IsOK()) {
			log_err("fio: xrootd: close %s failed: %s\n",
			        f->file_name, st.ToStr().c_str());
			td_verror(td, xrd_errno(st), "xrd_close");
			ret = 1;
		}
		delete xf;
		FILE_SET_ENG_DATA(f, NULL);
	}
	return ret;
}

// Set the size of a file from the result of its Stat. A missing file is not
// an error: a write job creates it, with the size of the job. This is what
// fio does for a missing local file.
static int xrd_set_file_size(struct thread_data *td, struct fio_file *f,
                             const XrdCl::XRootDStatus &st, uint64_t size)
{
	if (!st.IsOK()) {
		int err = xrd_errno(st);
		if (err == ENOENT) {
			f->real_file_size = 0;
			return 0;
		}
		log_err("fio: xrootd: stat %s failed: %s\n",
		        f->file_name, st.ToStr().c_str());
		td_verror(td, err, "xrd_stat");
		return 1;
	}

	f->real_file_size = size;
	fio_file_set_size_known(f);
	return 0;
}

// The URL of a file, or false with the error of the job set
static bool xrd_file_url(struct thread_data *td, struct fio_file *f,
                         XrdCl::URL &url)
{
	url = XrdCl::URL(xrd_url(f->file_name));
	if (url.IsValid())
		return true;
	log_err("fio: xrootd: invalid URL: %s\n", f->file_name);
	td_verror(td, EINVAL, "xrd_url");
	return false;
}

static int fio_xrd_get_file_size(struct thread_data *td, struct fio_file *f)
{
	XrdCl::URL url;
	if (!xrd_file_url(td, f, url))
		return 1;

	struct xrd_options *o = (struct xrd_options *) td->eo;
	XrdCl::FileSystem fs(url);
	XrdCl::StatInfo  *si = nullptr;
	XrdCl::XRootDStatus st = fs.Stat(url.GetPathWithParams(), si,
	                                 (time_t) o->timeout);
	std::unique_ptr<XrdCl::StatInfo> info(si);

	return xrd_set_file_size(td, f, st, info ? info->GetSize() : 0);
}

// The response to the Stat of one file of xrd_stat_files
class StatHandler : public XrdCl::ResponseHandler
{
public:
	XrdCl::XRootDStatus status;
	uint64_t            size = 0;

	explicit StatHandler(XrdSysSemaphore &done) : done(done) {}

	void HandleResponse(XrdCl::XRootDStatus *st,
	                    XrdCl::AnyObject    *response) override
	{
		XrdCl::StatInfo *si = nullptr;
		if (response)
			response->Get(si);
		status = *st;
		size = si ? si->GetSize() : 0;
		delete st;
		delete response;
		// the waiter can delete this handler once the post is done
		done.Post();
	}

private:
	XrdSysSemaphore &done;
};

// Stat all files of a job, XRD_STAT_BATCH at a time. A job can have thousands
// of files, and a Stat after the other costs one round trip for each file.
static int xrd_stat_files(struct thread_data *td)
{
	static const size_t XRD_STAT_BATCH = 64;
	struct xrd_options *o = (struct xrd_options *) td->eo;
	const time_t to = (time_t) o->timeout;

	std::vector<struct fio_file *> files;
	struct fio_file *f;
	unsigned int i;
	for_each_file(td, f, i)
		files.push_back(f);

	for (size_t first = 0; first < files.size(); first += XRD_STAT_BATCH) {
		size_t n = std::min(XRD_STAT_BATCH, files.size() - first);
		XrdSysSemaphore done(0);
		std::vector<std::unique_ptr<XrdCl::FileSystem> > fs(n);
		std::vector<std::unique_ptr<StatHandler> > handlers(n);
		size_t sent = 0;
		int ret = 0;

		for (size_t k = 0; k < n; k++) {
			XrdCl::URL url;
			if (!xrd_file_url(td, files[first + k], url)) {
				ret = 1;
				break;
			}
			fs[k].reset(new XrdCl::FileSystem(url));
			handlers[k].reset(new StatHandler(done));
			XrdCl::XRootDStatus st = fs[k]->Stat(url.GetPathWithParams(),
			                                     handlers[k].get(), to);
			if (st.IsOK())
				sent++;
			else
				handlers[k]->status = st;
		}

		// all handlers which XrdCl has must run before they go away
		for (size_t k = 0; k < sent; k++)
			done.Wait();

		for (size_t k = 0; !ret && k < n; k++) {
			ret = xrd_set_file_size(td, files[first + k],
			                        handlers[k]->status, handlers[k]->size);
		}
		if (ret)
			return 1;
	}
	return 0;
}

// fio expects an errno: the layout of a local file ignores ENOENT.
static int fio_xrd_unlink(struct thread_data *td, struct fio_file *f)
{
	XrdCl::URL url(xrd_url(f->file_name));
	if (!url.IsValid())
		return EINVAL;

	struct xrd_options *o = (struct xrd_options *) td->eo;
	XrdCl::FileSystem fs(url);
	XrdCl::XRootDStatus st = fs.Rm(url.GetPathWithParams(), (time_t) o->timeout);
	return xrd_errno(st);
}

//------------------------------------------------------------------------------
// External engine registration. C++ engines fill in the ops table through
// get_ioengine() (fio ioengines.c). Populate by assignment to avoid depending
// on struct field order.
//------------------------------------------------------------------------------
static struct ioengine_ops ioengine;

extern "C" void get_ioengine(struct ioengine_ops **ops_ptr)
{
	struct ioengine_ops *ops = &ioengine;

	memset(ops, 0, sizeof(*ops));
	ops->name               = "xrootd";
	ops->version            = FIO_IOOPS_VERSION;
	ops->flags              = FIO_DISKLESSIO | FIO_NODISKUTIL;
#if XRD_FIO_VERSION >= 332
	// Before fio 3.32, fio sets the issue time again after ->queue().
	ops->flags             |= FIO_ASYNCIO_SETS_ISSUE_TIME;
#endif
	ops->setup              = fio_xrd_setup;
	ops->init               = fio_xrd_init;
	ops->post_init          = fio_xrd_post_init;
	ops->cleanup            = fio_xrd_cleanup;
	ops->queue              = fio_xrd_queue;
	ops->commit             = fio_xrd_commit;
	ops->getevents          = fio_xrd_getevents;
	ops->event              = fio_xrd_event;
	ops->errdetails         = fio_xrd_errdetails;
	ops->open_file          = fio_xrd_open;
	ops->close_file         = fio_xrd_close;
	ops->unlink_file        = fio_xrd_unlink;
	ops->get_file_size      = fio_xrd_get_file_size;
	ops->io_u_init          = fio_xrd_io_u_init;
	ops->io_u_free          = fio_xrd_io_u_free;
	ops->options            = options;
	ops->option_struct_size = sizeof(struct xrd_options);

	*ops_ptr = ops;
}
