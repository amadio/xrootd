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
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

// fio's arch.h includes <atomic> under C++. Pull it (and the other C++ standard
// headers) in here, before the extern "C" block below, so their include guards
// make the re-includes inside extern "C" no-ops. Otherwise <atomic>'s templates
// would be parsed with C linkage and fail to compile.
#include <atomic>

#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdCl/XrdClURL.hh"

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
	unsigned int  makepath;       // create parent dirs on write open
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
		.name     = "xrd_makepath",
		.lname    = "XRootD create parent path",
		.type     = FIO_OPT_BOOL,
		.off1     = offsetof(struct xrd_options, makepath),
		.help     = "Create parent directories when opening for write",
		.def      = "1",
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
static const size_t XRD_MAX_VREAD_CHUNKS = 1024;   // XrdCl default per-request cap

struct xrd_iou;   // forward declaration

// Per-thread engine state (td->io_ops_data).
struct xrd_data {
	pthread_mutex_t   lock;
	pthread_cond_t    cond;
	struct flist_head completed;    // finished xrd_iou's awaiting ->getevents()
	struct flist_head pending;      // reads awaiting VectorRead coalescing (readv)
	struct io_u     **events;       // scratch returned by ->event(), sized to iodepth
	unsigned int      nr_completed;
	unsigned int      nr_pending;
};

// Response handler for a single-operation submission, embedded in and reused by
// its owning xrd_iou across successive I/Os.
class IoHandler : public XrdCl::ResponseHandler
{
public:
	struct xrd_data *d   = nullptr;
	struct xrd_iou  *iou = nullptr;

	void HandleResponse(XrdCl::XRootDStatus *status,
	                    XrdCl::AnyObject    *response) override;
};

// Per-io_u context (io_u->engine_data), allocated in ->io_u_init().
struct xrd_iou {
	struct flist_head     list;
	struct io_u          *io_u;
	IoHandler             handler;
	std::vector<uint32_t> cksums;   // per-page CRC32C for PgWrite
};

// Translate an XRootDStatus into a POSIX errno for fio.
static inline int xrd_errno(const XrdCl::XRootDStatus &st)
{
	if (st.IsOK())
		return 0;
	return st.errNo ? (int) st.errNo : EIO;
}

void IoHandler::HandleResponse(XrdCl::XRootDStatus *status,
                               XrdCl::AnyObject    *response)
{
	struct io_u *io_u = iou->io_u;

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
	}

	pthread_mutex_lock(&d->lock);
	flist_add_tail(&iou->list, &d->completed);
	d->nr_completed++;
	pthread_mutex_unlock(&d->lock);
	pthread_cond_signal(&d->cond);

	delete status;
	delete response;
	// handler is embedded in xrd_iou and reused: do not delete this.
}

// Response handler for a coalesced VectorRead: completes every io_u in the
// batch at once. Allocated per commit-batch and self-deleting.
class VReadHandler : public XrdCl::ResponseHandler
{
public:
	struct xrd_data           *d;
	std::vector<struct xrd_iou *> ious;

	VReadHandler(struct xrd_data *data, std::vector<struct xrd_iou *> &&batch)
		: d(data), ious(std::move(batch)) {}

	void HandleResponse(XrdCl::XRootDStatus *status,
	                    XrdCl::AnyObject    *response) override
	{
		int err = xrd_errno(*status);

		pthread_mutex_lock(&d->lock);
		for (struct xrd_iou *iou : ious) {
			iou->io_u->error = err;
			iou->io_u->resid = err ? iou->io_u->xfer_buflen : 0;
			flist_add_tail(&iou->list, &d->completed);
			d->nr_completed++;
		}
		pthread_mutex_unlock(&d->lock);
		pthread_cond_broadcast(&d->cond);

		delete status;
		delete response;
		delete this;
	}
};

//------------------------------------------------------------------------------
// Engine entry points
//------------------------------------------------------------------------------
static int fio_xrd_setup(struct thread_data *td)
{
	struct xrd_options *o = (struct xrd_options *) td->eo;

	// XrdCl's background threads and global environment do not survive fork(),
	// so run all fio jobs as threads sharing a single XrdCl runtime.
	td->o.use_thread = 1;

	if (o && o->workerthreads) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%u", o->workerthreads);
		setenv("XRD_WORKERTHREADS", buf, 1);
	}
	return 0;
}

