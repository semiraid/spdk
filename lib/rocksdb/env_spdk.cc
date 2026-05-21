/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "../../../draid/shared/readerwriterqueue.h"

#include "rocksdb/env.h"
#include <set>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <inttypes.h>

extern "C" {
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/blob.h"
#include "spdk/blobfs.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
}

namespace rocksdb
{

struct spdk_filesystem *g_fs = NULL;
struct spdk_bs_dev *g_bs_dev;
uint32_t g_lcore = 0;
std::string g_bdev_name;
volatile bool g_spdk_ready = false;
volatile bool g_spdk_start_failure = false;

struct spdk_bdev_desc *g_prewipe_desc = NULL;
struct spdk_io_channel *g_prewipe_channel = NULL;
void *g_prewipe_buffer = NULL;
uint64_t g_prewipe_offset = 0;
uint64_t g_prewipe_total = 0;
uint64_t g_prewipe_chunk = 0;
uint64_t g_prewipe_current = 0;

void SpdkInitializeThread(void);

class SpdkThreadCtx
{
public:
	struct spdk_fs_thread_ctx *channel;

	SpdkThreadCtx(void) : channel(NULL)
	{
		SpdkInitializeThread();
	}

	~SpdkThreadCtx(void)
	{
		if (channel) {
			spdk_fs_free_thread_ctx(channel);
			channel = NULL;
		}
	}

private:
	SpdkThreadCtx(const SpdkThreadCtx &);
	SpdkThreadCtx &operator=(const SpdkThreadCtx &);
};

thread_local SpdkThreadCtx g_sync_args;

static void
set_channel()
{
	struct spdk_thread *thread;

	if (g_fs != NULL && g_sync_args.channel == NULL) {
		thread = spdk_thread_create("spdk_rocksdb", NULL);
		spdk_set_thread(thread);
		g_sync_args.channel = spdk_fs_alloc_thread_ctx(g_fs);
	}
}

static void
__call_fn(void *arg1, void *arg2)
{
	fs_request_fn fn;

	fn = (fs_request_fn)arg1;
	fn(arg2);
}

static void
__send_request(fs_request_fn fn, void *arg)
{
	struct spdk_event *event;

	event = spdk_event_allocate(g_lcore, __call_fn, (void *)fn, arg);
	spdk_event_call(event);
}

static std::string
sanitize_path(const std::string &input, const std::string &mount_directory)
{
	int index = 0;
	std::string name;
	std::string input_tmp;

	input_tmp = input.substr(mount_directory.length(), input.length());
	for (const char &c : input_tmp) {
		if (index == 0) {
			if (c != '/') {
				name = name.insert(index, 1, '/');
				index++;
			}
			name = name.insert(index, 1, c);
			index++;
		} else {
			if (name[index - 1] == '/' && c == '/') {
				continue;
			} else {
				name = name.insert(index, 1, c);
				index++;
			}
		}
	}

	if (name[name.size() - 1] == '/') {
		name = name.erase(name.size() - 1, 1);
	}
	return name;
}

class SpdkSequentialFile : public SequentialFile
{
	struct spdk_file *mFile;
	uint64_t mOffset;
public:
	SpdkSequentialFile(struct spdk_file *file) : mFile(file), mOffset(0) {}
	virtual ~SpdkSequentialFile();

	virtual Status Read(size_t n, Slice *result, char *scratch) override;
	virtual Status Skip(uint64_t n) override;
	virtual Status InvalidateCache(size_t offset, size_t length) override;
};

SpdkSequentialFile::~SpdkSequentialFile(void)
{
	set_channel();
	spdk_file_close(mFile, g_sync_args.channel);
}

Status
SpdkSequentialFile::Read(size_t n, Slice *result, char *scratch)
{
	int64_t ret;

	set_channel();
	// SPDK_NOTICELOG("sequential read size %d\n", n);
	ret = spdk_file_read(mFile, g_sync_args.channel, scratch, mOffset, n);
	if (ret >= 0) {
		mOffset += ret;
		*result = Slice(scratch, ret);
		return Status::OK();
	} else {
		errno = -ret;
		return Status::IOError(spdk_file_get_name(mFile), strerror(errno));
	}
}

Status
SpdkSequentialFile::Skip(uint64_t n)
{
	mOffset += n;
	return Status::OK();
}

Status
SpdkSequentialFile::InvalidateCache(__attribute__((unused)) size_t offset,
				    __attribute__((unused)) size_t length)
{
	return Status::OK();
}

class SpdkRandomAccessFile : public RandomAccessFile
{
	struct spdk_file *mFile;
public:
	SpdkRandomAccessFile(struct spdk_file *file) : mFile(file) {}
	virtual ~SpdkRandomAccessFile();

	virtual Status Read(uint64_t offset, size_t n, Slice *result, char *scratch) const override;
	virtual Status InvalidateCache(size_t offset, size_t length) override;
};

SpdkRandomAccessFile::~SpdkRandomAccessFile(void)
{
	set_channel();
	spdk_file_close(mFile, g_sync_args.channel);
}

Status
SpdkRandomAccessFile::Read(uint64_t offset, size_t n, Slice *result, char *scratch) const
{
	int64_t rc;

	set_channel();
	// SPDK_NOTICELOG("random read size %d\n", n);
	rc = spdk_file_read(mFile, g_sync_args.channel, scratch, offset, n);
	if (rc >= 0) {
		*result = Slice(scratch, n);
		return Status::OK();
	} else {
		errno = -rc;
		return Status::IOError(spdk_file_get_name(mFile), strerror(errno));
	}
}

Status
SpdkRandomAccessFile::InvalidateCache(__attribute__((unused)) size_t offset,
				      __attribute__((unused)) size_t length)
{
	return Status::OK();
}

class SpdkWritableFile : public WritableFile
{
	struct spdk_file *mFile;
	uint64_t mSize;

public:
	SpdkWritableFile(struct spdk_file *file) : mFile(file), mSize(0) {}
	~SpdkWritableFile()
	{
		if (mFile != NULL) {
			Close();
		}
	}

