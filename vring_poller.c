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
#include <sys/time.h>
// lock-less ring buffer implementation from DPDK
#include <rte_ring.h>

#include "vring_poller.h"
#include "vhost_user.h"
#include "common.h"

// when sleep interval is dynamic this is lower and upper bound in ms
#define SLEEP_MIN	5
#define SLEEP_MAX	150
#define SLEEP_DELTA_MAX	((SLEEP_MAX - SLEEP_MIN) >> 1)
// boundary at which we switch from event based to time based poll
#ifndef IOPS_HIGH
#define IOPS_HIGH	120000
#endif

struct vring_poller {
	pthread_t tid;	// poller's thread ID
	vring_t *vring;
	bool end;	// terminate poller thread
	unsigned int iops;
	bool sleepy_poll; // optimized for throughput instead of latency
	int sleep;	// sleep interval in poll loop in ms
	int sleep_delta;// last increment(+)/decrement(-) of sleep interval
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
#ifdef POLL_DELAY
	poller->sleepy_poll = true;
	poller->sleep = POLL_DELAY;
#else
	poller->sleepy_poll = false;
	poller->sleep = (SLEEP_MIN + SLEEP_MAX) / 2;
#endif
	poller->sleep_delta = SLEEP_DELTA_MAX;

	return (poller);
}

void
vring_poller_destroy(vring_poller_t *poller)
{
	if (poller->tid != 0)
		vring_poller_stop(poller);

	free(poller);
}

#ifndef POLL_DELAY
/*
 * Evaluate performance and make necessary changes based knowing:
 * current and previous IOPS, last sleep interval delta.
 */
static void
eval_performance(vring_poller_t *poller, unsigned int iops_old,
    unsigned int iops_new)
{
	int new_delta;
	int delta_sign = (poller->sleep_delta >= 0) ? 1 : -1;

	if (iops_old == 0 || iops_new == 0)
		return;

	// figure out change amplifier based on observed perf difference
	// (i.e. 10k IOPS diff from 400k IOPS -> ~ 10s)
	new_delta = (((int)(iops_new - iops_old)) << 9) / (int)iops_old;
	if (new_delta == 0) {
		if (iops_new >= iops_old)
			new_delta = 1;
		else
			new_delta = -1;
	}
	new_delta *= delta_sign;

	if (new_delta > SLEEP_DELTA_MAX)
		new_delta = SLEEP_DELTA_MAX;
	if (new_delta < -SLEEP_DELTA_MAX)
		new_delta = -SLEEP_DELTA_MAX;

	poller->sleep += new_delta;
	poller->sleep_delta = new_delta;

	if (poller->sleep < SLEEP_MIN)
		poller->sleep = SLEEP_MIN;
	if (poller->sleep > SLEEP_MAX)
		poller->sleep = SLEEP_MAX;

	debug("sleep interval delta: %d\n", poller->sleep_delta);
	debug("new sleep interval: %d\n", poller->sleep);
}
#endif

/*
 * Wait for a new work. This one is based on events, so expect good latency but
 * bad throughput.
 */
static void
do_poll(vring_poller_t *poller)
{
	int rc;
	struct pollfd fds;
	uint64_t poll_data;
	vring_t *vring = poller->vring;

	fds.fd = vring->callfd;
	fds.events = POLLIN;
	fds.revents = 0;

	rc = poll(&fds, 1, SLEEP_MAX);
	if (rc < 0) {
		perror("poll");
		return;
	} else if (rc > 0 && (fds.revents & POLLIN) != 0) {
		rc = read(fds.fd, &poll_data, sizeof (poll_data));
		assert(rc == sizeof (poll_data));
	}
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
	int iocount;
	unsigned int iocount_per_sec = 0;
#ifndef POLL_DELAY
	time_t second = 0;
	struct timeval ts;
	int rc;
#endif

	while (!self->end) {
		iocount = 0;

		// look for finished IOs
		while ((task = vring_get_task(vring)) != NULL) {
			if (task->cb != NULL) {
				task->cb(task, task->arg);
			}
			iocount++;
		}
		iocount_per_sec += iocount;

#ifndef POLL_DELAY
		// keep track of iops
		rc = gettimeofday(&ts, NULL);
		assert(rc == 0);
		if (ts.tv_sec != second) {
			debug("IOPS: %d\n", iocount_per_sec);
			if (self->sleepy_poll) {
				// adjust sleep interval
				eval_performance(self, self->iops,
				    iocount_per_sec);
			}
			self->iops = iocount_per_sec;
			iocount_per_sec = 0;
			second = ts.tv_sec;
			if (!self->sleepy_poll && self->iops > IOPS_HIGH) {
				debug("Switching to time based poll\n");
				self->sleepy_poll = true;
				vring->avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;
			} else if (self->sleepy_poll && self->iops < IOPS_HIGH) {
				debug("Switching to event based poll\n");
				self->sleepy_poll = false;
				vring->avail->flags &= (~VRING_AVAIL_F_NO_INTERRUPT);
			}
		}
#endif

		if (!self->sleepy_poll) {
			do_poll(self);
		} else {
			usleep(self->sleep);
		}
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
	uint64_t event = 1;
	int rc;

	if (poller->tid == 0)
		return;

	poller->end = true;

	rc = write(poller->vring->callfd, &event, sizeof (event));
	assert(rc == sizeof (event));

	while (poller->tid != 0) {
		usleep(SLEEP_MAX);
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
