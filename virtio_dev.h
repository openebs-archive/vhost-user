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

/*
 * API which presents virtio device accessed over vhost-user protocol through
 * methods resembling file access.
 */

#ifndef VIRTIODEV_H_
#define VIRTIODEV_H_

#include <stddef.h>
#include <sys/types.h>

struct virtio_dev;
typedef struct virtio_dev *dev_handle_t;

enum io_type {
	VIRTIO_TYPE_READ,
	VIRTIO_TYPE_WRITE
};

/*
 * IO descriptor passed as arg when submitting async IO.
 */
typedef struct async_io_desc {
	void *reserved;     // for struct virtio_dev pointer
	enum io_type type;
	void *buf;
	size_t offset;
	size_t nbytes;
	void *user_ctx;
	ssize_t retval;  // code as would have been normally returned by read/write call
} async_io_desc_t;

int libvirtio_dev_init(int mem_mb, int core_mask);
dev_handle_t open_virtio_dev(const char *sock);
size_t virtio_dev_block_size(dev_handle_t hdl);
size_t virtio_dev_blocks_num(dev_handle_t hdl);
void close_virtio_dev(dev_handle_t hdl);
void *virtio_buf_alloc(struct virtio_dev *dev, int size);
void virtio_buf_free(void *buf);

// sync API
int read_virtio_dev(dev_handle_t hdl, void *buf, size_t blk_offset, size_t nblk);
int write_virtio_dev(dev_handle_t hdl, void *buf, size_t blk_offset, size_t nblk);

// async API
int virtio_submit(struct virtio_dev *dev, async_io_desc_t *io_desc);
int virtio_completed(struct virtio_dev *dev, int mincount, int timeout);
async_io_desc_t *virtio_get_completed(struct virtio_dev *dev);

#endif