	virtual void SetIOPriority(Env::IOPriority pri)
	{
		if (pri == Env::IO_HIGH) {
			spdk_file_set_priority(mFile, SPDK_FILE_PRIORITY_HIGH);
		}
	}

	virtual Status Truncate(uint64_t size) override
	{
		int rc;

		set_channel();
		rc = spdk_file_truncate(mFile, g_sync_args.channel, size);
		if (!rc) {
			mSize = size;
			return Status::OK();
		} else {
			errno = -rc;
			return Status::IOError(spdk_file_get_name(mFile), strerror(errno));
		}
	}
	virtual Status Close() override
	{
		set_channel();
		spdk_file_close(mFile, g_sync_args.channel);
		mFile = NULL;
		return Status::OK();
	}
	virtual Status Append(const Slice &data) override;
	virtual Status Flush() override
	{
		return Status::OK();
	}
	virtual Status Sync() override
	{
		int rc;

		set_channel();
		rc = spdk_file_sync(mFile, g_sync_args.channel);
		if (!rc) {
			return Status::OK();
		} else {
			errno = -rc;
			return Status::IOError(spdk_file_get_name(mFile), strerror(errno));
		}
	}
	virtual Status Fsync() override
	{
		int rc;

		set_channel();
		rc = spdk_file_sync(mFile, g_sync_args.channel);
		if (!rc) {
			return Status::OK();
		} else {
			errno = -rc;
			return Status::IOError(spdk_file_get_name(mFile), strerror(errno));
		}
	}
	virtual bool IsSyncThreadSafe() const override
	{
		return true;
	}
	virtual uint64_t GetFileSize() override
	{
		return mSize;
	}
	virtual Status InvalidateCache(__attribute__((unused)) size_t offset,
				       __attribute__((unused)) size_t length) override
	{
		return Status::OK();
	}
	virtual Status Allocate(uint64_t offset, uint64_t len) override
	{
		int rc;

		set_channel();
		rc = spdk_file_truncate(mFile, g_sync_args.channel, offset + len);
		if (!rc) {
			return Status::OK();
		} else {
			errno = -rc;
			return Status::IOError(spdk_file_get_name(mFile), strerror(errno));
		}
	}
	virtual Status RangeSync(__attribute__((unused)) uint64_t offset,
				 __attribute__((unused)) uint64_t nbytes) override
	{
		int rc;

		/*
		 * SPDK BlobFS does not have a range sync operation yet, so just sync
		 *  the whole file.
		 */
		set_channel();
		rc = spdk_file_sync(mFile, g_sync_args.channel);
		if (!rc) {
			return Status::OK();
		} else {
			errno = -rc;
			return Status::IOError(spdk_file_get_name(mFile), strerror(errno));
		}
	}
	virtual size_t GetUniqueId(char *id, size_t max_size) const override
	{
		int rc;

		rc = spdk_file_get_id(mFile, id, max_size);
		if (rc < 0) {
			return 0;
		} else {
			return rc;
		}
	}
};

Status
SpdkWritableFile::Append(const Slice &data)
{
	int64_t rc;

	set_channel();
	// SPDK_NOTICELOG("spdk file write data size: %d\n", data.size());
	rc = spdk_file_write(mFile, g_sync_args.channel, (void *)data.data(), mSize, data.size());
	if (rc >= 0) {
		mSize += data.size();
		return Status::OK();
	} else {
		errno = -rc;
		return Status::IOError(spdk_file_get_name(mFile), strerror(errno));
	}
}

class SpdkDirectory : public Directory
{
public:
	SpdkDirectory() {}
	~SpdkDirectory() {}
	Status Fsync() override
	{
		return Status::OK();
	}
};

class SpdkAppStartException : public std::runtime_error
{
public:
	SpdkAppStartException(std::string mess): std::runtime_error(mess) {}
};

class SpdkEnv : public EnvWrapper
{
private:
	pthread_t mSpdkTid;
	std::string mDirectory;
	std::string mConfig;
	std::string mBdev;

public:
	SpdkEnv(Env *base_env, const std::string &dir, const std::string &conf,
		const std::string &bdev, uint64_t cache_size_in_mb);

	virtual ~SpdkEnv();

	virtual Status NewSequentialFile(const std::string &fname,
					 std::unique_ptr<SequentialFile> *result,
					 const EnvOptions &options) override
	{
		if (fname.compare(0, mDirectory.length(), mDirectory) == 0) {
			struct spdk_file *file;
			int rc;

			std::string name = sanitize_path(fname, mDirectory);
			set_channel();
			rc = spdk_fs_open_file(g_fs, g_sync_args.channel,
					       name.c_str(), 0, &file);
			if (rc == 0) {
				result->reset(new SpdkSequentialFile(file));
				return Status::OK();
			} else {
				/* Myrocks engine uses errno(ENOENT) as one
				 * special condition, for the purpose to
				 * support MySQL, set the errno to right value.
				 */
				errno = -rc;
				return Status::IOError(name, strerror(errno));
			}
		} else {
			return EnvWrapper::NewSequentialFile(fname, result, options);
		}
	}

	virtual Status NewRandomAccessFile(const std::string &fname,
					   std::unique_ptr<RandomAccessFile> *result,
					   const EnvOptions &options) override
	{
		if (fname.compare(0, mDirectory.length(), mDirectory) == 0) {
			std::string name = sanitize_path(fname, mDirectory);
			struct spdk_file *file;
			int rc;

			set_channel();
			rc = spdk_fs_open_file(g_fs, g_sync_args.channel,
					       name.c_str(), 0, &file);
			if (rc == 0) {
				result->reset(new SpdkRandomAccessFile(file));
				return Status::OK();
			} else {
				errno = -rc;
				return Status::IOError(name, strerror(errno));
			}
		} else {
			return EnvWrapper::NewRandomAccessFile(fname, result, options);
		}
	}

