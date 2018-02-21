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

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "virtio_task.h"
#include "shm.h"

/*
 * Allocate a task structure with limit on number of tasks passed as parameter.
 */
virtio_task_t *
virtio_task_create(int nbufs)
{
	virtio_buffer_t *vbuf;
	virtio_task_t *task;

	task = calloc(1, sizeof (*task));
	if (task == NULL)
		return (NULL);

	task->count = 0;
	task->limit = nbufs;

	task->vbufs = malloc(nbufs * sizeof (*vbuf));
	if (task->vbufs == NULL) {
		free(task);
		return (NULL);
	}

	return (task);
}

void
virtio_task_destroy(virtio_task_t *task)
{
	virtio_buffer_t *vbuf;

	for (int i = 0; i < task->count; i++) {
		vbuf = &task->vbufs[i];
		if (vbuf->copy)
			free_shared_buf(vbuf->buf);
	}

	free(task->vbufs);
	free(task);
}

/*
 * Allocate buffer suitable for IO and add it to the tail of task buf list.
 */
void *
virtio_task_alloc_buf(virtio_task_t *task, size_t size, bool response,
    bool zero)
{
	return (virtio_task_alloc_buf_part(task, size, response, zero, 0, size));
}

/*
 * Same as above but pass auxiliary byte range information.
 */
void *
virtio_task_alloc_buf_part(virtio_task_t *task, size_t size, bool response,
    bool zero, size_t offset_from, size_t offset_to)
{
	virtio_buffer_t *vbuf;
	void *data;

	assert(task->count < task->limit);

	data = alloc_shared_buf(size, 0);
	if (data == NULL)
		return (NULL);
	if (zero)
		memset(data, 0, size);

	vbuf = &task->vbufs[task->count++];
	vbuf->response = response;
	vbuf->size = size;
	vbuf->buf = data;
	vbuf->copy = true;
	vbuf->offset_from = offset_from;
	vbuf->offset_to = offset_to;

	return (data);
}

/*
 * Add IO buffer which is already allocated to tail of buf list in task.
 */
void
virtio_task_add_buf(virtio_task_t *task, void *buf, size_t size, bool response)
{
	virtio_buffer_t *vbuf;

	assert(task->count < task->limit);

	vbuf = &task->vbufs[task->count++];
	vbuf->response = response;
	vbuf->size = size;
	vbuf->buf = buf;
	vbuf->copy = false;
	vbuf->offset_from = 0;
	vbuf->offset_to = size;
}

/*
 * Return pointer to buffer with given index.
 */
void *
virtio_task_get_buf(virtio_task_t *task, int idx)
{
	if (idx >= task->count)
		return (NULL);

	return (task->vbufs[idx].buf);
}
