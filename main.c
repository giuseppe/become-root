/* become-root
 * Copyright (C) 2018 Giuseppe Scrivano
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define _GNU_SOURCE

#include "subugidmap.h"
#include <error.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string.h>
#include <stdbool.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>

static void
write_mapping (char *program, pid_t pid, uint32_t host_id,
               uint32_t first_subid, uint32_t n_subids)
{
  pid_t fpid;
  char arg_buffer[32][16];
  char *argv[32] = { 0 };
  int argc = 0;
  int exit_status;

#define APPEND_ARGUMENT(x)                        \
  do                                              \
    {                                             \
      sprintf (arg_buffer[argc], "%i", x);        \
      argv[argc] = arg_buffer[argc];              \
      argc++;                                     \
    } while (0)

  argv[argc++] = program;
  APPEND_ARGUMENT (pid);

  APPEND_ARGUMENT (0);
  APPEND_ARGUMENT (host_id);
  APPEND_ARGUMENT (1);

  APPEND_ARGUMENT (1);
  APPEND_ARGUMENT (first_subid);
  APPEND_ARGUMENT (n_subids);

  argv[argc] = NULL;

  fpid = fork ();
  if (fpid < 0)
    error (EXIT_FAILURE, errno, "cannot fork");
  if (fpid)
    {
      int r;
      do
        r = waitpid (fpid, NULL, 0);
      while (r < 0 && errno == EINTR);
      if (r < 0)
        error (EXIT_FAILURE, errno, "waitpid");
    }
  else
    {
      execvp (argv[0], argv);
      _exit (EXIT_FAILURE);
    }
}

static void
write_user_group_mappings (struct user_mapping *user_mapping, uid_t uid, gid_t gid, pid_t pid)
{
  write_mapping ("/usr/bin/newuidmap", pid, uid, user_mapping->first_subuid, user_mapping->n_subuid);
  write_mapping ("/usr/bin/newgidmap", pid, gid, user_mapping->first_subgid, user_mapping->n_subgid);
}

static void
copy_mappings (const char *from, const char *to)
{
#define SIZE (1 << 12)
  char *b, *b_it, *it;
  ssize_t s, r, c = 0;
  int srcfd = -1, destfd = -1;
  uint32_t so_far = 0, id, len;

  b = malloc (SIZE);
  if (b == NULL)
    error (EXIT_FAILURE, errno, "cannot allocate memory");

  srcfd = open (from, O_RDONLY);
  if (srcfd < 0)
    error (EXIT_FAILURE, errno, "could not open %s", from);

  destfd = open (to, O_RDWR);
  if (destfd < 0)
    error (EXIT_FAILURE, errno, "could not open %s", to);

  do
    r = read (srcfd, b, SIZE);
  while (r < 0 && errno == EINTR);
  if (r < 0)
    error (EXIT_FAILURE, errno, "could not read from %s", from);

  for (b_it = b, c = 1, it = strtok (b, " "); it; it = strtok (NULL, " "), c++)
    {
      switch (c % 3)
        {
        case 0:
          len = strtoull (it, NULL, 10);
          b_it += sprintf (b_it, "%lu %lu %lu\n", so_far, id, len);
          so_far += len;
          break;

        case 1:
          id = strtoull (it, NULL, 10);
          break;

        case 2:
          /* Ignore.  */
          break;
        }
    }

  do
    s = write (destfd, b, r);
  while (s < 0 && errno == EINTR);
  if (s < 0)
    error (EXIT_FAILURE, errno, "could not write to %s", to);

  close (srcfd);
  close (destfd);

  free (b);
}