	virtual Status NewWritableFile(const std::string &fname,
				       std::unique_ptr<WritableFile> *result,
				       const EnvOptions &options) override
	{
		if (fname.compare(0, mDirectory.length(), mDirectory) == 0) {
			std::string name = sanitize_path(fname, mDirectory);
			struct spdk_file *file;
			int rc;

			set_channel();
			rc = spdk_fs_open_file(g_fs, g_sync_args.channel, name.c_str(),
					       SPDK_BLOBFS_OPEN_CREATE, &file);
			if (rc == 0) {
				result->reset(new SpdkWritableFile(file));
				return Status::OK();
			} else {
				errno = -rc;
				return Status::IOError(name, strerror(errno));
			}
		} else {
			return EnvWrapper::NewWritableFile(fname, result, options);
		}
	}

	virtual Status ReuseWritableFile(const std::string &fname,
					 const std::string &old_fname,
					 std::unique_ptr<WritableFile> *result,
					 const EnvOptions &options) override
	{
		return EnvWrapper::ReuseWritableFile(fname, old_fname, result, options);
	}

	virtual Status NewDirectory(__attribute__((unused)) const std::string &name,
				    std::unique_ptr<Directory> *result) override
	{
		result->reset(new SpdkDirectory());
		return Status::OK();
	}
	virtual Status FileExists(const std::string &fname) override
	{
		struct spdk_file_stat stat;
		int rc;
		std::string name = sanitize_path(fname, mDirectory);

		set_channel();
		rc = spdk_fs_file_stat(g_fs, g_sync_args.channel, name.c_str(), &stat);
		if (rc == 0) {
			return Status::OK();
		}
		return EnvWrapper::FileExists(fname);
	}
	virtual Status RenameFile(const std::string &src, const std::string &t) override
	{
		int rc;
		std::string src_name = sanitize_path(src, mDirectory);
		std::string target_name = sanitize_path(t, mDirectory);

		set_channel();
		rc = spdk_fs_rename_file(g_fs, g_sync_args.channel,
					 src_name.c_str(), target_name.c_str());
		if (rc == -ENOENT) {
			return EnvWrapper::RenameFile(src, t);
		}
		return Status::OK();
	}
	virtual Status LinkFile(__attribute__((unused)) const std::string &src,
				__attribute__((unused)) const std::string &t) override
	{
		return Status::NotSupported("SpdkEnv does not support LinkFile");
	}
	virtual Status GetFileSize(const std::string &fname, uint64_t *size) override
	{
		struct spdk_file_stat stat;
		int rc;
		std::string name = sanitize_path(fname, mDirectory);

		set_channel();
		rc = spdk_fs_file_stat(g_fs, g_sync_args.channel, name.c_str(), &stat);
		if (rc == -ENOENT) {
			return EnvWrapper::GetFileSize(fname, size);
		}
		*size = stat.size;
		return Status::OK();
	}
	virtual Status DeleteFile(const std::string &fname) override
	{
		int rc;
		std::string name = sanitize_path(fname, mDirectory);

		set_channel();
		rc = spdk_fs_delete_file(g_fs, g_sync_args.channel, name.c_str());
		if (rc == -ENOENT) {
			return EnvWrapper::DeleteFile(fname);
		}
		return Status::OK();
	}
	virtual Status LockFile(const std::string &fname, FileLock **lock) override
	{
		std::string name = sanitize_path(fname, mDirectory);
		int64_t rc;

		set_channel();
		rc = spdk_fs_open_file(g_fs, g_sync_args.channel, name.c_str(),
				       SPDK_BLOBFS_OPEN_CREATE, (struct spdk_file **)lock);
		if (!rc) {
			return Status::OK();
		} else {
			errno = -rc;
			return Status::IOError(name, strerror(errno));
		}
	}
	virtual Status UnlockFile(FileLock *lock) override
	{
		set_channel();
		spdk_file_close((struct spdk_file *)lock, g_sync_args.channel);
		return Status::OK();
	}
	virtual Status GetChildren(const std::string &dir,
				   std::vector<std::string> *result) override
	{
		std::string::size_type pos;
		std::set<std::string> dir_and_file_set;
		std::string full_path;
		std::string filename;
		std::string dir_name;

		if (dir.find("archive") != std::string::npos) {
			return Status::OK();
		}
		if (dir.compare(0, mDirectory.length(), mDirectory) == 0) {
			spdk_fs_iter iter;
			struct spdk_file *file;
			dir_name = sanitize_path(dir, mDirectory);

			iter = spdk_fs_iter_first(g_fs);
			while (iter != NULL) {
				file = spdk_fs_iter_get_file(iter);
				full_path = spdk_file_get_name(file);
				if (strncmp(dir_name.c_str(), full_path.c_str(), dir_name.length())) {
					iter = spdk_fs_iter_next(iter);
					continue;
				}
				pos = full_path.find("/", dir_name.length() + 1);

				if (pos != std::string::npos) {
					filename = full_path.substr(dir_name.length() + 1, pos - dir_name.length() - 1);
				} else {
					filename = full_path.substr(dir_name.length() + 1);
				}
				dir_and_file_set.insert(filename);
				iter = spdk_fs_iter_next(iter);
			}

			for (auto &s : dir_and_file_set) {
				result->push_back(s);
			}

			result->push_back(".");
			result->push_back("..");

			return Status::OK();
		}
		return EnvWrapper::GetChildren(dir, result);
	}
};

/* The thread local constructor doesn't work for the main thread, since
 * the filesystem hasn't been loaded yet.  So we break out this
 * SpdkInitializeThread function, so that the main thread can explicitly
 * call it after the filesystem has been loaded.
 */
void SpdkInitializeThread(void)
{
	struct spdk_thread *thread;

	if (g_fs != NULL) {
		if (g_sync_args.channel) {
			spdk_fs_free_thread_ctx(g_sync_args.channel);
		}
		thread = spdk_thread_create("spdk_rocksdb", NULL);
		spdk_set_thread(thread);
		g_sync_args.channel = spdk_fs_alloc_thread_ctx(g_fs);
	}
}

static void
fs_load_cb(__attribute__((unused)) void *ctx,
	   struct spdk_filesystem *fs, int fserrno)
{
	fprintf(stderr, "semiraid_app_trace: fs_load_cb fserrno=%d fs=%p\n", fserrno, fs);
	fflush(stderr);
	if (fserrno == 0) {
		g_fs = fs;
	}
	g_spdk_ready = true;
}

static void
base_bdev_event_cb(enum spdk_bdev_event_type type, __attribute__((unused)) struct spdk_bdev *bdev,
		   __attribute__((unused)) void *event_ctx)
{
	printf("Unsupported bdev event: type %d\n", type);
}

static void
cleanup_blobfs_prewipe(void)
{
	if (g_prewipe_channel != NULL) {
		spdk_put_io_channel(g_prewipe_channel);
		g_prewipe_channel = NULL;
	}
	if (g_prewipe_desc != NULL) {
		spdk_bdev_close(g_prewipe_desc);
		g_prewipe_desc = NULL;
	}
	if (g_prewipe_buffer != NULL) {
		spdk_free(g_prewipe_buffer);
		g_prewipe_buffer = NULL;
	}
	g_prewipe_offset = 0;
	g_prewipe_total = 0;
	g_prewipe_chunk = 0;
	g_prewipe_current = 0;
}

static bool
blobfs_format_requested(void)
{
	const char *format = std::getenv("SEMIRAID_APP_BLOBFS_FORMAT_ON_START");
	return format != nullptr && format[0] != '\0' &&
	       !(format[0] == '0' && format[1] == '\0');
}

static uint64_t
env_u64(const char *name, uint64_t default_value)
{
	const char *value = std::getenv(name);
	if (value == nullptr || value[0] == '\0') {
		return default_value;
	}
	return std::strtoull(value, nullptr, 10);
}

static void
start_blobfs_open(void)
{
	int rc;

	fprintf(stderr, "semiraid_app_trace: rocksdb_run enter bdev=%s\n", g_bdev_name.c_str());
	fflush(stderr);
	rc = spdk_bdev_create_bs_dev_ext(g_bdev_name.c_str(), base_bdev_event_cb, NULL,
					 &g_bs_dev);
	fprintf(stderr, "semiraid_app_trace: create_bs_dev rc=%d bs_dev=%p\n", rc, g_bs_dev);
	fflush(stderr);
	if (rc != 0) {
		printf("Could not create blob bdev\n");
		spdk_app_stop(0);
		exit(1);
	}

	g_lcore = spdk_env_get_first_core();

	printf("using bdev %s\n", g_bdev_name.c_str());
	fflush(stdout);
	if (blobfs_format_requested()) {
		struct spdk_blobfs_opts blobfs_opts;
		spdk_fs_opts_init(&blobfs_opts);
		const char *cluster = std::getenv("SEMIRAID_APP_BLOBFS_CLUSTER_SIZE");
		if (cluster != nullptr && cluster[0] != '\0') {
				blobfs_opts.cluster_sz = static_cast<uint32_t>(std::strtoul(cluster, nullptr, 10));
		}
		fprintf(stderr, "semiraid_app_trace: spdk_fs_init cluster_sz=%u\n",
			static_cast<unsigned>(blobfs_opts.cluster_sz));
		fflush(stderr);
		spdk_fs_init(g_bs_dev, &blobfs_opts, __send_request, fs_load_cb, NULL);
	} else {
		fprintf(stderr, "semiraid_app_trace: spdk_fs_load\n");
		fflush(stderr);
		spdk_fs_load(g_bs_dev, __send_request, fs_load_cb, NULL);
	}
}

static void
blobfs_prewipe_failed(int rc)
{
	fprintf(stderr, "semiraid_app_trace: blobfs pre-wipe failed rc=%d offset=%" PRIu64 "\n",
		rc, g_prewipe_offset);
	fflush(stderr);
	cleanup_blobfs_prewipe();
	g_spdk_start_failure = true;
	g_spdk_ready = true;
	spdk_app_stop(rc == 0 ? 1 : rc);
}

static void submit_next_blobfs_prewipe(void);

static void
blobfs_prewipe_done(struct spdk_bdev_io *bdev_io, bool success, __attribute__((unused)) void *cb_arg)
{
	spdk_bdev_free_io(bdev_io);
	if (!success) {
		blobfs_prewipe_failed(-EIO);
		return;
	}
	g_prewipe_offset += g_prewipe_current;
	submit_next_blobfs_prewipe();
}

static void
submit_next_blobfs_prewipe(void)
{
	if (g_prewipe_offset >= g_prewipe_total) {
		fprintf(stderr, "semiraid_app_trace: blobfs pre-wipe done bytes=%" PRIu64 "\n",
			g_prewipe_total);
		fflush(stderr);
		cleanup_blobfs_prewipe();
		start_blobfs_open();
		return;
	}

	g_prewipe_current = std::min<uint64_t>(g_prewipe_chunk,
					       g_prewipe_total - g_prewipe_offset);
	int rc = spdk_bdev_write(g_prewipe_desc, g_prewipe_channel, g_prewipe_buffer,
				 g_prewipe_offset, g_prewipe_current,
				 blobfs_prewipe_done, NULL);
	if (rc != 0) {
		blobfs_prewipe_failed(rc);
	}
}

static void
maybe_prewipe_blobfs_then_open(void)
{
	uint64_t prewipe_bytes = env_u64("SEMIRAID_APP_BLOBFS_PRE_WIPE_BYTES", 0);
	if (!blobfs_format_requested() || prewipe_bytes == 0) {
		start_blobfs_open();
		return;
	}

	int rc = spdk_bdev_open_ext(g_bdev_name.c_str(), true, base_bdev_event_cb,
				    NULL, &g_prewipe_desc);
	if (rc != 0) {
		blobfs_prewipe_failed(rc);
		return;
	}

	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(g_prewipe_desc);
	g_prewipe_channel = spdk_bdev_get_io_channel(g_prewipe_desc);
	if (g_prewipe_channel == NULL) {
		blobfs_prewipe_failed(-ENOMEM);
		return;
	}

	const uint64_t block_size = spdk_bdev_get_block_size(bdev);
	const uint64_t bdev_bytes = spdk_bdev_get_num_blocks(bdev) * block_size;
	g_prewipe_total = std::min<uint64_t>(prewipe_bytes, bdev_bytes);
	g_prewipe_total -= g_prewipe_total % block_size;
	if (g_prewipe_total == 0) {
		cleanup_blobfs_prewipe();
		start_blobfs_open();
		return;
	}

	g_prewipe_chunk = std::min<uint64_t>(4ULL * 1024ULL * 1024ULL, g_prewipe_total);
	g_prewipe_chunk -= g_prewipe_chunk % block_size;
	if (g_prewipe_chunk == 0) {
		blobfs_prewipe_failed(-EINVAL);
		return;
	}
	g_prewipe_buffer = spdk_dma_zmalloc(g_prewipe_chunk, 4096, NULL);
	if (g_prewipe_buffer == NULL) {
		blobfs_prewipe_failed(-ENOMEM);
		return;
	}
	fprintf(stderr, "semiraid_app_trace: blobfs pre-wipe start bytes=%" PRIu64
		" chunk=%" PRIu64 "\n",
		g_prewipe_total, g_prewipe_chunk);
	fflush(stderr);
	submit_next_blobfs_prewipe();
}

static void
rocksdb_run(__attribute__((unused)) void *arg1)
{
	maybe_prewipe_blobfs_then_open();
}

static void
fs_unload_cb(__attribute__((unused)) void *ctx,
	     __attribute__((unused)) int fserrno)
{
	fprintf(stderr, "semiraid_app_trace: fs_unload_cb fserrno=%d\n", fserrno);
	fflush(stderr);
	assert(fserrno == 0);

	spdk_app_stop(0);
}

static void
rocksdb_shutdown(void)
{
	fprintf(stderr, "semiraid_app_trace: rocksdb_shutdown enter fs=%p\n", g_fs);
	fflush(stderr);
	if (g_fs != NULL) {
		spdk_fs_unload(g_fs, fs_unload_cb, NULL);
	} else {
		fs_unload_cb(NULL, 0);
	}
}

static bool kvstore_no_pci_default();
static void kvstore_apply_bdev_opts_from_env();

static void *
initialize_spdk(void *arg)
{
	struct spdk_app_opts *opts = (struct spdk_app_opts *)arg;
	int rc;

	kvstore_apply_bdev_opts_from_env();
	fprintf(stderr, "semiraid_app_trace: spdk_app_start config=%s reactor_mask=%s mem=%d rpc=%s\n",
		opts->json_config_file ? opts->json_config_file : "",
		opts->reactor_mask ? opts->reactor_mask : "",
		opts->mem_size,
		opts->rpc_addr ? opts->rpc_addr : "");
	fflush(stderr);
	rc = spdk_app_start(opts, rocksdb_run, NULL);
	/*
	 * TODO:  Revisit for case of internal failure of
	 * spdk_app_start(), itself.  At this time, it's known
	 * the only application's use of spdk_app_stop() passes
	 * a zero; i.e. no fail (non-zero) cases so here we
	 * assume there was an internal failure and flag it
	 * so we can throw an exception.
	 */
	if (rc) {
		g_spdk_start_failure = true;
	} else {
		spdk_app_fini();
		delete opts;
	}
	pthread_exit(NULL);

}

SpdkEnv::SpdkEnv(Env *base_env, const std::string &dir, const std::string &conf,
		 const std::string &bdev, uint64_t cache_size_in_mb)
	: EnvWrapper(base_env), mDirectory(dir), mConfig(conf), mBdev(bdev)
{
	struct spdk_app_opts *opts = new struct spdk_app_opts;
	const char *reactor_mask = std::getenv("SEMIRAID_APP_SPDK_REACTOR_MASK");
	const char *rpc_addr = std::getenv("SEMIRAID_APP_SPDK_RPC_ADDR");
	const char *mem_mb = std::getenv("SEMIRAID_APP_SPDK_MEM_MB");

	spdk_app_opts_init(opts, sizeof(*opts));
	opts->name = "rocksdb";
	opts->json_config_file = mConfig.c_str();
	opts->shutdown_cb = rocksdb_shutdown;
	opts->tpoint_group_mask = "0x1000";
	if (mem_mb != nullptr && mem_mb[0] != '\0') {
		opts->mem_size = static_cast<int>(std::strtol(mem_mb, nullptr, 10));
	}
	opts->reactor_mask = (reactor_mask != nullptr && reactor_mask[0] != '\0') ?
		reactor_mask : "0x6000";
	if (rpc_addr != nullptr && rpc_addr[0] != '\0') {
		opts->rpc_addr = rpc_addr;
	}
	opts->num_entries = 0;
	opts->no_pci = kvstore_no_pci_default();
	opts->iova_mode = std::getenv("SEMIRAID_APP_SPDK_IOVA_MODE");
	if (opts->iova_mode == nullptr || opts->iova_mode[0] == '\0') {
		opts->iova_mode = "va";
	}

	spdk_fs_set_cache_size(cache_size_in_mb);
	g_bdev_name = mBdev;

	pthread_create(&mSpdkTid, NULL, &initialize_spdk, opts);
	while (!g_spdk_ready && !g_spdk_start_failure)
		;
	if (g_spdk_start_failure) {
		delete opts;
		throw SpdkAppStartException("spdk_app_start() unable to start rocksdb_run()");
	}

	SpdkInitializeThread();
}

SpdkEnv::~SpdkEnv()
{
	fprintf(stderr, "semiraid_app_trace: SpdkEnv dtor enter\n");
	fflush(stderr);
	/* This is a workaround for rocksdb test, we close the files if the rocksdb not
	 * do the work before the test quit.
	 */
	if (g_fs != NULL) {
		spdk_fs_iter iter;
		struct spdk_file *file;

		if (!g_sync_args.channel) {
			fprintf(stderr, "semiraid_app_trace: SpdkEnv dtor initialize thread ctx\n");
			fflush(stderr);
			SpdkInitializeThread();
		}

		fprintf(stderr, "semiraid_app_trace: SpdkEnv dtor close files begin\n");
		fflush(stderr);
		iter = spdk_fs_iter_first(g_fs);
		while (iter != NULL) {
			file = spdk_fs_iter_get_file(iter);
			spdk_file_close(file, g_sync_args.channel);
			iter = spdk_fs_iter_next(iter);
		}
		fprintf(stderr, "semiraid_app_trace: SpdkEnv dtor close files done\n");
		fflush(stderr);
		if (g_sync_args.channel) {
			fprintf(stderr, "semiraid_app_trace: SpdkEnv dtor free thread ctx\n");
			fflush(stderr);
			spdk_fs_free_thread_ctx(g_sync_args.channel);
			g_sync_args.channel = nullptr;
		}
	}

	fprintf(stderr, "semiraid_app_trace: SpdkEnv dtor before app_start_shutdown\n");
	fflush(stderr);
	spdk_app_start_shutdown();
	const char *skip_join = std::getenv("SEMIRAID_APP_SKIP_SPDK_JOIN_ON_SHUTDOWN");
	if (skip_join != nullptr && skip_join[0] != '\0' &&
	    !(skip_join[0] == '0' && skip_join[1] == '\0')) {
		fprintf(stderr, "semiraid_app_trace: SpdkEnv dtor skip pthread_join\n");
		fflush(stderr);
		pthread_detach(mSpdkTid);
		return;
	}
	fprintf(stderr, "semiraid_app_trace: SpdkEnv dtor before pthread_join\n");
	fflush(stderr);
	pthread_join(mSpdkTid, NULL);
	fprintf(stderr, "semiraid_app_trace: SpdkEnv dtor done\n");
	fflush(stderr);
}

Env *NewSpdkEnv(Env *base_env, const std::string &dir, const std::string &conf,
		const std::string &bdev, uint64_t cache_size_in_mb)
{
	try {
		SpdkEnv *spdk_env = new SpdkEnv(base_env, dir, conf, bdev, cache_size_in_mb);
		if (g_fs != NULL) {
			return spdk_env;
		} else {
			delete spdk_env;
			return NULL;
		}
	} catch (SpdkAppStartException &e) {
		SPDK_ERRLOG("NewSpdkEnv: exception caught: %s", e.what());
		return NULL;
	} catch (...) {
		SPDK_ERRLOG("NewSpdkEnv: default exception caught");
		return NULL;
	}
}

class spdk_KVStore: public KVStore {
public:

