/* become-root
 * Copyright (C) 2018-2020 Giuseppe Scrivano
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
#include <sys/socket.h>
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
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <grp.h>

static int
write_mapping (char *program, pid_t pid, uint32_t host_id,
               uint32_t first_subid, uint32_t n_subids)
{
  pid_t fpid;
  char arg_buffer[32][16];
  char *argv[32] = { 0 };
  int argc = 0;

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
      int r, status;
      do
        r = waitpid (fpid, &status, 0);
      while (r < 0 && errno == EINTR);
      if (r < 0)
        error (EXIT_FAILURE, errno, "waitpid");

      if (WIFEXITED (status))
        return WEXITSTATUS (status);
      if (WIFSIGNALED (status))
        return 128 + WTERMSIG (status);
      return 0;
    }
  else
    {
      execvp (argv[0], argv);
      _exit (EXIT_FAILURE);
    }
  return 0;
}

static void
write_user_group_mappings (struct user_mapping *user_mapping, uid_t uid, gid_t gid, pid_t pid)
{
  char *newuidmap = getenv ("NEWUIDMAP");
  char *newgidmap = getenv ("NEWGIDMAP");
  if (newuidmap == NULL)
    newuidmap = "/usr/bin/newuidmap";
  if (newgidmap == NULL)
    newgidmap = "/usr/bin/newgidmap";

  if (write_mapping (newuidmap, pid, uid, user_mapping->first_subuid, user_mapping->n_subuid))
    error (EXIT_FAILURE, 0, "could not write mappings");
  if (write_mapping (newgidmap, pid, gid, user_mapping->first_subgid, user_mapping->n_subgid))
    error (EXIT_FAILURE, 0, "could not write mappings");
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
          b_it += sprintf (b_it, "%u %u %u\n", so_far, id, len);
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
          char *path;

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

          path = getenv ("SLIRP4NETNS");
          if (path == NULL)
            path = "slirp4netns";
          execlp (path, "slirp4netns", "-c", "-e", pipe_fmt, "-r", sync_fmt, parent_fmt, "tap0", NULL);
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

static void
usage (FILE *o, char **argv)
{
  fprintf (o, "Usage: %s -acimnpuPSN COMMAND [ARGS]\n", argv[0]);
  fprintf (o, "  -A unshare all the namespaces and do fresh mounts\n");
  fprintf (o, "  -a unshare all the namespaces\n");
  fprintf (o, "  -c specify CLONE_NEWCGROUP\n");
  fprintf (o, "  -i specify CLONE_NEWIPC\n");
  fprintf (o, "  -m specify CLONE_NEWNS (mount namespace)\n");
  fprintf (o, "  -N configure the network with slirp4netns\n");
  fprintf (o, "  -n specify CLONE_NEWNET (no configuration performed)\n");
  fprintf (o, "  -p specify CLONE_NEWPID\n");
  fprintf (o, "  -u specify CLONE_NEWUTS\n");
  fprintf (o, "  -P mount a fresh /proc\n");
  fprintf (o, "  -S mount a fresh /sys\n");
  fprintf (o, "  -C mount cgroup2 under /sys/fs/cgroup\n");
  fprintf (o, "  -k do not drop secondary groups\n");
  fprintf (o, "  -r run a reaper as PID 1 (forces -p)\n");
}

static
void run_reaper ()
{
  pid_t r;

  if (prctl (PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) < 0)
    error (EXIT_FAILURE, errno, "cannot set subreaper");

  do
    r = waitpid (-1, NULL, 0);
  while (r > 0 || (errno != ECHILD && errno != EINTR));
  _exit (EXIT_SUCCESS);
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
  bool mount_cgroup2 = false;
  bool configure_network = false;
  bool keep_groups = false;
  bool reaper = false;
  bool keep_mapping = uid == 0;
  int network_pipe[2];
  char uid_fmt[16];

  argv++;

  if (argc == 1 && getenv ("SHELL") == NULL)
    error (EXIT_FAILURE, 0, "please specify a command");

  for (; *argv && *argv[0] == '-'; argv++)
    {
      char *c;
      if (strcmp (*argv, "--help") == 0 || strcmp (*argv, "-h") == 0)
        {
          usage (stdout, argv);
          exit (EXIT_SUCCESS);
        }

      for (c = *argv + 1; *c; c++)
        {
          switch (*c)
            {
            case 'A':
              mount_proc = true;
              mount_sys = true;

              /* fallthrough.  */

            case 'a':
#ifdef CLONE_NEWCGROUP
              flags |= CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWCGROUP | CLONE_NEWUTS;
#else
              flags |= CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWUTS;
#endif
              break;

            case 'c':
#ifdef CLONE_NEWCGROUP
              flags |= CLONE_NEWCGROUP;
#endif
              break;

            case 'C':
              mount_cgroup2 = true;
              break;

            case 'i':
              flags |= CLONE_NEWIPC;
              break;

            case 'm':
              flags |= CLONE_NEWNS;
              break;

            case 'N':
              configure_network = true;
              /* fallthrough */
            case 'n':
              flags |= CLONE_NEWNET;
              break;

            case 'r':
              reaper = true;
              /* fallthrough */
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

            case 'k':
              keep_groups = true;
              break;

            default:
              error (0, 0, "unknown option: %s", c);
              usage (stderr, argv);
              exit (EXIT_FAILURE);
            }
        }
    }

  if (! keep_mapping)
    {
      if (getsubidrange (uid, 1, &user_mapping.first_subuid, &user_mapping.n_subuid) < 0)
        error (EXIT_FAILURE, errno, "cannot read subuid file or find the user");
      if (getsubidrange (uid, 0, &user_mapping.first_subgid, &user_mapping.n_subgid) < 0)
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

      if (!keep_groups && setgroups (0, NULL) < 0)
        error (EXIT_FAILURE, errno, "setgroups");

      if (prctl (PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0)
            error (EXIT_FAILURE, errno, "cannot set keepcaps");

      if (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        error (EXIT_FAILURE, errno, "cannot set no new privileges");

      set_all_caps ();

      if (mount_proc)
        {
          if (mount ("proc", "/proc", "proc", MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL) < 0)
            error (EXIT_FAILURE, errno, "could not mount proc");
        }

      if (mount_sys && mount ("sysfs", "/sys", "sysfs", MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL) < 0)
            error (EXIT_FAILURE, errno, "could not mount sys");

      if (mount_cgroup2 && mount ("cgroup2", "/sys/fs/cgroup", "cgroup2", MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL) < 0)
            error (EXIT_FAILURE, errno, "could not mount cgroup2");

      if (*argv == NULL)
        {
          const char *shell = getenv ("SHELL");
          if (execlp (shell, shell, NULL) < 0)
            error (EXIT_FAILURE, errno, "cannot exec %s", shell);
        }

      /* Used by podman when setting up a rootless user namespace.  */
      setenv ("_LIBPOD_USERNS_CONFIGURED", "init", 1);
      sprintf (uid_fmt, "%d", uid);
      setenv ("_LIBPOD_ROOTLESS_UID", uid_fmt, 1);

      if (reaper)
        {
          pid_t p = fork ();
          if (p < 0)
            error (EXIT_FAILURE, errno, "fork");
          if (p)
            run_reaper ();
        }

      if (execvp (*argv, argv) < 0)
        error (EXIT_FAILURE, errno, "cannot exec %s", argv[1]);
      _exit (EXIT_FAILURE);
    }
  return 0;
}
