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

#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include <rte_eal.h>
#include <rte_malloc.h>

#include "shm.h"

struct hugepage_file_info {
	uint64_t addr;            /**< virtual addr */
	size_t   size;            /**< the file size */
	char     path[PATH_MAX];  /**< path to backing file */
};

static int nregions;
static struct hugepage_file_info huges[VHOST_MEMORY_MAX_NREGIONS];

/*
 * This function is taken from SPDK. It opens memory map information file for
 * current process and determines huge page memory mapping ranges. As it is
 * not possible to get this info through standard DPDK's rte_eal API.
 */
static int
get_hugepage_file_info(struct hugepage_file_info huges[], int max)
{
	int idx;
	FILE *f;
	char buf[BUFSIZ], *tmp, *tail;
	char *str_underline, *str_start;
	int huge_index;
	uint64_t v_start, v_end;

	f = fopen("/proc/self/maps", "r");
	if (!f) {
		fprintf(stderr, "cannot open /proc/self/maps\n");
		return (-1);
	}

	idx = 0;
	while (fgets(buf, sizeof(buf), f) != NULL) {
		if (sscanf(buf, "%lx-%lx", &v_start, &v_end) < 2) {
			fprintf(stderr, "Failed to parse address\n");
			goto error;
		}

		tmp = strchr(buf, ' ') + 1; /** skip address */
		tmp = strchr(tmp, ' ') + 1; /** skip perm */
		tmp = strchr(tmp, ' ') + 1; /** skip offset */
		tmp = strchr(tmp, ' ') + 1; /** skip dev */
		tmp = strchr(tmp, ' ') + 1; /** skip inode */
		while (*tmp == ' ') {       /** skip spaces */
			tmp++;
		}
		tail = strrchr(tmp, '\n');  /** remove newline if exists */
		if (tail) {
			*tail = '\0';
		}

		/* Match HUGEFILE_FMT, aka "%s/%smap_%d",
		 * which is defined in eal_filesystem.h
		 */
		str_underline = strrchr(tmp, '_');
		if (!str_underline) {
			continue;
		}

		str_start = str_underline - strlen("map");
		if (str_start < tmp) {
			continue;
		}

		if (sscanf(str_start, "map_%d", &huge_index) != 1) {
			continue;
		}

		if (idx >= max) {
			fprintf(stderr, "Exceed maximum of %d\n", max);
			goto error;
		}
		huges[idx].addr = v_start;
		huges[idx].size = v_end - v_start;
		snprintf(huges[idx].path, PATH_MAX, "%s", tmp);
		idx++;
	}

	fclose(f);
	return (idx);

error:
	fclose(f);
	return (-1);
}

/*
 * Initializes DPDK providing fake command line arguments trying to avoid all
 * functionality which is not needed.
 */
int
init_shared_mem(int mem_mb)
{
	char *argv[20];
	int argc = 0;
	char mem_opt[20];

	snprintf(mem_opt, 20, "-m%d", mem_mb);

	argv[argc++] = "vhost";
	//argv[argc++] = "--log-level=10";
	argv[argc++] = "--no-pci";
	argv[argc++] = "--no-hpet";
	argv[argc++] = mem_opt;
	argv[argc] = NULL;

	if (rte_eal_init(argc, argv) < 0) {
		fprintf(stderr, "DPDK init failed\n");
		return (-1);
	}

	nregions = get_hugepage_file_info(huges, VHOST_MEMORY_MAX_NREGIONS);
	if (nregions < 0) {
		fprintf(stderr, "Failed to obtain information about memory\n");
		return (-1);
	}

	return (0);
}

void *
alloc_shared_buf(int size)
{
	return rte_malloc_socket(NULL, size, 0, SOCKET_ID_ANY);
}

void *
zalloc_shared_buf(int size)
{
	void *buf = rte_malloc_socket(NULL, size, 0, SOCKET_ID_ANY);
	if (buf != NULL)
		memset(buf, 0, size);
	return (buf);
}

void
free_shared_buf(void *buf)
{
	rte_free(buf);
}

void
get_memory_info(VhostUserMemory *memory)
{
	memory->nregions = nregions;

	for (int i = 0; i < nregions; i++) {
		VhostUserMemoryRegion *reg = &memory->regions[i];

		/*
		 * Physical address is not needed because SPDK vhost can map
		 * vaddr1 in prog1 to vaddr2 in prog2 based on offset from
		 * beginning of memory region (see qva_to_vva() in spdk).
		 */
		reg->guest_phys_addr = huges[i].addr;
		reg->userspace_addr = huges[i].addr;
		reg->memory_size = huges[i].size;
		reg->mmap_offset = 0;
	}
}

/*
 * Get file descriptors for huge pages so that we can pass them to another
 * process and share the memory with it.
 */
int
get_memory_fds(int *fds, size_t *size)
{
	unsigned int i;

	for (i = 0; i < *size && i < nregions; i++) {
		fds[i] = open(huges[i].path, O_RDWR);
		if (fds[i] < 0) {
			*size = 0;
			for (int j = 0; j < i; j++)
				(void) close(fds[j]);
			return (-1);
		}
	}
	*size = i;
	return (0);
}
