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
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>
// DPDK include
#include <rte_ring.h>

// includes coming from SPDK
#include "virtio_scsi.h"
#include "scsi_spec.h"
#include "endian.h"
// Our own includes
#include "virtio_dev.h"
#include "virtio_task.h"
#include "vhost_client.h"
#include "vring_poller.h"
#include "vring.h"
#include "shm.h"
#include "common.h"

// output queue size for async operations
#define OUTRING_SIZE	4096
// payload size for SCSI inquiry command
#define VIRTIO_SCAN_PAYLOAD_SIZE 256
// hardcoded target ID and LUN
#define TARGET_ID	1
// hardcoded because we don't scan all possible LUN IDs
#define LUN_ID	0

/*
 * Each type of queue has its own vring
 * but we use only the request queue.
 */
enum virtio_queue_type {
	VIRTIO_SCSI_CONTROLQ = 0,
	VIRTIO_SCSI_EVENTQ,
	VIRTIO_SCSI_REQUESTQ,
	VIRTIO_SCSI_QCOUNT,
};

/*
 * Virtio device state.
 */
struct virtio_dev {
	vhost_client_t *client;
	vring_t *vring[VIRTIO_SCSI_QCOUNT];
	vring_poller_t *poller;
	int block_size;
	size_t num_blocks;
	size_t cap;
	// async IOs
	struct rte_ring *outring;
#if (SLEEPY_POLL == 0)
	int outfd;
#endif
};

int
libvirtio_dev_init(int mem_mb)
{
	return (init_shared_mem(mem_mb));
}

static int
send_inquiry(struct virtio_dev *dev)
{
	virtio_task_t *task;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	struct spdk_scsi_cdb_inquiry *cdb;
	struct spdk_scsi_cdb_inquiry_data *inquiry_data;
	int rc;

	task = virtio_task_create(3);
	if (task == NULL)
		return (-1);

	req = virtio_task_alloc_buf(task, sizeof (*req), false, true);
	resp = virtio_task_alloc_buf(task, sizeof (*resp), true, true);
	inquiry_data = virtio_task_alloc_buf(task, VIRTIO_SCAN_PAYLOAD_SIZE, true, false);

	req->lun[0] = TARGET_ID;
	req->lun[1] = LUN_ID;

	cdb = (struct spdk_scsi_cdb_inquiry *)req->cdb;
	cdb->opcode = SPDK_SPC_INQUIRY;
	cdb->evpd = 0;
	cdb->page_code = 0;
	to_be16(cdb->alloc_len, VIRTIO_SCAN_PAYLOAD_SIZE);
	//cdb->control =

	rc = vring_submit_task_sync(dev->poller, task);
	if (rc != 0) {
		virtio_task_destroy(task);
		return (-1);
	}

	if (resp->response != VIRTIO_SCSI_S_OK ||
	    resp->status != SPDK_SCSI_STATUS_GOOD) {
		virtio_task_destroy(task);
		return (-1);
	}

	if (inquiry_data->peripheral_device_type != SPDK_SPC_PERIPHERAL_DEVICE_TYPE_DISK ||
	    inquiry_data->peripheral_qualifier != SPDK_SPC_PERIPHERAL_QUALIFIER_CONNECTED) {
		fprintf(stderr, "Unsupported peripheral device type 0x%02x (qualifier 0x%02x)\n",
			     inquiry_data->peripheral_device_type,
			     inquiry_data->peripheral_qualifier);
		return (-1);
	}

	inquiry_data->product_id[sizeof (inquiry_data->product_id) - 1] = '\0';
	inquiry_data->t10_vendor_id[sizeof (inquiry_data->t10_vendor_id) - 1] = '\0';
	debug("Device name: %s, Vendor: %s\n",
	    inquiry_data->product_id, inquiry_data->t10_vendor_id);
	return (0);
}

static int
send_test_unit_ready(struct virtio_dev *dev)
{
	virtio_task_t *task;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	int rc;

	task = virtio_task_create(2);
	if (task == NULL)
		return (-1);

	req = virtio_task_alloc_buf(task, sizeof (*req), false, true);
	resp = virtio_task_alloc_buf(task, sizeof (*resp), true, true);

	req->lun[0] = TARGET_ID;
	req->lun[1] = LUN_ID;

	req->cdb[0] = SPDK_SPC_TEST_UNIT_READY;

	rc = vring_submit_task_sync(dev->poller, task);
	if (rc != 0) {
		virtio_task_destroy(task);
		return (-1);
	}

	if (resp->response != VIRTIO_SCSI_S_OK ||
	    resp->status != SPDK_SCSI_STATUS_GOOD) {
		virtio_task_destroy(task);
		return (-1);
	}

	return (0);
}

