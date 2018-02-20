/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Cloudbyte Corporation.
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
 *     * Neither the name of Cloudbyte Corporation nor the names of its
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

#include "virtio_dev.h"
#include "shm.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

struct virtio_fio_options {
	void *padding;
	unsigned mem_mb;
};

static dev_handle_t g_hdl = NULL;
static bool g_virtio_env_initialized = false;

/* Called for each thread to fill in the 'real_file_size' member for
 * each file associated with this thread. This is called prior to
 * the init operation. This call will occur
 * on the initial start up thread if 'create_serialize' is true, or
 * on the thread actually associated with 'thread_data' if 'create_serialize'
 * is false.
 */
static int
virtio_fio_setup(struct thread_data *td)
{
	struct virtio_fio_options *eo = (struct virtio_fio_options *)td->eo;
	int mem_mb = (eo->mem_mb == 0) ? 1024 : eo->mem_mb;
	unsigned int i;
	struct fio_file *f;
	const char *filename = NULL;

	if (!td->o.use_thread) {
		fprintf(stderr, "must set thread=1 when using virtio plugin\n");
		return -1;
	}

	if (!g_virtio_env_initialized) {
		if (libvirtio_dev_init(mem_mb) != 0) {
			fprintf(stderr, "Failed to initialize\n");
			return -1;
		}
		g_virtio_env_initialized = true;
	}

	for_each_file(td, f, i) {
		if (filename != NULL && strcmp(filename, f->file_name) != 0) {
			fprintf(stderr, "Only one device is supported\n");
			return -1;
		}
		if (g_hdl == NULL) {
			g_hdl = open_virtio_dev(f->file_name);
			if (g_hdl == NULL) {
				fprintf(stderr, "Unable to open vhost socket %s\n", f->file_name);
				return -1;
			}

			filename = f->file_name;
			f->real_file_size = virtio_dev_block_size(g_hdl) *
			    virtio_dev_blocks_num(g_hdl);
			f->engine_data = g_hdl;
		} else {
			f->real_file_size = virtio_dev_block_size(g_hdl) *
			    virtio_dev_blocks_num(g_hdl);
			f->engine_data = g_hdl;
		}
	}

	return 0;
}

static int
virtio_fio_open(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int
virtio_fio_close(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int
virtio_fio_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	td->orig_buffer = virtio_buf_alloc(g_hdl, total_mem);
	return td->orig_buffer == NULL;
}

static void
virtio_fio_iomem_free(struct thread_data *td)
{
	virtio_buf_free(td->orig_buffer);
}

static int
virtio_fio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	async_io_desc_t	*io_desc;

	io_desc = calloc(1, sizeof (*io_desc));
	if (io_desc == NULL) {
		return 1;
	}
	io_desc->user_ctx = io_u;
	io_u->engine_data = io_desc;

	return 0;
}

static void
virtio_fio_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	async_io_desc_t *io_desc = io_u->engine_data;

	if (io_desc) {
		free(io_desc);
		io_u->engine_data = NULL;
	}
}

static int
virtio_fio_queue_sync(struct thread_data *td, struct io_u *io_u)
{
	int rc = 1;
	dev_handle_t dev = io_u->file->engine_data;

	if (dev == NULL) {
		fprintf(stderr, "Unable to look up correct I/O target.\n");
		return -1;
	}

	switch (io_u->ddir) {
	case DDIR_READ:
		rc = read_virtio_dev(dev, io_u->buf, io_u->offset, io_u->xfer_buflen);
		break;
	case DDIR_WRITE:
		rc = write_virtio_dev(dev, io_u->buf, io_u->offset, io_u->xfer_buflen);
		break;
	case DDIR_TRIM:
		rc = 0;
		break;
	default:
		assert(false);
		break;
	}

	if (rc < 0) {
		return FIO_Q_BUSY;
	}

	return FIO_Q_COMPLETED;
}

