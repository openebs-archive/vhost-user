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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
// lock-less ring buffer implementation from DPDK
#include <rte_ring.h>

#include "vring_poller.h"
#include "common.h"

struct vring_poller {
	pthread_t tid;
	bool end;
	vring_t *vring;
};

vring_poller_t *
vring_poller_create(vring_t *vring)
{
	vring_poller_t *poller;

	poller = malloc(sizeof (*poller));
	if (poller == NULL) {
		fprintf(stderr, "Unable to allocate poller\n");
		return (NULL);
	}

	poller->vring = vring;
	poller->tid = 0;
	poller->end = false;

	return (poller);
}

void
vring_poller_destroy(vring_poller_t *poller)
{
	if (poller->tid != 0)
		vring_poller_stop(poller);

	free(poller);
}

/*
 * Call callbacks for processed IOs in a loop.
 */
static void *
vring_poll(void *arg)
{
	vring_poller_t *self = arg;
	vring_t *vring = self->vring;
	virtio_task_t *task;
	int worked;

	while (!self->end) {
		worked = 0;

		// look for finished IOs
		while ((task = vring_get_task(vring)) != NULL) {
			if (task->cb != NULL) {
				task->cb(task, task->arg);
			}
			worked++;
		}

#if (SLEEPY_POLL == 0)
		// if no work, wait for being notified
		int rc;
		struct pollfd fds;
		uint64_t poll_data;

		fds.fd = vring->callfd;
		fds.events = POLLIN;
		fds.revents = 0;

		rc = poll(&fds, 1, 100);
		if (rc < 0) {
			perror("poll");
			break;
		} else if (rc > 0 && (fds.revents & POLLIN) != 0) {
			rc = read(fds.fd, &poll_data, sizeof (poll_data));
			assert(rc == sizeof (poll_data));
		}
#else
		// if no work, sleep for a short time
		usleep(SLEEPY_POLL);
#endif
	}

	self->tid = 0;
	return (NULL);
}

/*
 * Starts a poller thread.
 */
int
vring_poller_start(vring_poller_t *poller)
{
	if (poller->tid != 0)
		return (0);

	poller->end = false;

	if (pthread_create(&poller->tid, NULL, vring_poll, poller) != 0) {
		fprintf(stderr, "Failed to create poller thread\n");
		return (-1);
	}

	return (0);
}

/*
 * Stops a poller thread.
 */
void
vring_poller_stop(vring_poller_t *poller)
{
	if (poller->tid == 0)
		return;

	poller->end = true;

#if (SLEEPY_POLL == 0)
	uint64_t event = 1;
	int rc = write(poller->vring->callfd, &event, sizeof (event));
	assert(rc == sizeof (event));
#endif

	while (poller->tid != 0) {
		usleep(100);
	}
}

/*
 * Save new task into ring buffer for later dispatch to vring.
 *
 * The callback is executed directly by poller thread, so it should be fast and
 * it should not block.
 */
int
vring_submit_task(vring_poller_t *poller, virtio_task_t *task, task_cb_t cb,
    void *ctx)
{
	task->cb = cb;
	task->arg = ctx;

	return vring_put_task(poller->vring, task);
}

/*
 * Synchronization between the task submitter and poller.
 */
struct task_sync_arg {
	pthread_mutex_t *mtx;
	pthread_cond_t *cv;
};

/*
 * Wake up the waiting thread which submitted IO.
 */
static void
vring_submit_sync_callback(virtio_task_t *task, void *arg)
{
	struct task_sync_arg *sync_arg = arg;

	pthread_mutex_lock(sync_arg->mtx);
	pthread_cond_signal(sync_arg->cv);
	pthread_mutex_unlock(sync_arg->mtx);
}

/*
 * Submit the task synchronously, so when this function returns, the result is
 * available.
 */
int
vring_submit_task_sync(vring_poller_t *poller, virtio_task_t *task)
{
	pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
	struct task_sync_arg arg;
	int rc;

	arg.mtx = &mtx;
	arg.cv = &cv;

	pthread_mutex_lock(&mtx);

	rc = vring_submit_task(poller, task, vring_submit_sync_callback, &arg);
	if (rc != 0) {
		pthread_mutex_unlock(&mtx);
		return (-1);
	}
	rc = pthread_cond_wait(&cv, &mtx);
	if (rc != 0)
		return (-1);

	pthread_mutex_unlock(&mtx);

	return (0);
}
