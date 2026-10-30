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
//
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skill.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "CellImpl.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DynamicObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryResult.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// RO ground skills (rAthena's skill units): a ground skill puts a group of units on cells around its spot (skill_unitsetting), each a cell
// that acts on what steps in (skill_unit_onplace), stands in (skill_unit_onplace_timer, by the group's interval), steps out
// (skill_unit_onout) or leaves the skill's cells altogether (skill_unit_onleft).  A map's groups live on the map (ROWOWMapSkillUnits, rAthena's
// skillunit_group_db with the map's BL_SKILL blocks); skill_unit_timer runs from the map's update and skill_unit_move from each RO combatant's
// update as its cell changes, so all of it runs in the map's own thread.  The unit cases of rAthena's switches are ported with the jobs they
// belong to (the first jobs here; the later jobs add theirs to the same switches)

namespace
{
    int64 GetCellKey(int32 x, int32 y)
    {
        return (int64(x) << 32) | int64(uint32(y));
    }

    // The RO combatants (BL_CHAR) whose cell is in a square of cells, found around the square's middle (map_foreachinallarea without a unit at
    // its center: the skill units' own searches)
    class ROWOWCellAreaSearcher
    {
    public:
        ROWOWCellAreaSearcher(uint32 phaseMask, std::list<Unit*>& units) : PhaseMask(phaseMask), Units(units) { }

        void Visit(PlayerMapType& m)
        {
            for (PlayerMapType::iterator itr = m.begin(); itr != m.end(); ++itr)
                if (itr->GetSource()->InSamePhase(PhaseMask) == true && itr->GetSource()->IsInWorld() == true)
                    Units.push_back(itr->GetSource());
        }

        void Visit(CreatureMapType& m)
        {
            for (CreatureMapType::iterator itr = m.begin(); itr != m.end(); ++itr)
                if (itr->GetSource()->InSamePhase(PhaseMask) == true && itr->GetSource()->IsInWorld() == true)
                    Units.push_back(itr->GetSource());
        }

        template<class NOT_INTERESTED> void Visit(GridRefMgr<NOT_INTERESTED>&) { }

    private:
        uint32 PhaseMask;
        std::list<Unit*>& Units;
    };

    // The caster of a unit group, on the group's map (map_id2bl(group->src_id)), or null when it left
    Unit* GetSkillUnitGroupSource(Map* map, ObjectGuid const& srcGUID)
    {
        if (srcGUID.IsPlayer() == true)
            return ObjectAccessor::GetPlayer(map, srcGUID);
        return map->GetCreature(srcGUID);
    }

    // The dynamic object that shows a unit (or a whole group) on the ground: the skill's unit spell for as long as it lasts, without an aura
    // (the units do what the skill does, not WoW)
    ObjectGuid CreateSkillUnitVisual(Unit* src, uint32 spellID, float x, float y, float z, float radius, int64 duration)
    {
        if (spellID == 0 || src == nullptr || src->IsInWorld() == false)
            return ObjectGuid::Empty;
        Position position(x, y, z, 0);
        DynamicObject* dynObj = new DynamicObject();
        if (dynObj->CreateDynamicObject(src->GetMap()->GenerateLowGuid<HighGuid::DynamicObject>(), src, spellID, position, radius, DYNAMIC_OBJECT_AREA_SPELL) == false)
        {
            delete dynObj;
            return ObjectGuid::Empty;
        }
        dynObj->SetDuration(int32(std::clamp<int64>(duration, 1, INT32_MAX)));
        return dynObj->GetGUID();
    }

    void RemoveSkillUnitVisual(Map* map, ObjectGuid& visualGUID)
    {
        if (visualGUID.IsEmpty() == true)
            return;
        if (DynamicObject* dynObj = map->GetDynamicObject(visualGUID))
            dynObj->Remove();
        visualGUID.Clear();
    }
}

void ROWOWMod::LoadSkillUnitData()
{
    // skill_init_unit_layout: the square layouts first (1x1 to 15x15), then the layouts of the skills whose Unit Layout is -1, by skill ID,
    // which take that layout's index as their layout type.  Fire Wall's and Ice Wall's 8 layouts by direction come last
    SkillUnitLayouts.clear();
    if (IsEnabled == false)
        return;
    for (int32 i = 0; i <= ROWOW_MAX_SQUARE_LAYOUT; i++)
    {
        ROWOWSkillUnitLayout layout;
        int32 size = i * 2 + 1;
        for (int32 j = 0; j < size * size; j++)
            layout.Cells.emplace_back(int16(j % size - i), int16(j / size - i));
        SkillUnitLayouts.push_back(layout);
    }
    std::vector<uint16> skillIDs;
    for (auto const& skillByID : SkillsByID)
        skillIDs.push_back(skillByID.first);
    std::sort(skillIDs.begin(), skillIDs.end());
    for (uint16 skillID : skillIDs)
    {
        ROWOWSkill& skill = SkillsByID[skillID];
        if (skill.UnitTypeID == 0 || skill.UnitLayout[0] != -1)
            continue;
        ROWOWSkillUnitLayout layout;
        auto setCells = [&layout](std::initializer_list<int16> dx, std::initializer_list<int16> dy)
        {
            auto dyIter = dy.begin();
            for (int16 x : dx)
                layout.Cells.emplace_back(x, *dyIter++);
        };
        switch (skillID)
        {
            case ROWOW_SKILL_MG_FIREWALL:
            case ROWOW_SKILL_WZ_ICEWALL:
                // these will be handled later
                break;
            case ROWOW_SKILL_PR_SANCTUARY:
            case ROWOW_SKILL_NPC_EVILLAND:
                setCells({ -1, 0, 1, -2, -1, 0, 1, 2, -2, -1, 0, 1, 2, -2, -1, 0, 1, 2, -1, 0, 1 },
                    { -2, -2, -2, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2 });
                break;
            case ROWOW_SKILL_PR_MAGNUS:
            case ROWOW_SKILL_PA_GOSPEL:
                setCells({ -1, 0, 1, -1, 0, 1, -3, -2, -1, 0, 1, 2, 3, -3, -2, -1, 0, 1, 2, 3, -3, -2, -1, 0, 1, 2, 3, -1, 0, 1, -1, 0, 1 },
                    { -3, -3, -3, -2, -2, -2, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3 });
                break;
            case ROWOW_SKILL_AS_VENOMDUST:
                setCells({ -1, 0, 0, 0, 1 }, { 0, -1, 0, 1, 0 });
                break;
            case ROWOW_SKILL_CR_GRANDCROSS:
            case ROWOW_SKILL_NPC_GRANDDARKNESS:
                setCells({ 0, 0, -1, 0, 1, -2, -1, 0, 1, 2, -4, -3, -2, -1, 0, 1, 2, 3, 4, -2, -1, 0, 1, 2, -1, 0, 1, 0, 0 },
                    { -4, -3, -2, -2, -2, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 3, 4 });
                break;
            case ROWOW_SKILL_PF_FOGWALL:
                setCells({ -2, -1, 0, 1, 2, -2, -1, 0, 1, 2, -2, -1, 0, 1, 2 }, { -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1 });
                break;
            case ROWOW_SKILL_NJ_KAENSIN:
                setCells({ -2, -1, 0, 1, 2, -2, -1, 0, 1, 2, -2, -1, 1, 2, -2, -1, 0, 1, 2, -2, -1, 0, 1, 2 },
                    { 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0, -1, -1, -1, -1, -1, -2, -2, -2, -2, -2 });
                break;
            case ROWOW_SKILL_NJ_TATAMIGAESHI:
            {
                // Level 1 a cross of 3x3, levels 2-3 of 5x5, levels 4-5 (and on) of 7x7
                ROWOWSkillUnitLayout crossLayouts[3];
                std::initializer_list<int16> const dx1 = { -1, 1, 0, 0 }, dy1 = { 0, 0, -1, 1 };
                std::initializer_list<int16> const dx2 = { -2, -1, 1, 2, 0, 0, 0, 0 }, dy2 = { 0, 0, 0, 0, -2, -1, 1, 2 };
                std::initializer_list<int16> const dx3 = { -3, -2, -1, 1, 2, 3, 0, 0, 0, 0, 0, 0 }, dy3 = { 0, 0, 0, 0, 0, 0, -3, -2, -1, 1, 2, 3 };
                for (auto const& cross : { std::make_pair(dx1, dy1), std::make_pair(dx2, dy2), std::make_pair(dx3, dy3) })
                {
                    ROWOWSkillUnitLayout crossLayout;
                    auto dyIter = cross.second.begin();
                    for (int16 x : cross.first)
                        crossLayout.Cells.emplace_back(x, *dyIter++);
                    SkillUnitLayouts.push_back(crossLayout);
                }
                int32 firstPos = int32(SkillUnitLayouts.size()) - 3;
                for (int32 level = 0; level < ROWOW_MAX_SKILL_LEVEL; level++)
                    skill.UnitLayout[level] = firstPos + (level == 0 ? 0 : (level <= 2 ? 1 : 2));
                continue; // Skip, this way the check below will fail and continue to the next skill
            }
            default:
                LOG_ERROR("module.ROWOW", "ROWOWMod::LoadSkillUnitData found no unit layout for skill {}", skill.Name);
                break;
        }
        if (layout.Cells.empty() == true)
            continue;
        for (int32 level = 0; level < ROWOW_MAX_SKILL_LEVEL; level++)
            skill.UnitLayout[level] = int32(SkillUnitLayouts.size());
        SkillUnitLayouts.push_back(layout);
    }

    // firewall and icewall have 8 layouts (direction-dependent)
    FirewallUnitPos = int32(SkillUnitLayouts.size());
    for (int32 i = 0; i < ROWOW_RO_DIRECTION_COUNT; i++)
    {
        ROWOWSkillUnitLayout layout;
        if ((i & 1) != 0)
        {
            if ((i & 0x2) != 0)
                layout.Cells = { { -1, 1 }, { -1, 0 }, { 0, 0 }, { 0, -1 }, { 1, -1 } };
            else
                layout.Cells = { { 1, 1 }, { 1, 0 }, { 0, 0 }, { 0, -1 }, { -1, -1 } };
        }
        else if (i % 4 == 0)
            layout.Cells = { { -1, 0 }, { 0, 0 }, { 1, 0 } };
        else
            layout.Cells = { { 0, -1 }, { 0, 0 }, { 0, 1 } };
        SkillUnitLayouts.push_back(layout);
    }
    IcewallUnitPos = int32(SkillUnitLayouts.size());
    for (int32 i = 0; i < ROWOW_RO_DIRECTION_COUNT; i++)
    {
        ROWOWSkillUnitLayout layout;
        if ((i & 1) != 0)
        {
            if ((i & 0x2) != 0)
                layout.Cells = { { -2, 2 }, { -1, 1 }, { 0, 0 }, { 1, -1 }, { 2, -2 } };
            else
                layout.Cells = { { 2, 2 }, { 1, 1 }, { 0, 0 }, { -1, -1 }, { -2, -2 } };
        }
        else if (i % 4 == 0)
            layout.Cells = { { -2, 0 }, { -1, 0 }, { 0, 0 }, { 1, 0 }, { 2, 0 } };
        else
            layout.Cells = { { 0, -2 }, { 0, -1 }, { 0, 0 }, { 0, 1 }, { 0, 2 } };
        SkillUnitLayouts.push_back(layout);
    }

    // The spells that show the units on the ground (the converter makes this table, so it may not exist yet)
    uint32 unitSpellCount = 0;
    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_skill_unit_spell'");
    if (tableResult)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `SkillName`, `SkillLevel`, `UnitSpellID`, `IsPerCell`, `SpringSpellID` FROM `mod_rowow_skill_unit_spell`"))
        {
            do
            {
                Field* fields = result->Fetch();
                auto skillIter = SkillsByID.find(GetSkillIDByName(fields[0].Get<std::string>()));
                uint32 level = fields[1].Get<uint32>();
                if (skillIter == SkillsByID.end() || level == 0 || level > skillIter->second.MaxLevel)
                    continue;
                ROWOWSkill& skill = skillIter->second;
                if (skill.UnitSpellIDsByLevel.size() < level)
                {
                    skill.UnitSpellIDsByLevel.resize(level, 0);
                    skill.IsUnitSpellPerCellByLevel.resize(level, false);
                    skill.SpringSpellIDsByLevel.resize(level, 0);
                }
                skill.UnitSpellIDsByLevel[level - 1] = fields[2].Get<uint32>();
                skill.IsUnitSpellPerCellByLevel[level - 1] = fields[3].Get<uint8>() != 0;
                skill.SpringSpellIDsByLevel[level - 1] = fields[4].Get<uint32>();
                unitSpellCount++;
            } while (result->NextRow());
        }
    }
    else
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadSkillUnitData found no mod_rowow_skill_unit_spell table, so ground skills don't show on the ground (run the converter)");
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadSkillUnitData built {} unit layouts and loaded {} unit spells", SkillUnitLayouts.size(), unitSpellCount);
}

