//  Author: Nathan Handley (nathanhandley@protonmail.com)
//  Copyright (c) 2026 Nathan Handley + Automation
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

#include "Player.h"

#include "ROWOW.h"

// The one place mod-rowow reads (and calls) mod-cityofheroes (the identities' City of Heroes side, Nathan 2026-11-16).  As with mod-everquest
// (ROWOW_WorldIdentityEverQuest.cpp) the two modules are built into the same library, so this file includes mod-cityofheroes' header when it is in
// the source tree, and without it Primal Earth and the City of Heroes identity simply don't exist: mod-rowow still builds and runs alone, and tells
// the client the three worlds and identities it always did.  Every call here is on the player's own thread (the identity is its CustomData)
#if __has_include("CityOfHeroes.h")
#include "CityOfHeroes.h"
#define ROWOW_HAS_MOD_CITYOFHEROES 1
#else
#define ROWOW_HAS_MOD_CITYOFHEROES 0
#endif

bool ROWOWMod::HasCityOfHeroes() const
{
#if ROWOW_HAS_MOD_CITYOFHEROES
    return CityOfHeroes->IsEnabled == true;
#else
    return false;
#endif
}

bool ROWOWMod::HasCityOfHeroesHero(Player* player) const
{
#if ROWOW_HAS_MOD_CITYOFHEROES
    return player != nullptr && CityOfHeroes->IsEnabled == true && CoHIdentityHasLook(player) == true;
#else
    (void)player;
    return false;
#endif
}

void ROWOWMod::SetCityOfHeroesIdentityLook(Player* player, bool isWanted) const
{
#if ROWOW_HAS_MOD_CITYOFHEROES
    if (player != nullptr && CityOfHeroes->IsEnabled == true)
        CoHIdentitySetLookWanted(player, isWanted);
#else
    (void)player;
    (void)isWanted;
#endif
}
