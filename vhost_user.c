/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
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

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "vhost_user.h"
#include "shm.h"

/*
 * Vhost user protocol routines.
 */

int
vhost_user_send_fds(int fd, const VhostUserMsg *msg, int *fds, size_t fd_num)
{
	int ret;
	struct msghdr msgh;
	struct iovec iov[1];
	size_t fd_size = fd_num * sizeof(int);
	char control[CMSG_SPACE(fd_size)];
	struct cmsghdr *cmsg;

	memset(&msgh, 0, sizeof(msgh));
	memset(control, 0, sizeof(control));

	/* set the payload */
	iov[0].iov_base = (void *) msg;
	iov[0].iov_len = VHOST_USER_HDR_SIZE + msg->size;

	msgh.msg_iov = iov;
	msgh.msg_iovlen = 1;

	if (fd_num) {
		msgh.msg_control = control;
		msgh.msg_controllen = sizeof(control);

		cmsg = CMSG_FIRSTHDR(&msgh);

		cmsg->cmsg_len = CMSG_LEN(fd_size);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(cmsg), fds, fd_size);
	} else {
		msgh.msg_control = 0;
		msgh.msg_controllen = 0;
	}

	do {
		ret = sendmsg(fd, &msgh, 0);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		fprintf(stderr, "Failed to send msg, reason: %s\n",
		    strerror(errno));
	}

	return (ret);
}

int
vhost_user_recv_fds(int fd, const VhostUserMsg *msg, int *fds, size_t *fd_num)
{
	int ret, rc;

	struct msghdr msgh;
	struct iovec iov[1];

	size_t fd_size = (*fd_num) * sizeof(int);
	char control[CMSG_SPACE(fd_size)];
	struct cmsghdr *cmsg;

	memset(&msgh, 0, sizeof(msgh));
	memset(control, 0, sizeof(control));
	*fd_num = 0;

	/* set the payload */
	iov[0].iov_base = (void *) msg;
	iov[0].iov_len = VHOST_USER_HDR_SIZE;

	msgh.msg_iov = iov;
	msgh.msg_iovlen = 1;
	msgh.msg_control = control;
	msgh.msg_controllen = sizeof(control);

	ret = recvmsg(fd, &msgh, 0);
	if (ret > 0) {
		if (msgh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
			ret = -1;
		} else {
			cmsg = CMSG_FIRSTHDR(&msgh);
			if (cmsg && cmsg->cmsg_len > 0 &&
			    cmsg->cmsg_level == SOL_SOCKET &&
			    cmsg->cmsg_type == SCM_RIGHTS) {
				if (fd_size >= cmsg->cmsg_len - CMSG_LEN(0)) {
					fd_size = cmsg->cmsg_len - CMSG_LEN(0);
					memcpy(fds, CMSG_DATA(cmsg), fd_size);
					*fd_num = fd_size / sizeof(int);
				}
			}
		}
	}

	if (ret < 0) {
		fprintf(stderr, "Failed recvmsg, reason: %s\n",
		    strerror(errno));
	} else {
		rc = read(fd, ((char *)msg) + ret, msg->size);
		if (rc != msg->size) {
			fprintf(stderr, "Incomplete read of a message\n");
			ret = -1;
		}
	}

	return ret;
}

int
vhost_ioctl(int sock, VhostUserRequest request, void *arg)
{
	VhostUserMsg msg;
	struct vhost_vring_file *file = 0;
	int need_reply = 0;
	int fds[VHOST_MEMORY_MAX_NREGIONS];
	size_t fd_num = 0;

	memset(&msg, 0, sizeof (msg));
	msg.request = request;
	msg.flags &= ~VHOST_USER_VERSION_MASK;
	msg.flags |= VHOST_USER_VERSION;
	msg.size = 0;

	switch (request) {
	case VHOST_USER_GET_FEATURES:
	case VHOST_USER_GET_VRING_BASE:
		need_reply = 1;
		break;

	case VHOST_USER_SET_FEATURES:
	case VHOST_USER_SET_LOG_BASE:
		msg.u64 = *((uint64_t *) arg);
		msg.size = sizeof (msg.u64);
		break;

	case VHOST_USER_SET_OWNER:
	case VHOST_USER_RESET_OWNER:
		break;

	case VHOST_USER_SET_MEM_TABLE:
		msg.memory = *((VhostUserMemory *)arg);
		msg.size = sizeof (msg.memory.nregions) +
		    sizeof (msg.memory.padding) +
		    (msg.memory.nregions * sizeof (VhostUserMemoryRegion));
		fd_num = VHOST_MEMORY_MAX_NREGIONS;
		if (get_memory_fds(fds, &fd_num) != 0) {
			fprintf(stderr, "Failed to open memory region\n");
			return (-1);
		}
		break;

	case VHOST_USER_SET_LOG_FD:
		fds[fd_num++] = *((int *) arg);
		break;

	case VHOST_USER_SET_VRING_NUM:
	case VHOST_USER_SET_VRING_BASE:
		memcpy(&msg.state, arg, sizeof (msg.state));
		msg.size = sizeof (msg.state);
		break;

	case VHOST_USER_SET_VRING_ADDR:
		memcpy(&msg.addr, arg, sizeof (msg.addr));
		msg.size = sizeof (msg.addr);
		break;

	case VHOST_USER_SET_VRING_KICK:
	case VHOST_USER_SET_VRING_CALL:
	case VHOST_USER_SET_VRING_ERR:
		file = arg;
		msg.u64 = file->index;
		msg.size = sizeof (msg.u64);
		if (file->fd > 0) {
			fds[fd_num++] = file->fd;
		}
		break;

	case VHOST_USER_NONE:
		break;
	default:
		return (-1);
	}


	if (vhost_user_send_fds(sock, &msg, fds, fd_num) < 0) {
		fprintf(stderr, "ioctl send failed\n");
		return (-1);
	}

	if (request == VHOST_USER_SET_MEM_TABLE) {
		for (int i = 0; i < fd_num; i++)
			(void) close(fds[i]);
	}

	if (need_reply) {
		msg.request = VHOST_USER_NONE;
		msg.flags = 0;

		if (vhost_user_recv_fds(sock, &msg, fds, &fd_num) < 0) {
			fprintf(stderr, "ioctl rcv failed\n");
			return (-1);
		}

		assert(msg.request == request);
		assert((msg.flags & VHOST_USER_VERSION_MASK) ==
		    VHOST_USER_VERSION);

		switch (request) {
		case VHOST_USER_GET_FEATURES:
			*((uint64_t *) arg) = msg.u64;
			break;
		case VHOST_USER_GET_VRING_BASE:
			memcpy(arg, &msg.state,
			    sizeof (struct vhost_vring_state));
			break;
		default:
			return (-1);
		}

	}

	return (0);
}