static int
send_read_cap(struct virtio_dev *dev)
{
	virtio_task_t *task;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	uint8_t *payload;
	int rc;

	task = virtio_task_create(3);
	if (task == NULL)
		return (-1);

	req = virtio_task_alloc_buf(task, sizeof (*req), false, true);
	resp = virtio_task_alloc_buf(task, sizeof (*resp), true, true);
	payload = virtio_task_alloc_buf(task, 32, true, true);

	req->lun[0] = TARGET_ID;
	req->lun[1] = LUN_ID;

	req->cdb[0] = SPDK_SPC_SERVICE_ACTION_IN_16;
	req->cdb[1] = SPDK_SBC_SAI_READ_CAPACITY_16;
	to_be32(&req->cdb[10], 32);

	rc = vring_submit_task_sync(dev->poller, task);
	if (rc != 0) {
		virtio_task_destroy(task);
		return (-1);
	}

	if (resp->response != VIRTIO_SCSI_S_OK ||
	    resp->status != SPDK_SCSI_STATUS_GOOD) {
		virtio_task_destroy(task);
		return (-1);
	}

	dev->num_blocks = from_be64(payload) + 1;
	dev->block_size = from_be32(payload + 8);
	dev->cap = dev->block_size * dev->num_blocks;
	debug("Block size %d. Number of blocks %lu => Capacity %lu bytes.\n",
	    dev->block_size, dev->num_blocks, dev->cap);

	return (0);
}

dev_handle_t
open_virtio_dev(const char *sock)
{
	struct virtio_dev *dev;

	dev = calloc(1, sizeof (*dev));
	if (dev == NULL)
		return (NULL);

	dev->client = new_vhost_client(sock);
	if (dev->client == NULL)
		goto error;

	for (int i = 0; i < VIRTIO_SCSI_QCOUNT; i++) {
		dev->vring[i] = vring_create();
		if (dev->vring[i] == NULL)
			goto error;
	}

	if (init_vhost_client(dev->client) != 0)
		goto error;

	/* push vring info to the server */
	for (int i = 0; i < VIRTIO_SCSI_QCOUNT; i++) {
		if (set_vhost_vring(dev->client, dev->vring[i], i) != 0)
			goto error;
	}

	/* output queue for async requests */
	dev->outring = rte_ring_create("output_request_ring", OUTRING_SIZE,
				 SOCKET_ID_ANY, 0);
	if (dev->outring == NULL)
		goto error;
#if (SLEEPY_POLL == 0)
	/* notifications when new item arrives to output ring go here */
	dev->outfd = eventfd(0, EFD_NONBLOCK);
	if (dev->outfd < 0) {
		goto error;
	}
#endif

	// TODO: we ignore all other rings except request ring
	dev->poller = vring_poller_create(dev->vring[VIRTIO_SCSI_REQUESTQ]);
	if (dev->poller == NULL)
		goto error;

	if (vring_poller_start(dev->poller) != 0)
		goto error;

	if (send_inquiry(dev) != 0)
		goto error;
	if (send_test_unit_ready(dev) != 0)
		goto error;
	if (send_read_cap(dev) != 0)
		goto error;

	return (dev);

error:
	if (dev->poller != NULL)
		vring_poller_destroy(dev->poller);
	if (dev->client != NULL)
		end_vhost_client(dev->client);
	for (int i = 0; i < VIRTIO_SCSI_QCOUNT; i++) {
		if (dev->vring[i] != NULL)
			vring_destroy(dev->vring[i]);
	}
	if (dev->outring != NULL)
		rte_ring_free(dev->outring);
#if (SLEEPY_POLL == 0)
	if (dev->outfd > 0)
		close(dev->outfd);
#endif
	free(dev);
	return (NULL);
}

void
close_virtio_dev(dev_handle_t hdl)
{
	struct virtio_dev *dev = hdl;

	vring_poller_destroy(dev->poller);
	end_vhost_client(dev->client);
	for (int i = 0; i < VIRTIO_SCSI_QCOUNT; i++)
		vring_destroy(dev->vring[i]);
#if (SLEEPY_POLL == 0)
	close(dev->outfd);
#endif
	rte_ring_free(dev->outring);
	free(dev);
}

