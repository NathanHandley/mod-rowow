//  Author: Nathan Handley (nathanhandley@protonmail.com)
//  Copyright (c) 2026 Nathan Handley
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU Affero General Public License as published by the
//  Free Software Foundation; either version 3 of the License, or (at your
//  option) any later version.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.See the GNU Affero General Public License for
//  more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "Creature.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// RO combatants: the units whose combat follows RO's rules.  Everything here reads data loaded at startup, so any map thread may call it

bool ROWOWMod::IsROCombatant(Unit const* unit) const
{
    if (unit == nullptr || IsEnabled == false)
        return false;
    if (unit->IsCreature() == true)
        return GetROMobID(unit->ToCreature()) != 0;
    if (unit->IsPlayer() == true)
        return IsInROForm(unit->ToPlayer());
    return false;
}

uint32 ROWOWMod::GetROMobID(Creature const* creature) const
{
    if (creature == nullptr)
        return 0;
    auto mobIDIter = MobIDsByCreatureEntry.find(creature->GetEntry());
    if (mobIDIter == MobIDsByCreatureEntry.end())
        return 0;
    return mobIDIter->second;
}

bool ROWOWMod::IsInROForm(Player const* player) const
{
    if (player == nullptr)
        return false;
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr && identity->IsInForm == true;
}
