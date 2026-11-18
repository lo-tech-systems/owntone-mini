/*
 * Copyright (C) 2026 James Pearce
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __OUTPUTS_GROUP_FLAGS_H__
#define __OUTPUTS_GROUP_FLAGS_H__

#include <stdbool.h>

// A device's group facts, and whether the Apple TV leading its group has been
// excluded from the group's session
struct tv_proxy_flags
{
  bool tv_proxy_group;
  bool grouped;
  bool leader;
  bool leader_speakers_only;
};

struct tv_proxy_roles
{
  bool follower;
  bool leader;
  bool leads_group;
};

// Behavioural roles of a device in a TV proxy group from its group flags and
// its leader's exclusion flag. A group whose Apple TV is excluded behaves as
// a plain stereo pair: neither role applies, but the structural leadership
// is still reported.
static inline struct tv_proxy_roles
tv_proxy_flags_resolve(struct tv_proxy_flags f)
{
  struct tv_proxy_roles r = { 0 };
  bool structural_leader = f.tv_proxy_group && f.grouped && f.leader;

  r.leads_group = structural_leader;
  if (f.leader_speakers_only)
    return r;

  r.follower = f.tv_proxy_group && !(f.grouped && f.leader);
  r.leader   = structural_leader;

  return r;
}

// What an enable request does about the Apple TV option. include_tv is -1 when
// the request does not carry the option, else 0 or 1. A refusal changes
// nothing: -4 asks the caller to disable and enable again, -5 reports that
// the HomePods the Apple TV would lead are not present.
struct tv_proxy_enable
{
  int refusal;
  bool set_flag;
  bool flag;
};

static inline struct tv_proxy_enable
tv_proxy_enable_resolve(bool leads_group, int include_tv, bool selected, bool excluded_now)
{
  struct tv_proxy_enable e = { 0, false, false };
  bool speakers_only = (include_tv == 0);

  if (!leads_group)
    {
      // Only an Apple TV leading a HomePod group can be left out of it, and
      // one that does not lead a group must not be started on its own
      if (speakers_only)
        e.refusal = -5;
      else
        e.set_flag = true;
      return e;
    }

  if (include_tv < 0)
    return e;

  if (selected && excluded_now != speakers_only)
    {
      e.refusal = -4;
      return e;
    }

  e.set_flag = true;
  e.flag = speakers_only;

  return e;
}

// Whether a stereo pair with no leader of its own keeps the member that led it
// last: while any member advertises the group our session issued to that
// member, which is the case our own multi-select sessions cause. The members
// move onto that group, and back off it, one at a time, and in between neither
// advertises itself as the leader
struct stereo_leader_memory
{
  bool leader_known;        // a member leads the pair by its own advertisement
  bool remembered_present;  // the member that led it last is still a member
  int members;
  int members_on_issued;    // members whose advertised gid is the one we issued
};

static inline bool
stereo_leader_memory_applies(struct stereo_leader_memory m)
{
  return !m.leader_known && m.remembered_present && m.members > 0 && m.members_on_issued > 0;
}

#endif /* !__OUTPUTS_GROUP_FLAGS_H__ */
