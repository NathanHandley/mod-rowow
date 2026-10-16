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
#include "LootMgr.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "Unit.h"
#include "WorldSession.h"

#include "ROWOW.h"

#include <cmath>

using namespace std;

// The parts of RO skills that WoW's spell effects can't do (from mod_rowow_skill_effect): a status that goes on by chance (with RO's
// level based chances), a zeny cost, stealing from a monster, teleporting to a random spot on the map, and ground skills that cover
// their cells one by one (Fire Wall's wall across the caster's line, Pneuma's 3x3).  Runs in the caster's map thread, and only reads
// the skill data loaded at startup
class ROWOW_SkillSpellScript : public SpellScript
{
    PrepareSpellScript(ROWOW_SkillSpellScript);

    vector<ROWOWSkillEffect> const* GetSkillEffects()
    {
        if (ROWOW->IsEnabled == false)
            return nullptr;
        return ROWOW->GetSkillEffects(GetSpellInfo()->Id);
    }

    SpellCastResult CheckZenyCost()
    {
        vector<ROWOWSkillEffect> const* skillEffects = GetSkillEffects();
        if (skillEffects == nullptr)
            return SPELL_CAST_OK;
        Player* player = GetCaster() != nullptr ? GetCaster()->ToPlayer() : nullptr;
        if (player == nullptr)
            return SPELL_CAST_OK;
        for (ROWOWSkillEffect const& skillEffect : *skillEffects)
        {
            if (skillEffect.EffectType != ROWOW_SKILL_EFFECT_ZENY_COST || skillEffect.ValueA <= 0)
                continue;
            if (player->HasEnoughMoney(uint32(skillEffect.ValueA)) == false)
            {
                ChatHandler(player->GetSession()).SendNotification("You don't have enough zeny.");
                return SPELL_FAILED_DONT_REPORT;
            }
        }
        return SPELL_CAST_OK;
    }

    // Once per cast: places the ground cells, pays the zeny, and teleports
    void HandleAfterCast()
    {
        vector<ROWOWSkillEffect> const* skillEffects = GetSkillEffects();
        if (skillEffects == nullptr)
            return;
        Unit* caster = GetCaster();
        if (caster == nullptr)
            return;
        Player* player = caster->ToPlayer();
        for (ROWOWSkillEffect const& skillEffect : *skillEffects)
        {
            if (skillEffect.EffectType == ROWOW_SKILL_EFFECT_CELL_LAYOUT && skillEffect.TriggerSpellID != 0)
                PlaceCells(caster, skillEffect.TriggerSpellID);
            else if (player == nullptr)
                continue;
            else if (skillEffect.EffectType == ROWOW_SKILL_EFFECT_ZENY_COST && skillEffect.ValueA > 0)
                player->ModifyMoney(-skillEffect.ValueA);
            else if (skillEffect.EffectType == ROWOW_SKILL_EFFECT_TELEPORT_RANDOM)
                TeleportToRandomSpot(player);
        }
    }

    // Casts the cell spell on each cell of the skill's layout around the target spot.  On an RO map the cells are RO's own (the
    // target's cell and its neighbors, with a wall turned by RO's direction from the caster's cell).  Elsewhere the same shape sits
    // at the target spot, a wall turned square across the line from the caster.  Cells with no ground, or behind a wall from the
    // target spot, are left out, as RO leaves out cells that can't be reached
    void PlaceCells(Unit* caster, uint32 cellSpellID)
    {
        ROWOWSkillCellLayout const* cellLayout = ROWOW->GetSkillCellLayout(GetSpellInfo()->Id);
        WorldLocation const* targetDest = GetExplTargetDest();
        if (cellLayout == nullptr || targetDest == nullptr)
            return;
        Map* map = caster->GetMap();
        float targetX = targetDest->GetPositionX();
        float targetY = targetDest->GetPositionY();
        float targetZ = targetDest->GetPositionZ();
        ROWOWMapGrid const* mapGrid = ROWOW->GetMapGrid(caster->GetMapId());

        // Where the layout is centered, and how its RO cell offsets (x east, y north) become WoW offsets
        float centerX = targetX;
        float centerY = targetY;
        float cellSize = ROWOW->CellSize;
        float rotationSin = 0;
        float rotationCos = 1;
        uint8 direction = 0;
        if (mapGrid != nullptr)
        {
            int32 targetCellX = mapGrid->GetCellX(targetY);
            int32 targetCellY = mapGrid->GetCellY(targetX);
            centerX = mapGrid->GetCellCenterWorldX(targetCellY);
            centerY = mapGrid->GetCellCenterWorldY(targetCellX);
            cellSize = mapGrid->CellSize;
            if (cellLayout->IsDirectional == true)
                direction = ROWOWMod::GetRODirection(float(targetCellX - mapGrid->GetCellX(caster->GetPositionY())), float(targetCellY - mapGrid->GetCellY(caster->GetPositionX())));
        }
        else if (cellLayout->IsDirectional == true)
        {
            // The north facing shape (a wall across west to east), turned to face along the caster's line to the target
            float angle = caster->GetOrientation();
            if (caster->GetExactDist2d(targetX, targetY) > 0.1f)
                angle = std::atan2(targetY - caster->GetPositionY(), targetX - caster->GetPositionX());
            rotationSin = std::sin(angle);
            rotationCos = std::cos(angle);
        }

        for (std::pair<int16, int16> const& cell : cellLayout->CellsByDirection[direction])
        {
            float forwardOffset = float(cell.second) * cellSize;
            float leftOffset = -float(cell.first) * cellSize;
            float cellX = centerX + forwardOffset * rotationCos - leftOffset * rotationSin;
            float cellY = centerY + forwardOffset * rotationSin + leftOffset * rotationCos;
            float cellZ = map->GetHeight(caster->GetPhaseMask(), cellX, cellY, targetZ + ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT, true, ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE);
            if (cellZ <= INVALID_HEIGHT)
                continue;
            if (map->isInLineOfSight(targetX, targetY, targetZ + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT, cellX, cellY, cellZ + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT,
                caster->GetPhaseMask(), LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing) == false)
                continue;
            caster->CastSpell(cellX, cellY, cellZ, cellSpellID, true);
        }
    }

