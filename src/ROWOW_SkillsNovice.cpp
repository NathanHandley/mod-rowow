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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/novice, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

// The Novice's skills (rAthena's skills/novice, skill_factory_novice), one class per skill.  They run in the caster's map thread.  (Basic
// Skill is the tree's and the actions' gate; the Super Novice's skills come with the expanded jobs)

namespace
{
    // SkillFirstAid: heals 5
    class ROWOWSkillFirstAid : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFirstAid() : ROWOWSkillImpl(ROWOW_SKILL_NV_FIRSTAID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->StatusHeal(target, 5, 0, 0, src, ROWOW->GetSkillSpellID(SkillID, skillLevel));
        }
    };

    // Safe Step (MergedWorlds' own, Nathan 2026-11-12): to the nearest spot the player can stand on (FindSafeStepSpot)
    class ROWOWSkillSafeStep : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSafeStep() : ROWOWSkillImpl(ROWOW_SKILL_MW_SAFESTEP) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                ROWOW->SafeStep(sd);
        }
    };
}

namespace
{
    // FindSafeStepSpot's look at the cells around the player: each cell's standing point once, and which cells a walk can leave
    class ROWOWSafeStepSearch
    {
    public:
        ROWOWSafeStepSearch(ROWOWMod const& mod, Map* map, uint32 phaseMask, ROWOWMapGrid const& mapGrid) : Mod(mod), MapPtr(map), PhaseMask(phaseMask), Grid(mapGrid) { }

        // A normal standing point: walkable (the GAT's, as a Fly Wing's landing), its floor (GetCellFloorZ: the collision surface nearest the cell's RO
        // ground) a real surface near that ground, and nothing solid over the floor up to head height.  floorOut is its floor
        bool IsStandable(int32 cellX, int32 cellY, float& floorOut)
        {
            auto [iter, isNew] = Cells.try_emplace(Key(cellX, cellY));
            CellState& state = iter->second;
            if (isNew == true)
            {
                state.IsStandable = false;
                if (Grid.IsCellInGrid(cellX, cellY) == true && Grid.IsCellWalkable(cellX, cellY) == true)
                {
                    float x = Grid.GetCellCenterWorldX(cellY);
                    float y = Grid.GetCellCenterWorldY(cellX);
                    float roGroundZ = Grid.GetCellGroundZ(cellX, cellY) - ROWOW_GAT_GROUND_Z_LIFT;
                    float floorZ = Mod.GetCellFloorZ(MapPtr, PhaseMask, Grid, cellX, cellY, x, y);
                    // A surface really there (GetCellFloorZ gives the GAT's height when it finds none), and room to stand on it: not inside a wall, a
                    // rock or a roof's underside
                    float surfaceZ = MapPtr->GetHeight(PhaseMask, x, y, floorZ + 0.5f, true, 1.0f);
                    state.IsStandable = std::fabs(floorZ - roGroundZ) <= ROWOW_SAFE_STEP_FLOOR_TOLERANCE && surfaceZ > INVALID_HEIGHT && std::fabs(surfaceZ - floorZ) <= 0.5f
                        && MapPtr->isInLineOfSight(x, y, floorZ + 0.3f, x, y, floorZ + ROWOW_SAFE_STEP_HEADROOM, PhaseMask, LINEOFSIGHT_CHECK_VMAP,
                            VMAP::ModelIgnoreFlags::Nothing) == true;
                    state.FloorZ = floorZ;
                }
            }
            floorOut = state.FloorZ;
            return state.IsStandable;
        }

        // A spot a walk can leave: from it, over standable cells a step apart (no higher than a step up or down, nothing solid between them at waist
        // height), ROWOW_SAFE_STEP_OPEN_CELLS cells or more can be reached.  A floor shut in by walls (the inside of a model RO walks through, which
        // Bits stood in on the Training Ground, Nathan 2026-11-12) reaches only its few cells.  The cells one search reached share its answer
        bool IsOpen(int32 cellX, int32 cellY)
        {
            CellState& start = Cells[Key(cellX, cellY)];
            if (start.Openness != 0)
                return start.Openness > 0;
            std::vector<std::pair<int32, int32>> reached = { { cellX, cellY } };
            std::unordered_set<int64> seen = { Key(cellX, cellY) };
            for (size_t i = 0; i < reached.size() && reached.size() < ROWOW_SAFE_STEP_OPEN_CELLS; i++)
            {
                auto [fromX, fromY] = reached[i];
                float fromFloorZ = 0;
                IsStandable(fromX, fromY, fromFloorZ);
                for (int32 dy = -1; dy <= 1; dy++)
                {
                    for (int32 dx = -1; dx <= 1; dx++)
                    {
                        int32 toX = fromX + dx;
                        int32 toY = fromY + dy;
                        float toFloorZ = 0;
                        if ((dx == 0 && dy == 0) || seen.count(Key(toX, toY)) != 0 || IsStandable(toX, toY, toFloorZ) == false
                            || std::fabs(toFloorZ - fromFloorZ) > ROWOW_SAFE_STEP_MAX_STEP_HEIGHT)
                            continue;
                        if (MapPtr->isInLineOfSight(Grid.GetCellCenterWorldX(fromY), Grid.GetCellCenterWorldY(fromX), fromFloorZ + ROWOW_SAFE_STEP_WALK_CHECK_HEIGHT,
                            Grid.GetCellCenterWorldX(toY), Grid.GetCellCenterWorldY(toX), toFloorZ + ROWOW_SAFE_STEP_WALK_CHECK_HEIGHT, PhaseMask,
                            LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing) == false)
                            continue;
                        seen.insert(Key(toX, toY));
                        reached.push_back({ toX, toY });
                    }
                }
            }
            int8 openness = reached.size() >= ROWOW_SAFE_STEP_OPEN_CELLS ? 1 : -1;
            for (auto const& [x, y] : reached)
                Cells[Key(x, y)].Openness = openness;
            return openness > 0;
        }

