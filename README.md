
# vhost-user client library

Goal of this project is to provide simple API for reading and writing bytes
from storage backend provided by [SPDK](https://github.com/spdk/spdk) vhost
app. Compared to SPDK's virtio library this library does not enforce any
particular application design (non-blocking and busy polling style adopted by
SPDK and applications built on top of it). Thus it is more suitable for legacy
applications which are typicaly multithreaded and use classic synchronization
primitives.

This is a **proof of concept** suitable for experimenting and evaluating
performance. It is neither feature complete nor stable enough and design of
some parts would need to change considerably to make it production ready.

## System configuration

IO buffers and shared virtio structures (vrings) must reside in huge pages.
There is a limit on number of shared memory regions, which does not make
practical to use any other huge page size than 1GB. The most convenient is
to automatically allocate huge pages during system boot. Modify
/etc/default/grub to contain following line (in this case we allocate 4 pages,
but can be anything greater or equal to 2):

```
GRUB_CMDLINE_LINUX_DEFAULT="hugepagesz=1GB hugepages=4 default_hugepagesz=1GB"
```
Then regenerate grub configuration file (update-grub command) and reboot.
Allocating huge pages dynamically when system is booted is also possible by
writting to a file in /sys (as root):

```
echo 4 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
```

## Dependencies

DPDK's memory allocator and ring buffer implementation is used in the library
(until we have our own implementation of these two features). You need to checkout
code from DPDK's repo:

```
git clone git://dpdk.org/dpdk
cd dpdk
make config T=x86_64-native-linuxapp-gcc
make T=x86_64-native-linuxapp-gcc EXTRA_CFLAGS=-fPIC
```

fPIC is needed for building fio engine shared library. For building FIO engine
fio sources are needed (in fact only the header files, but you can build the
fio and run it from repo):

```
git clone https://github.com/axboe/fio
cd fio
./configure
make
```

and of course SPDK is needed because it is our storage backend. Follow
instructions in SPDK's README for building it.

## Building

The build system is hacky currently. Makefiles from DPDK repo for building
external applications and libraries are used. However they are not suitable
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
sudo ./app/vhost/vhost -c spdk.conf -t all
```

Following command runs the test program which does 100 async write-read
iterations verifying the content starting from offset 0 (second
iteration from offset 1000, then from 2000, ...).
If the test passes then the return code is zero.
```
sudo ./build/app/test -s /path/to/spdk/vhost.0 -i 100 -a -S 1000
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

#### SLEEPY_POLL

There is a performance compile-time tunable - SLEEPY_POLL C preprocessor define.
During testing it was discovered that instead of using eventfd and read/write
syscalls for delivering events to threads if condition is checked in a loop
with usleep(interval), the performance is up to 2x better. If you want to try
it out, you can modify Makefile.fio and change SLEEP_POLL define to i.e. 100ms.
