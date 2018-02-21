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
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "virtio_dev.h"
#include "shm.h"

// Amount of memory allocated from huge pages
#define MEM_MB  100
#define PATTERN 0xdeadbeef
#define LENGTH  4000

static struct sigaction sigact;
static int app_running = 0;

static void signal_handler(int);
static void init_signals(void);
static void cleanup(void);

/*
 * Wait for completion of asynchronous IO.
 */
ssize_t wait_for_io(dev_handle_t dev, uintptr_t io_num)
{
    struct timespec ts1, ts2;
    async_io_desc_t *io_desc;
    int64_t us;
    int n;

    clock_gettime(CLOCK_REALTIME, &ts1);

    n = virtio_completed(dev, 1, 1000);
    if (n == 0) {
        fprintf(stderr, "Timed out waiting for IO\n");
        return (-1);
    }
    io_desc = virtio_get_completed(dev);
    assert(io_desc != NULL);

    if (io_desc->user_ctx != (void *)io_num) {
        fprintf(stderr, "Unexpected IO completed\n");
        return (-1);
    }

    clock_gettime(CLOCK_REALTIME, &ts2);
    us = ts2.tv_sec * 1000000 + ts2.tv_nsec / 1000;
    us -= (ts1.tv_sec * 1000000 + ts1.tv_nsec / 1000);
    fprintf(stderr, "IO finished in %lu us\n", us);

    return (io_desc->retval);
}

/*
 * Write bytes and wait for async write completion
 */
int write_async(dev_handle_t dev, void *data, size_t offset, size_t bufsiz,
    uintptr_t id)
{
	async_io_desc_t io_desc;
	int rc;

	io_desc.type = VIRTIO_TYPE_WRITE;
	io_desc.buf = data;
	io_desc.offset = offset;
	io_desc.nbytes = bufsiz;
	io_desc.user_ctx = (void *)id;

	rc = virtio_submit(dev, &io_desc);
        if (rc != 0) {
            fprintf(stderr, "IO submit failed\n");
            virtio_buf_free(data);
            return (-1);
        }
        return wait_for_io(dev, id);

}

/*
 * Read bytes and wait for async read completion
 */
int read_async(dev_handle_t dev, void *data, size_t offset, size_t bufsiz,
    uintptr_t id)
{
	async_io_desc_t io_desc;
	int rc;

        io_desc.type = VIRTIO_TYPE_READ;
        io_desc.buf = data;
        io_desc.offset = offset;
        io_desc.nbytes = bufsiz;
        io_desc.user_ctx = (void *)id;

        rc = virtio_submit(dev, &io_desc);
        if (rc != 0) {
            fprintf(stderr, "IO submit failed\n");
            virtio_buf_free(data);
            return (-1);
        }
        return wait_for_io(dev, id);
}

/*
 * Write byte pattern and read it back in a loop increasing offset.
 */
int test_io(dev_handle_t dev, int iters, size_t offset, int bufsiz, bool async)
{
    size_t capacity;
    uintptr_t iter = 0;
    int write_len, read_len;
    unsigned char *data;

    capacity = virtio_dev_block_size(dev) * virtio_dev_blocks_num(dev);

    data = virtio_buf_alloc(dev, bufsiz);
    if (data == NULL)
        return (-1);

    for (int i = 0; i < bufsiz; i++)
        data[i] = i % 0xff;

    while (app_running && iter++ < iters) {
        fprintf(stderr, "--------------- iter %lu ----------------------\n", iter);

        fprintf(stderr, "write offset %lu, nbytes: %u\n", offset, bufsiz);
	if (async)
		write_len = write_async(dev, data, offset, bufsiz, iter);
	else
		write_len = write_virtio_dev(dev, data, offset, bufsiz);

        if (write_len < 0) {
            fprintf(stderr, "Write dev failed\n");
            virtio_buf_free(data);
            return (-1);
        } else if (write_len != bufsiz) {
            if (write_len != capacity - offset) {
                fprintf(stderr, "Short write (%d instead of %d)\n",
                    write_len, bufsiz);
                virtio_buf_free(data);
                return (-1);
            }
        }

        memset(data, 0, bufsiz);

        fprintf(stderr, "read  offset %lu, nbytes: %d\n", offset, write_len);
	if (async)
		read_len = read_async(dev, data, offset, write_len, iter);
	else
		read_len = read_virtio_dev(dev, data, offset, write_len);

        if (read_len < 0) {
            fprintf(stderr, "Read dev failed\n");
            virtio_buf_free(data);
            return (-1);
        } else if (read_len != write_len) {
            fprintf(stderr, "Short read (%d instead of %d)\n", read_len, write_len);
            virtio_buf_free(data);
            return (-1);
        }

	// verify that what has been written is what we get in read
        for (int i = 0; i < read_len; i++) {
            if (data[i] != i % 0xff) {
                fprintf(stderr, "Data corruption at index %d (%u instead of %u)\n",
                    i, data[i], i);
                virtio_buf_free(data);
                return (-1);
            }
        }
        offset += bufsiz;
        offset %= capacity;
    }

    virtio_buf_free(data);
    return (0);
}

static void signal_handler(int sig){
    switch(sig)
    {
    case SIGINT:
    case SIGKILL:
    case SIGTERM:
        app_running = 0;
        break;
    default:
        break;
    }
}

static void init_signals(void){
    sigact.sa_handler = signal_handler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, (struct sigaction *)NULL);
}

static void cleanup(void){
    sigemptyset(&sigact.sa_mask);
    app_running = 0;
}

int main(int argc, char* argv[])
{
    int opt = 0;
    char *sock_path = NULL;
    dev_handle_t dev;
    int iters = 3;
    int async = 0;
    int rc;
    int offset = 0;
    int buffer_size = 4 * 1024;

    atexit(cleanup);
    init_signals();

    if (libvirtio_dev_init(MEM_MB) != 0) {
    	fprintf(stderr, "Cannot init libvirtio_dev\n");
    	exit(-1);
    }

    while ((opt = getopt(argc, argv, "ai:o:S:s:")) != -1) {

        switch (opt) {
        case 'a':
            async = 1;
            break;
        case 'S':
            buffer_size = atoi(optarg);
            break;
        case 'i':
            iters = atoi(optarg);
            break;
        case 'o':
            offset = atoi(optarg);
            break;
        case 's':
            sock_path = optarg;
            break;
        default:
            break;
        }
    }

    if (!sock_path) {
        fprintf(stderr, "Usage: %s [-aq path]\n", argv[0]);
        fprintf(stderr, "\t-a : use async IO instead of default sync\n");
        fprintf(stderr, "\t-S : set read and write size\n");
        fprintf(stderr, "\t-i : number of write-read iterations\n");
        fprintf(stderr, "\t-s : socket path to connect to\n");
        exit(-1);
    }

    dev = open_virtio_dev(sock_path);
    if (dev == NULL) {
        fprintf(stderr, "Failed to open virtio dev\n");
    	exit(-1);
    }
    app_running = 1;

    rc = test_io(dev, iters, offset, buffer_size, async);

    close_virtio_dev(dev);
    return (rc);
}
