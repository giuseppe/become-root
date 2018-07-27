become-root
===========

Minimal tool (when compiled with CFLAGS='-s', the binary is around
10Kb) for launching a program into a new user namespace and have
multiple users mapped.

The `subuidmap` and `subgidmap` tools are required for setting up the
user namespace.

The current user is mapped to the root user into the namespace, while
any additional uid/gid in `/etc/subuid` and `/etc/subgid` is mapped
starting with the ID 1.

## Build

Assuming you have the autotools and gcc installed:

```console
$ ./autogen.sh && ./configure && make
```

## Options

Some options are available:

* `a`: create all the namespaces
* `c`: create a CGroup namespace
* `i`: create an IPC namespace
* `m`: create a mount namespace
* `n`: create a network namespace
* `p`: create a PID namespace and fork
* `u`: create an UTS namespace
* `P`: mount a new `/proc`
* `S`: mount a new `/sys`
* `N`: configure the network with slirp4netns

## Examples

```console
$ become-root unshare -m echo hi from a new user and mount namespace
hi from a new user and mount namespace

$ become-root cat /proc/self/uid_map
         0       1000          1
         1     110000      65536

$ become-root id
uid=0(root) gid=0(root) groups=0(root),65534(nfsnobody) context=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023

$ become-root -aPS ps aux
USER       PID %CPU %MEM    VSZ   RSS TTY      STAT START   TIME COMMAND
root         1  0.0  0.0 246344  2016 pts/7   R+   13:58   0:00 ps aux
```
