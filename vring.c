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
 * VirtIO Vring implementation.
 *
 * The module provides vring and basic operations.
 * Enqueuing is multi-threaded safe, dequeuing is not. That is aligned with
 * how the library is intended to be used.
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/eventfd.h>

#include "vring.h"
#include "shm.h"
#include "common.h"

vring_t *
vring_create(void)
{
	vring_t *vring;
	struct vhost_vring *vring_shm;

	vring = calloc(1, sizeof (*vring));
	if (vring == NULL) {
		fprintf(stderr, "vring allocation failed\n");
		return (NULL);
	}

	/* allocate vring from the shared memory */
	vring_shm = zalloc_shared_buf(sizeof (struct vhost_vring));
	if (vring_shm == NULL) {
		fprintf(stderr, "vring allocation failed\n");
		free(vring);
		return (NULL);
	}

	vring->vring_shm = vring_shm;
	vring->desc = vring_shm->desc;
	vring->avail = &vring_shm->avail;
	vring->used = &vring_shm->used;
	vring->num = VHOST_VRING_SIZE;
	vring->last_used_idx = vring->used->idx;
	vring->last_avail_idx = vring->avail->idx;

	vring->kickfd = eventfd(0, EFD_NONBLOCK);
	if (vring->kickfd < 0) {
		free_shared_buf(vring_shm);
		free(vring);
		return (NULL);
	}
	vring->callfd = eventfd(0, EFD_NONBLOCK);
	if (vring->callfd < 0) {
		close(vring->kickfd);
		free_shared_buf(vring_shm);
		free(vring);
		return (NULL);
	}

	return (vring);
}

void
vring_destroy(vring_t *vring)
{
	close(vring->kickfd);
	close(vring->callfd);
	free_shared_buf(vring->vring_shm);
	free(vring);
}

/*
 * Save buffer to vring slot.
 */
static void
vring_set_desc(vring_t *vring, uint16_t idx, virtio_buffer_t *vbuf, bool last)
{
	uint16_t flags = 0;

	if (vbuf->response)
		flags |= VIRTIO_DESC_F_WRITE;
	if (!last)
		flags |= VIRTIO_DESC_F_NEXT;

	/*
	 * We can pass virtual address here and SPDK will translate it to
	 * relative offset from begining of shared region based on information
	 * about memory regions we passed to it during initialization
	 * (see qva_to_vva() in spdk).
	 */
	vring->desc[idx].addr = (uintptr_t) vbuf->buf;
	vring->desc[idx].len = vbuf->size;
	vring->desc[idx].flags = flags;
	//desc[idx].next = ... will be set later
	debug("Setting descriptor %d\n", idx);
}

/*
 * Update avail ring with index of added descriptor and optionally notify the
 * other end about the change.
 *
 * Function is MT-safe.
 */
static void
vring_flush(vring_t *vring, uint16_t desc_idx)
{
	struct vring_avail *avail = vring->avail;
	uint16_t idx;
	uint16_t mask = vring->num - 1;
	bool success = false;
	int rc;

	// this must succeed, ring cannot be full if we previously allocated descs
	while (!success) {
		idx = vring->last_avail_idx;
		success = __sync_bool_compare_and_swap(&vring->last_avail_idx, idx, idx + 1);
	}
	avail->ring[idx & mask] = desc_idx;

	__asm volatile("" ::: "memory");

	success = false;
	while (!success) {
		success = __sync_bool_compare_and_swap(&avail->idx, idx, idx + 1);
	}

	debug("Flush vring (descriptor = %d, avail idx = %d)\n",
	    desc_idx, idx);

	if ((vring->used->flags & VRING_USED_F_NO_NOTIFY) == 0) {
		uint64_t kick_it = 1;

		debug("Kick\n");
		rc = write(vring->kickfd, &kick_it, sizeof(kick_it));
		assert(rc == sizeof(kick_it));
		fsync(vring->kickfd);
	}
}

/*
 * Save task to vring descriptors. This function is multi-producer safe.
 * It returns zero if successful or negative number if vring is full.
 *
 * Function is MT-safe.
 */
int
vring_put_task(vring_t *vring, virtio_task_t *task)
{
	uint16_t prev_idx;
	uint16_t slots[task->count];
	uint16_t mask = vring->num - 1;

	for (int i = 0; i < task->count; i++) {
		uint16_t desc_idx_saved = vring->task_idx;
		uint16_t desc_idx = desc_idx_saved;
		bool success = false;
		bool full = false;

		while (!success) {
			// find a free slot
			while (vring->tasks[desc_idx] != NULL) {
				desc_idx = (desc_idx + 1) & mask;
				if (desc_idx == desc_idx_saved) {
					full = true;
					break;
				}

			}
			if (full)
				break;
			success = __sync_bool_compare_and_swap(&vring->tasks[desc_idx],
			    NULL, task);
		}
		if (!success || full) {
			// all slots occupied, free previously allocated slots
			for (int j = 0; j < i; j++)
				vring->tasks[slots[j]] = NULL;
			return (-1);
		}
		slots[i] = desc_idx;

		// exact value of desc_idx in vring struct is not important - just
		// an optimization not to start iteration always from beginning
		vring->task_idx = (desc_idx + 1) & mask;

		if (i == 0) {
			// store head of the descriptor chain to task
			task->vring_idx = desc_idx;
		} else {
			// set next pointer of previous descriptor now when we know it
			vring->desc[prev_idx].next = desc_idx;
		}

		vring_set_desc(vring, desc_idx, &task->vbufs[i],
		    (i == task->count - 1));
		// mark the desc slot as used by the task
		prev_idx = desc_idx;
	}
	vring_flush(vring, task->vring_idx);

	return (0);
}

/*
 * Return task which has been completed (from used ring) or NULL.
 *
 * This is *NOT* MT-safe.
 */
virtio_task_t *
vring_get_task(vring_t *vring)
{
	struct vring_used *used = vring->used;
	uint16_t u_idx = vring->last_used_idx;
	uint16_t desc_idx, idx;
	virtio_task_t *task;
	int mask = vring->num - 1;

	// used index in SPDK can overflow
	if ((u_idx & mask) == (used->idx & mask))
		return (NULL);

	desc_idx = used->ring[u_idx & mask].id;
	task = vring->tasks[desc_idx];
	task->used_bytes = used->ring[u_idx & mask].len;
	assert(task->vring_idx == desc_idx);
	debug("Descriptor %d is ready\n", desc_idx);
	// mark all descs used by the task as free
	for (idx = desc_idx;
	    (vring->desc[idx].flags & VIRTIO_DESC_F_NEXT) != 0;
	    idx = vring->desc[idx].next) {
		assert(vring->tasks[idx] != NULL);
		vring->tasks[idx] = NULL;
	}
	vring->tasks[idx] = NULL;
	vring->last_used_idx = u_idx + 1;

	return (task);
}
