become-root
===========

Minimal tool (when compiled with CFLAGS='-s', the binary is around
10Kb) for launching a program into a new usernamespace and have
multiple users mapped.

The `subuidmap` and `subgidmap` tools are required for setting up the
user namespace.

The current user is mapped to the root user into the namespace, while
any additional uid/gid in `/etc/subuid` and `/etc/subgid` is mapped
starting with ID 1.

become-root doesn't currently support creating other kind of
namespaces, but it can be easily used together with `unshare`:

```console
$ become-root unshare -m echo hi from a new user and mount namespace
$ become-root cat /proc/self/uid_map
         0       1000          1
         1     110000      65536
$ become-root id
uid=0(root) gid=0(root) groups=0(root),65534(nfsnobody) context=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023
```
