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

#ifndef VIRTIO_TASK_H
#define VIRTIO_TASK_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * IO buffer with metadata.
 */
typedef struct virtio_buffer {
	void *buf;
	size_t size;
	bool response; // is the buffer writable for consumer?
	bool copy;     // did we allocate this buf?
	// Higher level code can store information about sub-range of bytes.
	// Handy for reading/writing partial blocks.
	size_t offset_from;
	size_t offset_to;
} virtio_buffer_t;

struct virtio_task;

/* Callback invoked for task which is done */
typedef void (*task_cb_t)(struct virtio_task *task, void *arg);

/*
 * List of IO buffers which are part of a single task.
 */
typedef struct virtio_task {
	virtio_buffer_t *vbufs;  // buffers array
	int count;  // actual # of bufs in task
	int limit;  // hard limit for # of bufs in task
	uint16_t vring_idx; // vring index if currently dispatched
	uint32_t used_bytes;// bytes which were actually used by consumer
	task_cb_t cb;  // called when buffer is used
	void *arg;     // arg passed to callback
} virtio_task_t;


virtio_task_t *virtio_task_create(int nbufs);
void virtio_task_destroy(virtio_task_t *task);
void *virtio_task_alloc_buf(virtio_task_t *task, size_t size,
    bool response, bool zero);
void *virtio_task_alloc_buf_part(virtio_task_t *task, size_t size,
    bool response, bool zero, size_t offset_from, size_t offset_to);
void virtio_task_add_buf(virtio_task_t *task, void *buf, size_t size,
    bool response);
void *virtio_task_get_buf(virtio_task_t *task, int idx);

#endif  /* VIRTIO_TASK_H */
