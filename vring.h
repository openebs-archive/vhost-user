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

#ifndef _VRING_H
#define _VRING_H

#include <limits.h>
#include <stdbool.h>

#include "virtio_task.h"

/* SPDK does not allow more than 1024 entries */
#define	VHOST_VRING_SIZE	1024

// vring_desc I/O buffer descriptor
struct vring_desc {
	uint64_t addr;  // packet data buffer address
	uint32_t len;   // packet data buffer size
	uint16_t flags; // (see below)
	uint16_t next;  // optional index next descriptor in chain
};

// Available vring_desc.flags
enum {
	VIRTIO_DESC_F_NEXT = 1,    // Descriptor continues via 'next' field
	VIRTIO_DESC_F_WRITE = 2,   // Write-only descriptor (otherwise read-only)
	VIRTIO_DESC_F_INDIRECT = 4 // Buffer contains a list of descriptors
};

enum { // flags for avail and used rings
	VRING_F_NO_INTERRUPT  = 1,  // Hint: don't bother to call process
	VRING_F_NO_NOTIFY     = 1,  // Hint: don't bother to kick kernel
	VRING_F_INDIRECT_DESC = 28, // Indirect descriptors are supported
	VRING_F_EVENT_IDX     = 29  // (boring complicated interrupt behavior..)
};

// ring of descriptors that are available to be processed
struct vring_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[VHOST_VRING_SIZE];
};

// ring of descriptors that have already been processed
struct vring_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem { uint32_t id; uint32_t len; } ring[VHOST_VRING_SIZE];
};

/*
 * vring representation from vhost protocol (in shared memory).
 */
struct vhost_vring {
	struct vring_desc desc[VHOST_VRING_SIZE] __attribute__((aligned(4)));
	struct vring_avail avail                 __attribute__((aligned(2)));
	struct vring_used used                   __attribute__((aligned(4096)));
};

/*
 * Client's internal representation of vring
 */
typedef struct vring {
	int kickfd;
	int callfd;
	struct vhost_vring *vring_shm;	/* vring in shared memory */
	struct vring_desc *desc;	/* points into shared mem */
	struct vring_avail *avail;	/* points into shared mem */
	struct vring_used *used;	/* points into shared mem */
	uint16_t num;	/* total # of descriptors */
	uint16_t free_count;	/* # of free descriptors in desc table */
	uint16_t last_avail_idx;
	uint16_t last_used_idx;
	virtio_task_t *tasks[VHOST_VRING_SIZE];
	uint16_t task_idx;	/* index of next inspected entry in tasks array */
} vring_t;

/*
 * Vring methods
 */

vring_t *vring_create(void);
void vring_destroy(vring_t *vring);
int vring_put_task(vring_t *vring, virtio_task_t *task);
virtio_task_t *vring_get_task(vring_t *vring);

#endif /* _VRING_H */