static int fio_xrd_init(struct thread_data *td)
{
	struct xrd_data *d = new xrd_data();

	pthread_mutex_init(&d->lock, NULL);
	pthread_cond_init(&d->cond, NULL);
	INIT_FLIST_HEAD(&d->completed);
	INIT_FLIST_HEAD(&d->pending);
	d->nr_completed = 0;
	d->nr_pending = 0;

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
	XrdCl::File        *f   = (XrdCl::File *) FILE_ENG_DATA(io_u->file);
	time_t              to  = (time_t) o->timeout;

	iou->io_u = io_u;
	iou->handler.d = d;
	iou->handler.iou = iou;

	XrdCl::XRootDStatus st;

	switch (io_u->ddir) {
	case DDIR_READ:
		if (o->readv) {
			// Defer submission; ->commit() issues a single VectorRead.
			flist_add_tail(&iou->list, &d->pending);
			d->nr_pending++;
			return FIO_Q_QUEUED;
		}
		if (o->pgio)
			st = f->PgRead(io_u->offset, io_u->xfer_buflen,
			               io_u->xfer_buf, &iou->handler, to);
		else
			st = f->Read(io_u->offset, io_u->xfer_buflen,
			             io_u->xfer_buf, &iou->handler, to);
		break;
	case DDIR_WRITE:
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
		st = f->Sync(&iou->handler, to);
		break;
	case DDIR_TRIM:
		// XRootD has no file-level trim; treat as an immediate no-op success.
		io_u->error = 0;
		return FIO_Q_COMPLETED;
	default:
		io_u->error = EINVAL;
		return FIO_Q_COMPLETED;
	}

	if (!st.IsOK()) {
		io_u->error = xrd_errno(st);
		return FIO_Q_COMPLETED;
	}
	return FIO_Q_QUEUED;
}

