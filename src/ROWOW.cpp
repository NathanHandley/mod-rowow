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
#include <cstring>
#include <zlib.h>

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

    ConfigMoveMaxRisePerStep = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveMaxRisePerStep", 1.2f);
    ConfigMoveFloorSearchHeight = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveFloorSearchHeight", 3.0f);
    ConfigMoveFloorSearchDistance = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveFloorSearchDistance", 30.0f);
    ConfigGroundSnapCheckIntervalInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.GroundSnapCheckIntervalInMS", 1000);
    ConfigGroundSnapTolerance = sConfigMgr->GetOption<float>("ROWOW.Creature.GroundSnapTolerance", 0.15f);
    ConfigCorpseTimeInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.CorpseTimeInMS", 3000);

    // Event intervals of zero would make the AI's event loop run the same event over and over in one update
    if (ConfigGroundSnapCheckIntervalInMS < 100)
        ConfigGroundSnapCheckIntervalInMS = 100;
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
    QueryResult result = WorldDatabase.Query("SELECT `CreatureTemplateID`, `AttackVisualSpellID` FROM `mod_rowow_creature_template`");
    if (!result)
        return;
    do
    {
        Field* fields = result->Fetch();
        ROWOWCreatureTemplate creatureTemplate;
        creatureTemplate.AttackVisualSpellID = fields[1].Get<uint32>();
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
    // A stitched map has a row of its own (PartMapKey 0: its size and origin, no cells) and one per member (its map key, its own GAT and its offset
    // there); a conversion from before has one row a map, the stitched maps' cells all in it.  Columns it doesn't have read 0
    auto getColumn = [](char const* columnName)
    {
        bool hasColumn = WorldDatabase.Query("SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_map_grid' AND column_name = '{}'",
            columnName) != nullptr;
        return hasColumn == true ? std::string("`") + columnName + "`" : std::string("0");
    };
    QueryResult result = WorldDatabase.Query("SELECT `MapID`, `OriginX`, `OriginY`, `CellSize`, `Width`, `Height`, `Cells`, {}, {}, {} FROM `mod_rowow_map_grid`",
        getColumn("PartMapKey"), getColumn("CellOffsetX"), getColumn("CellOffsetY"));
    if (!result)
        return;

    // The GAT, deflated (hex): two bits a cell, then the height of each cell's ground in tenths of a yard (int16, little endian).  False when it can't be read
    auto readCells = [](std::string const& cellsHex, int32 width, int32 height, std::vector<uint8>& cellsOut, std::vector<int16>& groundHeightsOut)
    {
        size_t cellCount = size_t(std::max(width, 0)) * size_t(std::max(height, 0));
        if (cellCount == 0 || cellsHex.size() < 2)
            return false;
        auto getNibble = [](char hexDigit) -> uint8
        {
            if (hexDigit >= '0' && hexDigit <= '9')
                return uint8(hexDigit - '0');
            if (hexDigit >= 'a' && hexDigit <= 'f')
                return uint8(hexDigit - 'a' + 10);
            if (hexDigit >= 'A' && hexDigit <= 'F')
                return uint8(hexDigit - 'A' + 10);
            return 0;
        };
        std::vector<uint8> packedBytes(cellsHex.size() / 2);
        for (size_t byteIndex = 0; byteIndex < packedBytes.size(); byteIndex++)
            packedBytes[byteIndex] = uint8((getNibble(cellsHex[byteIndex * 2]) << 4) | getNibble(cellsHex[byteIndex * 2 + 1]));
        size_t packedSize = (cellCount + 3) / 4;
        std::vector<uint8> unpackedBytes(packedSize + cellCount * 2);
        uLongf unpackedSize = uLongf(unpackedBytes.size());
        if (uncompress(unpackedBytes.data(), &unpackedSize, packedBytes.data(), uLong(packedBytes.size())) != Z_OK || unpackedSize != unpackedBytes.size())
            return false;
        cellsOut.assign(unpackedBytes.begin(), unpackedBytes.begin() + packedSize);
        groundHeightsOut.resize(cellCount);
        memcpy(groundHeightsOut.data(), unpackedBytes.data() + packedSize, cellCount * 2);
        return true;
    };
    std::unordered_map<uint32, std::vector<ROWOWMapGridPart>> gridPartsByMapID;
    do
    {
        Field* fields = result->Fetch();
        uint32 mapID = fields[0].Get<uint32>();
        uint32 partMapKey = fields[7].Get<uint32>();
        if (partMapKey != 0)
        {
            ROWOWMapGridPart gridPart;
            gridPart.MapKey = partMapKey;
            gridPart.OffsetX = fields[8].Get<int32>();
            gridPart.OffsetY = fields[9].Get<int32>();
            gridPart.Width = int32(fields[4].Get<uint32>());
            gridPart.Height = int32(fields[5].Get<uint32>());
            if (readCells(fields[6].Get<std::string>(), gridPart.Width, gridPart.Height, gridPart.Cells, gridPart.GroundHeights) == false)
            {
                LOG_ERROR("module.ROWOW", "ROWOWMod::LoadMapGridData can't read the cells of part {} of map {}, so they're skipped (walls)", partMapKey, mapID);
                continue;
            }
            gridPartsByMapID[mapID].push_back(std::move(gridPart));
            continue;
        }
        ROWOWMapGrid mapGrid;
        mapGrid.OriginX = fields[1].Get<float>();
        mapGrid.OriginY = fields[2].Get<float>();
        mapGrid.CellSize = fields[3].Get<float>();
        if (mapGrid.CellSize < 0.1f)
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadMapGridData has a cell size of {} for map {}, so it's skipped", mapGrid.CellSize, mapID);
            continue;
        }
        CellSize = mapGrid.CellSize;
        mapGrid.Width = int32(fields[4].Get<uint32>());
        mapGrid.Height = int32(fields[5].Get<uint32>());

        // A stitched map's own row has no cells (its parts' rows have them)
        std::string cellsHex = fields[6].Get<std::string>();
        if (cellsHex.empty() == false && readCells(cellsHex, mapGrid.Width, mapGrid.Height, mapGrid.Cells, mapGrid.GroundHeights) == false)
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadMapGridData can't read the cells of map {}, so they're skipped", mapID);
            mapGrid.Width = 0;
            mapGrid.Height = 0;
        }
        MapGridsByMapID[mapID] = std::move(mapGrid);
    } while (result->NextRow());

    // Each stitched map's parts onto it, found by block
    size_t gridPartCount = 0;
    for (auto& gridPartsOfMap : gridPartsByMapID)
    {
        auto mapGridIter = MapGridsByMapID.find(gridPartsOfMap.first);
        if (mapGridIter == MapGridsByMapID.end() || mapGridIter->second.Width <= 0 || gridPartsOfMap.second.size() > std::numeric_limits<uint16>::max())
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadMapGridData has {} part grids of map {}, which has no grid of its own (or too many parts), so they're skipped",
                gridPartsOfMap.second.size(), gridPartsOfMap.first);
            continue;
        }
        ROWOWMapGrid& mapGrid = mapGridIter->second;
        if (mapGrid.Cells.empty() == false)
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadMapGridData: map {} has cells of its own and {} part grids; the parts' are used", gridPartsOfMap.first,
                gridPartsOfMap.second.size());
        mapGrid.Cells.clear();
        mapGrid.GroundHeights.clear();
        mapGrid.Parts = std::move(gridPartsOfMap.second);
        std::vector<ROWOWCellRect> partRects;
        for (ROWOWMapGridPart const& gridPart : mapGrid.Parts)
            partRects.push_back({ gridPart.OffsetX, gridPart.OffsetY, gridPart.Width, gridPart.Height });
        mapGrid.PartIndex.Build(partRects, 0);
        gridPartCount += mapGrid.Parts.size();
    }
    for (auto const& mapGridByMapID : MapGridsByMapID)
        if (mapGridByMapID.second.Width > 0 && mapGridByMapID.second.HasCells() == false)
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadMapGridData: map {} has a grid with no cells (no part grids), so all of it is walls", mapGridByMapID.first);
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadMapGridData loaded the cell grids of {} maps ({} stitched map part grids)", MapGridsByMapID.size(), gridPartCount);
}

