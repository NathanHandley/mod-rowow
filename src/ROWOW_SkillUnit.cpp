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
#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
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
#include "SpellInfo.h"
#include "SpellMgr.h"
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

    // Whether a live unit of the skill is on the cell (the cell flags its units set, skill_unitsetmapcell)
    bool HasSkillUnitOnCell(ROWOWMapSkillUnits const* mapUnits, int32 x, int32 y, uint16 skillID)
    {
        if (mapUnits == nullptr)
            return false;
        auto cellIter = mapUnits->UnitsByCell.find(GetCellKey(x, y));
        if (cellIter == mapUnits->UnitsByCell.end())
            return false;
        for (std::pair<int32, int32> const& unitRef : cellIter->second)
        {
            auto groupIter = mapUnits->GroupsByID.find(unitRef.first);
            if (groupIter != mapUnits->GroupsByID.end() && groupIter->second->SkillID == skillID && groupIter->second->Units[unitRef.second].IsAlive == true)
                return true;
        }
        return false;
    }

    // Whether Land Protector stops a magic unit acting on a char: the unit's own cell is protected, or (land_protector_behavior) the char's
    bool IsCellLandProtected(Map* map, ROWOWSkillUnit const& unit, Unit const* bl)
    {
        if (ROWOW->BattleConfig.LandProtectorBehavior == 0)
            return ROWOW->MapGetCellLandProtector(map, unit.X, unit.Y);
        int32 x = 0, y = 0;
        ROWOW->GetUnitCell(bl, x, y);
        return ROWOW->MapGetCellLandProtector(map, x, y);
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

bool ROWOWMod::MapGetCellLandProtector(Map* map, int32 x, int32 y) const
{
    // CELL_CHKLANDPROTECTOR: Land Protector's units set it on their cell (skill_unitsetmapcell, their range being none)
    return HasSkillUnitOnCell(GetMapSkillUnits(map, false), x, y, ROWOW_SKILL_SA_LANDPROTECTOR);
}

bool ROWOWMod::MapGetCellIceWall(Map* map, int32 x, int32 y) const
{
    // CELL_CHKICEWALL: an Ice Wall unit makes its cell a wall that can be shot over (map_setgatcell 5)
    return HasSkillUnitOnCell(GetMapSkillUnits(map, false), x, y, ROWOW_SKILL_WZ_ICEWALL);
}

bool ROWOWMod::MapGetCellBasilica(Map* map, int32 x, int32 y) const
{
    // CELL_CHKBASILICA: Basilica's units set it on their cell (skill_unitsetmapcell, their range being none)
    return HasSkillUnitOnCell(GetMapSkillUnits(map, false), x, y, ROWOW_SKILL_HP_BASILICA);
}

void ROWOWMod::PCCellBasilica(Player* sd)
{
    // pc_cell_basilica: a player standing on a Basilica cell has SC_BASILICA (no attacks, no skills, and safe from monsters' hits), which
    // stepping off ends.  The caster's own (val4 its id) holds it still
    int32 x = 0, y = 0;
    GetUnitCell(sd, x, y);
    if (MapGetCellBasilica(sd->GetMap(), x, y) == false)
    {
        if (GetSCE(sd, ROWOW_SC_BASILICA) != nullptr)
            StatusChangeEnd(sd, ROWOW_SC_BASILICA);
    }
    else if (GetSCE(sd, ROWOW_SC_BASILICA) == nullptr)
        SCStart(sd, sd, ROWOW_SC_BASILICA, 100, 0, ROWOW_INFINITE_TICK);
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
    // UF_PATHCHECK skills), each acting at once on what stands in it.  A trap remembers the item it was made of; a song or dance starts its
    // performer's SC_DANCING (and an ensemble its partner's).  (The other jobs' cases of its switches are ported with them)
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
    int32 linkGroupID = 0;
    uint32 reqItem = 0;
    int32 activeFlag = 1;
    int32 subunt = 0; // A unit id after the skill's own (Ground Drift's, by its element)
    ROWOWStatus const* status = GetROStatus(src);
    // A hidden trap (UF_HIDDENTRAP) is hidden by traps_setting: 2 everywhere, 1 on the PvP maps (none here)
    bool hidden = skill->UnitFlags[ROWOW_UF_HIDDENTRAP] == true && BattleConfig.TrapsSetting == 2;

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
        case ROWOW_SKILL_WZ_METEOR:
            limit = flag;
            flag = 0; // Flag should not influence anything else for these skills
            break;
        case ROWOW_SKILL_WZ_FIREPILLAR:
            if (MapGetCellLandProtector(map, x, y) == true)
                return nullptr;
            if ((flag & 1) != 0)
                limit = 1000;
            val1 = skillLevel + 2;
            break;
        case ROWOW_SKILL_SA_LANDPROTECTOR:
        case ROWOW_SKILL_SA_VOLCANO:
        case ROWOW_SKILL_SA_DELUGE:
        case ROWOW_SKILL_SA_VIOLENTGALE:
        {
            // HelloKitty confirmed that these are interchangeable, so you can change element and not consume gemstones: the caster's
            // old field goes, and a new elemental field takes the old one's time left [Skotlex]
            if (std::shared_ptr<ROWOWSkillUnitGroup> oldGroup = SkillLocateElementField(src))
            {
                if ((oldGroup->SkillID == ROWOW_SKILL_SA_VOLCANO || oldGroup->SkillID == ROWOW_SKILL_SA_DELUGE || oldGroup->SkillID == ROWOW_SKILL_SA_VIOLENTGALE) &&
                    oldGroup->Limit > 0)
                {
                    limit = oldGroup->Limit - int64(GetTick() - oldGroup->Tick);
                    if (limit < 0) // This can happen...
                        limit = SkillGetTime(skillID, skillLevel);
                }
                SkillClearGroup(src, 1);
            }
            break;
        }
        case ROWOW_SKILL_HP_BASILICA:
            val1 = int32(src->GetGUID().GetCounter()); // Store caster id.
            break;
        case ROWOW_SKILL_HT_SKIDTRAP:
        case ROWOW_SKILL_MA_SKIDTRAP:
        {
            // Save position of caster
            int32 srcX = 0, srcY = 0;
            GetUnitCell(src, srcX, srcY);
            val1 = (srcX << 16) | srcY;
            [[fallthrough]];
        }
        case ROWOW_SKILL_HT_ANKLESNARE:
        case ROWOW_SKILL_HT_SHOCKWAVE:
        case ROWOW_SKILL_HT_SANDMAN:
        case ROWOW_SKILL_MA_SANDMAN:
        case ROWOW_SKILL_HT_CLAYMORETRAP:
        case ROWOW_SKILL_HT_LANDMINE:
        case ROWOW_SKILL_MA_LANDMINE:
        case ROWOW_SKILL_HT_FLASHER:
        case ROWOW_SKILL_HT_FREEZINGTRAP:
        case ROWOW_SKILL_MA_FREEZINGTRAP:
        case ROWOW_SKILL_HT_BLASTMINE:
            // The trap item it was made of (Booby Trap, or Special Alloy Trap), which it turns back into.  (gvg_traps_target_all's targets need
            // the PvP maps)
            if (sd != nullptr)
            {
                ROWOWSkillCondition req = SkillGetRequirement(sd, skillID, skillLevel);
                for (int32 i = 0; i < ROWOW_MAX_SKILL_ITEM_REQUIRE && reqItem == 0; i++)
                    if (req.ItemIDs[i] == ROWOW_ITEMID_TRAP || req.ItemIDs[i] == ROWOW_ITEMID_TRAP_ALLOY)
                        reqItem = req.ItemIDs[i];
            }
            break;
        case ROWOW_SKILL_BA_WHISTLE:
            val1 = skillLevel + status->Stats[ROWOW_STAT_AGI] / 10; // Flee increase
            val2 = (skillLevel + 1) / 2 + status->Stats[ROWOW_STAT_LUK] / 30; // Perfect dodge increase
            if (sd != nullptr)
            {
                val1 += PCCheckSkill(sd, ROWOW_SKILL_BA_MUSICALLESSON) / 2;
                val2 += PCCheckSkill(sd, ROWOW_SKILL_BA_MUSICALLESSON) / 5;
            }
            break;
        case ROWOW_SKILL_DC_HUMMING:
            val1 = 1 + 2 * skillLevel + status->Stats[ROWOW_STAT_DEX] / 10; // Hit increase
            if (sd != nullptr)
                val1 += PCCheckSkill(sd, ROWOW_SKILL_DC_DANCINGLESSON);
            break;
        case ROWOW_SKILL_BA_POEMBRAGI:
            val1 = 3 * skillLevel + status->Stats[ROWOW_STAT_DEX] / 10; // Casting time reduction
            // For some reason at level 10 the base delay reduction is 50%.
            val2 = (skillLevel < 10 ? 3 * skillLevel : 50) + status->Stats[ROWOW_STAT_INT] / 5; // After-cast delay reduction
            if (sd != nullptr)
            {
                val1 += PCCheckSkill(sd, ROWOW_SKILL_BA_MUSICALLESSON);
                val2 += 2 * PCCheckSkill(sd, ROWOW_SKILL_BA_MUSICALLESSON);
            }
            break;
        case ROWOW_SKILL_DC_DONTFORGETME:
            val1 = 5 + 3 * skillLevel + status->Stats[ROWOW_STAT_DEX] / 10; // ASPD decrease
            val2 = 5 + 3 * skillLevel + status->Stats[ROWOW_STAT_AGI] / 10; // Movement speed adjustment.
            if (sd != nullptr)
            {
                val1 += PCCheckSkill(sd, ROWOW_SKILL_DC_DANCINGLESSON);
                val2 += PCCheckSkill(sd, ROWOW_SKILL_DC_DANCINGLESSON);
            }
            val1 *= 10; // Because 10 is actually 1% aspd
            break;
        case ROWOW_SKILL_DC_SERVICEFORYOU:
            val1 = 15 + skillLevel + (status->Stats[ROWOW_STAT_INT] / 10); // MaxSP percent increase
            val2 = 20 + 3 * skillLevel + (status->Stats[ROWOW_STAT_INT] / 10); // SP cost reduction
            if (sd != nullptr)
            {
                val1 += PCCheckSkill(sd, ROWOW_SKILL_DC_DANCINGLESSON) / 2;
                val2 += PCCheckSkill(sd, ROWOW_SKILL_DC_DANCINGLESSON) / 2;
            }
            break;
        case ROWOW_SKILL_BA_ASSASSINCROSS:
            if (sd != nullptr)
                val1 = PCCheckSkill(sd, ROWOW_SKILL_BA_MUSICALLESSON) / 2;
            val1 += 5 + skillLevel + (status->Stats[ROWOW_STAT_AGI] / 20);
            val1 *= 10; // ASPD works with 1000 as 100%
            break;
        case ROWOW_SKILL_DC_FORTUNEKISS:
            val1 = 10 + skillLevel + (status->Stats[ROWOW_STAT_LUK] / 10); // Critical increase
            val1 *= 10; // Because every 10 crit is an actual cri point.
            if (sd != nullptr)
                val1 += 5 * PCCheckSkill(sd, ROWOW_SKILL_DC_DANCINGLESSON);
            break;
        case ROWOW_SKILL_BD_DRUMBATTLEFIELD:
            val1 = (skillLevel + 1) * 25; // Atk increase
            val2 = (skillLevel + 1) * 2; // Def increase
            break;
        case ROWOW_SKILL_BD_RINGNIBELUNGEN:
            val1 = (skillLevel + 2) * 25; // Atk increase
            break;
        case ROWOW_SKILL_BD_RICHMANKIM:
            val1 = 25 + 11 * skillLevel; // Exp increase bonus.
            break;
        case ROWOW_SKILL_BD_SIEGFRIED:
            val1 = 55 + skillLevel * 5; // Elemental Resistance
            val2 = skillLevel * 10; // Status ailment resistance
            break;
        case ROWOW_SKILL_PR_SANCTUARY:
            val1 = skillLevel + 3; // The undead and demons it can still hit
            break;
        case ROWOW_SKILL_NJ_KAENSIN:
            SkillClearGroup(src, 1); // Delete previous Kaensins/Suitons
            val2 = (skillLevel + 1) / 2 + 4;
            break;
        case ROWOW_SKILL_NJ_SUITON:
            SkillClearGroup(src, 1);
            break;
        case ROWOW_SKILL_GS_GROUNDDRIFT:
        {
            // Ground Drift Element is decided when it's placed.
            int32 ele = skill->Element[ROWOWSkill::GetLevelIndex(skillLevel)];
            int32 const element[5] = { ROWOW_ELEMENT_WIND, ROWOW_ELEMENT_DARK, ROWOW_ELEMENT_POISON, ROWOW_ELEMENT_WATER, ROWOW_ELEMENT_FIRE };
            if (ele == ROWOW_ELEMENT_RANDOM)
                val1 = element[urand(0, 4)]; // Use random from available unit visual?
            else if (ele == ROWOW_ELEMENT_ENDOWED)
                val1 = StatusCalcAttackElement(sc, ROWOW_ELEMENT_NEUTRAL);
            else if (ele == ROWOW_ELEMENT_WEAPON)
            {
                val1 = status->WeaponElement;
                if (ROWOWStatusChangeEntry const* enchantArms = sc != nullptr ? sc->GetSCE(ROWOW_SC_ENCHANTARMS) : nullptr)
                    val1 = enchantArms->Val1;
            }
            switch (val1)
            {
                case ROWOW_ELEMENT_FIRE:
                    subunt++;
                    [[fallthrough]];
                case ROWOW_ELEMENT_WATER:
                    subunt++;
                    [[fallthrough]];
                case ROWOW_ELEMENT_POISON:
                    subunt++;
                    [[fallthrough]];
                case ROWOW_ELEMENT_DARK:
                    subunt++;
                    [[fallthrough]];
                case ROWOW_ELEMENT_WIND:
                    break;
                default:
                    subunt = int32(urand(0, 4));
                    break;
            }
            break;
        }
        case ROWOW_SKILL_HW_GRAVITATION:
            // The caster's own Gravitation status deletes the field with it
            if (ROWOWStatusChangeEntry const* gravitation = sc != nullptr ? sc->GetSCE(ROWOW_SC_GRAVITATION) : nullptr; gravitation != nullptr &&
                gravitation->Val3 == ROWOW_BCT_SELF)
                linkGroupID = gravitation->Val4;
            break;
        // (Quagmire's and Demonstration's targets turn to all on the PvP maps with vs_traps_bctall, which no RO map has here)
    }

    // Init skill unit group
    std::shared_ptr<ROWOWSkillUnitGroup> group = SkillInitUnitGroup(src, int32(layout->Cells.size()), skillID, skillLevel,
        uint16(((flag & 1) != 0 ? skill->UnitTypeID2 : skill->UnitTypeID) + subunt), limit, interval);
    if (group == nullptr)
        return nullptr;
    group->Val1 = val1;
    group->Val2 = val2;
    group->Val3 = val3;
    group->LinkGroupID = linkGroupID;
    group->TargetFlag = target;
    group->BLFlag = skill->UnitBLTarget;
    // Store if this skill needs to consume ammo.
    group->IsAmmoConsume = sd != nullptr && status != nullptr && status->IsArrowAttack == true && skillID != ROWOW_SKILL_GS_GROUNDDRIFT && (flag & ROWOW_UNIT_NOCONSUME_AMMO) == 0;
    group->SongDance = uint8(((skill->UnitFlags[ROWOW_UF_DANCE] == true || skill->UnitFlags[ROWOW_UF_SONG] == true) ? 1 : 0) | (skill->UnitFlags[ROWOW_UF_ENSEMBLE] == true ? 2 : 0)); // Signals if this is a song/dance/duet
    group->IsGuildAura = skillID >= ROWOW_SKILL_GD_LEADERSHIP && skillID <= ROWOW_SKILL_GD_HAWKEYES;
    group->ItemID = reqItem;
    // If tick is greater than current, do not invoke onplace function just yet. [Skotlex]
    if (group->Tick > GetTick() + ROWOW_SKILLUNITTIMER_INTERVAL)
        activeFlag = 0;

    // Put message for Talkie Box & Graffiti
    if (skillID == ROWOW_SKILL_HT_TALKIEBOX || skillID == ROWOW_SKILL_RG_GRAFFITI)
        group->ValStr = sd != nullptr ? GetSkillUnitData(sd)->Message : "Boo!"; // Eh... we have to write something here... even though mobs shouldn't use this. [Skotlex]

    // Dance skill: the performer dances it (an ensemble waits for its partner, BCT_SELF, whom the partner search then makes dance it too)
    if (group->SongDance != 0)
    {
        if (sd != nullptr)
        {
            ROWOWSkillUnitData* sud = GetSkillUnitData(sd);
            sud->SkillIDDance = skillID;
            sud->SkillLevelDance = skillLevel;
        }
        if (SCStart4(src, src, ROWOW_SC_DANCING, 100, skillID, group->GroupID, skillLevel, ((group->SongDance & 2) != 0 ? ROWOW_BCT_SELF : 0), limit + 1000) == true &&
            sd != nullptr && (group->SongDance & 2) != 0 && skillID != ROWOW_SKILL_CG_HERMODE) // Hermod is a encore with a warp!
        {
            uint16 partnerLevel = skillLevel;
            SkillCheckPCPartner(sd, skillID, partnerLevel, 1, 1);
        }
    }

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
            case ROWOW_SKILL_GS_DESPERADO:
            {
                // Each cell's chance (in 100) to hit what's on it: 36 next to the middle falling by 12 a cell along the cross, less on the diagonals
                unitVal1 = std::abs(layout->Cells[i].first);
                unitVal2 = std::abs(layout->Cells[i].second);
                if (unitVal1 < 2 || unitVal2 < 2)
                {
                    // Nearby cross, linear decrease with no diagonals
                    if (unitVal2 > unitVal1)
                        unitVal1 = unitVal2;
                    if (unitVal1 != 0)
                        unitVal1--;
                    unitVal1 = 36 - 12 * unitVal1;
                }
                else // Diagonal edges
                    unitVal1 = 28 - 4 * unitVal1 - 4 * unitVal2;
                if (unitVal1 < 1)
                    unitVal1 = 1;
                unitVal2 = 0;
                break;
            }
            case ROWOW_SKILL_CR_GRANDCROSS:
            case ROWOW_SKILL_NPC_GRANDDARKNESS:
                unitVal1 = int32(SkillGetTime(skillID, skillLevel) / std::max(1, interval)); // Default: 950/300 = 3 hits
                break;
            case ROWOW_SKILL_WZ_ICEWALL:
                unitVal1 = 200 + 200 * skillLevel; // Its HP
                break;
            case ROWOW_SKILL_WZ_WATERBALL:
            {
                // Check if there are cells that can be turned into waterball units: water, Deluge or Suiton (a monster's needs none)
                int32 dummyIndex = -1;
                if (sd == nullptr || map->IsInWater(src->GetPhaseMask(), cellX, cellY, cellZ + 0.5f, 0.0f) == true ||
                    MapFindSkillUnitOnCell(src, ux, uy, ROWOW_SKILL_SA_DELUGE, 1, dummyIndex) != nullptr ||
                    MapFindSkillUnitOnCell(src, ux, uy, ROWOW_SKILL_NJ_SUITON, 1, dummyIndex) != nullptr)
                    break; // Turn water, deluge or suiton into waterball cell
                continue;
            }
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
        // skill_initunit's oninit actions: a song or dance cell on another's turns both into Dissonance (or Ugly Dance)
        if ((group->SongDance & 0x1) != 0) // Check for dissonance.
            SkillDanceOverlap(map, *group, i, ROWOW_OVERLAP_SET);
        if (skillID == ROWOW_SKILL_PF_FOGWALL && alive == 2)
        {
            // Double duration of cells on top of Deluge/Suiton
            unit->Limit *= 2;
            group->Limit = unit->Limit;
        }
        if (skill->UnitSpellIDsByLevel.size() > size_t(levelIndex) && skill->IsUnitSpellPerCellByLevel[levelIndex] == true && unit->IsHidden == false &&
            unit->VisualGUID.IsEmpty() == true) // (a song cell that turned into Dissonance on its init already shows that)
        {
            // An Ice Wall cell shows for as long as its HP lasts (it loses 50 a second), its limit being renewed each second
            int64 visualLimit = skillID == ROWOW_SKILL_WZ_ICEWALL ? int64(unit->Val1 / 50 + 1) * std::max(1, interval) : unit->Limit;
            unit->VisualGUID = CreateSkillUnitVisual(src, skill->UnitSpellIDsByLevel[levelIndex], cellX, cellY, cellZ, (GetMapGrid(map->GetId()) != nullptr ?
                GetMapGrid(map->GetId())->CellSize : CellSize) * 0.5f, visualLimit);
        }

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

    // success, unit created.
    if (skillID == ROWOW_SKILL_NJ_TATAMIGAESHI)
        group->Val1 = group->AliveCount; // Store number of tiles.

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
    // skill_initunit: a cell of the group, on the caster's map.  (Ice Wall's, Land Protector's and Basilica's cell changes are their live units;
    // its oninit Dissonance check is skill_unitsetting's, once the cell's ground is known)
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
    // skill_delunit: the cell goes, acting on what stands on it as if it left (onout, then onleft), and the group with its last cell; a song's
    // Dissonance with it ends, and Ankle Snare lets go of what it caught.  (Ice Wall's, Land Protector's and Basilica's cells are their live
    // units, MapGetCellIceWall, MapGetCellLandProtector and MapGetCellBasilica)
    if (index < 0 || index >= int32(group.Units.size()))
        return;
    ROWOWSkillUnit& unit = group.Units[index];
    if (unit.IsAlive == false)
        return;
    unit.IsAlive = false;
    std::shared_ptr<ROWOWSkillUnitGroup> groupRef = SkillID2Group(map, group.GroupID); // Kept while the cell's onout may delete things
    if ((group.SongDance & 0x1) != 0) // Cancel dissonance effect.
        SkillDanceOverlap(map, group, index, ROWOW_OVERLAP_REMOVE);

    // invoke onout event
    if (unit.Range == 0)
    {
        MapForEachInCellArea(map, PHASEMASK_NORMAL, unit.X, unit.Y, unit.X, unit.Y, [this, map, &group, index](Unit* bl)
        {
            SkillUnitEffect(map, bl, group, index, GetTick(), 4);
            return 0;
        });
    }

    // perform ondelete actions
    switch (group.SkillID)
    {
        case ROWOW_SKILL_HT_ANKLESNARE:
            if (Unit* target = group.Val2GUID.IsEmpty() == false ? GetSkillUnitGroupSource(map, group.Val2GUID) : nullptr)
                StatusChangeEnd(target, ROWOW_SC_ANKLE);
            break;
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

        // Warmth's area going ends the Star Gladiator's status (which then has no group to delete)
        if (group->SkillID == ROWOW_SKILL_SG_SUN_WARM || group->SkillID == ROWOW_SKILL_SG_MOON_WARM || group->SkillID == ROWOW_SKILL_SG_STAR_WARM)
        {
            if (ROWOWStatusChange* sc = GetStatusChange(src))
            {
                if (ROWOWStatusChangeEntry* warm = sc->GetSCE(ROWOW_SC_WARM))
                {
                    warm->Val4 = 0;
                    StatusChangeEnd(src, ROWOW_SC_WARM);
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

    if (group->SkillID == ROWOW_SKILL_PF_SPIDERWEB && group->Val2GUID.IsEmpty() == false)
    {
        // Clear group id from status change; the last web cleared ends it (a web that status_change_end already let go is no longer named)
        Unit* target = GetSkillUnitGroupSource(map, group->Val2GUID);
        ROWOWStatusChange* sc = target != nullptr ? GetStatusChange(target) : nullptr;
        if (ROWOWStatusChangeEntry* sce = sc != nullptr ? sc->GetSCE(ROWOW_SC_SPIDERWEB) : nullptr)
        {
            bool isRemoved = true;
            if (sce->Val2 == group->GroupID)
                sce->Val2 = 0;
            else if (sce->Val3 == group->GroupID)
                sce->Val3 = 0;
            else if (sce->Val4 == group->GroupID)
                sce->Val4 = 0;
            else
                isRemoved = false; // Group was already removed in status_change_end, don't call it again!
            if (isRemoved == true && sce->Val2 == 0 && sce->Val3 == 0 && sce->Val4 == 0)
                StatusChangeEnd(target, ROWOW_SC_SPIDERWEB);
        }
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
    // skill_unit_onplace: what stepping into the unit's cell (or the cell being placed under it) does; a magic unit does nothing on Land
    // Protector, nor a song on Basilica.  A song's or an ensemble's status is given to those in it (not to its performer, but with the Bard
    // and Dancer spirit), lasting a moment past the group (its icon endless).  (Maelstrom's cells, Hovering, and the other jobs' units come
    // with them)
    ROWOWSkillUnit& unit = group.Units[index];
    if (bl->IsInWorld() == false || unit.IsAlive == false || bl->IsAlive() == false)
        return 0;
    Unit* ss = GetSkillUnitGroupSource(map, group.SrcGUID);
    ROWOWSkill const* skill = GetSkill(group.SkillID);
    if (ss == nullptr || skill == nullptr)
        return 0;
    if (skill->AttackType == ROWOW_BF_MAGIC && group.SkillID != ROWOW_SKILL_SA_LANDPROTECTOR && IsCellLandProtected(map, unit, bl) == true)
        return 0; // AoE skills are ineffective. [Skotlex]
    if (skill->Inf2[ROWOW_INF2_ISSONG] == true || skill->Inf2[ROWOW_INF2_ISENSEMBLE] == true)
    {
        int32 blX = 0, blY = 0;
        GetUnitCell(bl, blX, blY);
        if (MapGetCellBasilica(map, blX, blY) == true)
            return 0; // Songs don't work in Basilica
    }
    ROWOWStatusChange* sc = GetStatusChange(bl);
    if (sc != nullptr && (sc->Option & ROWOW_OPTION_HIDE) != 0 && skill->Inf2[ROWOW_INF2_TARGETHIDDEN] == false)
        return 0; // Hidden characters are immune to AoE skills except to these. [Skotlex]
    int32 type = skill->StatusID;
    ROWOWStatusChangeEntry* sce = (sc != nullptr && type != ROWOW_SC_NONE) ? sc->GetSCE(type) : nullptr;
    uint16 skillID = group.SkillID; // In case the group is deleted, we need to return the correct skill id, still.
    switch (group.UnitID)
    {
        case ROWOW_UNT_SPIDERWEB:
        {
            // Duration in PVM is: 1st - 8s, 2nd - 16s, 3rd - 8s (PvP's halved durations need the PvP maps).  The web holds up to three webs'
            // ids on the status (val2-4); the target is pulled onto the web's cell
            sc = bl->CustomData.GetDefault<ROWOWStatusChange>("ROWOWStatusChange");
            sce = sc->GetSCE(type);
            uint64 now = GetTick();
            int64 sec = SkillGetTime2(group.SkillID, group.SkillLevel);
            if (sce != nullptr)
            {
                if (sce->Val2 != 0 && sce->Val3 != 0 && sce->Val4 != 0)
                {
                    // Already triple affected, immune
                    group.Limit = int64(tick - group.Tick);
                    break;
                }
                // Don't increase val1 here, we need a higher val in status_change_start so it overwrites the old one
                if (sce->Val1 < 2)
                    sec *= (sce->Val1 + 1);
                // Add group id to status change
                if (sce->Val2 == 0)
                    sce->Val2 = group.GroupID;
                else if (sce->Val3 == 0)
                    sce->Val3 = group.GroupID;
                else if (sce->Val4 == 0)
                    sce->Val4 = group.GroupID;
                // Overwrite status change with new duration
                if (sce->TimerTime != 0)
                    StatusChangeStart(ss, bl, type, 10000, sce->Val1 + 1, sce->Val2, sce->Val3, sce->Val4, std::max<int64>(int64(sce->TimerTime) - int64(now), sec),
                        ROWOW_SCSTART_NORATEDEF);
            }
            else
            {
                if (StatusChangeStart(ss, bl, type, 10000, 1, group.GroupID, 0, 0, sec, ROWOW_SCSTART_NORATEDEF) == true)
                {
                    if (ROWOWStatusChangeEntry const* started = sc->GetSCE(type); started != nullptr && started->TimerTime != 0)
                        sec = int64(started->TimerTime) - int64(now);
                    UnitMovePos(bl, unit.X, unit.Y); // map_moveblock onto the web
                }
                else
                    sec = 3000; // Couldn't trap it?
            }
            group.Val2GUID = bl->GetGUID();
            group.Limit = int64(tick - group.Tick) + sec;
            break;
        }

        case ROWOW_UNT_QUAGMIRE:
            if (sce == nullptr && BattleCheckTargetSkillUnit(group, map, bl, group.TargetFlag) > 0)
                SCStart4(ss, bl, type, 100, group.SkillLevel, group.GroupID, 0, 0, group.Limit);
            break;

        case ROWOW_UNT_VOLCANO:
        case ROWOW_UNT_DELUGE:
        case ROWOW_UNT_VIOLENTGALE:
            if (sce == nullptr)
                SCStart(ss, bl, type, 100, group.SkillLevel, group.Limit);
            break;

        case ROWOW_UNT_FOGWALL:
            if (sce == nullptr)
            {
                SCStart4(ss, bl, type, 100, group.SkillLevel, group.Val1, group.Val2, group.GroupID, group.Limit);
                if (BattleCheckTargetSkillUnit(group, map, bl, ROWOW_BCT_ENEMY) > 0)
                    SkillAdditionalEffect(ss, bl, group.SkillID, group.SkillLevel, ROWOW_BF_MISC, ROWOW_ATK_DEF, tick);
            }
            break;

        case ROWOW_UNT_GRAVITATION:
            if (sce == nullptr)
                SCStart4(ss, bl, type, 100, group.SkillLevel, 0, ROWOW_BCT_ENEMY, group.GroupID, group.Limit);
            break;

        case ROWOW_UNT_SUITON:
            // Slower everyone in it; the Ninja's enemies (val2 1) lose AGI too
            if (GetSCE(bl, ROWOW_SC_SUITON) == nullptr)
                SCStart4(ss, bl, ROWOW_SC_SUITON, 100, group.SkillLevel, BattleCheckTargetSkillUnit(group, map, bl, ROWOW_BCT_ENEMY) > 0 ? 1 : 0, 0, 0, group.Limit);
            break;

        case ROWOW_UNT_SAFETYWALL:
            if (sce == nullptr)
                SCStart4(ss, bl, type, 100, group.SkillLevel, group.SkillID, group.GroupID, 0, group.Limit);
            break;

        case ROWOW_UNT_BASILICA:
        {
            // Placed under a char: rAthena asks the char about itself, which is never an enemy, so all on its cells get the status (val3 the
            // group, val4 the caster)
            int32 i = BattleCheckTarget(bl, bl, ROWOW_BCT_ENEMY);
            if (i > 0)
            {
                SkillBlown(ss, bl, skill->Knockback[ROWOWSkill::GetLevelIndex(group.SkillLevel)], int8(UnitGetDir(bl)), ROWOW_BLOWN_NONE);
                break;
            }
            if (sce == nullptr && i <= 0)
                SCStart4(ss, bl, type, 100, 0, 0, group.GroupID, int32(ss->GetGUID().GetCounter()), group.Limit);
            break;
        }

        case ROWOW_UNT_PNEUMA:
            if (sce == nullptr)
                SCStart4(ss, bl, type, 100, group.SkillLevel, group.GroupID, 0, 0, group.Limit);
            break;

        case ROWOW_UNT_HERMODE:
            if (group.SrcGUID != bl->GetGUID() && BattleCheckTargetSkillUnit(group, map, bl, ROWOW_BCT_PARTY | ROWOW_BCT_GUILD) > 0)
                StatusChangeClearBuffs(bl, ROWOW_SCCB_HERMODE); // Should dispell only allies.
            [[fallthrough]];
        case ROWOW_UNT_RICHMANKIM:
        case ROWOW_UNT_ETERNALCHAOS:
        case ROWOW_UNT_DRUMBATTLEFIELD:
        case ROWOW_UNT_RINGNIBELUNGEN:
        case ROWOW_UNT_ROKISWEIL:
        case ROWOW_UNT_INTOABYSS:
        case ROWOW_UNT_SIEGFRIED:
        {
            // Needed to check when a dancer/bard leaves their ensemble area.
            ROWOWStatusChangeEntry const* spirit = sc != nullptr ? sc->GetSCE(ROWOW_SC_SPIRIT) : nullptr;
            if (group.SrcGUID == bl->GetGUID() && (spirit == nullptr || spirit->Val2 != ROWOW_SKILL_SL_BARDDANCER))
                return skillID;
            if (sce == nullptr)
                SCStart4(ss, bl, type, 100, group.SkillLevel, group.Val1, group.Val2, 0, group.Limit);
            break;
        }

        case ROWOW_UNT_WHISTLE:
        case ROWOW_UNT_ASSASSINCROSS:
        case ROWOW_UNT_POEMBRAGI:
        case ROWOW_UNT_APPLEIDUN:
        case ROWOW_UNT_HUMMING:
        case ROWOW_UNT_DONTFORGETME:
        case ROWOW_UNT_FORTUNEKISS:
        case ROWOW_UNT_SERVICEFORYOU:
        {
            ROWOWStatusChangeEntry const* spirit = sc != nullptr ? sc->GetSCE(ROWOW_SC_SPIRIT) : nullptr;
            if (group.SrcGUID == bl->GetGUID() && (spirit == nullptr || spirit->Val2 != ROWOW_SKILL_SL_BARDDANCER))
                return 0;
            // Pseudo-infinite duration.  In case it is just started, it needs to last one interval longer than the skill unit lasts.  This is to
            // ensure the skill unit is removed before the status change ends
            if (sce == nullptr)
                SCStart4(ss, bl, type, 100, group.SkillLevel, group.Val1, group.Val2, 0, group.Limit + ROWOW_SKILLUNITTIMER_INTERVAL);
            else if (BattleConfig.RefreshSong == 1 && sce->Val4 == 1)
            {
                // Readjust timers since the effect will not last long.
                sce->Val4 = 0; // remove the mark that we stepped out
                sce->TimerTime = tick + uint64(group.Limit) + ROWOW_SKILLUNITTIMER_INTERVAL;
            }
            break;
        }

        case ROWOW_UNT_MOONLIT:
        {
            // Knockback out of area if affected char isn't in Moonlit effect
            ROWOWStatusChangeEntry const* dancing = sc != nullptr ? sc->GetSCE(ROWOW_SC_DANCING) : nullptr;
            if (dancing != nullptr && (dancing->Val1 & 0xFFFF) == ROWOW_SKILL_CG_MOONLIT)
                break;
            if (ss == bl) // Also needed to prevent infinite loop crash.
                break;
            SkillBlown(ss, bl, skill->Knockback[ROWOWSkill::GetLevelIndex(group.SkillLevel)], int8(UnitGetDir(bl)), ROWOW_BLOWN_NONE);
            break;
        }

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
    // skill_unit_onplace_timer: each interval (the group's), what standing in the unit's reach does: a trap goes off (and is spent for 1.5
    // seconds), a song or dance does its part.  (The other jobs' units, Siren's Voice and Hovering come with them)
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

        case ROWOW_UNT_TATAMIGAESHI:
        case ROWOW_UNT_DEMONSTRATION:
            SkillAttack(ROWOW_BF_WEAPON, ss, ss, bl, skillID, group.SkillLevel, tick, 0, &unit);
            break;

        case ROWOW_UNT_GROUNDDRIFT_WIND:
        case ROWOW_UNT_GROUNDDRIFT_DARK:
        case ROWOW_UNT_GROUNDDRIFT_POISON:
        case ROWOW_UNT_GROUNDDRIFT_WATER:
        case ROWOW_UNT_GROUNDDRIFT_FIRE:
        {
            // The grenade goes off on everything around its cell (skill_trap_splash), then stays a used trap for 1.5 s
            int32 splashRange = SkillGetSplash(group.SkillID, group.SkillLevel);
            ROWOWSkillUnitGroup& trapGroup = group;
            MapForEachInCellArea(map, bl->GetPhaseMask(), unit.X - splashRange, unit.Y - splashRange, unit.X + splashRange, unit.Y + splashRange,
                [this, map, &trapGroup, index, tick](Unit* target)
            {
                return SkillTrapSplash(map, target, trapGroup, index, tick);
            });
            ClifChangeTrapLook(map, group, index, bl);
            group.UnitID = ROWOW_UNT_USED_TRAPS;
            group.Limit = int64(tick - group.Tick) + ROWOW_TRAP_USED_LIMIT;
            break;
        }

        case ROWOW_UNT_VENOMDUST:
            // Poison on whoever stands in the dust and isn't poisoned yet
            if (GetSCE(bl, skill->StatusID) == nullptr)
                StatusChangeStart(ss, bl, skill->StatusID, 10000, group.SkillLevel, int32(ss->GetGUID().GetCounter()), 0, 0,
                    SkillGetTime2(skillID, group.SkillLevel), ROWOW_SCSTART_NONE);
            break;

        case ROWOW_UNT_DUMMYSKILL:
            // A Grand Cross cell hits as many times as its val1 (its caster too), and the unit timer takes the spent cell away.  (The NPC
            // skills come with the monster skills)
            if (skillID == ROWOW_SKILL_SG_SUN_WARM || skillID == ROWOW_SKILL_SG_MOON_WARM || skillID == ROWOW_SKILL_SG_STAR_WARM)
            {
                // SG skills [Komurka]: a player in it loses SP; a monster is knocked back, at 2 SP of the Star Gladiator's a hit.  If target
                // isn't knocked back it should hit every "interval" ms [Playtester]
                int32 count = 0;
                ROWOWSkillUnitData* bud = GetSkillUnitData(bl);
                uint64 blownTick = bud->SkillBlownTick;
                do
                {
                    if (bl->IsPlayer() == true)
                        StatusZap(bl, 0, 10); // sp damage to players
                    else
                    {
                        // Bosses trigger the effect only 1 out of 5 times
                        if (GetROStatus(bl)->Class == ROWOW_CLASS_BOSS && urand(0, 4) != 0)
                            continue;
                        // costs 2 SP per hit
                        if (StatusCharge(ss, 0, 2) == false)
                        {
                            // should end when out of sp.
                            group.Limit = int64(tick - group.Tick);
                            break;
                        }
                        SkillAttack(ROWOW_BF_WEAPON, ss, ss, bl, skillID, group.SkillLevel, tick + uint64(count) * uint64(group.Interval), 0, &unit);
                    }
                } while (group.Interval > 0 && bud->SkillBlownTick == blownTick && ++count < ROWOW_SKILLUNITTIMER_INTERVAL / group.Interval && bl->IsAlive() == true);
                break;
            }
            if (skillID == ROWOW_SKILL_GS_DESPERADO)
            {
                // Each cell hits what's on it at its own chance
                if (int32(urand(0, 99)) < unit.Val1)
                    SkillAttack(ROWOW_BF_WEAPON, ss, ss, bl, skillID, group.SkillLevel, tick, 0, &unit);
                break;
            }
            if (skillID == ROWOW_SKILL_WZ_STORMGUST)
            {
                // SG counter does not reset per stormgust. IE: One hit from a SG and two hits from another will freeze you.
                ROWOWStatusChange* tsc = bl->CustomData.GetDefault<ROWOWStatusChange>("ROWOWStatusChange");
                tsc->SGCounter++; // SG hit counter.
                if (SkillAttack(skill->AttackType, ss, ss, bl, skillID, group.SkillLevel, tick, 0, &unit) <= 0)
                    tsc->SGCounter = 0; // Attack absorbed.
                break;
            }
            if ((skillID == ROWOW_SKILL_CR_GRANDCROSS || skillID == ROWOW_SKILL_NPC_GRANDDARKNESS) && BattleConfig.GxAllHit == 0)
                unit.Val1--;
            SkillAttack(skill->AttackType, ss, ss, bl, skillID, group.SkillLevel, tick, 0, &unit);
            break;

        case ROWOW_UNT_SANCTUARY:
        {
            ROWOWStatus const* tstatus = GetROStatus(bl);
            if (BattleCheckUndead(tstatus->Race, tstatus->DefElement) == true || tstatus->Race == ROWOW_RACE_DEMON)
            {
                // Only damage enemies with offensive Sanctuary. [Skotlex]
                if (BattleCheckTargetSkillUnit(group, map, bl, ROWOW_BCT_ENEMY) > 0 && SkillAttack(ROWOW_BF_MAGIC, ss, ss, bl, skillID, group.SkillLevel, tick, 0, &unit) != 0)
                    group.Val1 -= 1; // Reduce the number of targets that can still be hit
            }
            else
            {
                // Heals the others standing in it, shown as the heal of the Sanctuary spell
                int32 heal = SkillCalcHeal(ss, bl, skillID, group.SkillLevel, true);
                if (bl->IsCreature() == true && tstatus->Class == ROWOW_CLASS_BATTLEFIELD)
                    break;
                if (bl->GetHealth() >= bl->GetMaxHealth())
                    break;
                if (StatusIsImmune(bl) != 0)
                    heal = 0;
                StatusHeal(bl, heal, 0, 0, ss, GetSkillSpellID(skillID, group.SkillLevel));
            }
            break;
        }

        case ROWOW_UNT_MAGNUS:
        {
            // Pre-renewal's hits only the undead and demons
            ROWOWStatus const* tstatus = GetROStatus(bl);
            if (BattleCheckUndead(tstatus->Race, tstatus->DefElement) == false && tstatus->Race != ROWOW_RACE_DEMON)
                break;
            SkillAttack(ROWOW_BF_MAGIC, ss, ss, bl, skillID, group.SkillLevel, tick, 0, &unit);
            break;
        }

        case ROWOW_UNT_BASILICA:
        {
            // Each interval an enemy in it is pushed out (back from the way it faces), and anyone else not yet in its status gets it
            int32 i = BattleCheckTargetSkillUnit(group, map, bl, ROWOW_BCT_ENEMY);
            if (i > 0)
            {
                SkillBlown(ss, bl, skill->Knockback[ROWOWSkill::GetLevelIndex(group.SkillLevel)], int8(UnitGetDir(bl)), ROWOW_BLOWN_NONE, &unit);
                break;
            }
            if (i <= 0 && GetSCE(bl, ROWOW_SC_BASILICA) == nullptr)
                SCStart4(ss, bl, skill->StatusID, 100, 0, 0, group.GroupID, int32(ss->GetGUID().GetCounter()), group.Limit);
            break;
        }

        case ROWOW_UNT_FIREPILLAR_WAITING:
            // Someone stepped near: the pillar goes up (its active unit), and the waiting one goes
            GetSkillUnitData(ss)->SkillZ = unit.Z; // The cell's ground (skillx and skilly's, here the unit's)
            SkillUnitSetting(ss, group.SkillID, group.SkillLevel, unit.X, unit.Y, 1);
            SkillDelUnit(map, group, index);
            break;

        case ROWOW_UNT_SKIDTRAP:
        {
            // Knockback away from position of user during placement [Playtester]
            int32 blX = 0, blY = 0;
            GetUnitCell(bl, blX, blY);
            int32 srcX = group.Val1 >> 16, srcY = group.Val1 & 0xFFFF;
            SkillBlown(ss, bl, skill->Knockback[ROWOWSkill::GetLevelIndex(group.SkillLevel)], int8(GetRODirection(float(srcX - blX), float(srcY - blY))),
                ROWOW_BLOWN_NONE, &unit);
            group.UnitID = ROWOW_UNT_USED_TRAPS;
            ClifChangeTrapLook(map, group, index, bl);
            group.Limit = int64(tick - group.Tick) + ROWOW_TRAP_USED_LIMIT;
            // In pre-renewal, if target was a monster, it will unlock target and become idle (mob_unlocktarget)
            if (Creature* md = bl->ToCreature(); md != nullptr && md->IsAIEnabled == true && md->IsInCombat() == true)
                md->AI()->EnterEvadeMode(CreatureAI::EVADE_REASON_OTHER);
            break;
        }

        case ROWOW_UNT_ANKLESNARE:
        {
            // It holds the first it catches (val2) for its time, less with AGI (a fifth for bosses), at least 3 + 0.03 a caster's level seconds; the
            // caught are pulled onto its cell, and it stays as long as it holds them
            if (group.Val2 != 0)
                break;
            int64 sec = SkillGetTime2(group.SkillID, group.SkillLevel);
            ROWOWStatus const* sstatus = GetROStatus(ss);
            ROWOWStatus const* tstatus = GetROStatus(bl);
            int64 mintime = 30 * (int64(sstatus != nullptr ? sstatus->Level : 1) + 100);
            if ((tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0)
                sec /= 5; // Bosses cannot activate Ankle Snare in renewal so we don't need this code
            sec = std::max((sec * tstatus->Stats[ROWOW_STAT_AGI]) / -200 + sec, mintime);
            if (StatusChangeStart(ss, bl, skill->StatusID, 10000, group.SkillLevel, group.GroupID, 0, 0, sec, ROWOW_SCSTART_NORATEDEF) == true)
            {
                if (ROWOWStatusChangeEntry const* ankle = GetSCE(bl, skill->StatusID); ankle != nullptr && ankle->TimerTime != 0)
                    sec = int64(ankle->TimerTime) - int64(GetTick());
                if (bl->IsCreature() == false || (tstatus->Mode & ROWOW_MODE_KNOCKBACK_IMMUNE) == 0) // unit_blown_immune
                    UnitMovePos(bl, unit.X, unit.Y);
                group.Val2 = int32(bl->GetGUID().GetCounter());
                group.Val2GUID = bl->GetGUID();
            }
            else
                sec = 3000; // Couldn't trap it?
            ClifChangeTrapLook(map, group, index, bl);
            group.Limit = int64(tick - group.Tick) + sec;
            group.Interval = -1;
            unit.Range = 0;
            break;
        }

        case ROWOW_UNT_FLASHER:
        {
            // Flasher doesn't activate on plant bosses
            ROWOWStatus const* tstatus = GetROStatus(bl);
            if (bl->IsCreature() == true && tstatus->Class == ROWOW_CLASS_BOSS && tstatus->Race == ROWOW_RACE_PLANT)
                break;
            [[fallthrough]];
        }
        case ROWOW_UNT_LANDMINE:
        case ROWOW_UNT_BLASTMINE:
        case ROWOW_UNT_SHOCKWAVE:
        case ROWOW_UNT_SANDMAN:
        case ROWOW_UNT_FREEZINGTRAP:
        case ROWOW_UNT_FIREPILLAR_ACTIVE:
        case ROWOW_UNT_CLAYMORETRAP:
        {
            // A trap goes off: it hits what set it off, or (with a splash) everything around its cell, or around the one that set it off for
            // Sandman and Freezing Trap (skill_trap_splash, each its attack), then stays a used trap for 1.5 s.  (Claymore Trap's blast also
            // springs other traps in it: traps aren't attack targets here)
            if (GetSCE(bl, ROWOW_SC__MANHOLE) != nullptr)
                break;
            int32 centerX = unit.X, centerY = unit.Y;
            if (group.UnitID == ROWOW_UNT_SANDMAN || group.UnitID == ROWOW_UNT_FREEZINGTRAP)
                GetUnitCell(bl, centerX, centerY); // These have their effect centered around the unit that triggered the trap
            if (int32 splashRange = skill->SplashArea[ROWOWSkill::GetLevelIndex(group.SkillLevel)]; splashRange == 0)
            {
                // If no splash range, it only hits the unit that activated the trap
                if (skill->NK[ROWOW_NK_NODAMAGE] == true)
                    SkillAdditionalEffect(ss, bl, group.SkillID, group.SkillLevel, ROWOW_BF_MISC, ROWOW_ATK_DEF, tick);
                else
                    SkillAttack(skill->AttackType, ss, ss, bl, skillID, group.SkillLevel, tick, 0, &unit);
            }
            else
            {
                ROWOWSkillUnitGroup& trapGroup = group;
                MapForEachInCellArea(map, bl->GetPhaseMask(), centerX - splashRange, centerY - splashRange, centerX + splashRange, centerY + splashRange,
                    [this, map, &trapGroup, index, tick](Unit* target)
                {
                    return SkillTrapSplash(map, target, trapGroup, index, tick);
                });
            }
            if (group.UnitID != ROWOW_UNT_FIREPILLAR_ACTIVE && (BattleConfig.MultiTriggerTrap == 0 || unit.Range >= 0))
                ClifChangeTrapLook(map, group, index, bl);
            group.Limit = int64(tick - group.Tick) + ROWOW_TRAP_USED_LIMIT;
            if (BattleConfig.MultiTriggerTrap == 1)
                unit.Range = -1; // Trap will still process all units on it and will then be disabled in the calling function
            else
                group.UnitID = ROWOW_UNT_USED_TRAPS; // Change ID so it does not invoke for each in area again
            break;
        }

        case ROWOW_UNT_TALKIEBOX:
            // Its message to those around, once, for 5 seconds
            if (group.SrcGUID == bl->GetGUID())
                break;
            if (group.Val2 == 0)
            {
                ClifTalkieBox(map, unit, group.ValStr);
                group.UnitID = ROWOW_UNT_USED_TRAPS;
                ClifChangeTrapLook(map, group, index, bl);
                group.Limit = int64(tick - group.Tick) + 5000;
                group.Val2 = -1;
            }
            break;

        case ROWOW_UNT_LULLABY:
            if (ss == bl)
                break;
            SkillAdditionalEffect(ss, bl, group.SkillID, group.SkillLevel, ROWOW_BF_LONG | ROWOW_BF_SKILL | ROWOW_BF_MISC, ROWOW_ATK_DEF, tick);
            break;

        case ROWOW_UNT_UGLYDANCE: // Ugly Dance [Skotlex]
            if (ss != bl)
                SkillAdditionalEffect(ss, bl, group.SkillID, group.SkillLevel, ROWOW_BF_LONG | ROWOW_BF_SKILL | ROWOW_BF_MISC, ROWOW_ATK_DEF, tick);
            break;

        case ROWOW_UNT_DISSONANCE:
            SkillAttack(ROWOW_BF_MISC, ss, ss, bl, group.SkillID, group.SkillLevel, tick, 0, &unit);
            break;

        case ROWOW_UNT_APPLEIDUN:
        {
            // Apple of Idun [Skotlex]: heals those in it each interval (not its performer, but with the Bard and Dancer spirit), shown as the
            // heal of Heal
            ROWOWStatusChangeEntry const* spirit = GetSCE(bl, ROWOW_SC_SPIRIT);
            if (group.SrcGUID == bl->GetGUID() && (spirit == nullptr || spirit->Val2 != ROWOW_SKILL_SL_BARDDANCER))
                break; // affects self only when soullinked
            int32 heal = SkillCalcHeal(ss, bl, group.SkillID, group.SkillLevel, true);
            StatusHeal(bl, heal, 0, 0, ss, GetSkillSpellID(ROWOW_SKILL_AL_HEAL, 1));
            break;
        }

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
    // skill_unit_onout: what stepping out of the unit's cell does.  A performer stepping out of its own song keeps it (-1: no onleft)
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
        case ROWOW_UNT_BASILICA:
            // Its caster keeps it (it ends with the group)
            if (sce != nullptr && sce->Val4 != int32(bl->GetGUID().GetCounter()))
                StatusChangeEnd(bl, type);
            break;
        case ROWOW_UNT_HERMODE: // Clear Hermode if the owner moved.
            if (sce != nullptr && sce->Val3 == ROWOW_BCT_SELF && sce->Val4 == group.GroupID)
                StatusChangeEnd(bl, type);
            break;
        case ROWOW_UNT_DISSONANCE:
        case ROWOW_UNT_UGLYDANCE:
        case ROWOW_UNT_WHISTLE:
        case ROWOW_UNT_ASSASSINCROSS:
        case ROWOW_UNT_POEMBRAGI:
        case ROWOW_UNT_APPLEIDUN:
        case ROWOW_UNT_HUMMING:
        case ROWOW_UNT_DONTFORGETME:
        case ROWOW_UNT_FORTUNEKISS:
        case ROWOW_UNT_SERVICEFORYOU:
        {
            ROWOWStatusChangeEntry const* spirit = GetSCE(bl, ROWOW_SC_SPIRIT);
            if (group.SrcGUID == bl->GetGUID() && (spirit == nullptr || spirit->Val2 != ROWOW_SKILL_SL_BARDDANCER))
                return -1;
            break;
        }
    }
    return group.SkillID;
}

int32 ROWOWMod::SkillUnitOnLeft(uint16 skillID, Unit* bl, uint64 tick)
{
    // skill_unit_onleft: what leaving every cell of a skill does (the char stepped out of its last one).  A performer stepping out of its own
    // ensemble stops dancing it; a song's effect stays on a player for its Duration2 after (20 seconds), a monster's ends.  (The other jobs'
    // cases come with them)
    ROWOWSkill const* skill = GetSkill(skillID);
    int32 type = skill != nullptr ? skill->StatusID : ROWOW_SC_NONE;
    ROWOWStatusChangeEntry const* sce = type != ROWOW_SC_NONE ? GetSCE(bl, type) : nullptr;
    switch (skillID)
    {
        case ROWOW_SKILL_WZ_QUAGMIRE:
            if (bl->IsCreature() == true)
                break; // A monster keeps it for its time
            if (sce != nullptr)
                StatusChangeEnd(bl, type);
            break;
        case ROWOW_SKILL_NJ_SUITON:
            if (sce != nullptr)
                StatusChangeEnd(bl, type);
            break;
        case ROWOW_SKILL_BD_LULLABY:
        case ROWOW_SKILL_BD_RICHMANKIM:
        case ROWOW_SKILL_BD_ETERNALCHAOS:
        case ROWOW_SKILL_BD_DRUMBATTLEFIELD:
        case ROWOW_SKILL_BD_RINGNIBELUNGEN:
        case ROWOW_SKILL_BD_ROKISWEIL:
        case ROWOW_SKILL_BD_INTOABYSS:
        case ROWOW_SKILL_BD_SIEGFRIED:
            // Check if you just stepped out of your ensemble skill to cancel dancing. [Skotlex]  We don't check for SC_LONGING because someone could
            // always have knocked you back and out of the song/dance.  (rAthena's FIXME: it only checks that it's the same ensemble, not whose)
            if (ROWOWStatusChangeEntry const* dancing = GetSCE(bl, ROWOW_SC_DANCING); dancing != nullptr && (dancing->Val1 & 0xFFFF) == skillID)
                StatusChangeEnd(bl, ROWOW_SC_DANCING);
            // Sleep effect of Lullaby doesn't end even if song ends
            if (skillID == ROWOW_SKILL_BD_LULLABY)
                break;
            sce = type != ROWOW_SC_NONE ? GetSCE(bl, type) : nullptr;
            [[fallthrough]];
        case ROWOW_SKILL_CG_HERMODE:
            if (sce != nullptr)
                StatusChangeEnd(bl, type);
            break;
        case ROWOW_SKILL_BA_POEMBRAGI:
        case ROWOW_SKILL_BA_WHISTLE:
        case ROWOW_SKILL_BA_ASSASSINCROSS:
        case ROWOW_SKILL_BA_APPLEIDUN:
        case ROWOW_SKILL_DC_HUMMING:
        case ROWOW_SKILL_DC_DONTFORGETME:
        case ROWOW_SKILL_DC_FORTUNEKISS:
        case ROWOW_SKILL_DC_SERVICEFORYOU:
            if (bl->IsPlayer() == true && sce != nullptr && sce->Val4 == 0)
            {
                // NOTE: It'd be nice if we could get the skill_lv for a more accurate extra time, but alas... not possible on our current
                // implementation.  The icon keeps showing it as endless (refresh_song_icon off)
                ROWOWStatusChangeEntry* songSCE = GetStatusChange(bl)->GetSCE(type);
                songSCE->Val4 = 1; // Store the fact that this is a "reduced" duration effect.
                songSCE->TimerTime = tick + uint64(SkillGetTime2(skillID, 1));
            }
            // Non-players don't have lingering effects
            else if (bl->IsPlayer() == false && sce != nullptr)
                StatusChangeEnd(bl, type);
            break;
        case ROWOW_SKILL_MG_SAFETYWALL:
        case ROWOW_SKILL_AL_PNEUMA:
        case ROWOW_SKILL_SA_VOLCANO:
        case ROWOW_SKILL_SA_DELUGE:
        case ROWOW_SKILL_SA_VIOLENTGALE:
        case ROWOW_SKILL_HW_GRAVITATION:
            if (sce != nullptr)
                StatusChangeEnd(bl, type);
            break;
        case ROWOW_SKILL_PF_FOGWALL:
            if (sce != nullptr)
            {
                StatusChangeEnd(bl, type);
                // Players get blind ended inmediately, others have it still for 30 secs. [Skotlex]
                if (ROWOWStatusChange* sc = GetStatusChange(bl); sc != nullptr && sc->HasSCE(ROWOW_SC_BLIND) == true)
                {
                    if (bl->IsPlayer() == true)
                        StatusChangeEnd(bl, ROWOW_SC_BLIND);
                    else
                    {
                        ROWOWStatusChangeEntry* blind = sc->GetSCE(ROWOW_SC_BLIND);
                        blind->TimerTime = tick + 30000;
                        blind->EndTime = blind->TimerTime;
                    }
                }
            }
            break;
    }
    return skillID;
}

void ROWOWMod::SkillUnitEffect(Map* map, Unit* bl, ROWOWSkillUnitGroup& group, int32 index, uint64 tick, int32 flag)
{
    // skill_unit_effect: a unit cell placed (flag&1: onplace) or removed (onout, and flag&4 an onleft).  A song cell where another song (or
    // dance) overlaps it acts as Dissonance (or Ugly Dance) meanwhile, unless flag&8
    ROWOWSkillUnit const& unit = group.Units[index];
    if ((unit.IsAlive == false && (flag & 4) == 0) || bl->IsInWorld() == false)
        return;
    bool dissonance = false;
    if ((flag & 8) == 0)
        dissonance = SkillDanceSwitch(group, index, false);
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
    if (dissonance == true)
        SkillDanceSwitch(group, index, true);
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
        // clif_getareachar_skillunit: a Graffiti shows its message to the players it comes into view of
        if (group->UnitID == ROWOW_UNT_GRAFFITI)
            ClifGraffiti(map, *group);
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
                    case ROWOW_UNT_ICEWALL:
                        unit.Val1 -= 50; // icewall loses 50 hp every second
                        group->Limit = int64(tick) + group->Interval - int64(group->Tick);
                        unit.Limit = group->Limit;
                        if (unit.Val1 <= 0)
                            SkillDelUnit(map, *group, index);
                        break;
                    case ROWOW_UNT_BLASTMINE:
                    case ROWOW_UNT_GROUNDDRIFT_WIND:
                    case ROWOW_UNT_GROUNDDRIFT_DARK:
                    case ROWOW_UNT_GROUNDDRIFT_POISON:
                    case ROWOW_UNT_GROUNDDRIFT_WATER:
                    case ROWOW_UNT_GROUNDDRIFT_FIRE:
                        group->UnitID = ROWOW_UNT_USED_TRAPS;
                        group->Limit = int64(tick) + ROWOW_TRAP_USED_LIMIT - int64(group->Tick);
                        unit.Limit = group->Limit;
                        break;
                    case ROWOW_UNT_ANKLESNARE:
                        if (group->Val2 > 0)
                        {
                            // Used Trap doesn't return back to item
                            SkillDelUnit(map, *group, index);
                            break;
                        }
                        [[fallthrough]];
                    case ROWOW_UNT_SKIDTRAP:
                    case ROWOW_UNT_LANDMINE:
                    case ROWOW_UNT_SHOCKWAVE:
                    case ROWOW_UNT_SANDMAN:
                    case ROWOW_UNT_FLASHER:
                    case ROWOW_UNT_FREEZINGTRAP:
                    case ROWOW_UNT_CLAYMORETRAP:
                    case ROWOW_UNT_TALKIEBOX:
                        // revert unit back into a trap (an unspent one, set by a player)
                        if (unit.Val1 > 0)
                            SkillTrapGiveBack(GetSkillUnitGroupSource(map, group->SrcGUID), *group);
                        SkillDelUnit(map, *group, index);
                        break;
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
                        if (group->Val2 == 1 && group->SkillID == ROWOW_SKILL_WZ_METEOR)
                            break; // Deal damage before expiration
                        SkillDelUnit(map, *group, index);
                        break;
                }
            }
            else if (group->UnitID == ROWOW_UNT_BLASTMINE || group->UnitID == ROWOW_UNT_SKIDTRAP || group->UnitID == ROWOW_UNT_LANDMINE ||
                group->UnitID == ROWOW_UNT_SHOCKWAVE || group->UnitID == ROWOW_UNT_SANDMAN || group->UnitID == ROWOW_UNT_FLASHER ||
                group->UnitID == ROWOW_UNT_CLAYMORETRAP || group->UnitID == ROWOW_UNT_FREEZINGTRAP || group->UnitID == ROWOW_UNT_TALKIEBOX ||
                group->UnitID == ROWOW_UNT_ANKLESNARE)
            {
                // skill unit is still active: a trap whose HP is gone (skill_unit_ondamaged) is spent
                if (unit.Val1 <= 0)
                {
                    if (group->UnitID == ROWOW_UNT_ANKLESNARE && group->Val2 > 0)
                        SkillDelUnit(map, *group, index);
                    else
                    {
                        group->Limit = int64(tick) - int64(group->Tick) + ROWOW_TRAP_USED_LIMIT;
                        group->UnitID = ROWOW_UNT_USED_TRAPS;
                    }
                }
            }
            else if (group->SkillID == ROWOW_SKILL_WZ_METEOR)
            {
                // skill unit is still active: a meteor waits for the interval before it expires, then starts dropping (its look on the cell);
                // all meteors hit 700ms after the effect starts, regardless of their interval.  No damage until expiration.  (The other jobs'
                // cases come with them)
                if (group->Val2 == 0 && (age >= group->Limit - group->Interval || age >= unit.Limit - group->Interval))
                {
                    if (Unit* src = GetSkillUnitGroupSource(map, group->SrcGUID))
                    {
                        ClifSkillPosEffect(src, group->SkillID, group->SkillLevel, unit.X, unit.Y, unit.Z);
                        group->Val2 = 1;
                        group->Limit = group->Limit - group->Interval + 700;
                    }
                }
                continue;
            }

            // Don't continue if unit or even group is expired and has been deleted.
            if (unit.IsAlive == false || mapUnits->GroupsByID.find(group->GroupID) == mapUnits->GroupsByID.end())
                continue;
            bool dissonance = SkillDanceSwitch(*group, index, false);
            if (unit.Range >= 0 && group->Interval != -1)
            {
                // skill_unit_timer_sub_onplace on every char in the unit's reach
                ROWOWSkillUnitGroup& unitGroup = *group;
                MapForEachInCellArea(map, PHASEMASK_NORMAL, unit.X - unit.Range, unit.Y - unit.Range, unit.X + unit.Range, unit.Y + unit.Range,
                    [this, map, &unitGroup, index, tick](Unit* bl)
                {
                    ROWOWStatus const* status = GetROStatus(bl);
                    if (unitGroup.Units[index].IsAlive == false || status == nullptr || (unitGroup.BLFlag & GetBattleObjectType(*status)) == 0)
                        return 0;
                    // AoE skills are ineffective on Land Protector (all but songs, traps and the skills that ignore it). [Skotlex]
                    ROWOWSkill const* skill = GetSkill(unitGroup.SkillID);
                    if (skill != nullptr && skill->Inf2[ROWOW_INF2_ISSONG] == false && skill->Inf2[ROWOW_INF2_ISTRAP] == false &&
                        skill->Inf2[ROWOW_INF2_IGNORELANDPROTECTOR] == false && IsCellLandProtected(map, unitGroup.Units[index], bl) == true)
                        return 0;
                    if (BattleCheckTargetSkillUnit(unitGroup, map, bl, unitGroup.TargetFlag) <= 0)
                        return 0;
                    SkillUnitOnPlaceTimer(map, unitGroup, index, bl, tick);
                    return 1;
                });
                if (unit.Range == -1) // Unit disabled, but it should not be deleted yet.
                    group->UnitID = ROWOW_UNT_USED_TRAPS;
                else if (group->UnitID == ROWOW_UNT_TATAMIGAESHI)
                {
                    unit.Range = -1; // Disable processed cell.
                    if (--group->Val1 <= 0)
                    {
                        // number of live cells.  All tiles were processed, disable skill.
                        group->TargetFlag = ROWOW_BCT_NOONE;
                        group->BLFlag = 0;
                    }
                }
                else if (group->UnitID == ROWOW_UNT_SANCTUARY && group->Val1 <= 0)
                    SkillDelUnitGroup(map, group); // Its hits on the undead are spent
                else if (group->SkillID == ROWOW_SKILL_WZ_METEOR || ((group->SkillID == ROWOW_SKILL_CR_GRANDCROSS || group->SkillID == ROWOW_SKILL_NPC_GRANDDARKNESS) &&
                    unit.Val1 <= 0))
                {
                    SkillDelUnit(map, *group, index); // A meteor goes after its one strike
                    continue;
                }
            }
            if (dissonance == true)
                SkillDanceSwitch(*group, index, true);
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
        bool dissonance = SkillDanceSwitch(group, index, false);
        // Necessary in case the group is deleted after calling on_place/on_out [Skotlex]
        uint16 skillID = group.SkillID;
        ROWOWSkill const* skill = GetSkill(skillID);
        if (group.Interval != -1 && (skill == nullptr || skill->UnitFlags[ROWOW_UF_DUALMODE] == false) && skillID != ROWOW_SKILL_BD_LULLABY) // Lullaby is the exception, bugreport:411
        {
            // Non-dualmode unit skills with a timer don't trigger when walking, so just return
            if (dissonance == true)
            {
                SkillDanceSwitch(group, index, true);
                // This activates the 20 seconds countdown from leaving the song/dance due to dissonance
                int32 result = SkillUnitOnOut(group, index, bl, tick);
                if (result > 0)
                    SkillUnitOnLeft(uint16(result), bl, tick);
            }
            return 0;
        }

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
            if (dissonance == true)
                SkillDanceSwitch(group, index, true);
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
        // Currently it is safe because we know song/dance cells do not get deleted within them. [Skotlex]
        if (dissonance == true)
            SkillDanceSwitch(group, index, true);
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
    // An Ice Wall cell is a wall (map_setgatcell 5): a char's own walk can't take it into or across one, so it goes back to the last cell
    // before the wall, from where its next update goes on.  The server sees a walk a few cells at a time, so the cells between the last one and
    // this are checked too (a longer jump, a teleport, isn't a walk)
    int32 stepCount = std::max(std::abs(x - ud->SkillUnitCellX), std::abs(y - ud->SkillUnitCellY));
    if (stepCount <= ROWOW_ICEWALL_WALK_CHECK_CELLS && MapGetCellIceWall(unit->GetMap(), ud->SkillUnitCellX, ud->SkillUnitCellY) == false)
    {
        int32 lastX = ud->SkillUnitCellX, lastY = ud->SkillUnitCellY;
        for (int32 step = 1; step <= stepCount; step++)
        {
            int32 stepX = ud->SkillUnitCellX + int32(std::lround(double(x - ud->SkillUnitCellX) * step / stepCount));
            int32 stepY = ud->SkillUnitCellY + int32(std::lround(double(y - ud->SkillUnitCellY) * step / stepCount));
            if (MapGetCellIceWall(unit->GetMap(), stepX, stepY) == true)
            {
                UnitMovePos(unit, lastX, lastY);
                return;
            }
            lastX = stepX;
            lastY = stepY;
        }
    }
    int32 dx = x - ud->SkillUnitCellX, dy = y - ud->SkillUnitCellY;
    SkillUnitMove(unit, tick, 2, ud->SkillUnitCellX, ud->SkillUnitCellY, skillUnitCell);
    ud->SkillUnitCellX = x;
    ud->SkillUnitCellY = y;
    SkillUnitMove(unit, tick, 3, x, y, skillUnitCell);
    // A performer's song or dance goes along with it (map_moveblock)
    if (ROWOWStatusChangeEntry const* dancing = GetSCE(unit, ROWOW_SC_DANCING))
        SkillUnitMoveUnitGroup(unit->GetMap(), SkillID2Group(unit->GetMap(), dancing->Val2), dx, dy);
    else if (ROWOWStatusChangeEntry const* warm = GetSCE(unit, ROWOW_SC_WARM))
        SkillUnitMoveUnitGroup(unit->GetMap(), SkillID2Group(unit->GetMap(), warm->Val4), dx, dy);
    if (Player* sd = unit->ToPlayer()) // unit_walktoxy_timer's step
        PCCellBasilica(sd);
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

namespace
{
    // The look of a song or dance cell, shown again after it moved or turned into Dissonance (or Ugly Dance) and back: its group's per-cell
    // unit spell (Dissonance's or Ugly Dance's while it overlaps), for the time the group has left
    void RefreshSkillUnitCellVisual(Map* map, ROWOWSkillUnitGroup& group, int32 index)
    {
        ROWOWSkillUnit& unit = group.Units[index];
        RemoveSkillUnitVisual(map, unit.VisualGUID);
        Unit* src = GetSkillUnitGroupSource(map, group.SrcGUID);
        if (unit.IsAlive == false || unit.IsHidden == true || src == nullptr)
            return;
        uint16 lookSkillID = group.SkillID;
        if ((group.SongDance & 0x1) != 0)
            lookSkillID = (unit.Val2 & (1 << ROWOW_UF_ENSEMBLE)) == 0 ? uint16(unit.Val1) :
                ((unit.Val2 & (1 << ROWOW_UF_SONG)) != 0 ? ROWOW_SKILL_BA_DISSONANCE : ROWOW_SKILL_DC_UGLYDANCE);
        ROWOWSkill const* skill = ROWOW->GetSkill(lookSkillID);
        if (skill == nullptr)
            return;
        size_t levelIndex = size_t(ROWOWSkill::GetLevelIndex(std::min<uint16>(group.SkillLevel, skill->MaxLevel)));
        if (skill->UnitSpellIDsByLevel.size() <= levelIndex || skill->UnitSpellIDsByLevel[levelIndex] == 0 || skill->IsUnitSpellPerCellByLevel[levelIndex] == false)
            return;
        float x = 0, y = 0;
        ROWOW->GetCellWorldPosition(map->GetId(), unit.X, unit.Y, x, y);
        ROWOWMapGrid const* mapGrid = ROWOW->GetMapGrid(map->GetId());
        int64 duration = std::min(group.Limit, unit.Limit) - (int64(ROWOWMod::GetTick()) - int64(group.Tick));
        if (duration > 0)
            unit.VisualGUID = CreateSkillUnitVisual(src, skill->UnitSpellIDsByLevel[levelIndex], x, y, unit.Z, (mapGrid != nullptr ? mapGrid->CellSize : ROWOW->CellSize) * 0.5f,
                duration);
    }

    // map_moveblock for a skill unit: its cell (and the ground under it) changes, and its look goes along
    void SkillUnitMoveBlock(Map* map, ROWOWMapSkillUnits* mapUnits, ROWOWSkillUnitGroup& group, int32 index, int32 x, int32 y)
    {
        ROWOWSkillUnit& unit = group.Units[index];
        auto cellIter = mapUnits->UnitsByCell.find(GetCellKey(unit.X, unit.Y));
        if (cellIter != mapUnits->UnitsByCell.end())
        {
            std::vector<std::pair<int32, int32>>& cellUnits = cellIter->second;
            cellUnits.erase(std::remove(cellUnits.begin(), cellUnits.end(), std::make_pair(group.GroupID, index)), cellUnits.end());
            if (cellUnits.empty() == true)
                mapUnits->UnitsByCell.erase(cellIter);
        }
        float cellX = 0, cellY = 0;
        ROWOW->GetCellWorldPosition(map->GetId(), x, y, cellX, cellY);
        float cellZ = map->GetHeight(PHASEMASK_NORMAL, cellX, cellY, unit.Z + ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT, true, ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE);
        if (cellZ > INVALID_HEIGHT)
            unit.Z = cellZ;
        unit.X = x;
        unit.Y = y;
        mapUnits->UnitsByCell[GetCellKey(x, y)].emplace_back(group.GroupID, index);
        RefreshSkillUnitCellVisual(map, group, index);
    }
}

int32 ROWOWMod::SkillTrapSplash(Map* map, Unit* bl, ROWOWSkillUnitGroup& group, int32 index, uint64 tick)
{
    // skill_trap_splash: what a trap that went off does to each char in its splash: Sandman's sleep, the others' attack (split among those
    // around for a split skill), from the trap's cell.  (Claymore Trap springing the other traps in its blast needs traps as targets)
    ROWOWSkillUnit const& unit = group.Units[index];
    if (unit.IsAlive == false || bl->IsInWorld() == false)
        return 0;
    Unit* ss = GetSkillUnitGroupSource(map, group.SrcGUID);
    ROWOWSkill const* skill = GetSkill(group.SkillID);
    if (ss == nullptr || skill == nullptr || BattleCheckTargetSkillUnit(group, map, bl, group.TargetFlag) <= 0)
        return 0;
    switch (group.UnitID)
    {
        case ROWOW_UNT_SANDMAN:
            SkillAdditionalEffect(ss, bl, group.SkillID, group.SkillLevel, ROWOW_BF_MISC, ROWOW_ATK_DEF, tick);
            break;
        case ROWOW_UNT_GROUNDDRIFT_WIND:
            if (SkillAttack(skill->AttackType, ss, ss, bl, group.SkillID, group.SkillLevel, tick, group.Val1, &unit) != 0)
                SCStart(ss, bl, ROWOW_SC_STUN, 50, group.SkillLevel, SkillGetTime2(group.SkillID, 1));
            break;
        case ROWOW_UNT_GROUNDDRIFT_DARK:
            if (SkillAttack(skill->AttackType, ss, ss, bl, group.SkillID, group.SkillLevel, tick, group.Val1, &unit) != 0)
                SCStart(ss, bl, ROWOW_SC_BLIND, 50, group.SkillLevel, SkillGetTime2(group.SkillID, 2));
            break;
        case ROWOW_UNT_GROUNDDRIFT_POISON:
            if (SkillAttack(skill->AttackType, ss, ss, bl, group.SkillID, group.SkillLevel, tick, group.Val1, &unit) != 0)
                SCStart2(ss, bl, ROWOW_SC_POISON, 50, group.SkillLevel, int32(ss->GetGUID().GetCounter()), SkillGetTime2(group.SkillID, 3));
            break;
        case ROWOW_UNT_GROUNDDRIFT_WATER:
            if (SkillAttack(skill->AttackType, ss, ss, bl, group.SkillID, group.SkillLevel, tick, group.Val1, &unit) != 0)
                SCStart(ss, bl, ROWOW_SC_FREEZE, 50, group.SkillLevel, SkillGetTime2(group.SkillID, 4));
            break;
        case ROWOW_UNT_GROUNDDRIFT_FIRE:
            if (SkillAttack(skill->AttackType, ss, ss, bl, group.SkillID, group.SkillLevel, tick, group.Val1, &unit) != 0)
                SkillBlown(ss, bl, skill->Knockback[ROWOWSkill::GetLevelIndex(group.SkillLevel)], -1, ROWOW_BLOWN_NONE, &unit);
            break;
        default:
        {
            int32 splitCount = 0;
            if (skill->NK[ROWOW_NK_SPLASHSPLIT] == true)
            {
                int32 splashArea = skill->SplashArea[ROWOWSkill::GetLevelIndex(group.SkillLevel)];
                splitCount = std::max(1, MapForEachInCellArea(map, bl->GetPhaseMask(), unit.X - splashArea, unit.Y - splashArea, unit.X + splashArea,
                    unit.Y + splashArea, [this, ss](Unit* splashTarget) { return BattleCheckTarget(ss, splashTarget, ROWOW_BCT_ENEMY) > 0 ? 1 : 0; }));
            }
            SkillAttack(skill->AttackType, ss, ss, bl, group.SkillID, group.SkillLevel, tick, splitCount, &unit);
            break;
        }
    }
    return 1;
}

void ROWOWMod::ClifChangeTrapLook(Map* map, ROWOWSkillUnitGroup& group, int32 index, Unit* target)
{
    // clif_changetraplook (UNT_USED_TRAPS): a trap went off.  Its spring look (the trap's spring spell, from the converter) plays on what set
    // it off, and its look on the ground lasts only as long as the spent trap stays
    ROWOWSkill const* skill = GetSkill(group.SkillID);
    Unit* src = GetSkillUnitGroupSource(map, group.SrcGUID);
    size_t levelIndex = size_t(ROWOWSkill::GetLevelIndex(group.SkillLevel));
    if (DynamicObject* dynObj = group.Units[index].VisualGUID.IsEmpty() == false ? map->GetDynamicObject(group.Units[index].VisualGUID) : nullptr)
        dynObj->SetDuration(ROWOW_TRAP_USED_LIMIT);
    if (skill == nullptr || src == nullptr || target == nullptr || target->IsInWorld() == false || skill->SpringSpellIDsByLevel.size() <= levelIndex)
        return;
    if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(skill->SpringSpellIDsByLevel[levelIndex]))
        src->CastSpell(target, spellInfo, TRIGGERED_FULL_MASK);
}

void ROWOWMod::ClifTalkieBox(Map* map, ROWOWSkillUnit const& unit, std::string const& message)
{
    // clif_talkiebox: a Talkie Box's message, shown to the players around it
    MapForEachInCellArea(map, PHASEMASK_NORMAL, unit.X - BattleConfig.AreaSize, unit.Y - BattleConfig.AreaSize, unit.X + BattleConfig.AreaSize,
        unit.Y + BattleConfig.AreaSize, [&message](Unit* bl)
    {
        if (Player* sd = bl->ToPlayer())
            ChatHandler(sd->GetSession()).PSendSysMessage("Talkie Box: {}", message);
        return 0;
    });
}

bool ROWOWMod::SkillDanceSwitch(ROWOWSkillUnitGroup& group, int32 index, bool revert)
{
    // skill_dance_switch: a song or dance cell that overlaps another (val2's UF_ENSEMBLE bit) acts as Dissonance (two songs) or Ugly Dance (two
    // dances), its group switched to that skill for the call; revert puts the original back (val1).  Whether it overlaps
    if ((group.SongDance & 0x1) == 0)
        return false; // Not a song or dance
    ROWOWSkillUnit const& unit = group.Units[index];
    bool overlap = (unit.Val2 & (1 << ROWOW_UF_ENSEMBLE)) != 0;
    // No need to convert if there is no overlap.  But we still need to revert even if the cell is no longer overlapping
    if (revert == false && overlap == false)
        return false;
    // Transform or restore the skill group depending on flag
    uint16 skillID = 0;
    if (revert == false && overlap == true)
        skillID = (unit.Val2 & (1 << ROWOW_UF_SONG)) != 0 ? ROWOW_SKILL_BA_DISSONANCE : ROWOW_SKILL_DC_UGLYDANCE; // Transform
    else
        skillID = uint16(unit.Val1); // Restore (val1 contains original skill ID)
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr)
        return false;
    group.SkillID = skillID;
    group.UnitID = skill->UnitTypeID;
    group.TargetFlag = skill->UnitTarget;
    group.Interval = skill->UnitInterval;
    return overlap;
}

int32 ROWOWMod::SkillDanceOverlap(Map* map, ROWOWSkillUnitGroup& group, int32 index, uint8 flag)
{
    // skill_dance_overlap: a song or dance cell placed (OVERLAP_SET) on the cell of another of the same kind (two songs, or two dances; a song
    // and a dance are fine) marks both as overlapping, which then act as Dissonance or Ugly Dance (those on the cell get an onleft, the song's
    // own effect leaving them); one removed (OVERLAP_REMOVE) takes the mark back, from the other too unless more still overlap
    if ((group.SongDance & 0x1) == 0)
        return 0;
    ROWOWSkillUnit& unit = group.Units[index];
    // skill_dance_overlap_revert: back to its original form and look
    auto revertOverlap = [this, map](ROWOWSkillUnitGroup& revertGroup, int32 revertIndex)
    {
        revertGroup.Units[revertIndex].Val2 &= ~(1 << ROWOW_UF_ENSEMBLE); // Remove overlap signal
        SkillDanceSwitch(revertGroup, revertIndex, true); // If the unit is removed because overlap dissonance killed the caster, we need to reset it here
        RefreshSkillUnitCellVisual(map, revertGroup, revertIndex);
    };
    // skill_dance_overlap_sub: a unit on the same cell that meets the overlap criteria
    auto overlapSub = [this, map, &group, index, &revertOverlap](ROWOWSkillUnitGroup& targetGroup, int32 targetIndex, uint8 subFlag)
    {
        if (&targetGroup == &group && targetIndex == index)
            return 0;
        if ((group.SongDance & 0x1) == 0 || (targetGroup.SongDance & 0x1) == 0)
            return 0;
        if ((targetGroup.Units[targetIndex].Val2 & group.Units[index].Val2 & ~(1 << ROWOW_UF_ENSEMBLE)) == 0) // They don't match (song + dance) is valid.
            return 0;
        if (subFlag == ROWOW_OVERLAP_COUNT) // Just want to count the unit, stop here
            return 1;
        // These action needs to happen for both target (0) and the src (1)
        std::pair<ROWOWSkillUnitGroup*, int32> const overlappingUnits[2] = { std::make_pair(&targetGroup, targetIndex), std::make_pair(&group, index) };
        for (std::pair<ROWOWSkillUnitGroup*, int32> const& overlapping : overlappingUnits)
        {
            ROWOWSkillUnitGroup& overlapGroup = *overlapping.first;
            int32 overlapIndex = overlapping.second;
            ROWOWSkillUnit& overlapUnit = overlapGroup.Units[overlapIndex];
            if (subFlag == ROWOW_OVERLAP_SET && (overlapUnit.Val2 & (1 << ROWOW_UF_ENSEMBLE)) == 0)
            {
                // Set dissonance: add ensemble to signal this unit is overlapping, then trigger onleft event on all affected units on the cell
                overlapUnit.Val2 |= (1 << ROWOW_UF_ENSEMBLE);
                RefreshSkillUnitCellVisual(map, overlapGroup, overlapIndex);
                MapForEachInCellArea(map, PHASEMASK_NORMAL, overlapUnit.X, overlapUnit.Y, overlapUnit.X, overlapUnit.Y, [this, map, &overlapGroup, overlapIndex](Unit* bl)
                {
                    SkillUnitEffect(map, bl, overlapGroup, overlapIndex, GetTick(), 4 | 8);
                    return 0;
                });
            }
            else if (subFlag == ROWOW_OVERLAP_REMOVE && (overlapUnit.Val2 & (1 << ROWOW_UF_ENSEMBLE)) != 0)
            {
                // Remove dissonance, then trigger onplace event on all affected units on the cell.  Don't do this for src as that unit is to be removed
                revertOverlap(overlapGroup, overlapIndex);
                if (&overlapGroup == &targetGroup && overlapIndex == targetIndex)
                {
                    MapForEachInCellArea(map, PHASEMASK_NORMAL, overlapUnit.X, overlapUnit.Y, overlapUnit.X, overlapUnit.Y, [this, map, &overlapGroup, overlapIndex](Unit* bl)
                    {
                        SkillUnitEffect(map, bl, overlapGroup, overlapIndex, GetTick(), 1 | 8);
                        return 0;
                    });
                }
            }
        }
        return 1;
    };

    if (flag == ROWOW_OVERLAP_REMOVE)
    {
        // Nothing to remove, this unit is not overlapped
        if ((unit.Val2 & (1 << ROWOW_UF_ENSEMBLE)) == 0)
            return 0;
        // When the unit is removed, but there are more than one other skill unit that overlaps, dissonance is only removed from the unit that
        // is removed and not the other units
        int32 count = MapForEachSkillUnitInArea(map, unit.X, unit.Y, unit.X, unit.Y, [&overlapSub](ROWOWSkillUnitGroup& targetGroup, int32 targetIndex)
        {
            return overlapSub(targetGroup, targetIndex, ROWOW_OVERLAP_COUNT);
        });
        if (count > 1)
        {
            revertOverlap(group, index);
            return count;
        }
    }
    return MapForEachSkillUnitInArea(map, unit.X, unit.Y, unit.X, unit.Y, [&overlapSub, flag](ROWOWSkillUnitGroup& targetGroup, int32 targetIndex)
    {
        return overlapSub(targetGroup, targetIndex, flag);
    });
}

void ROWOWMod::SkillUnitMoveUnitGroup(Map* map, std::shared_ptr<ROWOWSkillUnitGroup> group, int32 dx, int32 dy)
{
    // skill_unit_move_unit_group: a performer's song or dance moves with it (an ensemble stays).  Only the cells that end up off the old area
    // move (a cell whose new spot another cell of the group held stays put, and the one leaving takes the free spot ahead), each getting the
    // onout/onleft of what it leaves and the onplace of what it moves onto, and its Dissonance checked again
    ROWOWMapSkillUnits* mapUnits = GetMapSkillUnits(map, false);
    ROWOWSkill const* skill = group != nullptr ? GetSkill(group->SkillID) : nullptr;
    if (mapUnits == nullptr || group == nullptr || skill == nullptr || group->Units.empty() == true || (dx == 0 && dy == 0))
        return;
    if (skill->UnitFlags[ROWOW_UF_ENSEMBLE] == true)
        return; // Ensembles may not be moved around.
    uint64 tick = GetTick();
    int32 count = int32(group->Units.size());
    // m_flag: 0 neither of the following (skill_unit_onplace & skill_unit_onout are needed); 1 the unit will move to a slot that had another unit
    // of the same group (skill_unit_onplace not needed); 2 another unit from same group will end up positioned on this unit (skill_unit_onout
    // not needed); 3 both 1+2
    std::vector<int32> mFlag(size_t(count), 0);
    for (int32 i = 0; i < count; i++)
    {
        ROWOWSkillUnit const& unit1 = group->Units[i];
        if (unit1.IsAlive == false)
            continue;
        for (int32 j = 0; j < count; j++)
        {
            ROWOWSkillUnit const& unit2 = group->Units[j];
            if (unit2.IsAlive == false)
                continue;
            if (unit1.X + dx == unit2.X && unit1.Y + dy == unit2.Y)
                mFlag[i] |= 0x1;
            if (unit1.X - dx == unit2.X && unit1.Y - dy == unit2.Y)
                mFlag[i] |= 0x2;
        }
    }
    int32 j = 0;
    for (int32 i = 0; i < count; i++)
    {
        if (mapUnits->GroupsByID.find(group->GroupID) == mapUnits->GroupsByID.end())
            return; // Deleted by what a cell did
        ROWOWSkillUnit& unit1 = group->Units[i];
        if (unit1.IsAlive == false)
            continue;
        if ((mFlag[i] & 0x2) == 0)
        {
            if ((group->SongDance & 0x1) != 0) // Cancel dissonance effect.
                SkillDanceOverlap(map, *group, i, ROWOW_OVERLAP_REMOVE);
            MapForEachInCellArea(map, PHASEMASK_NORMAL, unit1.X, unit1.Y, unit1.X, unit1.Y, [this, map, &group, i, tick](Unit* bl)
            {
                SkillUnitEffect(map, bl, *group, i, tick, 4);
                return 0;
            });
        }
        // Move Cell using "smart" criteria (avoid useless moving around)
        switch (mFlag[i])
        {
            case 0:
                // Cell moves independently, safely move it.
                SkillUnitMoveBlock(map, mapUnits, *group, i, unit1.X + dx, unit1.Y + dy);
                break;
            case 1:
                // Cell moves unto another cell, look for a replacement cell that won't collide and has no cell moving into it (flag == 2)
                for (; j < count; j++)
                {
                    if (mFlag[j] != 2 || group->Units[j].IsAlive == false)
                        continue;
                    // Move to where this cell would had moved.
                    ROWOWSkillUnit const& unit2 = group->Units[j];
                    SkillUnitMoveBlock(map, mapUnits, *group, i, unit2.X + dx, unit2.Y + dy);
                    j++; // Skip this cell as we have used it.
                    break;
                }
                break;
            case 2:
            case 3:
                break; // Don't move the cell as a cell will end on this tile anyway.
        }
        if ((mFlag[i] & 0x2) == 0)
        {
            // We only moved the cell in 0-1
            if ((group->SongDance & 0x1) != 0) // Check for dissonance effect.
                SkillDanceOverlap(map, *group, i, ROWOW_OVERLAP_SET);
            MapForEachInCellArea(map, PHASEMASK_NORMAL, unit1.X, unit1.Y, unit1.X, unit1.Y, [this, map, &group, i, tick](Unit* bl)
            {
                SkillUnitEffect(map, bl, *group, i, tick, 1);
                return 0;
            });
        }
    }
}
