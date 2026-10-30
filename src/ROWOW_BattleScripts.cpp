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

#include "Chat.h"
#include "Creature.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// RO combat's hooks: a swing between RO combatants is RO's normal attack (ROWOW_Battle.cpp), an RO monster spawns with its RO HP and
// attack delay, and the form attacks with its RO weapon.  All of it runs in the map's thread (the player's own, for the auto attack)

class ROWOW_BattleUnitScript : public UnitScript
{
public:
    ROWOW_BattleUnitScript() : UnitScript("ROWOW_BattleUnitScript", true, { UNITHOOK_ON_CALCULATE_MELEE_DAMAGE }) {}

    bool OnCalculateMeleeDamage(Unit* attacker, Unit* victim, CalcDamageInfo* damageInfo) override
    {
        return ROWOW->CalculateMeleeDamage(attacker, victim, damageInfo);
    }
};

class ROWOW_BattleAllCreatureScript : public AllCreatureScript
{
public:
    ROWOW_BattleAllCreatureScript() : AllCreatureScript("ROWOW_BattleAllCreatureScript") {}

    // The end of Creature::SelectLevel (spawn, respawn and entry changes), right after the core set full health from the base stats
    void OnCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* creature) override
    {
        if (ROWOW->IsEnabled == false || creature == nullptr)
            return;
        ROWOW->ApplyMobStatus(creature);
    }
};

class ROWOW_BattlePlayerScript : public PlayerScript
{
public:
    ROWOW_BattlePlayerScript() : PlayerScript("ROWOW_BattlePlayerScript", { PLAYERHOOK_ON_BEFORE_AUTO_ATTACK }) {}

    bool OnPlayerBeforeAutoAttack(Player* player, Unit* victim, float& extraRange, bool& hasOffhandAttack) override
    {
        // The form's attack is one swing (an RO dual wield included) whatever the character holds in its off hand, and reaches as far as
        // its RO weapon does
        if (ROWOW->IsInROForm(player) == false)
            return true;
        hasOffhandAttack = false;
        extraRange = ROWOW->GetAutoAttackExtraRange(player, victim);

        // A bow or gun without its ammo stops attacking (rAthena's clif_arrow_fail)
        if (ROWOW->HasNormalAttackAmmo(player) == true)
            return true;
        player->AttackStop();
        ChatHandler(player->GetSession()).SendSysMessage("Please equip the proper ammunition first.");
        return false;
    }
};

void AddROWOWBattleScripts()
{
    new ROWOW_BattleUnitScript();
    new ROWOW_BattleAllCreatureScript();
    new ROWOW_BattlePlayerScript();
}