    void Write(void* src, uint64_t offset, uint64_t length);

    void Read(void* dst, uint64_t offset, uint64_t length);

    spdk_KVStore(const std::string &conf, const std::string &bdev_name);
};


struct hello_context_t {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *bdev_io_channel;
	char *buff;
	const char *bdev_name;
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	size_t buff_len;
    size_t blk_size;
    size_t buf_align;
	uint64_t offset;
	uint64_t length;
    sem_t sem;
};

static constexpr size_t kCtxPoolSize = 512;
static constexpr size_t span_length = 1024;
static hello_context_t* g_hello_context = nullptr;
static struct spdk_thread *g_spdk_thread = nullptr;
static moodycamel::ReaderWriterQueue<hello_context_t*>* g_context_mempool = nullptr;

hello_context_t* alloc_context() {
    hello_context_t* tmp = (hello_context_t*) malloc(sizeof(hello_context_t));
    tmp->bdev = g_hello_context->bdev;
    tmp->bdev_desc = g_hello_context->bdev_desc;
    tmp->bdev_io_channel = g_hello_context->bdev_io_channel;
    tmp->bdev_name = g_hello_context->bdev_name;
    tmp->buff_len = g_hello_context->buff_len;
    tmp->blk_size = g_hello_context->blk_size;
    tmp->buf_align = g_hello_context->buf_align;
    tmp->buff = (char*) spdk_dma_zmalloc(g_hello_context->buff_len, g_hello_context->buf_align, NULL);
    if (!tmp->buff) {
        free(tmp);
        return nullptr;
    }
    sem_init(&tmp->sem, 0, 0);
    return tmp;
}


static void read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct hello_context_t *hello_context = (struct hello_context_t*) cb_arg;

