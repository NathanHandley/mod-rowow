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


#include "Creature.h"
#include "GridTerrainData.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "Player.h"

#include <cmath>

#include "ROWOW.h"

using namespace std;

// RO monster movement: ground-contoured small-step paths, and cell-to-cell paths on RO maps

bool ROWOWMod::GetGroundZ(Creature* creature, float x, float y, float referenceZ, float& groundZOut)
{
    return GetGroundZ(creature, x, y, referenceZ, ConfigMoveFloorSearchHeight, groundZOut);
}

bool ROWOWMod::GetGroundZ(Creature* creature, float x, float y, float referenceZ, float searchHeight, float& groundZOut)
{
    float groundZ = creature->GetMap()->GetHeight(creature->GetPhaseMask(), x, y, referenceZ + searchHeight, true, ConfigMoveFloorSearchDistance);
    if (groundZ <= INVALID_HEIGHT)
        return false;
    groundZOut = groundZ;
    return true;
}

bool ROWOWMod::BuildGroundContouredPath(Creature* creature, float targetX, float targetY, float stopShortDistance, Movement::PointsArray& pathNodesOut)
{
    pathNodesOut.clear();

    if (ConfigCellPathing == true)
    {
        ROWOWMapGrid const* mapGrid = GetMapGrid(creature->GetMapId());
        if (mapGrid != nullptr)
            return BuildCellPath(creature, *mapGrid, targetX, targetY, stopShortDistance, pathNodesOut);
    }

    float startX = creature->GetPositionX();
    float startY = creature->GetPositionY();
    float deltaX = targetX - startX;
    float deltaY = targetY - startY;
    float distance = std::sqrt(deltaX * deltaX + deltaY * deltaY);
    float travelDistance = distance - stopShortDistance;
    if (distance < 0.01f || travelDistance < 0.1f)
        return false;
    float directionX = deltaX / distance;
    float directionY = deltaY / distance;

    // Step along the line, pulling each step onto the ground
    uint32 stepCount = uint32(std::ceil(travelDistance / ConfigMoveStepDistance));
    if (stepCount > ROWOW_MOVE_MAX_PATH_NODES)
        stepCount = ROWOW_MOVE_MAX_PATH_NODES;
    float stepDistance = travelDistance / float(stepCount);
    float startZ = GetPathStartGroundZ(creature);
    pathNodesOut.emplace_back(startX, startY, creature->GetPositionZ());
    float priorX = startX;
    float priorY = startY;
    float priorZ = startZ;
    for (uint32 stepIndex = 1; stepIndex <= stepCount; ++stepIndex)
    {
        float stepX = startX + directionX * stepDistance * float(stepIndex);
        float stepY = startY + directionY * stepDistance * float(stepIndex);
        float stepZ = 0;
        if (CanStepTo(creature, priorX, priorY, priorZ, stepX, stepY, stepDistance, stepZ) == false)
            break;
        pathNodesOut.emplace_back(stepX, stepY, stepZ);
        priorX = stepX;
        priorY = stepY;
        priorZ = stepZ;
    }

    if (pathNodesOut.size() < 2)
    {
        pathNodesOut.clear();
        return false;
    }
    return true;
}