static void
do_setup (struct user_mapping *user_mapping,
          uid_t uid,
          gid_t gid,
          pid_t parent,
          int p1[2],
          int p2[2],
          bool keep_mapping,
          bool configure_network,
          int network_pipe)
{
  char b;
  ssize_t s;
  int sync_fd[2];

  do
    s = read (p1[0], &b, 1);
  while (s < 0 && errno == EINTR);
  if (s < 0)
    error (EXIT_FAILURE, errno, "cannot read from pipe");

  /* The parent process failed, so don't do anything.  */
  if (b != '0')
    return;

  if (!keep_mapping)
      write_user_group_mappings (user_mapping, uid, gid, parent);
  else
    {
      char dest[64];

      sprintf (dest, "/proc/%d/gid_map", parent);
      copy_mappings ("/proc/self/gid_map", dest);

      sprintf (dest, "/proc/%d/uid_map", parent);
      copy_mappings ("/proc/self/uid_map", dest);
    }

  if (configure_network)
    {
      pid_t fpid;
      int sync_fd[2];

      if (pipe (sync_fd) < 0)
        error (EXIT_FAILURE, errno, "cannot create pipe");

      fpid = fork ();
      if (fpid < 0)
        error (EXIT_FAILURE, errno, "cannot fork");
      if (fpid)
        {
          int ret;
          char b;

          close (sync_fd[1]);
          do
            ret = read (sync_fd[0], &b, 1);
          while (ret < 0 && errno == EINTR);
          if (ret < 0)
            error (EXIT_FAILURE, errno, "cannot read from sync pipe");
          close (sync_fd[0]);
          close (network_pipe);
        }
      else
        {
          char pipe_fmt[16];
          char parent_fmt[16];
          char sync_fmt[16];
          int dev_null;

          close (sync_fd[0]);

          setpgid (0, 0);
          /* double fork.  */
          if (fork ())
            _exit (EXIT_SUCCESS);

          dev_null = open ("/dev/null", O_RDWR);
          dup2 (dev_null, 0);
          dup2 (dev_null, 1);
          dup2 (dev_null, 2);

          sprintf (pipe_fmt, "%d", network_pipe);
          sprintf (parent_fmt, "%d", parent);
          sprintf (sync_fmt, "%d", sync_fd[1]);

          execlp ("slirp4netns", "slirp4netns", "-c", "-e", pipe_fmt, "-r", sync_fmt, parent_fmt, "tap0", NULL);
          _exit (EXIT_FAILURE);
        }

    }

  do
    s = write (p2[1], "0", 1);
  while (s < 0 && errno == EINTR);
  if (s < 0)
    error (EXIT_FAILURE, errno, "cannot write to pipe");
}

static void
set_all_caps ()
{
  int i;
  struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
  struct __user_cap_data_struct data[2] = { { 0 } };

  data[0].effective = 0xFFFFFFFF;
  data[0].permitted = 0xFFFFFFFF;
  data[0].inheritable = 0xFFFFFFFF;
  data[1].effective = 0xFFFFFFFF;
  data[1].permitted = 0xFFFFFFFF;
  data[1].inheritable = 0xFFFFFFFF;
  if (capset (&hdr, data) < 0)
    error (EXIT_FAILURE, errno, "cannot set capabilities");

  for (i = 0; ; i++)
    {
      if (prctl (PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, i, 0, 0) < 0)
        {
          if (errno == EINVAL)
            break;
          if (errno == EPERM)
            continue;
          error (EXIT_FAILURE, errno, "cannot raise capability");
        }
    }
}