ROWOWMapSkillUnits* ROWOWMod::GetMapSkillUnits(Map* map, bool isCreated) const
{
    if (map == nullptr)
        return nullptr;
    if (isCreated == true)
        return map->CustomData.GetDefault<ROWOWMapSkillUnits>("ROWOWMapSkillUnits");
    return map->CustomData.Get<ROWOWMapSkillUnits>("ROWOWMapSkillUnits");
}

std::shared_ptr<ROWOWSkillUnitGroup> ROWOWMod::SkillID2Group(Map* map, int32 groupID) const
{
    ROWOWMapSkillUnits* mapUnits = GetMapSkillUnits(map, false);
    if (mapUnits == nullptr)
        return nullptr;
    auto groupIter = mapUnits->GroupsByID.find(groupID);
    return groupIter != mapUnits->GroupsByID.end() ? groupIter->second : nullptr;
}

bool ROWOWMod::GetCellWorldPosition(uint32 mapID, int32 cellX, int32 cellY, float& xOut, float& yOut) const
{
    // The inverse of GetWorldCell: RO's x runs against WoW's Y and RO's y along WoW's X
    if (ROWOWMapGrid const* mapGrid = GetMapGrid(mapID))
    {
        xOut = mapGrid->GetCellCenterWorldX(cellY);
        yOut = mapGrid->GetCellCenterWorldY(cellX);
        return true;
    }
    xOut = (float(cellY) + 0.5f) * CellSize;
    yOut = -(float(cellX) + 0.5f) * CellSize;
    return false;
}

uint8 ROWOWMod::UnitGetDir(Unit const* unit)
{
    // unit_getdir: the RO direction of the way the unit faces (WoW's facing is along X at 0, toward Y at pi/2; RO's x is WoW's -Y, its y WoW's X)
    float orientation = unit->GetOrientation();
    return GetRODirection(-std::sin(orientation), std::cos(orientation));
}

int32 ROWOWMod::SkillGetUnitLayoutType(uint16 skillID, uint16 skillLevel) const
{
    ROWOWSkill const* skill = GetSkill(skillID);
    return skill != nullptr ? skill->UnitLayout[ROWOWSkill::GetLevelIndex(skillLevel)] : 0;
}

ROWOWSkillUnitLayout const* ROWOWMod::SkillGetUnitLayout(uint16 skillID, uint16 skillLevel, Unit const* src, int32 x, int32 y) const
{
    // skill_get_unit_layout
    int32 pos = SkillGetUnitLayoutType(skillID, skillLevel);
    if (pos < -1 || pos >= int32(SkillUnitLayouts.size()))
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::SkillGetUnitLayout: unsupported layout type {} for skill {} (level {})", pos, skillID, skillLevel);
        pos = std::clamp(pos, 0, ROWOW_MAX_SQUARE_LAYOUT); // cap to nearest square layout
    }
    // Monsters sometimes deploy more units on level 10
    if (src->IsCreature() == true && skillLevel >= 10 && skillID == ROWOW_SKILL_WZ_WATERBALL)
        pos = 4; // 9x9 Area
    if (pos != -1) // simple single-definition layout
        return &SkillUnitLayouts[pos];

    int32 srcX = 0, srcY = 0;
    GetUnitCell(src, srcX, srcY);
    uint8 dir = (srcX == x && srcY == y) ? ROWOW_RO_DIRECTION_EAST : GetRODirection(float(x - srcX), float(y - srcY)); // 6 - default aegis direction
    if (skillID == ROWOW_SKILL_MG_FIREWALL)
        return &SkillUnitLayouts[FirewallUnitPos + dir];
    if (skillID == ROWOW_SKILL_WZ_ICEWALL)
        return &SkillUnitLayouts[IcewallUnitPos + dir];
    LOG_ERROR("module.ROWOW", "ROWOWMod::SkillGetUnitLayout: unknown unit layout for skill {} (level {})", skillID, skillLevel);
    return &SkillUnitLayouts[0]; // default 1x1 layout
}

int32 ROWOWMod::MapForEachInCellArea(Map* map, uint32 phaseMask, int32 x0, int32 y0, int32 x1, int32 y1, std::function<int32(Unit*)> const& func) const
{
    // map_foreachinallarea (BL_CHAR) around a square of cells: the RO combatants standing in it, found within reach of its middle in yards
    int32 minX = std::min(x0, x1), maxX = std::max(x0, x1), minY = std::min(y0, y1), maxY = std::max(y0, y1);
    float centerX = 0, centerY = 0;
    GetCellWorldPosition(map->GetId(), (minX + maxX) / 2, (minY + maxY) / 2, centerX, centerY);
    ROWOWMapGrid const* mapGrid = GetMapGrid(map->GetId());
    float searchRadius = float(std::max(maxX - minX, maxY - minY) / 2 + 2) * (mapGrid != nullptr ? mapGrid->CellSize : CellSize) * 1.5f + 5.0f;
    std::list<Unit*> units;
    ROWOWCellAreaSearcher searcher(phaseMask, units);
    Cell::VisitObjects(centerX, centerY, map, searcher, searchRadius);
    int32 returnCount = 0;
    for (Unit* unit : units)
    {
        if (unit->IsInWorld() == false || unit->GetMap() != map || GetROStatus(unit) == nullptr)
            continue;
        int32 unitX = 0, unitY = 0;
        GetUnitCell(unit, unitX, unitY);
        if (unitX < minX || unitX > maxX || unitY < minY || unitY > maxY)
            continue;
        returnCount += func(unit);
    }
    return returnCount;
}

int32 ROWOWMod::MapForEachSkillUnitInArea(Map* map, int32 x0, int32 y0, int32 x1, int32 y1, std::function<int32(ROWOWSkillUnitGroup&, int32)> const& func)
{
    // map_foreachinallarea (BL_SKILL): the live units on the cells of the square.  The units are gathered first, so the function may delete
    // units and groups as it goes (a deleted one is skipped)
    ROWOWMapSkillUnits* mapUnits = GetMapSkillUnits(map, false);
    if (mapUnits == nullptr || mapUnits->GroupsByID.empty() == true)
        return 0;
    std::vector<std::pair<std::shared_ptr<ROWOWSkillUnitGroup>, int32>> units;
    for (int32 x = std::min(x0, x1); x <= std::max(x0, x1); x++)
    {
        for (int32 y = std::min(y0, y1); y <= std::max(y0, y1); y++)
        {
            auto cellIter = mapUnits->UnitsByCell.find(GetCellKey(x, y));
            if (cellIter == mapUnits->UnitsByCell.end())
                continue;
            for (std::pair<int32, int32> const& unitRef : cellIter->second)
            {
                auto groupIter = mapUnits->GroupsByID.find(unitRef.first);
                if (groupIter != mapUnits->GroupsByID.end())
                    units.emplace_back(groupIter->second, unitRef.second);
            }
        }
    }
    int32 returnCount = 0;
    for (auto const& unit : units)
        if (unit.first->Units[unit.second].IsAlive == true)
            returnCount += func(*unit.first, unit.second);
    return returnCount;
}