	spdk_bdev_free_io(bdev_io);

    if (success) {
        //SPDK_NOTICELOG("Read string from bdev\n");
    } else {
        SPDK_ERRLOG("bdev io read error\n");
    }

    sem_post(&hello_context->sem);
	g_context_mempool->enqueue(hello_context);
}

static void hello_read(void *arg)
{
    struct hello_context_t *hello_context = (struct hello_context_t*) arg;
    int rc = 0;

    //SPDK_NOTICELOG("Reading io\n");
    rc = spdk_bdev_read_blocks(hello_context->bdev_desc, hello_context->bdev_io_channel,
                hello_context->buff, hello_context->offset, hello_context->length, read_complete, hello_context);

    if (rc == -ENOMEM) {
        SPDK_NOTICELOG("Queueing io\n");
        /* In case we cannot perform I/O now, queue I/O */
        hello_context->bdev_io_wait.bdev = hello_context->bdev;
        hello_context->bdev_io_wait.cb_fn = hello_read;
        hello_context->bdev_io_wait.cb_arg = hello_context;
        spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
                    &hello_context->bdev_io_wait);
    } else if (rc) {
        //SPDK_ERRLOG("%s error while reading from bdev: %d\n", spdk_strerror(-rc), rc);
        spdk_put_io_channel(hello_context->bdev_io_channel);
        spdk_bdev_close(hello_context->bdev_desc);
        spdk_app_stop(-1);
    }
}