static int fio_xrd_commit(struct thread_data *td)
{
	struct xrd_data    *d = (struct xrd_data *) td->io_ops_data;
	struct xrd_options *o = (struct xrd_options *) td->eo;

	if (!o->readv || flist_empty(&d->pending))
		return 0;

	time_t to = (time_t) o->timeout;

	// Group deferred reads by their XrdCl::File so each file gets its own
	// VectorRead request(s).
	std::unordered_map<XrdCl::File *, std::vector<struct xrd_iou *> > groups;
	struct flist_head *n, *tmp;

	flist_for_each_safe(n, tmp, &d->pending) {
		struct xrd_iou *iou = flist_entry(n, struct xrd_iou, list);
		flist_del(n);
		d->nr_pending--;
		XrdCl::File *f = (XrdCl::File *) FILE_ENG_DATA(iou->io_u->file);
		groups[f].push_back(iou);
	}

	for (auto &kv : groups) {
		XrdCl::File                   *f    = kv.first;
		std::vector<struct xrd_iou *> &ious = kv.second;

		for (size_t i = 0; i < ious.size(); ) {
			size_t n = std::min(ious.size() - i, XRD_MAX_VREAD_CHUNKS);
			XrdCl::ChunkList chunks;
			std::vector<struct xrd_iou *> batch;
			chunks.reserve(n);
			batch.reserve(n);

			for (size_t j = 0; j < n; ++j) {
				struct xrd_iou *iou = ious[i + j];
				chunks.push_back(XrdCl::ChunkInfo(iou->io_u->offset,
				                                  iou->io_u->xfer_buflen,
				                                  iou->io_u->xfer_buf));
				batch.push_back(iou);
			}

			VReadHandler *h = new VReadHandler(d, std::move(batch));
			XrdCl::XRootDStatus st = f->VectorRead(chunks, 0, h, to);
			if (!st.IsOK()) {
				int err = xrd_errno(st);
				pthread_mutex_lock(&d->lock);
				for (struct xrd_iou *iou : h->ious) {
					iou->io_u->error = err;
					iou->io_u->resid = iou->io_u->xfer_buflen;
					flist_add_tail(&iou->list, &d->completed);
					d->nr_completed++;
				}
				pthread_mutex_unlock(&d->lock);
				pthread_cond_broadcast(&d->cond);
				delete h;
			}
			i += n;
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

static struct io_u *fio_xrd_event(struct thread_data *td, int event)
{
	struct xrd_data *d = (struct xrd_data *) td->io_ops_data;

	return d->events[event];
}

static int fio_xrd_open(struct thread_data *td, struct fio_file *f)
{
	struct xrd_options *o = (struct xrd_options *) td->eo;
	XrdCl::OpenFlags::Flags flags;
	XrdCl::Access::Mode     mode = XrdCl::Access::None;

	const bool writing = td_write(td) || td_trim(td);

	if (writing) {
		// Update opens an existing file for read+write. A bare Update fails
		// if the file does not exist yet, so recreate/create handling is
		// applied below.
		flags = XrdCl::OpenFlags::Update;
		if (o->makepath)
			flags |= XrdCl::OpenFlags::MakePath;
		if (o->recreate)
			flags |= XrdCl::OpenFlags::Delete;   // always start from a fresh file
		if (o->posc)
			flags |= XrdCl::OpenFlags::POSC;
		mode = XrdCl::Access::UR | XrdCl::Access::UW |
		       XrdCl::Access::GR | XrdCl::Access::OR;
	} else {
		flags = XrdCl::OpenFlags::Read;
	}

	XrdCl::File *file = new XrdCl::File();
	XrdCl::XRootDStatus st = file->Open(f->file_name, flags, mode);

	// For write workloads without recreate, the file may simply not exist yet:
	// retry with New so it is created (New alone would fail if it existed).
	if (!st.IsOK() && writing && !o->recreate) {
		delete file;
		file = new XrdCl::File();
		st = file->Open(f->file_name, flags | XrdCl::OpenFlags::New, mode);
	}

	if (!st.IsOK()) {
		log_err("fio: xrootd: open %s failed: %s\n",
		        f->file_name, st.ToStr().c_str());
		delete file;
		td_verror(td, xrd_errno(st), "xrd_open");
		return 1;
	}

	FILE_SET_ENG_DATA(f, file);
	return 0;
}

static int fio_xrd_close(struct thread_data *td, struct fio_file *f)
{
	XrdCl::File *file = (XrdCl::File *) FILE_ENG_DATA(f);
	int ret = 0;

	if (file) {
		XrdCl::XRootDStatus st = file->Close();
		if (!st.IsOK())
			ret = 1;
		delete file;
		FILE_SET_ENG_DATA(f, NULL);
	}
	return ret;
}

static int fio_xrd_get_file_size(struct thread_data *td, struct fio_file *f)
{
	XrdCl::URL url(f->file_name);
	if (!url.IsValid()) {
		log_err("fio: xrootd: invalid URL: %s\n", f->file_name);
		return 1;
	}

	XrdCl::FileSystem fs(url);
	XrdCl::StatInfo  *si = nullptr;
	XrdCl::XRootDStatus st = fs.Stat(url.GetPath(), si);

	// A missing file is not an error here: write jobs create it and fio uses
	// the configured size. Report 0 so setup can proceed.
	f->real_file_size = (st.IsOK() && si) ? si->GetSize() : 0;

	delete si;
	return 0;
}

static int fio_xrd_unlink(struct thread_data *td, struct fio_file *f)
{
	XrdCl::URL url(f->file_name);
	if (!url.IsValid())
		return 1;

	XrdCl::FileSystem fs(url);
	XrdCl::XRootDStatus st = fs.Rm(url.GetPath());
	return st.IsOK() ? 0 : 1;
}

static int fio_xrd_invalidate(struct thread_data *td, struct fio_file *f)
{
	// Network target: nothing to invalidate in a local page cache.
	return 0;
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
	ops->setup              = fio_xrd_setup;
	ops->init               = fio_xrd_init;
	ops->cleanup            = fio_xrd_cleanup;
	ops->queue              = fio_xrd_queue;
	ops->commit             = fio_xrd_commit;
	ops->getevents          = fio_xrd_getevents;
	ops->event              = fio_xrd_event;
	ops->open_file          = fio_xrd_open;
	ops->close_file         = fio_xrd_close;
	ops->invalidate         = fio_xrd_invalidate;
	ops->unlink_file        = fio_xrd_unlink;
	ops->get_file_size      = fio_xrd_get_file_size;
	ops->io_u_init          = fio_xrd_io_u_init;
	ops->io_u_free          = fio_xrd_io_u_free;
	ops->options            = options;
	ops->option_struct_size = sizeof(struct xrd_options);

	*ops_ptr = ops;
}