int32 ROWOWMod::BattleCheckTargetSkillUnit(ROWOWSkillUnitGroup const& group, Map* map, Unit const* target, int32 flag) const
{
    // battle_check_target with a skill unit as the source: the group's caster's view of the target, and its own self rules
    Unit const* src = GetSkillUnitGroupSource(map, group.SrcGUID);
    if (src == nullptr)
        return 0;
    if (group.SrcGUID == target->GetGUID())
    {
        ROWOWSkill const* skill = GetSkill(group.SkillID);
        if (skill != nullptr && skill->Inf2[ROWOW_INF2_NOTARGETSELF] == true)
            return -1;
        if (skill != nullptr && skill->Inf2[ROWOW_INF2_TARGETSELF] == true)
            return 1;
    }
    return BattleCheckTarget(src, target, flag);
}

std::shared_ptr<ROWOWSkillUnitGroup> ROWOWMod::SkillUnitSetting(Unit* src, uint16 skillID, uint16 skillLevel, int32 x, int32 y, int32 flag)
{
    // skill_unitsetting: the skill's unit group, its units on the layout's cells around x,y (not on walls, nor behind them from the caster for
    // UF_PATHCHECK skills), each acting at once on what stands in it.  (The other jobs' cases of its switches are ported with them)
    ROWOWSkill const* skill = GetSkill(skillID);
    if (src == nullptr || skill == nullptr || src->IsInWorld() == false)
        return nullptr;
    Map* map = src->GetMap();
    int32 levelIndex = ROWOWSkill::GetLevelIndex(skillLevel);
    int64 limit = skill->Duration1[levelIndex]; // skill_get_time3 (no map's skill_duration)
    int32 range = skill->UnitRange[levelIndex];
    int32 interval = skill->UnitInterval;
    int32 target = skill->UnitTarget;
    ROWOWSkillUnitLayout const* layout = SkillGetUnitLayout(skillID, skillLevel, src, x, y);
    Player* sd = src->ToPlayer();
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src)); // for traps, firewall and fogwall - celest
    int32 val1 = 0, val2 = 0, val3 = 0;
    uint32 reqItem = 0;
    int32 activeFlag = 1;
    bool hidden = false; // (traps_setting is 0 off the PvP maps: no trap is hidden)

    switch (skillID)
    {
        case ROWOW_SKILL_MG_SAFETYWALL:
            val2 = skillLevel + 1;
            break;
        case ROWOW_SKILL_MG_FIREWALL:
            if (sc != nullptr && sc->HasSCE(ROWOW_SC_VIOLENTGALE) == true)
                limit = limit * 3 / 2;
            val2 = 4 + skillLevel;
            break;
        case ROWOW_SKILL_AL_WARP:
            val1 = skillLevel + 6;
            if ((flag & 1) == 0)
                limit = 2000;
            break;
    }

    // Init skill unit group
    std::shared_ptr<ROWOWSkillUnitGroup> group = SkillInitUnitGroup(src, int32(layout->Cells.size()), skillID, skillLevel, ((flag & 1) != 0 ? skill->UnitTypeID2 : skill->UnitTypeID),
        limit, interval);
    if (group == nullptr)
        return nullptr;
    group->Val1 = val1;
    group->Val2 = val2;
    group->Val3 = val3;
    group->TargetFlag = target;
    group->BLFlag = skill->UnitBLTarget;
    // Store if this skill needs to consume ammo.
    ROWOWStatus const* status = GetROStatus(src);
    group->IsAmmoConsume = sd != nullptr && status != nullptr && status->IsArrowAttack == true && skillID != ROWOW_SKILL_GS_GROUNDDRIFT && (flag & ROWOW_UNIT_NOCONSUME_AMMO) == 0;
    group->SongDance = uint8(((skill->UnitFlags[ROWOW_UF_DANCE] == true || skill->UnitFlags[ROWOW_UF_SONG] == true) ? 1 : 0) | (skill->UnitFlags[ROWOW_UF_ENSEMBLE] == true ? 2 : 0)); // Signals if this is a song/dance/duet
    group->IsGuildAura = skillID >= ROWOW_SKILL_GD_LEADERSHIP && skillID <= ROWOW_SKILL_GD_HAWKEYES;
    group->ItemID = reqItem;
    // If tick is greater than current, do not invoke onplace function just yet. [Skotlex]
    if (group->Tick > GetTick() + ROWOW_SKILLUNITTIMER_INTERVAL)
        activeFlag = 0;

    // Set skill unit
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    float referenceZ = ud->SkillZ;
    limit = group->Limit;
    for (int32 i = 0; i < int32(layout->Cells.size()); i++)
    {
        int32 ux = x + layout->Cells[i].first;
        int32 uy = y + layout->Cells[i].second;
        int32 unitVal1 = skillLevel;
        int32 unitVal2 = 0;
        int32 alive = 1;

        // don't place skill units on walls (except for songs/dances/encores): a cell with ground near the spot's height
        float cellX = 0, cellY = 0;
        GetCellWorldPosition(map->GetId(), ux, uy, cellX, cellY);
        float cellZ = map->GetHeight(src->GetPhaseMask(), cellX, cellY, referenceZ + ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT, true, ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE);
        if (group->SongDance == 0 && cellZ <= INVALID_HEIGHT)
            continue;
        if (cellZ <= INVALID_HEIGHT)
            cellZ = referenceZ;
        // no path between cell and caster (skill_wall_check)
        if (skill->UnitFlags[ROWOW_UF_PATHCHECK] == true && map->isInLineOfSight(src->GetPositionX(), src->GetPositionY(), src->GetPositionZ() + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT,
            cellX, cellY, cellZ + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT, src->GetPhaseMask(), LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing) == false)
            continue;

        switch (skillID)
        {
            // HP for Skill unit that can be damaged, see also skill_unit_ondamaged
            case ROWOW_SKILL_HT_LANDMINE:
            case ROWOW_SKILL_MA_LANDMINE:
            case ROWOW_SKILL_HT_ANKLESNARE:
            case ROWOW_SKILL_HT_SHOCKWAVE:
            case ROWOW_SKILL_HT_SANDMAN:
            case ROWOW_SKILL_MA_SANDMAN:
            case ROWOW_SKILL_HT_FLASHER:
            case ROWOW_SKILL_HT_FREEZINGTRAP:
            case ROWOW_SKILL_MA_FREEZINGTRAP:
            case ROWOW_SKILL_HT_SKIDTRAP:
            case ROWOW_SKILL_MA_SKIDTRAP:
            case ROWOW_SKILL_HT_CLAYMORETRAP:
            case ROWOW_SKILL_HT_BLASTMINE:
                unitVal1 = 3500;
                break;
            case ROWOW_SKILL_MG_FIREWALL:
            case ROWOW_SKILL_NJ_KAENSIN:
                unitVal2 = group->Val2;
                break;
            case ROWOW_SKILL_CR_GRANDCROSS:
            case ROWOW_SKILL_NPC_GRANDDARKNESS:
                unitVal1 = int32(SkillGetTime(skillID, skillLevel) / std::max(1, interval)); // Default: 950/300 = 3 hits
                break;
            default:
                if ((group->SongDance & 0x1) != 0)
                {
                    // Songs / Dances.  val1: Original skill ID (needed for dissonance overlap check); val2: Store whether this is a song/dance
                    unitVal1 = group->SkillID;
                    unitVal2 = skill->UnitFlags[ROWOW_UF_DANCE] == true ? (1 << ROWOW_UF_DANCE) : (skill->UnitFlags[ROWOW_UF_SONG] == true ? (1 << ROWOW_UF_SONG) : 0);
                }
                break;
        }
        if (skill->UnitFlags[ROWOW_UF_RANGEDSINGLEUNIT] == true && i == int32(layout->Cells.size()) / 2)
            unitVal2 |= (1 << ROWOW_UF_RANGEDSINGLEUNIT); // center.

        // Check active cell to failing or remove current unit
        MapForEachSkillUnitInArea(map, ux, uy, ux, uy, [this, map, skillID, &alive](ROWOWSkillUnitGroup& cellGroup, int32 index)
        {
            return SkillCellOverlap(map, cellGroup, index, skillID, alive);
        });
        if (alive == 0)
            continue;

        ROWOWSkillUnit* unit = SkillInitUnit(src, *group, i, ux, uy, unitVal1, unitVal2, hidden, range, limit);
        if (unit == nullptr)
            return nullptr;
        unit->Z = cellZ;
        if (skill->UnitSpellIDsByLevel.size() > size_t(levelIndex) && skill->IsUnitSpellPerCellByLevel[levelIndex] == true && unit->IsHidden == false)
            unit->VisualGUID = CreateSkillUnitVisual(src, skill->UnitSpellIDsByLevel[levelIndex], cellX, cellY, cellZ, (GetMapGrid(map->GetId()) != nullptr ?
                GetMapGrid(map->GetId())->CellSize : CellSize) * 0.5f, limit);

        // Execute on all targets standing on this cell
        if (range == 0 && activeFlag != 0)
        {
            MapForEachInCellArea(map, src->GetPhaseMask(), ux, uy, ux, uy, [this, map, &group, i](Unit* bl)
            {
                SkillUnitEffect(map, bl, *group, i, GetTick(), 1);
                return 0;
            });
        }
    }

    if (group->AliveCount == 0)
    {
        // No cells? Something that was blocked completely by Land Protector?
        SkillDelUnitGroup(map, group);
        return nullptr;
    }

    // The whole group's look, when its cells don't show one by one (over the layout and the units' reach)
    if (skill->UnitSpellIDsByLevel.size() > size_t(levelIndex) && skill->UnitSpellIDsByLevel[levelIndex] != 0 && skill->IsUnitSpellPerCellByLevel[levelIndex] == false)
    {
        float centerX = 0, centerY = 0;
        GetCellWorldPosition(map->GetId(), x, y, centerX, centerY);
        float centerZ = map->GetHeight(src->GetPhaseMask(), centerX, centerY, referenceZ + ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT, true, ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE);
        int32 extent = range;
        for (std::pair<int16, int16> const& cell : layout->Cells)
            extent = std::max({ extent, range + std::abs(int32(cell.first)), range + std::abs(int32(cell.second)) });
        group->VisualGUID = CreateSkillUnitVisual(src, skill->UnitSpellIDsByLevel[levelIndex], centerX, centerY, centerZ > INVALID_HEIGHT ? centerZ : referenceZ,
            (float(extent) + 0.5f) * (GetMapGrid(map->GetId()) != nullptr ? GetMapGrid(map->GetId())->CellSize : CellSize), limit);
    }
    return group;
}

