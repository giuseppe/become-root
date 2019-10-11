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
#ifndef _SUBUGIDMAP_H
# define _SUBUGIDMAP_H

# include <stdint.h>
# include <unistd.h>
# include <sys/types.h>

struct user_mapping
{
  uint32_t first_subuid, n_subuid;
  uint32_t first_subgid, n_subgid;
};

int getsubidrange (uid_t uid, int is_uid, uint32_t *from, uint32_t *len);

#endif