size_t
capacity_virtio_dev(dev_handle_t hdl)
{
	struct virtio_dev *dev = hdl;

	return (dev->cap);
}

static void
create_rw_cdb(struct virtio_dev *dev, void *buf,
    size_t offset_blocks, size_t num_blocks, int is_write)
{
	struct virtio_scsi_cmd_req *req = buf;

	req->lun[0] = TARGET_ID;
	req->lun[1] = LUN_ID;

	if (dev->num_blocks > (1ULL << 32)) {
		req->cdb[0] = is_write ? SPDK_SBC_WRITE_16 : SPDK_SBC_READ_16;
		to_be64(&req->cdb[2], offset_blocks);
		to_be32(&req->cdb[10], num_blocks);
	} else {
		req->cdb[0] = is_write ? SPDK_SBC_WRITE_10 : SPDK_SBC_READ_10;
		to_be32(&req->cdb[2], offset_blocks);
		to_be16(&req->cdb[7], num_blocks);
	}
}

/*
 * Read buffer schema:
 *
 *  -----------------------
 *  virtio_scsi_cmd_req (R)
 *  -----------------------
 *  virtio_scsi_cmd_resp(W)
 *  -----------------------
 *  data ...            (W)
 *  -----------------------
 */
static int
blk_read_virtio_dev(struct virtio_dev *dev, void *buf, size_t blk_offset,
    size_t nblk)
{
	virtio_task_t *task;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	int rc;

	task = virtio_task_create(3);
	if (task == NULL)
		return (-1);

	req = virtio_task_alloc_buf(task, sizeof (*req), false, true);
	resp = virtio_task_alloc_buf(task, sizeof (*resp), true, true);
	virtio_task_add_buf(task, buf, nblk * dev->block_size, true);
	if (req == NULL || resp == NULL) {
		virtio_task_destroy(task);
		return (-1);
	}

	create_rw_cdb(dev, req, blk_offset, nblk, 0);

	rc = vring_submit_task_sync(dev->poller, task);
	if (rc != 0) {
		virtio_task_destroy(task);
		return (-1);
	}

	if (resp->response != VIRTIO_SCSI_S_OK ||
	    resp->status != SPDK_SCSI_STATUS_GOOD) {
		virtio_task_destroy(task);
		return (-1);
	}

	rc = task->used_bytes - sizeof (*resp);
	assert(rc >= 0);
	virtio_task_destroy(task);

	return (rc);
}

/*
 * Write buffer schema:
 *
 *  -----------------------
 *  virtio_scsi_cmd_req (R)
 *  -----------------------
 *  data ...            (R)
 *  -----------------------
 *  virtio_scsi_cmd_resp(W)
 *  -----------------------
 */
static int
blk_write_virtio_dev(struct virtio_dev *dev, void *buf, size_t blk_offset,
    size_t nblk)
{
	virtio_task_t *task;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	int nbytes = nblk * dev->block_size;

	task = virtio_task_create(3);
	if (task == NULL)
		return (-1);

	req = virtio_task_alloc_buf(task, sizeof (*req), false, true);
	virtio_task_add_buf(task, buf, nbytes, false);
	resp = virtio_task_alloc_buf(task, sizeof (*resp), true, true);
	if (req == NULL || resp == NULL) {
		virtio_task_destroy(task);
		return (-1);
	}

	create_rw_cdb(dev, req, blk_offset, nblk, 1);

	if (vring_submit_task_sync(dev->poller, task) != 0) {
		virtio_task_destroy(task);
		return (-1);
	}

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
		virtio_task_destroy(task);
		return (-1);
	}
	virtio_task_destroy(task);

	return (nbytes);
}

/*
 * Read range of bytes from a block.
 */
static int
read_partial_block(struct virtio_dev *dev, size_t blk_num,
    size_t off_start, void *data, size_t nbytes)
{
	char *blk_buf = alloc_shared_buf(dev->block_size);

	if (off_start + nbytes > dev->block_size)
		nbytes = dev->block_size - off_start;

	if (blk_read_virtio_dev(dev, blk_buf, blk_num, 1) != dev->block_size) {
		free_shared_buf(blk_buf);
		return (-1);
	}

	memcpy(data, blk_buf + off_start, nbytes);
	free_shared_buf(blk_buf);
	return (nbytes);
}

/*
 * Convenient but also slow function for reading bytes from device
 * synchronously without data copy. Provided buffer must be
 * allocated from huge pages.
 */