std::shared_ptr<ROWOWSkillUnitGroup> ROWOWMod::SkillInitUnitGroup(Unit* src, int32 count, uint16 skillID, uint16 skillLevel, uint16 unitID, int64 limit, int32 interval)
{
    // skill_initunitgroup: a new group on the caster's map, which the caster remembers (ud->skillunits)
    if (skillID == 0 || skillLevel == 0 || src->IsInWorld() == false)
        return nullptr;
    ROWOWMapSkillUnits* mapUnits = GetMapSkillUnits(src->GetMap(), true);
    std::shared_ptr<ROWOWSkillUnitGroup> group = std::make_shared<ROWOWSkillUnitGroup>();
    group->SrcGUID = src->GetGUID();
    // skill_get_new_group_id: the next ID not in use on the map, which never looks like a skill ID
    do
    {
        if (mapUnits->NextGroupID < ROWOW_SKILL_UNIT_GROUP_ID_START || mapUnits->NextGroupID == INT32_MAX)
            mapUnits->NextGroupID = ROWOW_SKILL_UNIT_GROUP_ID_START;
        group->GroupID = mapUnits->NextGroupID++;
    } while (mapUnits->GroupsByID.find(group->GroupID) != mapUnits->GroupsByID.end());
    group->Units.resize(size_t(std::max(0, count)));
    group->SkillID = skillID;
    group->SkillLevel = skillLevel;
    group->UnitID = unitID;
    group->MapID = src->GetMapId();
    group->InstanceID = src->GetInstanceId();
    group->Limit = limit;
    group->Interval = interval;
    group->Tick = GetTick();
    GetSkillUnitData(src)->SkillUnitGroupIDs.push_back(group->GroupID);
    mapUnits->GroupsByID[group->GroupID] = group;
    return group;
}

ROWOWSkillUnit* ROWOWMod::SkillInitUnit(Unit* src, ROWOWSkillUnitGroup& group, int32 index, int32 x, int32 y, int32 val1, int32 val2, bool hidden, int32 range, int64 limit)
{
    // skill_initunit: a cell of the group, on the caster's map.  (The cell changes of Ice Wall, Land Protector and Basilica, and the songs'
    // dissonance check, come with their jobs)
    if (index < 0 || index >= int32(group.Units.size()))
        return nullptr;
    ROWOWMapSkillUnits* mapUnits = GetMapSkillUnits(src->GetMap(), false);
    if (mapUnits == nullptr)
        return nullptr;
    ROWOWSkillUnit& unit = group.Units[index];
    if (unit.IsAlive == false)
        group.AliveCount++;
    unit.X = x;
    unit.Y = y;
    unit.IsAlive = true;
    unit.Val1 = val1;
    unit.Val2 = val2;
    unit.IsHidden = hidden;
    unit.Range = range;
    unit.Limit = limit;
    mapUnits->UnitsByCell[GetCellKey(x, y)].emplace_back(group.GroupID, index);
    return &unit;
}

void ROWOWMod::SkillDelUnit(Map* map, ROWOWSkillUnitGroup& group, int32 index)
{
    // skill_delunit: the cell goes, acting on what stands on it as if it left (onout, then onleft), and the group with its last cell.
    // (The cell changes of Ice Wall, Land Protector and Basilica, Ankle Snare's catch and the songs' dissonance come with their jobs)
    if (index < 0 || index >= int32(group.Units.size()))
        return;
    ROWOWSkillUnit& unit = group.Units[index];
    if (unit.IsAlive == false)
        return;
    unit.IsAlive = false;
    std::shared_ptr<ROWOWSkillUnitGroup> groupRef = SkillID2Group(map, group.GroupID); // Kept while the cell's onout may delete things

    // invoke onout event
    if (unit.Range == 0)
    {
        MapForEachInCellArea(map, PHASEMASK_NORMAL, unit.X, unit.Y, unit.X, unit.Y, [this, map, &group, index](Unit* bl)
        {
            SkillUnitEffect(map, bl, group, index, GetTick(), 4);
            return 0;
        });
    }

    RemoveSkillUnitVisual(map, unit.VisualGUID);
    if (ROWOWMapSkillUnits* mapUnits = GetMapSkillUnits(map, false))
    {
        auto cellIter = mapUnits->UnitsByCell.find(GetCellKey(unit.X, unit.Y));
        if (cellIter != mapUnits->UnitsByCell.end())
        {
            std::vector<std::pair<int32, int32>>& cellUnits = cellIter->second;
            cellUnits.erase(std::remove(cellUnits.begin(), cellUnits.end(), std::make_pair(group.GroupID, index)), cellUnits.end());
            if (cellUnits.empty() == true)
                mapUnits->UnitsByCell.erase(cellIter);
        }
    }
    if (--group.AliveCount == 0 && groupRef != nullptr)
        SkillDelUnitGroup(map, groupRef);
}

void ROWOWMod::SkillDelUnitGroup(Map* map, std::shared_ptr<ROWOWSkillUnitGroup> group)
{
    // skill_delunitgroup: every cell of the group goes, then the group.  (A song's saved replay, Spider Web's and the third jobs' statuses
    // come with their jobs)
    if (group == nullptr || map == nullptr)
        return;
    ROWOWMapSkillUnits* mapUnits = GetMapSkillUnits(map, false);
    if (mapUnits == nullptr || mapUnits->GroupsByID.find(group->GroupID) == mapUnits->GroupsByID.end())
        return;
    Unit* src = GetSkillUnitGroupSource(map, group->SrcGUID);
    ROWOWSkill const* skill = GetSkill(group->SkillID);
    if (src != nullptr)
    {
        if (skill != nullptr && (skill->UnitFlags[ROWOW_UF_DANCE] == true || skill->UnitFlags[ROWOW_UF_SONG] == true || skill->UnitFlags[ROWOW_UF_ENSEMBLE] == true))
        {
            if (ROWOWStatusChange* sc = GetStatusChange(src))
            {
                if (ROWOWStatusChangeEntry* dancing = sc->GetSCE(ROWOW_SC_DANCING))
                {
                    dancing->Val2 = 0; // This prevents status_change_end attempting to redelete the group. [Skotlex]
                    StatusChangeEnd(src, ROWOW_SC_DANCING);
                }
            }
        }

        // End SC from the master when the skill group is deleted
        int32 type = ROWOW_SC_NONE;
        switch (group->UnitID)
        {
            case ROWOW_UNT_GOSPEL: type = ROWOW_SC_GOSPEL; break;
            case ROWOW_UNT_BASILICA: type = ROWOW_SC_BASILICA; break;
        }
        if (type != ROWOW_SC_NONE)
        {
            if (ROWOWStatusChange* sc = GetStatusChange(src))
            {
                if (ROWOWStatusChangeEntry* sce = sc->GetSCE(type))
                {
                    sce->Val3 = 0; // Remove reference to this group. [Skotlex]
                    StatusChangeEnd(src, type);
                }
            }
        }

        if (Player* sd = src->ToPlayer(); sd != nullptr && group->IsAmmoConsume == true)
            BattleConsumeAmmo(sd, group->SkillID, group->SkillLevel);
    }

    // remove all unit cells (alive_count is never 0 again, so the last skill_delunit doesn't come back here)
    group->AliveCount = 0;
    for (int32 j = 0; j < int32(group->Units.size()); j++)
        SkillDelUnit(map, *group, j);
    RemoveSkillUnitVisual(map, group->VisualGUID);

    int32 linkGroupID = group->LinkGroupID;
    mapUnits->GroupsByID.erase(group->GroupID);
    if (src != nullptr)
    {
        std::vector<int32>& groupIDs = GetSkillUnitData(src)->SkillUnitGroupIDs;
        groupIDs.erase(std::remove(groupIDs.begin(), groupIDs.end(), group->GroupID), groupIDs.end());
    }
    if (linkGroupID != 0)
        if (std::shared_ptr<ROWOWSkillUnitGroup> linkGroup = SkillID2Group(map, linkGroupID))
            SkillDelUnitGroup(map, linkGroup);
}

void ROWOWMod::SkillClearUnitGroup(Unit* src)
{
    // skill_clear_unitgroup: every group of the unit (it leaves its map: logout or warp)
    ROWOWSkillUnitData* ud = src->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
    if (ud == nullptr || src->IsInWorld() == false)
        return;
    std::vector<int32> groupIDs = ud->SkillUnitGroupIDs;
    for (int32 groupID : groupIDs)
        SkillDelUnitGroup(src->GetMap(), SkillID2Group(src->GetMap(), groupID));
    ud->SkillUnitGroupIDs.clear();
}

