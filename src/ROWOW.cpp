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

#include "Configuration/Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "ModelIgnoreFlags.h"

#include "ROWOW.h"

#include <cmath>

using namespace std;


#include "Configuration/Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "ModelIgnoreFlags.h"

#include "ROWOW.h"

#include <cmath>

using namespace std;

void ROWOWMod::LoadConfigurationFile()
{
    IsEnabled = sConfigMgr->GetOption<bool>("ROWOW.Enabled", true);
    if (IsEnabled == false)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadConfigurationFile has ROWOW.Enabled as false, so the module is deactivated");
        return;
    }

    ConfigMoveStepDistance = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveStepDistance", 1.0f);
    if (ConfigMoveStepDistance < 0.25f)
        ConfigMoveStepDistance = 0.25f;
    ConfigMoveMaxRisePerStep = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveMaxRisePerStep", 1.2f);
    ConfigMoveFloorSearchHeight = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveFloorSearchHeight", 3.0f);
    ConfigMoveFloorSearchDistance = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveFloorSearchDistance", 30.0f);
    ConfigRandomWalkMaxDistance = sConfigMgr->GetOption<float>("ROWOW.Creature.RandomWalkMaxDistance", 7.35f);
    ConfigRandomWalkMinDelayInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.RandomWalkMinDelayInMS", 4000);
    ConfigRandomWalkMaxDelayInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.RandomWalkMaxDelayInMS", 5000);
    if (ConfigRandomWalkMaxDelayInMS < ConfigRandomWalkMinDelayInMS)
        ConfigRandomWalkMaxDelayInMS = ConfigRandomWalkMinDelayInMS;
    ConfigChaseRepathIntervalInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.ChaseRepathIntervalInMS", 250);
    ConfigChaseRepathTargetMoveDistance = sConfigMgr->GetOption<float>("ROWOW.Creature.ChaseRepathTargetMoveDistance", 1.0f);
    ConfigGroundSnapCheckIntervalInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.GroundSnapCheckIntervalInMS", 1000);
    ConfigGroundSnapTolerance = sConfigMgr->GetOption<float>("ROWOW.Creature.GroundSnapTolerance", 0.15f);
    ConfigUnreachableAfterTimeInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.UnreachableAfterTimeInMS", 2000);
    ConfigOutOfReachGiveUpTimeInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.OutOfReachGiveUpTimeInMS", 8000);
    ConfigRangedAttackApproachFraction = sConfigMgr->GetOption<float>("ROWOW.Creature.RangedAttackApproachFraction", 0.8f);
    if (ConfigRangedAttackApproachFraction < 0.1f)
        ConfigRangedAttackApproachFraction = 0.1f;
    if (ConfigRangedAttackApproachFraction > 1.0f)
        ConfigRangedAttackApproachFraction = 1.0f;
    ConfigCellPathing = sConfigMgr->GetOption<bool>("ROWOW.Creature.CellPathing", true);

    // Event intervals of zero would make the AI's event loop run the same event over and over in one update
    if (ConfigRandomWalkMinDelayInMS < 100)
        ConfigRandomWalkMinDelayInMS = 100;
    if (ConfigRandomWalkMaxDelayInMS < ConfigRandomWalkMinDelayInMS)
        ConfigRandomWalkMaxDelayInMS = ConfigRandomWalkMinDelayInMS;
    if (ConfigChaseRepathIntervalInMS < 50)
        ConfigChaseRepathIntervalInMS = 50;
    if (ConfigGroundSnapCheckIntervalInMS < 100)
        ConfigGroundSnapCheckIntervalInMS = 100;
    if (ConfigOutOfReachGiveUpTimeInMS < 1000)
        ConfigOutOfReachGiveUpTimeInMS = 1000;
}

