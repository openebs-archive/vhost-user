
# vhost-user client library

The Goal of this project is to provide simple API for reading and writing bytes
from storage backend provided by [SPDK](https://github.com/spdk/spdk) vhost
app. Compared to SPDK's virtio library this library does not enforce any
particular application design (non-blocking and busy polling style adopted by
SPDK and applications built on top of it). Thus it is more suitable for legacy
applications which are typically multithreaded and use classic synchronization
primitives.

This is a **proof of concept** suitable for experimenting and evaluating
performance. It neither feature complete nor stable enough and design of
some parts would need to change considerably to make it production ready.

## System configuration

IO buffers and shared virtio structures (vrings) must reside in huge pages.
There is a limit on the number of shared memory regions, which does not make
practical to use any other huge page size than 1GB. The most convenient is
to automatically allocate huge pages during system boot. Modify
/etc/default/grub to contain the following line (in this case we allocate 4 pages,
but can be anything greater or equal to 2):

```
GRUB_CMDLINE_LINUX_DEFAULT="hugepagesz=1GB hugepages=4 default_hugepagesz=1GB"
```
Then regenerate grub configuration file (update-grub command) and reboot.
Allocating huge pages dynamically when the system is booted is also possible by
writing to a file in /sys (as root):

```
echo 4 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
```

Note that the max number of huge pages that vhost-user lib can handle is
currently set to 8.

## Dependencies

DPDK's memory allocator and ring buffer implementation is used in the library
(until we have our own implementation of these two features). You can either
checkout DPDK repo or use SPDK's DPDK submodule in case you already have
SPDK sources on your machine. vhost-user was tested with DPDK version
18.08. Using DPDK repo:

```
git clone git://dpdk.org/dpdk
cd dpdk
make config T=x86_64-native-linuxapp-gcc
make T=x86_64-native-linuxapp-gcc EXTRA_CFLAGS=-fPIC
```

fPIC is needed for building fio engine shared library. Using SPDK's DPDK
submodule:

```
git clone https://github.com/spdk/spdk.git
cd spdk
git submodule update --init
./configure --with-fio
make
cd dpdk
ln -s build x86_64-native-linuxapp-gcc
```

Configure option --with-fio takes care of building DPDK with -fPIC. The symlink
created in the last step is needed for DPDK's makefiles for external apps, so
that they can find DPDK libraries.

For building FIO engine fio sources are needed (in fact only the header files,
but you can build the fio and run it from repo):

```
git clone https://github.com/axboe/fio
cd fio
./configure
make
```

and of course, SPDK is needed because it is our storage backend. Follow
instructions in SPDK's README for building it.

## Building

The build system is hacky currently. Makefiles from DPDK repo for building
external applications and libraries are used. However, they are not suitable
for building more complicated things with internal dependencies. So there are 
two makefiles for building two targets and "Makefile" symlink to one of them
decides what will be built. The two targets are:

 * Program for testing basic IO functionality
 * FIO virtio engine for testing performance

### Test program

Steps to build test program:
```
export RTE_SDK=/path/to/dpdk
rm Makefile
ln -s Makefile.test Makefile
make
```

Before running the test program, start SPDK vhost app in another window.
Example of spdk.conf file for vhost app is in the repo. Option "-t all"
prints detailed debug messages to output. In SPDK repo:
```
sudo ./app/vhost/vhost -c spdk.conf -S /tmp -t all
```

Following command runs the test program which does 100 async write-read
iterations verifying the content starting from offset 0 (second
iteration from offset 1000, then from 2000, ...).
If the test passes then the return code is zero.
```
sudo ./build/app/test -s /tmp/vhost.0 -i 100 -a -S 1000
```

### FIO virtio engine

Steps to build the engine:
```
export RTE_SDK=/path/to/dpdk
export FIO_SOURCE_DIR=/path/to/fio
rm Makefile
ln -s Makefile.fio Makefile
make
```

Before running fio, start SPDK vhost app in another window.
Example of spdk.conf file for vhost app is in the repo. In SPDK repo:

```
sudo ./app/vhost/vhost -c spdk.conf
```

Path to external engine must be explicitly specified since fio is unaware of it.
That is handled by setting LD_LIBRARY_PATH. Example of fio job configuration is
in the repo (virtio.fio). In fio repo run as root:

```
LD_LIBRARY_PATH=/path/to/repo/build/lib ./fio /path/to/repo/virtio.fio
```

Note that multiple fio jobs are not supported at this moment. It has to be
always set to 1.

## Adaptive polling

There are two basic approaches to synchronization between consumer and
producer. Either event-based notification (using eventfd file descriptor
and read/write syscall) or polling (checking condition in a busy loop).
As explained earlier busy polling does not play well with multithreaded
programs using blocking calls. We modified busy polling method by inserting
relatively short sleep into a busy loop. However, that does not perform well
in all cases and requires manual tuning of the sleep interval. For sequential IO
it increases latency way too much. Hence we utilize both approaches based on
detected workload. We distinguish between two types of workloads:

 * less than 120,000 IOPS: event based sync decreases latency and performs
   relatively well.
 * more than 120,000 IOPS: focused on throughput. During sleep the work is
   accumulated in queues and later efficiently processed in bulk.

Speaking about polling with sleep - there is no single time interval which fits
all workloads. Right interval depends on speed of storage device, io depth and
other characteristics of the system. Hence the library learns what is the best
sleep interval by dynamically changing it and observing what it does with IOPS.
That works well for stable workloads without many up and down changes. The main
advantage is simplicity of code.

## Parameters

At build time following C preprocessor defines can be specified:

 * IOPS_HIGH: Boundary between event driven and time driven poll. Can be
   set to very high or low value in order to eliminate one of the methods
   completely.
 * POLL_DELAY: Fixed delay in poll loop in msecs. If defined it eliminates
   event based poll altogether.