ROWOWSkillUnitGroupTickset* ROWOWMod::SkillUnitGroupTicksetSearch(Unit* bl, ROWOWSkillUnitGroup const& group, uint64 tick)
{
    // skill_unitgrouptickset_search: when the group (or, for UF_NOOVERLAP, any group of its skill) may next hit the unit
    if (group.Interval == -1)
        return nullptr;
    ROWOWSkillUnitData* ud = GetSkillUnitData(bl);
    ROWOWSkill const* skill = GetSkill(group.SkillID);
    int32 id = (skill != nullptr && skill->UnitFlags[ROWOW_UF_NOOVERLAP] == true) ? group.SkillID : group.GroupID;
    int32 s = id;
    int32 j = -1;
    for (int32 i = 0; i < ROWOW_MAX_SKILLUNITGROUPTICKSET; i++)
    {
        int32 k = (i + s) % ROWOW_MAX_SKILLUNITGROUPTICKSET;
        if (ud->SkillUnitTicks[k].ID == id)
            return &ud->SkillUnitTicks[k];
        else if (j == -1 && (tick > ud->SkillUnitTicks[k].Tick || ud->SkillUnitTicks[k].ID == 0))
            j = k;
    }
    if (j == -1)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::SkillUnitGroupTicksetSearch: tickset is full");
        j = id % ROWOW_MAX_SKILLUNITGROUPTICKSET;
    }
    ud->SkillUnitTicks[j].ID = id;
    ud->SkillUnitTicks[j].Tick = tick;
    return &ud->SkillUnitTicks[j];
}

int32 ROWOWMod::SkillUnitOnPlace(Map* map, ROWOWSkillUnitGroup& group, int32 index, Unit* bl, uint64 tick)
{
    // skill_unit_onplace: what stepping into the unit's cell (or the cell being placed under it) does.  (Land Protector and Maelstrom's
    // cells, Hovering, and the other jobs' units come with them)
    ROWOWSkillUnit& unit = group.Units[index];
    if (bl->IsInWorld() == false || unit.IsAlive == false || bl->IsAlive() == false)
        return 0;
    Unit* ss = GetSkillUnitGroupSource(map, group.SrcGUID);
    ROWOWSkill const* skill = GetSkill(group.SkillID);
    if (ss == nullptr || skill == nullptr)
        return 0;
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(bl));
    if (sc != nullptr && (sc->Option & ROWOW_OPTION_HIDE) != 0 && skill->Inf2[ROWOW_INF2_TARGETHIDDEN] == false)
        return 0; // Hidden characters are immune to AoE skills except to these. [Skotlex]
    int32 type = skill->StatusID;
    ROWOWStatusChangeEntry const* sce = (sc != nullptr && type != ROWOW_SC_NONE) ? sc->GetSCE(type) : nullptr;
    uint16 skillID = group.SkillID; // In case the group is deleted, we need to return the correct skill id, still.
    switch (group.UnitID)
    {
        case ROWOW_UNT_SAFETYWALL:
            if (sce == nullptr)
                SCStart4(ss, bl, type, 100, group.SkillLevel, group.SkillID, group.GroupID, 0, group.Limit);
            break;

        case ROWOW_UNT_PNEUMA:
            if (sce == nullptr)
                SCStart4(ss, bl, type, 100, group.SkillLevel, group.GroupID, 0, 0, group.Limit);
            break;

        case ROWOW_UNT_WARP_WAITING:
        {
            // A player walking onto the portal goes through (rAthena also needs it to be the cell it walked to, which the server doesn't know
            // here), a monster only with mob_warp's 2
            int32 working = group.Val1 & 0xFFFF;
            if (Player* sd = bl->ToPlayer(); sd != nullptr && working == 0)
            {
                int32 x = group.Val2 >> 16;
                int32 y = group.Val2 & 0xFFFF;
                int32 count = group.Val1 >> 16;
                uint32 m = uint32(group.Val3);
                int32 sdX = 0, sdY = 0;
                GetUnitCell(sd, sdX, sdY);
                if (--count <= 0)
                    SkillDelUnitGroup(map, SkillID2Group(map, group.GroupID));
                if (m == sd->GetMapId() && x == sdX && y == sdY)
                    working = 1; /* we break it because officials break it, lovely stuff. */
                group.Val1 = (count << 16) | working;
                ROWOWPosition destination;
                if (GetCellPosition(m, x, y, destination) == true)
                    sd->TeleportTo(destination.MapID, destination.X, destination.Y, destination.Z, sd->GetOrientation());
            }
            break;
        }
    }
    return skillID;
}

int32 ROWOWMod::SkillUnitOnPlaceTimer(Map* map, ROWOWSkillUnitGroup& group, int32 index, Unit* bl, uint64 tick)
{
    // skill_unit_onplace_timer: each interval (the group's), what standing in the unit's reach does.  (The other jobs' units, the Storm
    // Gust counter, Siren's Voice and Hovering come with them)
    ROWOWSkillUnit& unit = group.Units[index];
    if (bl->IsInWorld() == false || unit.IsAlive == false || bl->IsAlive() == false)
        return 0;
    Unit* ss = GetSkillUnitGroupSource(map, group.SrcGUID);
    if (ss == nullptr)
        return 0;
    uint16 skillID = group.SkillID;
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr)
        return 0;
    if (group.Interval == -1)
    {
        switch (group.UnitID)
        {
            case ROWOW_UNT_ANKLESNARE: // These happen when a trap is splash-triggered by multiple targets on the same cell.
            case ROWOW_UNT_FIREPILLAR_ACTIVE:
            case ROWOW_UNT_ELECTRICSHOCKER:
            case ROWOW_UNT_MANHOLE:
                return 0;
            default:
                // Dissonance effect might have killed another bard/dancer and skill returned to normal
                if ((group.SongDance & 0x1) == 0)
                    LOG_ERROR("module.ROWOW", "ROWOWMod::SkillUnitOnPlaceTimer: interval error (unit id {:x})", group.UnitID);
                return 0;
        }
    }
    if (ROWOWSkillUnitGroupTickset* ts = SkillUnitGroupTicksetSearch(bl, group, tick))
    {
        // Not all have it, eg: Traps don't have it even though they can be hit by Heaven's Drive [Skotlex]
        if (tick < ts->Tick)
            return 0;
        ts->Tick = tick + uint64(group.Interval);
    }

    switch (group.UnitID)
    {
        // Units that deals simple attack
        case ROWOW_UNT_GRAVITATION:
        case ROWOW_UNT_MAKIBISHI:
            SkillAttack(skill->AttackType, ss, ss, bl, skillID, group.SkillLevel, tick, 0, &unit);
            break;

        case ROWOW_UNT_DUMMYSKILL:
            // (Star Gladiator's warmth, the Storm Gust counter, Desperado and the NPC skills come with their jobs.)  A Grand Cross cell hits
            // as many times as its val1 (its caster too), and the unit timer takes the spent cell away
            if ((skillID == ROWOW_SKILL_CR_GRANDCROSS || skillID == ROWOW_SKILL_NPC_GRANDDARKNESS) && BattleConfig.GxAllHit == 0)
                unit.Val1--;
            SkillAttack(skill->AttackType, ss, ss, bl, skillID, group.SkillLevel, tick, 0, &unit);
            break;

        case ROWOW_UNT_GOSPEL:
        {
            // Battle Chant: half the time and more (50% + 5% a level) each interval, a random blessing on a party member standing in it (not
            // the caster), or a random curse on an enemy.  (The client's Gospel messages, clif_gospel_info, aren't shown)
            if (int32(urand(0, 99)) >= 50 + group.SkillLevel * 5 || ss == bl)
                break;
            if (BattleCheckTarget(ss, bl, ROWOW_BCT_PARTY) > 0)
            {
                // Support Effect only on party, not guild
                int64 time = SkillGetTime2(skillID, group.SkillLevel); // Duration
                switch (urand(0, 12)) // Positive buff count
                {
                    case 0: // Heal 1000~9999 HP
                        StatusHeal(bl, int64(urand(0, 8999)) + 1000, 0, 0, ss, GetSkillSpellID(ROWOW_SKILL_AL_HEAL, 1));
                        break;
                    case 1: // End all negative status
                        StatusChangeClearBuffs(bl, ROWOW_SCCB_DEBUFFS | ROWOW_SCCB_REFRESH);
                        break;
                    case 2: // Immunity to all status
                        SCStart(ss, bl, ROWOW_SC_SCRESIST, 100, 100, time);
                        break;
                    case 3: // MaxHP +100%
                        SCStart(ss, bl, ROWOW_SC_INCMHPRATE, 100, 100, time);
                        break;
                    case 4: // MaxSP +100%
                        SCStart(ss, bl, ROWOW_SC_INCMSPRATE, 100, 100, time);
                        break;
                    case 5: // All stats +20
                        SCStart(ss, bl, ROWOW_SC_INCALLSTATUS, 100, 20, time);
                        break;
                    case 6: // Level 10 Blessing
                        SCStart(ss, bl, ROWOW_SC_BLESSING, 100, 10, SkillGetTime(ROWOW_SKILL_AL_BLESSING, 10));
                        break;
                    case 7: // Level 10 Increase AGI
                        SCStart(ss, bl, ROWOW_SC_INCREASEAGI, 100, 10, SkillGetTime(ROWOW_SKILL_AL_INCAGI, 10));
                        break;
                    case 8: // Enchant weapon with Holy element
                        SCStart(ss, bl, ROWOW_SC_ASPERSIO, 100, 1, time);
                        break;
                    case 9: // Enchant armor with Holy element
                        SCStart(ss, bl, ROWOW_SC_BENEDICTIO, 100, 1, time);
                        break;
                    case 10: // DEF +25%
                        SCStart(ss, bl, ROWOW_SC_INCDEFRATE, 100, 25, 10000); // 10 seconds
                        break;
                    case 11: // ATK +100%
                        SCStart(ss, bl, ROWOW_SC_INCATKRATE, 100, 100, time);
                        break;
                    case 12: // HIT/Flee +50
                        SCStart(ss, bl, ROWOW_SC_INCHIT, 100, 50, time);
                        SCStart(ss, bl, ROWOW_SC_INCFLEE, 100, 50, time);
                        break;
                }
            }
            else if (BattleCheckTargetSkillUnit(group, map, bl, ROWOW_BCT_ENEMY) > 0)
            {
                // Offensive Effect
                int32 i = int32(urand(0, 9)); // Negative buff count
                switch (i)
                {
                    case 0: // Deal 3000~7999 damage reduced by DEF
                    case 1: // Deal 1500~5499 damage unreducable
                        SkillAttack(ROWOW_BF_MISC, ss, ss, bl, skillID, group.SkillLevel, tick, i, &unit);
                        break;
                    case 2: // Curse
                        SCStart(ss, bl, ROWOW_SC_CURSE, 100, 1, 1800000); // 30 minutes
                        break;
                    case 3: // Blind
                        SCStart(ss, bl, ROWOW_SC_BLIND, 100, 1, 1800000); // 30 minutes
                        break;
                    case 4: // Poison
                        SCStart2(ss, bl, ROWOW_SC_POISON, 100, 1, int32(ss->GetGUID().GetCounter()), 1800000); // 30 minutes
                        break;
                    case 5: // Level 10 Provoke
                        SCStart(ss, bl, ROWOW_SC_PROVOKE, 100, 10, ROWOW_INFINITE_TICK); // Infinite
                        break;
                    case 6: // DEF -100%
                        SCStart(ss, bl, ROWOW_SC_INCDEFRATE, 100, -100, 20000); // 20 seconds
                        break;
                    case 7: // ATK -100%
                        SCStart(ss, bl, ROWOW_SC_INCATKRATE, 100, -100, 20000); // 20 seconds
                        break;
                    case 8: // Flee -100%
                        SCStart(ss, bl, ROWOW_SC_INCFLEERATE, 100, -100, 20000); // 20 seconds
                        break;
                    case 9: // Speed/ASPD -25%
                        SCStart4(ss, bl, ROWOW_SC_GOSPEL, 100, 1, 0, 0, ROWOW_BCT_ENEMY, 20000); // 20 seconds
                        break;
                }
            }
            break;
        }

        case ROWOW_UNT_FIREWALL:
        case ROWOW_UNT_KAEN:
        {
            // Take into account these hit more times than the timer interval can handle.  A hit that knocks the unit back moves it off the
            // cell (rAthena's x == bl->x && y == bl->y): the slide takes a moment here, so a knockback started by the hit ends the hits too
            int32 count = 0;
            int32 x = 0, y = 0;
            GetUnitCell(bl, x, y);
            ROWOWSkillUnitData const* targetUD = GetSkillUnitData(bl);
            uint64 blownTick = targetUD->SkillBlownTick;
            int32 cellX = 0, cellY = 0;
            do
            {
                SkillAttack(ROWOW_BF_MAGIC, ss, ss, bl, skillID, group.SkillLevel, tick + uint64(count) * uint64(std::max(0, group.Interval)), 0, &unit);
                GetUnitCell(bl, cellX, cellY);
            } while (group.Interval > 0 && --unit.Val2 != 0 && x == cellX && y == cellY && targetUD->SkillBlownTick == blownTick &&
                ++count < ROWOW_SKILLUNITTIMER_INTERVAL / group.Interval && bl->IsAlive() == true);
            if (unit.Val2 <= 0)
                SkillDelUnit(map, group, index);
            break;
        }
    }
    return skillID;
}

