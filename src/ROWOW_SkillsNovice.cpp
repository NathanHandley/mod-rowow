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

bool ROWOWMod::FindSafeStepSpot(Player* player, Position& spotOut) const
{
    // The nearest RO cell (its middle) around the player, its own cell first, that's a normal standing point: walkable (the GAT's, as a Fly Wing's landing),
    // on the map (not in a stitched map's gap), its floor (GetCellFloorZ: the collision surface nearest the cell's RO ground) a real surface near that
    // ground, and nothing solid over the floor up to head height.  Cells are tried nearest first, out to ROWOW_SAFE_STEP_SEARCH_CELLS
    Map* map = player->GetMap();
    ROWOWMapGrid const* mapGrid = map != nullptr ? GetMapGrid(player->GetMapId()) : nullptr;
    if (mapGrid == nullptr || mapGrid->HasCells() == false)
        return false;
    uint32 phaseMask = player->GetPhaseMask();
    int32 centerX = mapGrid->GetCellX(player->GetPositionY());
    int32 centerY = mapGrid->GetCellY(player->GetPositionX());
    std::vector<std::pair<float, std::pair<int32, int32>>> candidates;
    for (int32 dy = -ROWOW_SAFE_STEP_SEARCH_CELLS; dy <= ROWOW_SAFE_STEP_SEARCH_CELLS; dy++)
    {
        for (int32 dx = -ROWOW_SAFE_STEP_SEARCH_CELLS; dx <= ROWOW_SAFE_STEP_SEARCH_CELLS; dx++)
        {
            int32 cellX = centerX + dx;
            int32 cellY = centerY + dy;
            if (mapGrid->IsCellInGrid(cellX, cellY) == false || mapGrid->IsCellWalkable(cellX, cellY) == false)
                continue;
            float deltaX = mapGrid->GetCellCenterWorldX(cellY) - player->GetPositionX();
            float deltaY = mapGrid->GetCellCenterWorldY(cellX) - player->GetPositionY();
            candidates.push_back({ deltaX * deltaX + deltaY * deltaY, { cellX, cellY } });
        }
    }
    std::sort(candidates.begin(), candidates.end());
    for (auto const& candidate : candidates)
    {
        int32 cellX = candidate.second.first;
        int32 cellY = candidate.second.second;
        float x = mapGrid->GetCellCenterWorldX(cellY);
        float y = mapGrid->GetCellCenterWorldY(cellX);
        float roGroundZ = mapGrid->GetCellGroundZ(cellX, cellY) - ROWOW_GAT_GROUND_Z_LIFT;
        float floorZ = GetCellFloorZ(map, phaseMask, *mapGrid, cellX, cellY, x, y);
        if (std::fabs(floorZ - roGroundZ) > ROWOW_SAFE_STEP_FLOOR_TOLERANCE)
            continue;
        // A surface really there (GetCellFloorZ gives the GAT's height when it finds none)
        float surfaceZ = map->GetHeight(phaseMask, x, y, floorZ + 0.5f, true, 1.0f);
        if (surfaceZ <= INVALID_HEIGHT || std::fabs(surfaceZ - floorZ) > 0.5f)
            continue;
        // Room to stand: not inside a wall, a rock or a roof's underside
        if (map->isInLineOfSight(x, y, floorZ + 0.3f, x, y, floorZ + ROWOW_SAFE_STEP_HEADROOM, phaseMask, LINEOFSIGHT_CHECK_VMAP,
            VMAP::ModelIgnoreFlags::Nothing) == false)
            continue;
        spotOut.Relocate(x, y, std::max(floorZ, map->GetWaterLevel(x, y)));
        return true;
    }
    return false;
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