int
read_virtio_dev(struct virtio_dev *dev, void *buf, size_t offset,
    size_t nbytes)
{
	size_t nblk;
	size_t blk_offset;
	size_t read_bytes = 0;
	int resid_offset;
	int ret;

	if (offset > dev->cap)
		return (-1);
	if (nbytes == 0)
		return (0);

	if (offset + nbytes > dev->cap)
		nbytes = dev->cap - offset;

	blk_offset = offset / dev->block_size;
	resid_offset = offset % dev->block_size;

	if (resid_offset != 0) {
		ret = read_partial_block(dev, blk_offset, resid_offset, buf, nbytes);
		if (ret < 0)
			return (-1);
		if (ret < dev->block_size - resid_offset)
			return (ret);

		read_bytes += ret;
		blk_offset++;
	}

	nblk = (nbytes - read_bytes) / dev->block_size;
	if (nblk > 0) {
		ret = blk_read_virtio_dev(dev, (char *)buf + read_bytes, blk_offset, nblk);
		if (ret < 0)
			return (read_bytes);
		if (ret < nblk * dev->block_size)
			return (read_bytes + ret);
		read_bytes += ret;
		blk_offset += nblk;
	}

	resid_offset = (nbytes - read_bytes) % dev->block_size;
	if (resid_offset != 0) {
		ret = read_partial_block(dev, blk_offset, 0, (char *)buf + read_bytes, resid_offset);
		if (ret < 0)
			return (-1);
		read_bytes += ret;
	}

	return (read_bytes);
}

/*
 * Write a block which is not fully overwritten, hence it needs to be
 * read from storage, modified and written back.
 */
static int
write_partial_block(struct virtio_dev *dev, size_t blk_num,
    size_t off_start, void *data, size_t nbytes)
{
	char *blk_buf = alloc_shared_buf(dev->block_size);
	int ret;

	if (off_start + nbytes > dev->block_size)
		nbytes = dev->block_size - off_start;

	if (blk_read_virtio_dev(dev, blk_buf, blk_num, 1) != dev->block_size) {
		free_shared_buf(blk_buf);
		return (-1);
	}

	memcpy(blk_buf + off_start, data, nbytes);

	ret = blk_write_virtio_dev(dev, blk_buf, blk_num, 1);
	free_shared_buf(blk_buf);
	return (ret - dev->block_size + nbytes);
}

/*
 * Convenient but also slow function for writing bytes to device
 * synchronously without data copy. Provided buffer must be
 * allocated from huge pages.
 */
int
write_virtio_dev(struct virtio_dev *dev, void *buf, size_t offset,
    size_t nbytes)
{
	size_t nblk;
	size_t blk_offset;
	size_t written_bytes = 0;
	int resid_offset;
	int ret;

	if (offset > dev->cap)
		return (-1);
	if (nbytes == 0)
		return (0);

	if (offset + nbytes > dev->cap)
		nbytes = dev->cap - offset;

	blk_offset = offset / dev->block_size;
	resid_offset = offset % dev->block_size;

	if (resid_offset != 0) {
		ret = write_partial_block(dev, blk_offset, resid_offset, buf, nbytes);
		if (ret < 0)
			return (-1);
		if (ret < dev->block_size - resid_offset)
			return (ret);
		written_bytes += ret;
		blk_offset++;
	}

	nblk = (nbytes - written_bytes) / dev->block_size;
	if (nblk > 0) {
		ret = blk_write_virtio_dev(dev, (char *)buf + written_bytes, blk_offset, nblk);
		if (ret < 0)
			return (written_bytes);
		if (ret < nblk * dev->block_size)
			return (written_bytes + ret);
		written_bytes += ret;
		blk_offset += nblk;
	}

	resid_offset = (nbytes - written_bytes) % dev->block_size;
	if (resid_offset != 0) {
		ret = write_partial_block(dev, blk_offset, 0, (char *)buf + written_bytes, resid_offset);
		if (ret < 0)
			return (-1);
		written_bytes += ret;
	}

	return (written_bytes);
}

/***********************************************
 * Async read/write.
 ***********************************************/

static void
push_to_outring(struct virtio_dev *dev, async_io_desc_t *io_desc)
{
	int rc;
#if (SLEEPY_POLL == 0)
	uint64_t kick_it = 1;
	int was_empty;

	was_empty = rte_ring_empty(dev->outring);
#endif
	rc = rte_ring_sp_enqueue(dev->outring, io_desc);
	// XXX implement throttling
	assert(rc == 0);

#if (SLEEPY_POLL == 0)
	// kick only if someone is possibly stuck at empty ring
	if (was_empty) {
		rc = write(dev->outfd, &kick_it, sizeof(kick_it));
		assert(rc == sizeof(kick_it));
		fsync(dev->outfd);
	}
#endif
}