int32 ROWOWMod::SkillUnitOnOut(ROWOWSkillUnitGroup& group, int32 /*index*/, Unit* bl, uint64 /*tick*/)
{
    // skill_unit_onout: what stepping out of the unit's cell does.  (The songs' and Basilica's cases come with their jobs)
    ROWOWSkill const* skill = GetSkill(group.SkillID);
    int32 type = skill != nullptr ? skill->StatusID : ROWOW_SC_NONE;
    ROWOWStatusChangeEntry const* sce = type != ROWOW_SC_NONE ? GetSCE(bl, type) : nullptr;
    if (bl->IsInWorld() == false || (bl->IsAlive() == false && group.UnitID != ROWOW_UNT_ANKLESNARE)) // Need to delete the trap if the source died.
        return 0;
    switch (group.UnitID)
    {
        case ROWOW_UNT_SAFETYWALL:
        case ROWOW_UNT_PNEUMA:
            if (sce != nullptr)
                StatusChangeEnd(bl, type);
            break;
    }
    return group.SkillID;
}

int32 ROWOWMod::SkillUnitOnLeft(uint16 skillID, Unit* bl, uint64 /*tick*/)
{
    // skill_unit_onleft: what leaving every cell of a skill does (the char stepped out of its last one).  (The songs', ensembles' and the
    // other jobs' cases come with them)
    ROWOWSkill const* skill = GetSkill(skillID);
    int32 type = skill != nullptr ? skill->StatusID : ROWOW_SC_NONE;
    ROWOWStatusChangeEntry const* sce = type != ROWOW_SC_NONE ? GetSCE(bl, type) : nullptr;
    switch (skillID)
    {
        case ROWOW_SKILL_MG_SAFETYWALL:
        case ROWOW_SKILL_AL_PNEUMA:
            if (sce != nullptr)
                StatusChangeEnd(bl, type);
            break;
    }
    return skillID;
}

void ROWOWMod::SkillUnitEffect(Map* map, Unit* bl, ROWOWSkillUnitGroup& group, int32 index, uint64 tick, int32 flag)
{
    // skill_unit_effect: a unit cell placed (flag&1: onplace) or removed (onout, and flag&4 an onleft).  (The songs' dissonance switch,
    // flag&8, comes with the Bard and Dancer)
    ROWOWSkillUnit const& unit = group.Units[index];
    if ((unit.IsAlive == false && (flag & 4) == 0) || bl->IsInWorld() == false)
        return;
    // Target-type check.
    ROWOWStatus const* status = GetROStatus(bl);
    bool isTarget = status != nullptr && (group.BLFlag & GetBattleObjectType(*status)) != 0 && BattleCheckTargetSkillUnit(group, map, bl, group.TargetFlag) > 0;
    // Necessary in case the group is deleted after calling on_place/on_out [Skotlex]
    uint16 skillID = group.SkillID;
    if (isTarget == true)
    {
        if ((flag & 1) != 0)
            SkillUnitOnPlace(map, group, index, bl, tick);
        else if (SkillUnitOnOut(group, index, bl, tick) == -1)
            return; // Don't let a Bard/Dancer update their own song timer
        if ((flag & 4) != 0)
            SkillUnitOnLeft(skillID, bl, tick);
    }
    else if ((flag & 4) != 0 && ((group.SongDance & 0x1) != 0 || (group.SrcGUID == bl->GetGUID() && (group.SongDance & 0x2) != 0)))
        SkillUnitOnLeft(skillID, bl, tick); // Ensemble check to terminate it.
}

