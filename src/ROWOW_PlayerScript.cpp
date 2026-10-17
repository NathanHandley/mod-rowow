//  Author: Nathan Handley (nathanhandley@protonmail.com)
//  Copyright (c) 2026 Nathan Handley
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"

#include "ROWOW.h"

using namespace std;

class ROWOW_PlayerScript : public PlayerScript
{
public:
    ROWOW_PlayerScript() : PlayerScript("ROWOW_PlayerScript") {}

    void OnPlayerEquip(Player* player, Item* /*it*/, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        ROWOW->RefreshIllusionDisplay(player);
    }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        // Unequipping, hiding the helm and the core putting the transform's own display back (after a shapeshift, for one) have no
        // hooks, so the display is checked each update.  It's cheap: nothing happens unless an RO illusion is the transform
        ROWOW->RefreshIllusionDisplay(player);
    }
};

class ROWOW_UnitScript : public UnitScript
{
public:
    ROWOW_UnitScript() : UnitScript("ROWOW_UnitScript") {}

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        // The core has just shown the illusion's form display; show the one for the weapon and shield held instead
        if (unit == nullptr || aura == nullptr || unit->IsPlayer() == false)
            return;
        if (ROWOW->IllusionsBySpellID.find(aura->GetId()) == ROWOW->IllusionsBySpellID.end())
            return;
        ROWOW->RefreshIllusionDisplay(unit->ToPlayer());
    }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        // A creature's RO steals are for one life (its respawn can be the same creature).  Runs in the creature's map thread
        if (unit == nullptr || unit->IsCreature() == false)
            return;
        unit->ToCreature()->CustomData.Erase("ROWOWStealState");
    }
};

void AddROWOWPlayerScripts()
{
    new ROWOW_PlayerScript();
    new ROWOW_UnitScript();
}
