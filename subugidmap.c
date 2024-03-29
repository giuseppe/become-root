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

#include <config.h>
#include "subugidmap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>

#ifdef HAVE_LIBSUBID
# include <shadow/subid.h>
#endif

static void
cleanup_freep (void *p)
{
  void **pp = (void **) p;
  free (*pp);
}

static void
cleanup_filep (FILE **f)
{
  FILE *file = *f;
  if (file)
    (void) fclose (file);
}


#ifdef HAVE_LIBSUBID

#if !defined(SUBID_ABI_MAJOR) || (SUBID_ABI_MAJOR < 4)
# define subid_get_uid_ranges get_subuid_ranges
# define subid_get_gid_ranges get_subgid_ranges
#endif


/*if subuid or subgid exist, take the first range for the user */
int
getsubidrange (uid_t uid, int is_uid, uint32_t *from, uint32_t *len)
{
  int i, ret;
  struct passwd *pwd;
  struct subid_range *ranges = NULL;

  pwd = getpwuid (uid);
  if (pwd == NULL)
    return -1;

  if (is_uid)
    ret = subid_get_uid_ranges (pwd->pw_name, &ranges);
  else
    ret = subid_get_gid_ranges (pwd->pw_name, &ranges);
  if (ret < 0)
    return ret;

  /* Nothing found.  */
  if (ret == 0)
    return -1;

  *from = ranges[0].start;
  *len = ranges[0].count;

  free (ranges);
  return 0;
}

#else

# define cleanup_free __attribute__((cleanup (cleanup_freep)))
# define cleanup_file __attribute__((cleanup (cleanup_filep)))

/*if subuid or subgid exist, take the first range for the user */
int
getsubidrange (uid_t uid, int is_uid, uint32_t *from, uint32_t *len)
{
  cleanup_file FILE *input = NULL;
  cleanup_free char *lineptr = NULL;
  ssize_t len_name;
  size_t lenlineptr = 0, uid_fmt_len;
  const char *name;
  char uid_fmt[16];
  struct passwd *pwd = getpwuid (uid);
  if (pwd == NULL)
    return -1;
  name = pwd->pw_name;

  len_name = strlen (name);

  sprintf (uid_fmt, "%d:", uid);

  uid_fmt_len = strlen (uid_fmt);

  input = fopen (is_uid ? "/etc/subuid" : "/etc/subgid", "r");
  if (input == NULL)
    return -1;

  for (;;)
    {
      char *endptr;
      int read = getline (&lineptr, &lenlineptr, input);
      if (read < 0)
        return -1;

      if (read < len_name + 2)
        continue;

      if ((memcmp (lineptr, name, len_name) || lineptr[len_name] != ':') && memcmp (lineptr, uid_fmt, uid_fmt_len))
        continue;

      *from = strtoull (strchr (lineptr, ':') + 1, &endptr, 10);

      if (endptr >= &lineptr[read])
        return -1;

      *len = strtoull (&endptr[1], &endptr, 10);

      return 0;
    }
}

#endif