static void write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct hello_context_t *hello_context = (struct hello_context_t*) cb_arg;
    uint32_t length;

    /* Complete the I/O */
    spdk_bdev_free_io(bdev_io);

    if (success) {
        //SPDK_NOTICELOG("bdev io write completed successfully\n");
    } else {
        SPDK_ERRLOG("bdev io write error: %d\n", EIO);
        spdk_put_io_channel(hello_context->bdev_io_channel);
        spdk_bdev_close(hello_context->bdev_desc);
        spdk_app_stop(-1);
        return;
    }

    /* Zero the buffer so that we can use it for reading */
    length = spdk_bdev_get_block_size(hello_context->bdev);
    memset(hello_context->buff, 0, length);

    sem_post(&hello_context->sem);
	g_context_mempool->enqueue(hello_context);
}

static void hello_write(void *arg) {
    struct hello_context_t *hello_context = (struct hello_context_t*) arg;
    int rc = 0;

    //SPDK_NOTICELOG("Writing to the bdev\n");
    rc = spdk_bdev_write_blocks(hello_context->bdev_desc, hello_context->bdev_io_channel,
                hello_context->buff, hello_context->offset, hello_context->length, write_complete, hello_context);

    if (rc == -ENOMEM) {
        SPDK_NOTICELOG("Queueing io\n");
        /* In case we cannot perform I/O now, queue I/O */
        hello_context->bdev_io_wait.bdev = hello_context->bdev;
        hello_context->bdev_io_wait.cb_fn = hello_write;
        hello_context->bdev_io_wait.cb_arg = hello_context;
        spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
                    &hello_context->bdev_io_wait);
    } else if (rc) {
        //SPDK_ERRLOG("%s error while writing to bdev: %d\n", spdk_strerror(-rc), rc);
        spdk_put_io_channel(hello_context->bdev_io_channel);
        spdk_bdev_close(hello_context->bdev_desc);
        spdk_app_stop(-1);
    }
}