/*
 * Async read blocks callback.
 */
static void
blk_read_done(struct virtio_task *task, void *arg)
{
	async_io_desc_t *io_desc = arg;
	struct virtio_dev *dev = io_desc->reserved;
	struct virtio_scsi_cmd_resp *resp = virtio_task_get_buf(task, 1);
	size_t nbytes = 0;
	size_t used_bytes = task->used_bytes - sizeof (*resp);

	io_desc->retval = -1;

	if (resp->response == VIRTIO_SCSI_S_OK &&
	    resp->status == SPDK_SCSI_STATUS_GOOD) {

		for (int i = 2; i < task->count; i++) {
			virtio_buffer_t *vbuf = &task->vbufs[i];
			size_t delta = vbuf->offset_to - vbuf->offset_from;

			if (nbytes + delta > used_bytes)
				delta = used_bytes - nbytes;

			if (vbuf->copy) {
				memcpy((char *)io_desc->buf + nbytes,
				    vbuf->buf + vbuf->offset_from, delta);
			}
			nbytes += delta;

			if (nbytes >= used_bytes)
				break;
		}
		assert(nbytes <= used_bytes);
		io_desc->retval = nbytes;
	}

	push_to_outring(dev, io_desc);
	virtio_task_destroy(task);
}

/*
 * Async zero-copy read operation. Provided buffer must be allocated from
 * huge pages.
 *
 * Read buffer schema:
 *
 *  -----------------------
 *  virtio_scsi_cmd_req (R)
 *  -----------------------
 *  virtio_scsi_cmd_resp(W)
 *  -----------------------
 *  data ...            (W)
 *  -----------------------
 */
static int
read_virtio_dev_async(struct virtio_dev *dev, async_io_desc_t *io_desc)
{
	virtio_task_t *task;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	char *buf = io_desc->buf;
	size_t offset = io_desc->offset;
	size_t nbytes = io_desc->nbytes;
	size_t nblk = 0;
	size_t temp;
	size_t blk_offset;
	size_t read_bytes = 0;
	int resid_offset;
	int rc;

	if (offset > dev->cap)
		return (-1);
	if (nbytes == 0)
		return (0);

	if (offset + nbytes > dev->cap)
		nbytes = dev->cap - offset;

	io_desc->reserved = dev;
	task = virtio_task_create(5);
	if (task == NULL)
		return (-1);

	req = virtio_task_alloc_buf(task, sizeof (*req), false, true);
	resp = virtio_task_alloc_buf(task, sizeof (*resp), true, true);
	if (req == NULL || resp == NULL) {
		virtio_task_destroy(task);
		return (-1);
	}

	blk_offset = offset / dev->block_size;
	resid_offset = offset % dev->block_size;

	// partial read of the first block
	if (resid_offset != 0) {
		nblk++;
		if (resid_offset + nbytes <= dev->block_size)
			read_bytes = nbytes;
		else
			read_bytes = (dev->block_size - resid_offset);

		if (virtio_task_alloc_buf_part(task, dev->block_size, true, false,
		    resid_offset, resid_offset + read_bytes) == NULL) {
			virtio_task_destroy(task);
		    return (-1);
		}
	}

	// middle part which is block size aligned
	if ((nbytes - read_bytes) / dev->block_size > 0) {
		temp = (nbytes - read_bytes) / dev->block_size;
		virtio_task_add_buf(task, buf + read_bytes, temp * dev->block_size, true);
		nblk += temp;
		read_bytes += temp * dev->block_size;
	}

	// partial read of the last block
	resid_offset = (nbytes - read_bytes) % dev->block_size;
	if (read_bytes < nbytes && resid_offset != 0) {
		nblk++;
		if (virtio_task_alloc_buf_part(task, dev->block_size, true, false,
		    0, resid_offset) == NULL) {
			virtio_task_destroy(task);
		    return (-1);
		}
	}

	create_rw_cdb(dev, req, blk_offset, nblk, false);

	rc = vring_submit_task(dev->poller, task, blk_read_done, io_desc);
	if (rc != 0) {
		virtio_task_destroy(task);
		return (-1);
	}
	return (0);
}

/*
 * Callback called after final partial block has been written.
 */