static int
virtio_fio_queue(struct thread_data *td, struct io_u *io_u)
{
	int rc = 1;
	dev_handle_t dev = io_u->file->engine_data;
	async_io_desc_t	*io_desc = io_u->engine_data;

	if (dev == NULL) {
		fprintf(stderr, "Unable to look up correct I/O target.\n");
		return -1;
	}

	io_desc->buf = io_u->buf;
	io_desc->offset = io_u->offset;
	io_desc->nbytes = io_u->xfer_buflen;

	switch (io_u->ddir) {
	case DDIR_READ:
		io_desc->type = VIRTIO_TYPE_READ;
		rc = virtio_submit(dev, io_desc);
		break;
	case DDIR_WRITE:
		io_desc->type = VIRTIO_TYPE_WRITE;
		rc = virtio_submit(dev, io_desc);
		break;
	case DDIR_TRIM:
		rc = 0;
		break;
	default:
		assert(false);
		break;
	}

	if (rc < 0) {
		return FIO_Q_BUSY;
	}

	return FIO_Q_QUEUED;
}

static struct io_u *
virtio_fio_event(struct thread_data *td, int event)
{
	assert(event >= 0);
	dev_handle_t dev = g_hdl;
	async_io_desc_t *io_desc = virtio_get_completed(dev);
	return (io_desc != NULL) ? io_desc->user_ctx : NULL;
}

/*
 * fio calls this function always with min argument equal to one, which is
 * very inefficient, so we override it (performance goes up by ~10%).
 */
static int
virtio_fio_getevents(struct thread_data *td, unsigned int min,
		   unsigned int max, const struct timespec *t)
{
	dev_handle_t dev = g_hdl;
	uint64_t ms = -1;
	min = (td->cur_depth < 10) ? td->cur_depth : 10;

	if (t != NULL)
		ms = t->tv_sec * 1000 + t->tv_nsec / 1000000;

	return virtio_completed(dev, min, ms);
}

static int
virtio_fio_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* TODO: This should probably send a flush to the device, but for now just return successful. */
	return 0;
}

static struct fio_option options[] = {
	{
		.name		= "hugepage_mem",
		.lname		= "Memory to allocate for virtio from huge pages in MB",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct virtio_fio_options, mem_mb),
		.help 		= "Memory to allocate for virtio from huge pages in MB",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= NULL,
	},
};

/*
 * Two fio engines are available, sync and async virtio. Unfortunately only one
 * has to be chosen at compile time.
 */

struct ioengine_ops ioengine = {
//struct ioengine_ops async_ioengine = {
	.name			= "virtio_async",
	.version		= FIO_IOOPS_VERSION,
	.flags			= FIO_RAWIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_MEMALIGN,
	.setup			= virtio_fio_setup,
	.queue			= virtio_fio_queue,
	.getevents		= virtio_fio_getevents,
	.event			= virtio_fio_event,
	.open_file		= virtio_fio_open,
	.close_file		= virtio_fio_close,
	.invalidate		= virtio_fio_invalidate,
	.iomem_alloc		= virtio_fio_iomem_alloc,
	.iomem_free		= virtio_fio_iomem_free,
	.io_u_init		= virtio_fio_io_u_init,
	.io_u_free		= virtio_fio_io_u_free,
	.option_struct_size	= sizeof(struct virtio_fio_options),
	.options		= options,
};

//struct ioengine_ops ioengine = {
struct ioengine_ops sync_ioengine = {
	.name			= "virtio_sync",
	.version		= FIO_IOOPS_VERSION,
	.flags			= FIO_SYNCIO,
	.setup			= virtio_fio_setup,
	.queue			= virtio_fio_queue_sync,
	.open_file		= virtio_fio_open,
	.close_file		= virtio_fio_close,
	.invalidate		= virtio_fio_invalidate,
	.iomem_alloc		= virtio_fio_iomem_alloc,
	.iomem_free		= virtio_fio_iomem_free,
	.option_struct_size	= sizeof(struct virtio_fio_options),
	.options		= options,
};

static void fio_init virtio_fio_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit virtio_fio_unregister(void)
{
	if (g_virtio_env_initialized) {
		if (g_hdl != NULL) {
			close_virtio_dev(g_hdl);
			g_hdl = NULL;
		}

		g_virtio_env_initialized = false;
	}
	unregister_ioengine(&ioengine);
}