static void
hello_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		    void *event_ctx)
{
	SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

static pthread_t init_thread;
static std::atomic<bool> init_spdk{false};
static std::atomic<bool> init_spdk_failed{false};

static bool kvstore_no_pci_default()
{
    const char *env = std::getenv("SEMIRAID_APP_SPDK_NO_PCI");
    return env == nullptr || env[0] == '\0' || !(env[0] == '0' && env[1] == '\0');
}

static void kvstore_apply_bdev_opts_from_env()
{
    const char *pool = std::getenv("SEMIRAID_APP_BDEV_IO_POOL_SIZE");
    const char *cache = std::getenv("SEMIRAID_APP_BDEV_IO_CACHE_SIZE");
    if ((pool == nullptr || pool[0] == '\0') &&
        (cache == nullptr || cache[0] == '\0')) {
        return;
    }

    struct spdk_bdev_opts bdev_opts = {};
    spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
    if (pool != nullptr && pool[0] != '\0') {
        bdev_opts.bdev_io_pool_size = static_cast<uint32_t>(std::strtoul(pool, nullptr, 10));
    }
    if (cache != nullptr && cache[0] != '\0') {
        bdev_opts.bdev_io_cache_size = static_cast<uint32_t>(std::strtoul(cache, nullptr, 10));
    }
    int rc = spdk_bdev_set_opts(&bdev_opts);
    if (rc != 0) {
        SPDK_ERRLOG("failed to set bdev options from environment: %d\n", rc);
    }
}

static void kvstore_start(void* arg) {
	SPDK_NOTICELOG("kvstore start\n");
    struct hello_context_t *hello_context = (hello_context_t*) arg;
    uint32_t blk_size, buf_align;
    int rc = 0;
    hello_context->bdev = NULL;
    hello_context->bdev_desc = NULL;

    g_spdk_thread = spdk_get_thread();

    SPDK_NOTICELOG("Successfully started the application\n");

    /*
    * There can be many bdevs configured, but this application will only use
    * the one input by the user at runtime.
    *
    * Open the bdev by calling spdk_bdev_open_ext() with its name.
    * The function will return a descriptor
    */
    SPDK_NOTICELOG("Opening the bdev %s\n", hello_context->bdev_name);
    rc = spdk_bdev_open_ext(hello_context->bdev_name, true, hello_bdev_event_cb, NULL,
                &hello_context->bdev_desc);
    if (rc) {
        SPDK_ERRLOG("Could not open bdev: %s\n", hello_context->bdev_name);
        spdk_app_stop(-1);
        return;
    }

    /* A bdev pointer is valid while the bdev is opened. */
    hello_context->bdev = spdk_bdev_desc_get_bdev(hello_context->bdev_desc);


    SPDK_NOTICELOG("Opening io channel\n");
    /* Open I/O channel */
    hello_context->bdev_io_channel = spdk_bdev_get_io_channel(hello_context->bdev_desc);
    if (hello_context->bdev_io_channel == NULL) {
        SPDK_ERRLOG("Could not create bdev I/O channel!!\n");
        spdk_bdev_close(hello_context->bdev_desc);
        spdk_app_stop(-1);
        return;
    }

    /* Allocate memory for the write buffer.
    * Initialize the write buffer with the string "Hello World!"
    */
    blk_size = spdk_bdev_get_block_size(hello_context->bdev);
    buf_align = spdk_bdev_get_buf_align(hello_context->bdev);
    hello_context->buff_len = blk_size * span_length;
    hello_context->blk_size = blk_size;
    hello_context->buf_align = buf_align;
    hello_context->buff = (char*) spdk_dma_zmalloc(hello_context->buff_len, buf_align, NULL);
    if (!hello_context->buff) {
        SPDK_ERRLOG("Failed to allocate buffer\n");
        spdk_put_io_channel(hello_context->bdev_io_channel);
        spdk_bdev_close(hello_context->bdev_desc);
        spdk_app_stop(-1);
        return;
    }

    for(int i = 0; i < kCtxPoolSize; ++i) {
        hello_context_t* hello_context = alloc_context();
        g_context_mempool->enqueue(hello_context);

    }

	init_spdk.store(true, std::memory_order_release);

	SPDK_NOTICELOG("start finish\n");
}

void spdk_KVStore::Write(void* src, uint64_t offset, uint64_t length) {
    hello_context_t* hello_context;
    bool found;
    do {
        found = g_context_mempool->try_dequeue(hello_context);
        // if (!found) {
        //     SPDK_ERRLOG("run out of bdev_context");
        // }
    } while (!found);
    hello_context->offset = offset;
    hello_context->length = length;
    memcpy(hello_context->buff, src, length * hello_context->blk_size);

    spdk_thread_send_msg(g_spdk_thread, hello_write, hello_context);
    sem_wait(&hello_context->sem);
}


void spdk_KVStore::Read(void* dst, uint64_t offset, uint64_t length) {
    hello_context_t* hello_context;
    bool found;
    do {
        found = g_context_mempool->try_dequeue(hello_context);
        // if (!found) {
        //     SPDK_ERRLOG("run out of bdev_context");
        // }
    } while (!found);
    hello_context->offset = offset;
    hello_context->length = length;

    spdk_thread_send_msg(g_spdk_thread, hello_read, hello_context);
    sem_wait(&hello_context->sem);
    memcpy(dst, hello_context->buff, length * hello_context->blk_size);
}

static void * init_kvstore(void* arg) {
	struct spdk_app_opts *opts = (struct spdk_app_opts *)arg;

    kvstore_apply_bdev_opts_from_env();
    int rc = spdk_app_start(opts, kvstore_start, g_hello_context);

    if (rc) {
        init_spdk_failed.store(true, std::memory_order_release);
        delete opts;
        SPDK_ERRLOG("cannot start kvstore\n");
    }
	return nullptr;
}

spdk_KVStore::spdk_KVStore(const std::string &_conf, const std::string &_bdev_name): KVStore(_conf, _bdev_name) {
    g_hello_context = new hello_context_t();
    g_context_mempool = new moodycamel::ReaderWriterQueue<hello_context_t*>(kCtxPoolSize);
    int rc;

    struct spdk_app_opts *opts = new struct spdk_app_opts;
    const char *reactor_mask = std::getenv("SEMIRAID_APP_SPDK_REACTOR_MASK");
    const char *rpc_addr = std::getenv("SEMIRAID_APP_SPDK_RPC_ADDR");
    const char *mem_mb = std::getenv("SEMIRAID_APP_SPDK_MEM_MB");

    sem_init(&g_hello_context->sem, 0, 0);

    spdk_app_opts_init(opts, sizeof(*opts));
    opts->name = "kvstore";
    opts->json_config_file = _conf.c_str();
    if (mem_mb != nullptr && mem_mb[0] != '\0') {
        opts->mem_size = static_cast<int>(std::strtol(mem_mb, nullptr, 10));
    }
    opts->reactor_mask = (reactor_mask != nullptr && reactor_mask[0] != '\0') ?
        reactor_mask : "0x3000";
    if (rpc_addr != nullptr && rpc_addr[0] != '\0') {
        opts->rpc_addr = rpc_addr;
    }
    opts->num_entries = 0;
    opts->no_pci = kvstore_no_pci_default();
    opts->iova_mode = std::getenv("SEMIRAID_APP_SPDK_IOVA_MODE");
    if (opts->iova_mode == nullptr || opts->iova_mode[0] == '\0') {
        opts->iova_mode = "va";
    }

    g_hello_context->bdev_name = _bdev_name.c_str();

	SPDK_NOTICELOG("before start, conf: %s\n", _conf.c_str());

	pthread_create(&init_thread, NULL, &init_kvstore, opts);

    int waited_ms = 0;
    while (!init_spdk.load(std::memory_order_acquire) &&
           !init_spdk_failed.load(std::memory_order_acquire) &&
           waited_ms < 30000) {
        usleep(10000);
        waited_ms += 10;
    }
    if (!init_spdk.load(std::memory_order_acquire)) {
        init_spdk_failed.store(true, std::memory_order_release);
        throw SpdkAppStartException("spdk_app_start() unable to start kvstore");
    }

	SPDK_NOTICELOG("create spdk kvstore\n");
}

KVStore* NewSpdkKVStore(const std::string &conf, const std::string &bdev_name) {	
	return new spdk_KVStore(conf, bdev_name);
}

} // namespace rocksdb