static void
blk_write_last_block_done(struct virtio_task *task, void *arg)
{
	async_io_desc_t *io_desc = arg;
	struct virtio_dev *dev = io_desc->reserved;
	struct virtio_scsi_cmd_resp *resp = virtio_task_get_buf(task, 2);
	virtio_buffer_t *vbuf;

	assert(task->count == 3);

	if (resp->response == VIRTIO_SCSI_S_OK &&
	    resp->status == SPDK_SCSI_STATUS_GOOD) {

		vbuf = &task->vbufs[1];
		if (io_desc->retval < 0)
			io_desc->retval = vbuf->offset_to - vbuf->offset_from;
		else
			io_desc->retval += vbuf->offset_to - vbuf->offset_from;
	}

	push_to_outring(dev, io_desc);
	virtio_task_destroy(task);
}

/*
 * Callback for reading last block which should be partially overwritten.
 */
static void
blk_read_last_block_done(struct virtio_task *task, void *arg)
{
	struct virtio_task *new_task = NULL;
	async_io_desc_t *io_desc = arg;
	struct virtio_dev *dev = io_desc->reserved;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp = virtio_task_get_buf(task, 1);
	virtio_buffer_t *vbuf;
	size_t delta;
	size_t blk_offset;
	char *buf;
	int rc;

	assert(task->count == 3);

	if (resp->response != VIRTIO_SCSI_S_OK ||
	    resp->status != SPDK_SCSI_STATUS_GOOD) {
		goto write_done;
	}

	new_task = virtio_task_create(4);
	if (new_task == NULL)
		goto write_done;

	req = virtio_task_alloc_buf(new_task, sizeof (*req), false, true);
	if (req == NULL)
		goto write_done;

	// copy vbuf from original read task to new write task
	// inefficient but we don't care that much about unaligned writes
	vbuf = &task->vbufs[2];
	buf = virtio_task_alloc_buf_part(new_task, dev->block_size, false, false,
	    vbuf->offset_from, vbuf->offset_to);
	if (buf == NULL)
		goto write_done;
	delta = vbuf->offset_to - vbuf->offset_from;
	memcpy(buf, vbuf->buf, vbuf->size);
	memcpy(buf, (char *)io_desc->buf + io_desc->nbytes - delta, delta);

	blk_offset = (io_desc->offset + io_desc->nbytes) / dev->block_size;
	create_rw_cdb(dev, req, blk_offset, 1, true);

	resp = virtio_task_alloc_buf(new_task, sizeof (*resp), true, true);
	if (resp == NULL)
		goto write_done;

	rc = vring_submit_task(dev->poller, new_task, blk_write_last_block_done,
	    io_desc);
	if (rc != 0)
		goto write_done;
	virtio_task_destroy(task);
	return;

write_done:
	push_to_outring(dev, io_desc);
	virtio_task_destroy(task);
	if (new_task != NULL)
		virtio_task_destroy(new_task);
}

/*
 * Callback called when optional first partial block and all following full
 * blocks have been written.
 */
static void
blk_write_full_blocks_done(struct virtio_task *task, void *arg)
{
	async_io_desc_t *io_desc = arg;
	struct virtio_dev *dev = io_desc->reserved;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp = virtio_task_get_buf(task, task->count - 1);
	virtio_buffer_t *vbuf;
	size_t nbytes = 0;
	size_t offset;
	size_t blk_offset;
	int resid_offset;
	int rc;

	assert(task->count > 2);

	if (resp->response != VIRTIO_SCSI_S_OK ||
	    resp->status != SPDK_SCSI_STATUS_GOOD) {
		goto write_done;
	}

	// we have written the blocks, see how much it was
	for (int i = 1; i < task->count - 1; i++) {
		vbuf = &task->vbufs[i];
		nbytes += vbuf->offset_to - vbuf->offset_from;
	}
	io_desc->retval = nbytes;

	// if the write was block aligned at the end then we are done
	if (nbytes == io_desc->nbytes)
		goto write_done;

	virtio_task_destroy(task);

	// create task for reading remaining bytes in the last block
	task = virtio_task_create(3);
	if (task == NULL)
		goto write_done;

	req = virtio_task_alloc_buf(task, sizeof (*req), false, true);
	resp = virtio_task_alloc_buf(task, sizeof (*resp), true, true);
	if (req == NULL || resp == NULL)
		goto write_done;

	offset = io_desc->offset + io_desc->nbytes;
	blk_offset = offset / dev->block_size;
	resid_offset = offset % dev->block_size;
	assert(resid_offset != 0);

	create_rw_cdb(dev, req, blk_offset, 1, false);

	if (virtio_task_alloc_buf_part(task, dev->block_size, true, false,
	    0, resid_offset) == NULL) {
		goto write_done;
	}

	rc = vring_submit_task(dev->poller, task, blk_read_last_block_done, io_desc);
	if (rc != 0)
		goto write_done;
	return;

write_done:
	push_to_outring(dev, io_desc);
	if (task != NULL)
		virtio_task_destroy(task);
}