void ROWOWCellRectIndex::Build(std::vector<ROWOWCellRect> const& rects, int32 margin)
{
    BlocksX = 0;
    BlocksY = 0;
    BlockStarts.clear();
    Entries.clear();
    bool hasRect = false;
    int32 minX = 0, minY = 0, maxX = 0, maxY = 0; // Inclusive, grown by the margin
    for (ROWOWCellRect const& rect : rects)
    {
        if (rect.Width <= 0 || rect.Height <= 0)
            continue;
        int32 rectMinX = rect.X - margin;
        int32 rectMinY = rect.Y - margin;
        int32 rectMaxX = rect.X + rect.Width - 1 + margin;
        int32 rectMaxY = rect.Y + rect.Height - 1 + margin;
        minX = hasRect == true ? std::min(minX, rectMinX) : rectMinX;
        minY = hasRect == true ? std::min(minY, rectMinY) : rectMinY;
        maxX = hasRect == true ? std::max(maxX, rectMaxX) : rectMaxX;
        maxY = hasRect == true ? std::max(maxY, rectMaxY) : rectMaxY;
        hasRect = true;
    }
    if (hasRect == false)
        return;
    MinBlockX = minX >> BLOCK_SHIFT;
    MinBlockY = minY >> BLOCK_SHIFT;
    BlocksX = (maxX >> BLOCK_SHIFT) - MinBlockX + 1;
    BlocksY = (maxY >> BLOCK_SHIFT) - MinBlockY + 1;

    // Count each block's rectangles, then list them (rectangle order kept)
    size_t blockCount = size_t(BlocksX) * size_t(BlocksY);
    std::vector<uint32> counts(blockCount, 0);
    auto forEachBlock = [&](ROWOWCellRect const& rect, auto&& action)
    {
        int32 firstBlockX = ((rect.X - margin) >> BLOCK_SHIFT) - MinBlockX;
        int32 firstBlockY = ((rect.Y - margin) >> BLOCK_SHIFT) - MinBlockY;
        int32 lastBlockX = ((rect.X + rect.Width - 1 + margin) >> BLOCK_SHIFT) - MinBlockX;
        int32 lastBlockY = ((rect.Y + rect.Height - 1 + margin) >> BLOCK_SHIFT) - MinBlockY;
        for (int32 blockY = firstBlockY; blockY <= lastBlockY; blockY++)
            for (int32 blockX = firstBlockX; blockX <= lastBlockX; blockX++)
                action(size_t(blockY) * size_t(BlocksX) + size_t(blockX));
    };
    for (ROWOWCellRect const& rect : rects)
        if (rect.Width > 0 && rect.Height > 0)
            forEachBlock(rect, [&](size_t blockIndex) { counts[blockIndex]++; });
    BlockStarts.assign(blockCount + 1, 0);
    for (size_t blockIndex = 0; blockIndex < blockCount; blockIndex++)
        BlockStarts[blockIndex + 1] = BlockStarts[blockIndex] + counts[blockIndex];
    Entries.assign(BlockStarts[blockCount], 0);
    std::vector<uint32> cursors(BlockStarts.begin(), BlockStarts.end() - 1);
    for (size_t rectIndex = 0; rectIndex < rects.size(); rectIndex++)
        if (rects[rectIndex].Width > 0 && rects[rectIndex].Height > 0)
            forEachBlock(rects[rectIndex], [&](size_t blockIndex) { Entries[cursors[blockIndex]++] = uint16(rectIndex); });
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