    // Once per target hit: the chance statuses, and stealing
    void HandleAfterHit()
    {
        vector<ROWOWSkillEffect> const* skillEffects = GetSkillEffects();
        if (skillEffects == nullptr)
            return;
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (caster == nullptr || target == nullptr || target->IsAlive() == false)
            return;
        for (ROWOWSkillEffect const& skillEffect : *skillEffects)
        {
            if (skillEffect.EffectType == ROWOW_SKILL_EFFECT_STATUS_CHANCE && skillEffect.TriggerSpellID != 0)
            {
                float chance = skillEffect.Chance + skillEffect.ChancePerCasterLevel * float(caster->GetLevel());
                if (skillEffect.IsChanceByLevelDifference == true)
                    chance += float(int32(caster->GetLevel()) - int32(target->GetLevel()));
                if (chance > 0.0f && roll_chance_f(chance) == true)
                    caster->CastSpell(target, skillEffect.TriggerSpellID, true);
            }
            else if (skillEffect.EffectType == ROWOW_SKILL_EFFECT_STEAL)
                TrySteal(caster, target, skillEffect.Chance);
        }
    }

    // RO's Steal: one success per monster, taking an item it would drop (a normal roll of its loot)
    void TrySteal(Unit* caster, Unit* target, float chance)
    {
        Player* player = caster->ToPlayer();
        Creature* creature = target->ToCreature();
        if (player == nullptr || creature == nullptr)
            return;
        if (creature->isWorldBoss() == true || creature->IsDungeonBoss() == true || creature->IsSummon() == true || creature->GetCreatureTemplate()->lootid == 0)
        {
            ChatHandler(player->GetSession()).SendNotification("You can't steal from that.");
            return;
        }
        ROWOWStealState* stealState = creature->CustomData.GetDefault<ROWOWStealState>("ROWOWStealState");
        if (stealState->IsStolenFrom == true || roll_chance_f(chance) == false)
        {
            ChatHandler(player->GetSession()).SendNotification("Steal failed.");
            return;
        }
        Loot loot;
        loot.FillLoot(creature->GetCreatureTemplate()->lootid, LootTemplates_Creature, player, true, true, LOOT_MODE_DEFAULT, creature);
        for (LootItem const& lootItem : loot.items)
        {
            if (lootItem.itemid == 0)
                continue;
            if (player->AddItem(lootItem.itemid, 1) == true)
            {
                stealState->IsStolenFrom = true;
                return;
            }
        }
        ChatHandler(player->GetSession()).SendNotification("Steal failed.");
    }

    // RO's Teleport level 1: to a random spot on the map, here the spawn spot of a random creature on it
    void TeleportToRandomSpot(Player* player)
    {
        Map* map = player->GetMap();
        if (map == nullptr || map->Instanceable() == true)
            return;
        Map::CreatureBySpawnIdContainer const& creaturesBySpawnID = map->GetCreatureBySpawnIdStore();
        if (creaturesBySpawnID.empty() == true)
            return;
        uint32 index = urand(0, uint32(creaturesBySpawnID.size() - 1));
        Map::CreatureBySpawnIdContainer::const_iterator creatureIter = creaturesBySpawnID.begin();
        advance(creatureIter, index);
        Creature* creature = creatureIter->second;
        if (creature == nullptr)
            return;
        Position const& homePosition = creature->GetHomePosition();
        player->NearTeleportTo(homePosition.GetPositionX(), homePosition.GetPositionY(), homePosition.GetPositionZ() + 0.5f, player->GetOrientation());
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(ROWOW_SkillSpellScript::CheckZenyCost);
        AfterCast += SpellCastFn(ROWOW_SkillSpellScript::HandleAfterCast);
        AfterHit += SpellHitFn(ROWOW_SkillSpellScript::HandleAfterHit);
    }
};

void AddROWOWSkillSpellScripts()
{
    RegisterSpellScript(ROWOW_SkillSpellScript);
}