/*
 * First async step when writing a buffer which is not block-aligned.
 */
static void
blk_read_first_block_done(struct virtio_task *task, void *arg)
{
	struct virtio_task *new_task = NULL;
	async_io_desc_t *io_desc = arg;
	struct virtio_dev *dev = io_desc->reserved;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp = virtio_task_get_buf(task, 1);
	virtio_buffer_t *vbuf;
	size_t delta;
	size_t blk_offset;
	size_t nblk = 0;
	char *buf;
	int rc;

	assert(task->count == 3);

	if (resp->response != VIRTIO_SCSI_S_OK ||
	    resp->status != SPDK_SCSI_STATUS_GOOD) {
		goto write_done;
	}

	new_task = virtio_task_create(4);
	if (new_task == NULL)
		goto write_done;

	req = virtio_task_alloc_buf(new_task, sizeof (*req), false, true);
	if (req == NULL)
		goto write_done;

	// copy vbuf from original read task to new write task
	// inefficient but we don't care that much about unaligned writes
	vbuf = &task->vbufs[2];
	buf = virtio_task_alloc_buf_part(new_task, dev->block_size, false, false,
	    vbuf->offset_from, vbuf->offset_to);
	if (buf == NULL)
		goto write_done;

	delta = vbuf->offset_to - vbuf->offset_from;
	memcpy(buf, vbuf->buf, vbuf->size);
	memcpy(buf + vbuf->offset_from, io_desc->buf, delta);

	// if the read was more than just one partial block then add more
	if (delta < io_desc->nbytes) {
		nblk = (io_desc->nbytes - delta) / dev->block_size;
		virtio_task_add_buf(new_task, (char *)io_desc->buf + delta,
		    nblk * dev->block_size, false);
	}
	nblk++;  // for the first partial block
	blk_offset = io_desc->offset / dev->block_size;
	create_rw_cdb(dev, req, blk_offset, nblk, true);

	resp = virtio_task_alloc_buf(new_task, sizeof (*resp), true, true);
	if (resp == NULL)
		goto write_done;

	rc = vring_submit_task(dev->poller, new_task, blk_write_full_blocks_done,
	    io_desc);
	if (rc != 0)
		goto write_done;
	virtio_task_destroy(task);
	return;

write_done:
	push_to_outring(dev, io_desc);
	virtio_task_destroy(task);
	if (new_task != NULL)
		virtio_task_destroy(new_task);
}

/*
 * Async zero-copy write operation. Provided buffer must be allocated from
 * huge pages.
 *
 * Write buffer schema:
 *
 *  -----------------------
 *  virtio_scsi_cmd_req (R)
 *  -----------------------
 *  data ...            (R)
 *  -----------------------
 *  virtio_scsi_cmd_resp(W)
 *  -----------------------
 */
