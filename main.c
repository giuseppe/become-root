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
do_setup (struct user_mapping *user_mapping, uid_t uid, gid_t gid, pid_t parent, int p1[2], int p2[2])
{
  char b;
  read (p1[0], &b, 1);
  write_user_group_mappings (user_mapping, uid, gid, parent);
  write (p2[1], "0", 1);
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

  if (argc == 1)
    error (EXIT_FAILURE, 0, "please specify a command");

  argv++;

  for (; *argv && *argv[0] == '-'; argv++)
    {
      char *c;
      if (strcmp (*argv, "--help") == 0 || strcmp (*argv, "-h") == 0)
        {
          printf ("Usage: %s -acimnpuPS COMMAND [ARGS]\n", argv[0]);
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

  if (getsubidrange (uid, 1, &user_mapping.first_subuid, &user_mapping.n_subuid) < 0)
    error (EXIT_FAILURE, errno, "cannot read subuid file or find the user");
  if (getsubidrange (gid, 1, &user_mapping.first_subgid, &user_mapping.n_subgid) < 0)
    error (EXIT_FAILURE, errno, "cannot read subgid file or find the user");

  if (pipe2 (p1, O_CLOEXEC) < 0)
    error (EXIT_FAILURE, errno, "cannot create pipe");
  if (pipe2 (p2, O_CLOEXEC) < 0)
    error (EXIT_FAILURE, errno, "cannot create pipe");

  parentpid = getpid ();

  pid = fork ();
  if (pid < 0)
    error (EXIT_FAILURE, errno, "cannot fork");
  if (pid == 0)
    {
      close (p1[1]);
      close (p2[0]);
      do_setup (&user_mapping, uid, gid, parentpid, p1, p2);
    }
  else
    {
      int r;
      char b;
      close (p1[0]);
      close (p2[1]);

      if (unshare (flags) < 0)
        error (EXIT_FAILURE, errno, "cannot create the user namespace");

      write (p1[1], "0", 1);
      read (p2[0], &b, 1);

      if (setresuid (0, 0, 0) < 0)
        error (EXIT_FAILURE, errno, "cannot setresuid");

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
