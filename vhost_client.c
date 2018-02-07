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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/unistd.h>

#include "shm.h"
#include "vring.h"
#include "vhost_user.h"
#include "vhost_client.h"

#define INSTANCE_CREATED        1
#define INSTANCE_INITIALIZED    2
#define INSTANCE_END            3

// the only features that we support
#define	VHOST_FEATURES ((1ULL << VHOST_USER_F_PROTOCOL_FEATURES) | \
    (1ULL << VIRTIO_F_VERSION_1) | \
    (1ULL << VIRTIO_F_NOTIFY_ON_EMPTY))


/*
 * Allocates client structure.
 */
vhost_client_t *
new_vhost_client(const char *path)
{
	vhost_client_t *client;
	assert(path != NULL);

	client = calloc(1, sizeof (vhost_client_t));
	if (client == NULL)
		return (NULL);

	strncpy(client->sock_path, path, PATH_MAX);

	client->status = INSTANCE_CREATED;

	return (client);
}

/*
 * Dummy negotiation as we don't really support any advanced features.
 */
static int
negotiate_features(vhost_client_t *client)
{
	uint64_t features;

    if (vhost_ioctl(client->sock, VHOST_USER_GET_FEATURES, &features) != 0) {
    	fprintf(stderr, "Unable to get vhost features\n");
    	return (-1);
    }
    client->features &= features;
    if (vhost_ioctl(client->sock, VHOST_USER_SET_FEATURES, &client->features) != 0) {
    	fprintf(stderr, "Unable to set vhost features\n");
    	return (-1);
    }

    return (0);
}

/*
 * Connects to vhost server and sets basic parameters.
 */
int
init_vhost_client(vhost_client_t *client)
{
	struct sockaddr_un un;
	size_t len;
	VhostUserMemory memory;

	if (client->status != INSTANCE_CREATED)
		return (-1);

	if ((client->sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		return (-1);
	}

	un.sun_family = AF_UNIX;
	strcpy(un.sun_path, client->sock_path);
	len = sizeof(un.sun_family) + strlen(client->sock_path);

	if (connect(client->sock, (struct sockaddr *) &un, len) == -1) {
		(void) close(client->sock);
		perror("connect");
		return (-1);
	}

	if (vhost_ioctl(client->sock, VHOST_USER_SET_OWNER, 0) != 0) {
		(void) close(client->sock);
		return (-1);
	}
	if (negotiate_features(client) != 0) {
		(void) close(client->sock);
		return (-1);
	}
	/* get mem regions info for passing it to the server */
	get_memory_info(&memory);
	if (vhost_ioctl(client->sock, VHOST_USER_SET_MEM_TABLE, &memory) != 0) {
		(void) close(client->sock);
		return (-1);
	}

	client->status = INSTANCE_INITIALIZED;

	return (0);
}

/*
 * Establishes shared ring buffer (vring) for doing IOs between client and
 * server.
 */
int
set_vhost_vring(vhost_client_t *client, vring_t *vring, unsigned int idx)
{
	struct vhost_vring *vring_shm = vring->vring_shm;
	struct vhost_vring_state num = { .index = idx, .num = vring->num };
	struct vhost_vring_state base = { .index = idx, .num = 0 };
	struct vhost_vring_file kick = { .index = idx, .fd = vring->kickfd };
	struct vhost_vring_file call = { .index = idx, .fd = vring->callfd };
	struct vhost_vring_addr addr = {
	    .index = idx,
	    .desc_user_addr = (uintptr_t) &vring_shm->desc,
	    .avail_user_addr = (uintptr_t) &vring_shm->avail,
	    .used_user_addr = (uintptr_t) &vring_shm->used,
	    .log_guest_addr = (uintptr_t) NULL,
	    .flags = 0
	};

	if (vhost_ioctl(client->sock, VHOST_USER_SET_VRING_NUM, &num) != 0 ||
	    vhost_ioctl(client->sock, VHOST_USER_SET_VRING_BASE, &base) != 0 ||
	    vhost_ioctl(client->sock, VHOST_USER_SET_VRING_KICK, &kick) != 0 ||
	    vhost_ioctl(client->sock, VHOST_USER_SET_VRING_CALL, &call) != 0 ||
	    vhost_ioctl(client->sock, VHOST_USER_SET_VRING_ADDR, &addr) != 0) {
		return (-1);
	}

	return (0);
}

/*
 * Terminate vhost connection.
 */
void
end_vhost_client(vhost_client_t *client)
{
	if (client->status != INSTANCE_INITIALIZED)
		return;

	vhost_ioctl(client->sock, VHOST_USER_RESET_OWNER, 0);
	close(client->sock);
	client->status = INSTANCE_END;
	free(client);
}