void ROWOWMod::SkillUnitTimer(Map* map, uint32 /*diff*/)
{
    // skill_unit_timer: every SKILLUNITTIMER_INTERVAL ms of game time, each unit of the map expires or acts on what stands in its reach
    // (skill_unit_timer_sub).  It runs with the tick it was due at, as rAthena's timer does, so a unit made between two runs never looks
    // older than its age.  A group whose caster left the map goes (skill_clear_unitgroup on unit_remove_map)
    ROWOWMapSkillUnits* mapUnits = GetMapSkillUnits(map, false);
    if (mapUnits == nullptr)
        return;
    uint64 now = GetTick();
    if (mapUnits->NextTimerTick == 0 || mapUnits->GroupsByID.empty() == true)
    {
        mapUnits->NextTimerTick = now + ROWOW_SKILLUNITTIMER_INTERVAL;
        return;
    }
    if (now < mapUnits->NextTimerTick)
        return;
    uint64 tick = mapUnits->NextTimerTick;
    mapUnits->NextTimerTick = std::max(tick + ROWOW_SKILLUNITTIMER_INTERVAL, now - std::min<uint64>(now, ROWOW_SKILLUNITTIMER_INTERVAL) + 1);

    std::vector<std::shared_ptr<ROWOWSkillUnitGroup>> groups;
    for (auto const& groupByID : mapUnits->GroupsByID)
        groups.push_back(groupByID.second);
    for (std::shared_ptr<ROWOWSkillUnitGroup> const& group : groups)
    {
        if (mapUnits->GroupsByID.find(group->GroupID) == mapUnits->GroupsByID.end())
            continue; // Deleted by another group's unit
        if (GetSkillUnitGroupSource(map, group->SrcGUID) == nullptr)
        {
            SkillDelUnitGroup(map, group);
            continue;
        }
        for (int32 index = 0; index < int32(group->Units.size()); index++)
        {
            if (mapUnits->GroupsByID.find(group->GroupID) == mapUnits->GroupsByID.end())
                break;
            ROWOWSkillUnit& unit = group->Units[index];
            if (unit.IsAlive == false)
                continue;

            // skill_unit_timer_sub: check for expiration
            int64 age = int64(tick) - int64(group->Tick);
            if (group->IsGuildAura == false && (age >= group->Limit || age >= unit.Limit))
            {
                // skill unit expired (inlined from skill_unit_onlimit())
                switch (group->UnitID)
                {
                    case ROWOW_UNT_WARP_ACTIVE:
                    {
                        // warp portal opens (morph to a UNT_WARP_WAITING cell)
                        ROWOWSkill const* skill = GetSkill(group->SkillID);
                        group->UnitID = skill != nullptr ? skill->UnitTypeID2 : ROWOW_UNT_WARP_WAITING; // UNT_WARP_WAITING
                        // restart timers
                        group->Limit = SkillGetTime(group->SkillID, group->SkillLevel);
                        unit.Limit = group->Limit;
                        if (DynamicObject* dynObj = unit.VisualGUID.IsEmpty() == false ? map->GetDynamicObject(unit.VisualGUID) : nullptr)
                            dynObj->SetDuration(int32(std::clamp<int64>(group->Limit - age, 1, INT32_MAX)));
                        if (DynamicObject* dynObj = group->VisualGUID.IsEmpty() == false ? map->GetDynamicObject(group->VisualGUID) : nullptr)
                            dynObj->SetDuration(int32(std::clamp<int64>(group->Limit - age, 1, INT32_MAX)));
                        // apply effect to all units standing on it
                        MapForEachInCellArea(map, PHASEMASK_NORMAL, unit.X, unit.Y, unit.X, unit.Y, [this, map, &group, index](Unit* bl)
                        {
                            SkillUnitEffect(map, bl, *group, index, GetTick(), 1);
                            return 0;
                        });
                        break;
                    }
                    default:
                        SkillDelUnit(map, *group, index);
                        break;
                }
            }
            // (Still active: the traps' and the other jobs' cases come with them)

            // Don't continue if unit or even group is expired and has been deleted.
            if (unit.IsAlive == false || mapUnits->GroupsByID.find(group->GroupID) == mapUnits->GroupsByID.end())
                continue;
            if (unit.Range >= 0 && group->Interval != -1)
            {
                // skill_unit_timer_sub_onplace on every char in the unit's reach.  (Land Protector's cells come with the Sage)
                ROWOWSkillUnitGroup& unitGroup = *group;
                MapForEachInCellArea(map, PHASEMASK_NORMAL, unit.X - unit.Range, unit.Y - unit.Range, unit.X + unit.Range, unit.Y + unit.Range,
                    [this, map, &unitGroup, index, tick](Unit* bl)
                {
                    ROWOWStatus const* status = GetROStatus(bl);
                    if (unitGroup.Units[index].IsAlive == false || status == nullptr || (unitGroup.BLFlag & GetBattleObjectType(*status)) == 0)
                        return 0;
                    if (BattleCheckTargetSkillUnit(unitGroup, map, bl, unitGroup.TargetFlag) <= 0)
                        return 0;
                    SkillUnitOnPlaceTimer(map, unitGroup, index, bl, tick);
                    return 1;
                });
                if (unit.Range == -1) // Unit disabled, but it should not be deleted yet.
                    group->UnitID = ROWOW_UNT_USED_TRAPS;
                else if ((group->SkillID == ROWOW_SKILL_CR_GRANDCROSS || group->SkillID == ROWOW_SKILL_NPC_GRANDDARKNESS) && unit.Val1 <= 0)
                    SkillDelUnit(map, *group, index); // (The meteor skills that go after one strike come with their jobs)
            }
        }
    }
}

void ROWOWMod::SkillUnitMove(Unit* bl, uint64 tick, int32 flag, int32 cellX, int32 cellY, std::vector<uint16>& skillUnitCell)
{
    // skill_unit_move: a char placed on a cell (flag&1: onplace, else onout), with flag&2 when it moves (the onout keeps the skills it left
    // in skillUnitCell, the onplace forgets those it's still in and calls onleft for the rest), and flag&4 forcing onleft (death)
    if (bl->IsInWorld() == false)
        return;
    Map* map = bl->GetMap();
    if ((flag & 2) != 0 && (flag & 1) == 0) // Onout, clear data
        skillUnitCell.clear();
    MapForEachSkillUnitInArea(map, cellX, cellY, cellX, cellY, [this, map, bl, tick, flag, &skillUnitCell](ROWOWSkillUnitGroup& group, int32 index)
    {
        // skill_unit_move_sub
        if ((flag & 1) != 0 && group.SkillID == ROWOW_SKILL_PF_SPIDERWEB)
            return 0; // Fiberlock is never supposed to trigger on skill_unit_move. [Inkfish]
        // Necessary in case the group is deleted after calling on_place/on_out [Skotlex]
        uint16 skillID = group.SkillID;
        ROWOWSkill const* skill = GetSkill(skillID);
        if (group.Interval != -1 && (skill == nullptr || skill->UnitFlags[ROWOW_UF_DUALMODE] == false) && skillID != ROWOW_SKILL_BD_LULLABY) // Lullaby is the exception, bugreport:411
            return 0; // Non-dualmode unit skills with a timer don't trigger when walking, so just return

        // Target-type check.
        ROWOWStatus const* status = GetROStatus(bl);
        if (status == nullptr || (group.BLFlag & GetBattleObjectType(*status)) == 0 || BattleCheckTargetSkillUnit(group, map, bl, group.TargetFlag) <= 0)
        {
            // Ensemble check to see if the caster or the partner went out/in of the area
            ROWOWStatusChangeEntry const* dancing = GetSCE(bl, ROWOW_SC_DANCING);
            if ((group.SongDance & 0x2) != 0 && (group.SrcGUID == bl->GetGUID() || (dancing != nullptr && int32(group.SrcGUID.GetCounter()) == dancing->Val4)))
            {
                if ((flag & 1) != 0)
                {
                    if ((flag & 2) != 0) // Clear this skill id.
                        skillUnitCell.erase(std::remove(skillUnitCell.begin(), skillUnitCell.end(), skillID), skillUnitCell.end());
                }
                else if ((flag & 2) != 0) // Store this skill id.
                    skillUnitCell.push_back(skillID);
                if ((flag & 4) != 0)
                    SkillUnitOnLeft(skillID, bl, tick);
            }
            return 0;
        }
        if ((flag & 1) != 0)
        {
            int32 result = SkillUnitOnPlace(map, group, index, bl, tick);
            if ((flag & 2) != 0 && result > 0) // Clear skill ids we have stored in onout.
                skillUnitCell.erase(std::remove(skillUnitCell.begin(), skillUnitCell.end(), uint16(result)), skillUnitCell.end());
        }
        else
        {
            int32 result = SkillUnitOnOut(group, index, bl, tick);
            if ((flag & 2) != 0 && result > 0) // Store this unit id.
                skillUnitCell.push_back(skillID);
        }
        if ((flag & 4) != 0)
            SkillUnitOnLeft(skillID, bl, tick);
        return 1;
    });

    if ((flag & 2) != 0 && (flag & 1) != 0)
    {
        // Onplace, check any skill units you have left.
        for (uint16 skillID : skillUnitCell)
            SkillUnitOnLeft(skillID, bl, tick);
    }
}

void ROWOWMod::UpdateSkillUnitMove(Unit* unit)
{
    // map_moveblock's skill_unit_move calls: when the char's cell changes (walking, a knockback, a near teleport), the units of the cell it
    // left get its onout and those of the new cell its onplace; arriving on the map is an onplace.  An invincible player is placed when its
    // invincibility ends.  Only on maps with units
    if (unit->IsInWorld() == false || GetROStatus(unit) == nullptr)
        return;
    ROWOWMapSkillUnits* mapUnits = GetMapSkillUnits(unit->GetMap(), false);
    ROWOWSkillUnitData* ud = unit->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
    if (mapUnits == nullptr || mapUnits->GroupsByID.empty() == true)
    {
        if (ud != nullptr)
            ud->IsSkillUnitCellSet = false;
        return;
    }
    if (ud == nullptr)
        ud = GetSkillUnitData(unit);
    int32 x = 0, y = 0;
    GetUnitCell(unit, x, y);
    uint64 tick = GetTick();
    std::vector<uint16> skillUnitCell;
    if (ud->IsSkillUnitCellSet == false || ud->SkillUnitMapID != unit->GetMapId() || ud->SkillUnitInstanceID != unit->GetInstanceId())
    {
        ud->IsSkillUnitCellSet = true;
        ud->SkillUnitMapID = unit->GetMapId();
        ud->SkillUnitInstanceID = unit->GetInstanceId();
        ud->SkillUnitCellX = x;
        ud->SkillUnitCellY = y;
        SkillUnitMove(unit, tick, 1, x, y, skillUnitCell);
        return;
    }
    if (ud->SkillUnitCellX == x && ud->SkillUnitCellY == y)
        return;
    SkillUnitMove(unit, tick, 2, ud->SkillUnitCellX, ud->SkillUnitCellY, skillUnitCell);
    ud->SkillUnitCellX = x;
    ud->SkillUnitCellY = y;
    SkillUnitMove(unit, tick, 3, x, y, skillUnitCell);
}