void ROWOWMod::LoadCreatureTemplateData()
{
    CreatureTemplatesByEntry.clear();
    if (IsEnabled == false)
        return;

    // The converter creates the table, so it may not exist yet on a server that hasn't had the ROWOW SQL applied
    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_creature_template'");
    if (!tableResult)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadCreatureTemplateData found no mod_rowow_creature_template table, so every creature is a melee attacker");
        return;
    }
    QueryResult result = WorldDatabase.Query("SELECT `CreatureTemplateID`, `AttackRange`, `AttackVisualSpellID` FROM `mod_rowow_creature_template`");
    if (!result)
        return;
    do
    {
        Field* fields = result->Fetch();
        ROWOWCreatureTemplate creatureTemplate;
        creatureTemplate.AttackRange = fields[1].Get<float>();
        creatureTemplate.AttackVisualSpellID = fields[2].Get<uint32>();
        CreatureTemplatesByEntry[fields[0].Get<uint32>()] = creatureTemplate;
    } while (result->NextRow());
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadCreatureTemplateData loaded {} creature templates", CreatureTemplatesByEntry.size());
}

void ROWOWMod::LoadMapGridData()
{
    MapGridsByMapID.clear();
    if (IsEnabled == false)
        return;

    // The converter creates the table, so it may not exist yet on a server that hasn't had the ROWOW SQL applied
    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_map_grid'");
    if (!tableResult)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadMapGridData found no mod_rowow_map_grid table, so no map has an RO cell grid");
        return;
    }
    QueryResult result = WorldDatabase.Query("SELECT `MapID`, `OriginX`, `OriginY`, `CellSize` FROM `mod_rowow_map_grid`");
    if (!result)
        return;
    do
    {
        Field* fields = result->Fetch();
        ROWOWMapGrid mapGrid;
        mapGrid.OriginX = fields[1].Get<float>();
        mapGrid.OriginY = fields[2].Get<float>();
        mapGrid.CellSize = fields[3].Get<float>();
        if (mapGrid.CellSize < 0.1f)
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadMapGridData has a cell size of {} for map {}, so it's skipped", mapGrid.CellSize, fields[0].Get<uint32>());
            continue;
        }
        CellSize = mapGrid.CellSize;
        MapGridsByMapID[fields[0].Get<uint32>()] = mapGrid;
    } while (result->NextRow());
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadMapGridData loaded the cell grids of {} maps", MapGridsByMapID.size());
}

ROWOWMapGrid const* ROWOWMod::GetMapGrid(uint32 mapID) const
{
    auto mapGridIter = MapGridsByMapID.find(mapID);
    if (mapGridIter == MapGridsByMapID.end())
        return nullptr;
    return &mapGridIter->second;
}

uint8 ROWOWMod::GetRODirection(float deltaX, float deltaY)
{
    // RO's directions: 0 north, 1 north-west, 2 west, 3 south-west, 4 south, 5 south-east, 6 east, 7 north-east (x east, y north)
    if (deltaX == 0 && deltaY == 0)
        return ROWOW_RO_DIRECTION_EAST;
    if (deltaX >= 0 && deltaY >= 0)
    {
        if (deltaX >= deltaY * 3)
            return 6;
        if (deltaX * 3 < deltaY)
            return 0;
        return 7;
    }
    if (deltaX >= 0 && deltaY <= 0)
    {
        if (deltaX >= -deltaY * 3)
            return 6;
        if (deltaX * 3 < -deltaY)
            return 4;
        return 5;
    }
    if (deltaX <= 0 && deltaY <= 0)
    {
        if (deltaX * 3 >= deltaY)
            return 4;
        if (deltaX < deltaY * 3)
            return 2;
        return 3;
    }
    if (-deltaX * 3 <= deltaY)
        return 0;
    if (-deltaX > deltaY * 3)
        return 2;
    return 1;
}

int32 ROWOWMod::MapCheckDir(int32 sDir, int32 tDir)
{
    // map_check_dir: whether tDir is off sDir by more than one of the eight directions (each direction's two neighbours count as it)
    int32 difference = ((tDir - sDir) % ROWOW_RO_DIRECTION_COUNT + ROWOW_RO_DIRECTION_COUNT) % ROWOW_RO_DIRECTION_COUNT;
    return (difference == 0 || difference == 1 || difference == ROWOW_RO_DIRECTION_COUNT - 1) ? 0 : 1;
}

ROWOWCreatureTemplate const* ROWOWMod::GetCreatureTemplate(uint32 creatureEntry) const
{
    auto creatureTemplateIter = CreatureTemplatesByEntry.find(creatureEntry);
    if (creatureTemplateIter == CreatureTemplatesByEntry.end())
        return nullptr;
    return &creatureTemplateIter->second;
}