bool ROWOWMod::BuildCellPath(Creature* creature, ROWOWMapGrid const& mapGrid, float targetX, float targetY, float stopShortDistance, Movement::PointsArray& pathNodesOut)
{
    float startX = creature->GetPositionX();
    float startY = creature->GetPositionY();
    int32 cellX = mapGrid.GetCellX(startY);
    int32 cellY = mapGrid.GetCellY(startX);
    int32 targetCellX = mapGrid.GetCellX(targetY);
    int32 targetCellY = mapGrid.GetCellY(targetX);
    float startZ = GetPathStartGroundZ(creature);
    pathNodesOut.emplace_back(startX, startY, creature->GetPositionZ());

    // Already in the target's cell: a creature off the cell's center (after a knockback or a fear) steps onto it
    float priorX = startX;
    float priorY = startY;
    float priorZ = startZ;
    if (cellX == targetCellX && cellY == targetCellY)
    {
        float cellCenterX = mapGrid.GetCellCenterWorldX(cellY);
        float cellCenterY = mapGrid.GetCellCenterWorldY(cellX);
        float centerDistance = std::sqrt((cellCenterX - startX) * (cellCenterX - startX) + (cellCenterY - startY) * (cellCenterY - startY));
        float centerZ = 0;
        if (centerDistance > 0.1f && CanStepTo(creature, priorX, priorY, priorZ, cellCenterX, cellCenterY, centerDistance, centerZ) == true)
            pathNodesOut.emplace_back(cellCenterX, cellCenterY, centerZ);
    }

    // Diagonal steps while the target's cell is off in both directions, then straight.  A blocked step tries the steps to either side of
    // it (still toward the target), never going back onto a cell already stepped on
    std::vector<std::pair<int32, int32>> visitedCells;
    visitedCells.emplace_back(cellX, cellY);
    while (pathNodesOut.size() < ROWOW_MOVE_MAX_PATH_NODES)
    {
        if (cellX == targetCellX && cellY == targetCellY)
            break;
        float nodeDeltaX = targetX - priorX;
        float nodeDeltaY = targetY - priorY;
        if (stopShortDistance > 0 && pathNodesOut.size() > 1 && std::sqrt(nodeDeltaX * nodeDeltaX + nodeDeltaY * nodeDeltaY) <= stopShortDistance)
            break;
        int32 stepCellX = targetCellX > cellX ? 1 : (targetCellX < cellX ? -1 : 0);
        int32 stepCellY = targetCellY > cellY ? 1 : (targetCellY < cellY ? -1 : 0);
        std::pair<int32, int32> candidateSteps[3];
        candidateSteps[0] = std::make_pair(stepCellX, stepCellY);
        if (stepCellX != 0 && stepCellY != 0)
        {
            candidateSteps[1] = std::make_pair(stepCellX, 0);
            candidateSteps[2] = std::make_pair(0, stepCellY);
        }
        else if (stepCellX != 0)
        {
            candidateSteps[1] = std::make_pair(stepCellX, 1);
            candidateSteps[2] = std::make_pair(stepCellX, -1);
        }
        else
        {
            candidateSteps[1] = std::make_pair(1, stepCellY);
            candidateSteps[2] = std::make_pair(-1, stepCellY);
        }
        bool hasStepped = false;
        for (uint8 candidateIndex = 0; candidateIndex < 3; ++candidateIndex)
        {
            int32 nextCellX = cellX + candidateSteps[candidateIndex].first;
            int32 nextCellY = cellY + candidateSteps[candidateIndex].second;
            bool isVisited = false;
            for (std::pair<int32, int32> const& visitedCell : visitedCells)
            {
                if (visitedCell.first == nextCellX && visitedCell.second == nextCellY)
                {
                    isVisited = true;
                    break;
                }
            }
            if (isVisited == true)
                continue;
            float stepX = mapGrid.GetCellCenterWorldX(nextCellY);
            float stepY = mapGrid.GetCellCenterWorldY(nextCellX);
            float stepDistance = std::sqrt((stepX - priorX) * (stepX - priorX) + (stepY - priorY) * (stepY - priorY));
            float stepZ = 0;
            if (CanStepTo(creature, priorX, priorY, priorZ, stepX, stepY, stepDistance, stepZ) == false)
                continue;

            // As in RO, a diagonal step needs both cells beside it open, so it can't squeeze between two blocked corners
            if (candidateSteps[candidateIndex].first != 0 && candidateSteps[candidateIndex].second != 0)
            {
                float sideAX = mapGrid.GetCellCenterWorldX(cellY);
                float sideAY = mapGrid.GetCellCenterWorldY(nextCellX);
                float sideBX = mapGrid.GetCellCenterWorldX(nextCellY);
                float sideBY = mapGrid.GetCellCenterWorldY(cellX);
                float sideDistanceA = std::sqrt((sideAX - priorX) * (sideAX - priorX) + (sideAY - priorY) * (sideAY - priorY));
                float sideDistanceB = std::sqrt((sideBX - priorX) * (sideBX - priorX) + (sideBY - priorY) * (sideBY - priorY));
                float sideZ = 0;
                if (CanStepTo(creature, priorX, priorY, priorZ, sideAX, sideAY, sideDistanceA, sideZ) == false
                    || CanStepTo(creature, priorX, priorY, priorZ, sideBX, sideBY, sideDistanceB, sideZ) == false)
                    continue;
            }
            pathNodesOut.emplace_back(stepX, stepY, stepZ);
            visitedCells.emplace_back(nextCellX, nextCellY);
            cellX = nextCellX;
            cellY = nextCellY;
            priorX = stepX;
            priorY = stepY;
            priorZ = stepZ;
            hasStepped = true;
            break;
        }
        if (hasStepped == false)
            break;
    }

    if (pathNodesOut.size() < 2)
    {
        pathNodesOut.clear();
        return false;
    }
    return true;
}

bool ROWOWMod::CanStepTo(Creature* creature, float priorX, float priorY, float priorZ, float stepX, float stepY, float stepDistance, float& stepZOut)
{
    // Search from a step's climb above the prior step, so a low arch or deck overhead isn't taken for the ground
    float stepZ = 0;
    if (GetGroundZ(creature, stepX, stepY, priorZ, ConfigMoveMaxRisePerStep * stepDistance + 0.5f, stepZ) == false)
        return false;

    // Too steep up or down is a wall or a cliff, which RO wouldn't let a monster walk over
    if (std::fabs(stepZ - priorZ) > ConfigMoveMaxRisePerStep * stepDistance)
        return false;

    // Something solid in the way (a building or a fence)
    if (creature->GetMap()->isInLineOfSight(priorX, priorY, priorZ + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT, stepX, stepY, stepZ + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT,
        creature->GetPhaseMask(), LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing) == false)
        return false;
    stepZOut = stepZ;
    return true;
}

float ROWOWMod::GetPathStartGroundZ(Creature* creature)
{
    // Steps are compared against the ground under the creature, not its own height, since other movement (a fear, a knockback)
    // can leave it a little off the ground.  Ground far above it is an overhang (a deck, a table), not what it stands on
    float startX = creature->GetPositionX();
    float startY = creature->GetPositionY();
    float startZ = creature->GetPositionZ();
    float startGroundZ = startZ;
    if (GetGroundZ(creature, startX, startY, startZ, startGroundZ) == false)
        return startZ;
    if (startGroundZ > startZ + ConfigMoveMaxRisePerStep)
    {
        if (GetGroundZ(creature, startX, startY, startZ, 0.5f, startGroundZ) == false)
            return startZ;
    }
    return startGroundZ;
}