static int
write_virtio_dev_async(struct virtio_dev *dev, async_io_desc_t *io_desc)
{
	virtio_task_t *task;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	char *buf = io_desc->buf;
	size_t offset = io_desc->offset;
	size_t nbytes = io_desc->nbytes;
	size_t nblk;
	size_t blk_offset;
	size_t read_bytes = 0;
	int resid_offset;
	int rc;

	if (offset > dev->cap)
		return (-1);
	// XXX should be return asynchronously
	if (nbytes == 0)
		return (0);

	if (offset + nbytes > dev->cap)
		nbytes = dev->cap - offset;

	io_desc->reserved = dev;
	io_desc->retval = -1;

	task = virtio_task_create(4);
	if (task == NULL)
		return (-1);

	req = virtio_task_alloc_buf(task, sizeof (*req), false, true);
	if (req == NULL) {
		virtio_task_destroy(task);
		return (-1);
	}

	blk_offset = offset / dev->block_size;
	resid_offset = offset % dev->block_size;

	// partial write of the first block (we have to read it first)
	if (resid_offset != 0) {
		if (resid_offset + nbytes <= dev->block_size)
			read_bytes = nbytes;
		else
			read_bytes = (dev->block_size - resid_offset);

		create_rw_cdb(dev, req, blk_offset, 1, false);
		resp = virtio_task_alloc_buf(task, sizeof (*resp), true, true);
		if (resp == NULL) {
			virtio_task_destroy(task);
			return (-1);
		}

		if (virtio_task_alloc_buf_part(task, dev->block_size, true, false,
		    resid_offset, resid_offset + read_bytes) == NULL) {
			virtio_task_destroy(task);
		    return (-1);
		}

		rc = vring_submit_task(dev->poller, task, blk_read_first_block_done, io_desc);
		if (rc != 0) {
			virtio_task_destroy(task);
			return (-1);
		}
		return (0);
	}

	// write all blocks which can be written as a whole (without reading)
	nblk = (nbytes - read_bytes) / dev->block_size;
	if (nblk > 0) {
		create_rw_cdb(dev, req, blk_offset, nblk, true);
		virtio_task_add_buf(task, buf, nblk * dev->block_size, false);
		resp = virtio_task_alloc_buf(task, sizeof (*resp), true, true);
		if (resp == NULL) {
			virtio_task_destroy(task);
			return (-1);
		}

		rc = vring_submit_task(dev->poller, task, blk_write_full_blocks_done, io_desc);
		if (rc != 0) {
			virtio_task_destroy(task);
			return (-1);
		}
		return (0);
	}

	// partial write of the last block (we have to read it first)
	assert(nbytes < dev->block_size);
	resid_offset = nbytes % dev->block_size;

	create_rw_cdb(dev, req, blk_offset, 1, false);
	resp = virtio_task_alloc_buf(task, sizeof (*resp), true, true);
	if (resp == NULL) {
		virtio_task_destroy(task);
		return (-1);
	}
	if (virtio_task_alloc_buf_part(task, dev->block_size, true, false,
	    0, resid_offset) == NULL) {
		virtio_task_destroy(task);
		return (-1);
	}

	rc = vring_submit_task(dev->poller, task, blk_read_last_block_done, io_desc);
	if (rc != 0) {
		virtio_task_destroy(task);
		return (-1);
	}
	return (0);
}

/*
 * Main entry point for async read and write operations.
 */
int
virtio_submit(struct virtio_dev *dev, async_io_desc_t *io_desc)
{
	if (io_desc->type == VIRTIO_TYPE_READ) {
		return (read_virtio_dev_async(dev, io_desc));
	} else if (io_desc->type == VIRTIO_TYPE_WRITE) {
		return (write_virtio_dev_async(dev, io_desc));
	} else {
		return (-1);
	}
}

/*
 * Return number of asynchronous tasks waiting for pick up from the
 * output ring.
 * Negative timeout means wait for mincount condition forever.
 */
int
virtio_completed(struct virtio_dev *dev, int mincount, int timeout)
{
#if (SLEEPY_POLL == 0)
	struct pollfd fds;
	uint64_t poll_data;
	int rc;
	int delay = 100;
#else
	int delay = SLEEPY_POLL;
#endif
	int total_time = 0;
	int n = rte_ring_count(dev->outring);

	if (n >= mincount)
		return (n);

	// enter poll loop until timeout or n == mincount
	do {
		if (timeout >= 0 && total_time >= timeout)
			return n;
#if (SLEEPY_POLL == 0)
		fds.fd = dev->outfd;
		fds.events = POLLIN;
		fds.revents = 0;

		rc = poll(&fds, 1, delay);
		if (rc < 0) {
			perror("poll");
		} else if (rc > 0 && (fds.revents & POLLIN) != 0) {
			rc = read(fds.fd, &poll_data, sizeof (poll_data));
			assert(rc == sizeof (poll_data));
		}
#else
		usleep(delay);
#endif
		total_time += delay;
	} while ((n = rte_ring_count(dev->outring)) < mincount);

	return (n);
}

/*
 * Return completed task from output ring buffer or NULL if there is none
 * and timeout (in ms) has ellapsed.
 */
async_io_desc_t *
virtio_get_completed(struct virtio_dev *dev, int timeout)
{
	void *desc;
	int rc;

	rc = virtio_completed(dev, 1, timeout);
	if (rc > 0) {
		rc = rte_ring_mc_dequeue(dev->outring, &desc);
		return (rc == 0) ? desc : NULL;
	} else {
		return (NULL);
	}
}