int
main (int argc, char **argv)
{
  pid_t pid, parentpid;
  int p1[2], p2[2];
  uid_t uid = geteuid ();
  gid_t gid = getegid ();
  struct user_mapping user_mapping;
  unsigned int flags = CLONE_NEWUSER;
  bool mount_proc = false;
  bool mount_sys = false;
  bool configure_network = false;
  bool keep_mapping = uid == 0;
  int network_pipe[2];

  if (argc == 1)
    error (EXIT_FAILURE, 0, "please specify a command");

  argv++;

  for (; *argv && *argv[0] == '-'; argv++)
    {
      char *c;
      if (strcmp (*argv, "--help") == 0 || strcmp (*argv, "-h") == 0)
        {
          printf ("Usage: %s -acimnpuPSN COMMAND [ARGS]\n", argv[0]);
          exit (EXIT_SUCCESS);
        }

      for (c = *argv + 1; *c; c++)
        {
          switch (*c)
            {
            case 'a':
              flags |= CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWCGROUP | CLONE_NEWUTS;
              break;

            case 'c':
              flags |= CLONE_NEWCGROUP;
              break;

            case 'i':
              flags |= CLONE_NEWIPC;
              break;

            case 'm':
              flags |= CLONE_NEWNS;
              break;

            case 'N':
              configure_network = true;
              /* passthrough */
            case 'n':
              flags |= CLONE_NEWNET;
              break;

            case 'p':
              flags |= CLONE_NEWPID;
              break;

            case 'u':
              flags |= CLONE_NEWUTS;
              break;

            case 'P':
              mount_proc = true;
              break;

            case 'S':
              mount_sys = true;
              break;

            default:
              error (EXIT_FAILURE, 0, "unknown option");
            }
        }
    }

  if (! keep_mapping)
    {
      if (getsubidrange (uid, 1, &user_mapping.first_subuid, &user_mapping.n_subuid) < 0)
        error (EXIT_FAILURE, errno, "cannot read subuid file or find the user");
      if (getsubidrange (gid, 1, &user_mapping.first_subgid, &user_mapping.n_subgid) < 0)
        error (EXIT_FAILURE, errno, "cannot read subgid file or find the user");
    }

  if (pipe2 (p1, O_CLOEXEC) < 0)
    error (EXIT_FAILURE, errno, "cannot create pipe");
  if (pipe2 (p2, O_CLOEXEC) < 0)
    error (EXIT_FAILURE, errno, "cannot create pipe");

  parentpid = getpid ();

  if (configure_network)
    {
      if (pipe (network_pipe) < 0)
        error (EXIT_FAILURE, errno, "cannot create pipe");
    }

  pid = fork ();
  if (pid < 0)
    error (EXIT_FAILURE, errno, "cannot fork");
  if (pid == 0)
    {
      close (p1[1]);
      close (p2[0]);
      close (network_pipe[1]);
      do_setup (&user_mapping, uid, gid, parentpid, p1, p2, keep_mapping, configure_network, network_pipe[0]);
    }
  else
    {
      int r;
      char b;
      close (p1[0]);
      close (p2[1]);
      if (configure_network)
        close (network_pipe[0]);
      /* leak network_pipe[1] */

      if (unshare (flags) < 0)
        error (EXIT_FAILURE, errno, "cannot create the user namespace");

      do
        r = write (p1[1], "0", 1);
      while (r < 0 && errno == EINTR);
      if (r < 0)
        error (EXIT_FAILURE, errno, "cannot write to pipe");

      do
        r = read (p2[0], &b, 1);
      while (r < 0 && errno == EINTR);
      /* Setup failed, just exit.  */
      if (b != '0')
        exit (EXIT_FAILURE);

      do
        r = waitpid (pid, NULL, 0);
      while (r < 0 && errno == EINTR);

      if (flags & CLONE_NEWPID)
        {
          pid_t child = fork ();
          if (child < 0)
            error (EXIT_FAILURE, errno, "could not fork");
          else if (child)
            {
              int status;
              do
                r = waitpid (child, &status, 0);
              while (r < 0 && errno == EINTR);
              if (r < 0)
                error (EXIT_FAILURE, errno, "error waitpid");
              if (WIFEXITED (status))
                r = WEXITSTATUS (status);
              else if (WIFSIGNALED (status))
                r = 128 + WTERMSIG (status);
              exit (r);
            }
        }

      if (setresuid (0, 0, 0) < 0)
        error (EXIT_FAILURE, errno, "cannot setresuid");

      if (prctl (PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0)
            error (EXIT_FAILURE, errno, "cannot set keepcaps");

      if (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        error (EXIT_FAILURE, errno, "cannot set no new privileges");

      set_all_caps ();

      if (mount_proc && mount ("proc", "/proc", "proc", 0, "nosuid,noexec,nodev") < 0)
            error (EXIT_FAILURE, errno, "could not mount proc");

      if (mount_sys && mount ("sysfs", "/sys", "sysfs", 0, "nosuid,noexec,nodev") < 0)
            error (EXIT_FAILURE, errno, "could not mount sys");

      if (execvp (*argv, argv) < 0)
        error (EXIT_FAILURE, errno, "cannot exec %s", argv[1]);
      _exit (EXIT_FAILURE);
    }
  return 0;
}