    private:
        struct CellState
        {
            bool IsStandable = false;
            float FloorZ = 0;
            int8 Openness = 0; // 1 a walk can leave it, -1 shut in, 0 not looked at
        };

        static int64 Key(int32 cellX, int32 cellY) { return (int64(cellX) << 32) | uint32(cellY); }

        ROWOWMod const& Mod;
        Map* MapPtr;
        uint32 PhaseMask;
        ROWOWMapGrid const& Grid;
        std::unordered_map<int64, CellState> Cells;
    };
}

bool ROWOWMod::FindSafeStepSpot(Player* player, Position& spotOut) const
{
    // A standing point near the player that a walk can leave (ROWOWSafeStepSearch): never the player's own cell or one next to it, so Safe Step always
    // moves it (cast again it moves again), and a floor shut in by walls is passed over however good it is.  The cells are looked at nearest first, out to
    // ROWOW_SAFE_STEP_SEARCH_CELLS; of the good ones about as near as the nearest (ROWOW_SAFE_STEP_PICK_SPREAD) one is taken at random
    Map* map = player->GetMap();
    ROWOWMapGrid const* mapGrid = map != nullptr ? GetMapGrid(player->GetMapId()) : nullptr;
    if (mapGrid == nullptr || mapGrid->HasCells() == false)
        return false;
    ROWOWSafeStepSearch search(*this, map, player->GetPhaseMask(), *mapGrid);
    int32 centerX = mapGrid->GetCellX(player->GetPositionY());
    int32 centerY = mapGrid->GetCellY(player->GetPositionX());
    std::vector<std::pair<float, std::pair<int32, int32>>> candidates;
    for (int32 dy = -ROWOW_SAFE_STEP_SEARCH_CELLS; dy <= ROWOW_SAFE_STEP_SEARCH_CELLS; dy++)
    {
        for (int32 dx = -ROWOW_SAFE_STEP_SEARCH_CELLS; dx <= ROWOW_SAFE_STEP_SEARCH_CELLS; dx++)
        {
            int32 cellX = centerX + dx;
            int32 cellY = centerY + dy;
            if (std::max(std::abs(dx), std::abs(dy)) < ROWOW_SAFE_STEP_MIN_CELLS || mapGrid->IsCellInGrid(cellX, cellY) == false
                || mapGrid->IsCellWalkable(cellX, cellY) == false)
                continue;
            float deltaX = mapGrid->GetCellCenterWorldX(cellY) - player->GetPositionX();
            float deltaY = mapGrid->GetCellCenterWorldY(cellX) - player->GetPositionY();
            candidates.push_back({ std::sqrt(deltaX * deltaX + deltaY * deltaY), { cellX, cellY } });
        }
    }
    std::sort(candidates.begin(), candidates.end());
    std::vector<Position> picks;
    float nearestDistance = -1.0f;
    for (auto const& [distance, cell] : candidates)
    {
        if (nearestDistance >= 0.0f && distance > nearestDistance + ROWOW_SAFE_STEP_PICK_SPREAD)
            break;
        float floorZ = 0;
        if (search.IsStandable(cell.first, cell.second, floorZ) == false || search.IsOpen(cell.first, cell.second) == false)
            continue;
        if (nearestDistance < 0.0f)
            nearestDistance = distance;
        float x = mapGrid->GetCellCenterWorldX(cell.second);
        float y = mapGrid->GetCellCenterWorldY(cell.first);
        picks.emplace_back(x, y, std::max(floorZ, map->GetWaterLevel(x, y)));
    }
    if (picks.empty() == true)
        return false;
    spotOut = picks[urand(0, uint32(picks.size()) - 1)];
    return true;
}

void ROWOWMod::SafeStep(Player* player)
{
    Position spot;
    if (FindSafeStepSpot(player, spot) == false)
    {
        ChatHandler(player->GetSession()).SendNotification("Safe Step found no place to stand nearby.");
        return;
    }
    // The skill's own look (Teleport's light) played as the cast went off; the entry effect plays where it lands
    GetIdentity(player)->IsTeleportEntryLookPending = true;
    player->NearTeleportTo(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ() + 0.5f, player->GetOrientation());
}

void AddROWOWSkillsNovice()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillFirstAid());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSafeStep());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_NV_TRICKDEAD, true)); // Play Dead: the StatusSkillImpl, ended when cast again
}