int32 ROWOWMod::SkillCellOverlap(Map* map, ROWOWSkillUnitGroup& group, int32 index, uint16 skillID, int32& alive)
{
    // skill_cell_overlap: a new unit of skillID on a cell where this unit is: whether it can go there (alive), and what it does to this one
    if (alive == 0 || group.IsGuildAura == true) /* guild auras are not cancelled! */
        return 0;
    ROWOWSkill const* groupSkill = GetSkill(group.SkillID);
    ROWOWSkill const* skill = GetSkill(skillID);
    if (groupSkill == nullptr || skill == nullptr)
        return 0;
    ROWOWSkillUnit const& unit = group.Units[index];
    switch (skillID)
    {
        case ROWOW_SKILL_SA_LANDPROTECTOR:
        {
            if (group.SkillID == ROWOW_SKILL_SA_LANDPROTECTOR)
            {
                // Check for offensive Land Protector to delete both. [Skotlex]
                alive = 0;
                SkillDelUnit(map, group, index);
                return 1;
            }
            // It deletes everything except traps and barriers
            if ((groupSkill->Inf2[ROWOW_INF2_ISTRAP] == false && groupSkill->Inf2[ROWOW_INF2_IGNORELANDPROTECTOR] == false) || group.SkillID == ROWOW_SKILL_WZ_FIREPILLAR)
            {
                if (groupSkill->UnitFlags[ROWOW_UF_RANGEDSINGLEUNIT] == true)
                {
                    if ((unit.Val2 & (1 << ROWOW_UF_RANGEDSINGLEUNIT)) != 0)
                        SkillDelUnitGroup(map, SkillID2Group(map, group.GroupID));
                }
                else
                    SkillDelUnit(map, group, index);
                return 1;
            }
            break;
        }
        case ROWOW_SKILL_HW_GANBANTEIN:
            // Officially songs/dances are removed
            if (groupSkill->UnitFlags[ROWOW_UF_RANGEDSINGLEUNIT] == true)
            {
                if ((unit.Val2 & (1 << ROWOW_UF_RANGEDSINGLEUNIT)) != 0)
                    SkillDelUnitGroup(map, SkillID2Group(map, group.GroupID));
            }
            else
                SkillDelUnit(map, group, index);
            return 1;
        case ROWOW_SKILL_SA_VOLCANO:
        case ROWOW_SKILL_SA_DELUGE:
        case ROWOW_SKILL_SA_VIOLENTGALE:
            // The official implementation makes them fail to appear when casted on top of ANYTHING
            if (unit.Range <= 0 && groupSkill->UnitTypeID != ROWOW_UNT_DUMMYSKILL)
            {
                alive = 0;
                return 1;
            }
            break;
        case ROWOW_SKILL_PF_FOGWALL:
            switch (group.SkillID)
            {
                case ROWOW_SKILL_SA_VOLCANO: // Can't be placed on top of these
                case ROWOW_SKILL_SA_VIOLENTGALE:
                    alive = 0;
                    return 1;
                case ROWOW_SKILL_SA_DELUGE:
                case ROWOW_SKILL_NJ_SUITON:
                    alive = 2; // Cheap 'hack' to notify the calling function that duration should be doubled [Skotlex]
                    break;
            }
            break;
        case ROWOW_SKILL_WZ_WATERBALL:
            switch (group.SkillID)
            {
                case ROWOW_SKILL_SA_DELUGE:
                case ROWOW_SKILL_NJ_SUITON:
                    // Consumes deluge/suiton
                    SkillDelUnit(map, group, index);
                    return 1;
            }
            break;
        case ROWOW_SKILL_WZ_ICEWALL:
        case ROWOW_SKILL_HP_BASILICA:
        case ROWOW_SKILL_HW_GRAVITATION:
            // These can't be placed on top of themselves (duration can't be refreshed)
            if (group.SkillID == skillID)
            {
                alive = 0;
                return 1;
            }
            break;
    }
    if (group.SkillID == ROWOW_SKILL_SA_LANDPROTECTOR && skill->Inf2[ROWOW_INF2_ISTRAP] == false && skill->Inf2[ROWOW_INF2_IGNORELANDPROTECTOR] == false)
    {
        // It deletes everything except traps and barriers
        alive = 0;
        return 1;
    }
    return 0;
}

int32 ROWOWMod::SkillCheckUnitRange(Unit* bl, int32 x, int32 y, uint16 skillID, uint16 skillLevel)
{
    // skill_check_unit_range: the units around the cells a skill would use that it can't stack on (UF_NOREITERATION).  Non players do not
    // check for the skill's splash-trigger area
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr)
        return 0;
    int32 range = bl->IsPlayer() == true ? skill->UnitRange[ROWOWSkill::GetLevelIndex(skillLevel)] : 0;
    int32 layoutType = SkillGetUnitLayoutType(skillID, skillLevel);
    if (layoutType == -1 || layoutType > ROWOW_MAX_SQUARE_LAYOUT)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::SkillCheckUnitRange: unsupported layout type {} for skill {}", layoutType, skillID);
        return 0;
    }
    range += layoutType;
    return MapForEachSkillUnitInArea(bl->GetMap(), x - range, y - range, x + range, y + range, [this, skillID](ROWOWSkillUnitGroup& group, int32 /*index*/)
    {
        // skill_check_unit_range_sub
        uint16 groupSkillID = group.SkillID;
        switch (skillID)
        {
            case ROWOW_SKILL_AL_PNEUMA: // Pneuma doesn't work even if just one cell overlaps with Land Protector
                if (groupSkillID == ROWOW_SKILL_SA_LANDPROTECTOR)
                    break;
                [[fallthrough]];
            case ROWOW_SKILL_MG_SAFETYWALL:
                if (groupSkillID != ROWOW_SKILL_MG_SAFETYWALL && groupSkillID != ROWOW_SKILL_AL_PNEUMA)
                    return 0;
                break;
            case ROWOW_SKILL_AL_WARP:
            case ROWOW_SKILL_HT_SKIDTRAP:
            case ROWOW_SKILL_MA_SKIDTRAP:
            case ROWOW_SKILL_HT_LANDMINE:
            case ROWOW_SKILL_MA_LANDMINE:
            case ROWOW_SKILL_HT_ANKLESNARE:
            case ROWOW_SKILL_HT_SHOCKWAVE:
            case ROWOW_SKILL_HT_SANDMAN:
            case ROWOW_SKILL_MA_SANDMAN:
            case ROWOW_SKILL_HT_FLASHER:
            case ROWOW_SKILL_HT_FREEZINGTRAP:
            case ROWOW_SKILL_MA_FREEZINGTRAP:
            case ROWOW_SKILL_HT_BLASTMINE:
            case ROWOW_SKILL_HT_CLAYMORETRAP:
            case ROWOW_SKILL_HT_TALKIEBOX:
            case ROWOW_SKILL_HP_BASILICA:
            {
                // Non stackable on themselves and traps (including venom dust which does not has the trap inf2 set)
                ROWOWSkill const* groupSkill = GetSkill(groupSkillID);
                if (skillID != groupSkillID && (groupSkill == nullptr || groupSkill->Inf2[ROWOW_INF2_ISTRAP] == false) && groupSkillID != ROWOW_SKILL_AS_VENOMDUST)
                    return 0;
                break;
            }
            default: // Avoid stacking with same kind of trap. [Skotlex]
                if (groupSkillID != skillID)
                    return 0;
                break;
        }
        return 1;
    });
}

int32 ROWOWMod::SkillCheckUnitRange2(Unit* bl, int32 x, int32 y, uint16 skillID, uint16 skillLevel)
{
    // skill_check_unit_range2 for UF_NOFOOTSET: the chars standing where the skill's units would go (skill_nofootset 1: only a player
    // checks, for everyone; a monster for monsters)
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr)
        return 0;
    int32 range = 0;
    switch (skillID)
    {
        // to be expanded later
        case ROWOW_SKILL_WZ_ICEWALL:
            range = 2;
            break;
        default:
        {
            int32 layoutType = SkillGetUnitLayoutType(skillID, skillLevel);
            if (layoutType == -1 || layoutType > ROWOW_MAX_SQUARE_LAYOUT)
            {
                LOG_ERROR("module.ROWOW", "ROWOWMod::SkillCheckUnitRange2: unsupported layout type {} for skill {}", layoutType, skillID);
                return 0;
            }
            range = skill->UnitRange[ROWOWSkill::GetLevelIndex(skillLevel)] + layoutType;
            break;
        }
    }
    // If the caster is a monster/NPC, only check for players. Otherwise just check characters
    bool isMonsterOnly = bl->IsPlayer() == false;
    return MapForEachInCellArea(bl->GetMap(), bl->GetPhaseMask(), x - range, y - range, x + range, y + range, [skillID, isMonsterOnly](Unit* target)
    {
        // skill_check_unit_range2_sub
        if (target->IsAlive() == false && skillID != ROWOW_SKILL_AL_WARP)
            return 0;
        if (isMonsterOnly == true && target->IsCreature() == false)
            return 0;
        if (skillID == ROWOW_SKILL_HP_BASILICA && target->IsPlayer() == true)
            return 0;
        return 1;
    });
}

bool ROWOWMod::SkillPosMaxcountCheck(Unit* src, int32 x, int32 y, uint16 skillID, uint16 skillLevel, bool displayFailure)
{
    // skill_pos_maxcount_check: a ground skill can't go on a cell its units can't stack on (UF_NOREITERATION; skill_reiteration 0), where a
    // char stands (UF_NOFOOTSET; skill_nofootset 1 for players), nor past its ActiveInstance count of groups (land_skill_limit)
    ROWOWSkill const* skill = GetSkill(skillID);
    Player* sd = src->ToPlayer();
    if (skill == nullptr)
        return false;
    if (skill->UnitFlags[ROWOW_UF_NOREITERATION] == true && SkillCheckUnitRange(src, x, y, skillID, skillLevel) != 0)
    {
        if (sd != nullptr && displayFailure == true)
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
        return false;
    }
    if (src->IsPlayer() == true && skill->UnitFlags[ROWOW_UF_NOFOOTSET] == true && SkillCheckUnitRange2(src, x, y, skillID, skillLevel) != 0)
    {
        if (sd != nullptr && displayFailure == true)
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
        return false;
    }
    int32 maxcount = skill->MaxCount[ROWOWSkill::GetLevelIndex(skillLevel)];
    if (src->IsPlayer() == true && maxcount > 0) // land_skill_limit: players (and homunculi)
    {
        // unit_skillunit_maxcount: each of the caster's groups of the skill uses up one
        ROWOWSkillUnitData* ud = GetSkillUnitData(src);
        for (int32 groupID : ud->SkillUnitGroupIDs)
        {
            std::shared_ptr<ROWOWSkillUnitGroup> group = SkillID2Group(src->GetMap(), groupID);
            if (group != nullptr && group->SkillID == skillID && --maxcount == 0)
                break;
        }
        if (maxcount == 0)
        {
            if (sd != nullptr && displayFailure == true)
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
            return false;
        }
    }
    return true;
}
