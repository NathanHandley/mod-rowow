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
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include <cmath>

#include "ROWOW.h"
#include "ROWOW_Skill.h"

#include <queue>

using namespace std;

// RO monster movement and attacks, a port of rAthena's unit.cpp for monsters, and of path.cpp's search on the map's GAT.  A path is RO's own, cell to
// cell, walked as one WoW spline over the ground under each cell.  All of it runs in the monster's map thread

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

float ROWOWMod::GetCellFloorZ(Creature* creature, ROWOWMapGrid const& mapGrid, int32 cellX, int32 cellY, float x, float y) const
{
    if (mapGrid.IsCellInGrid(cellX, cellY) == false)
        return creature->GetPositionZ();
    return GetCellFloorZ(creature->GetMap(), creature->GetPhaseMask(), mapGrid, cellX, cellY, x, y);
}

bool ROWOWMod::GetROFloorZ(Map const* map, uint32 phaseMask, float x, float y, float& zOut) const
{
    ROWOWMapGrid const* mapGrid = GetMapGrid(map->GetId());
    if (mapGrid == nullptr)
        return false;
    int32 cellX = mapGrid->GetCellX(y);
    int32 cellY = mapGrid->GetCellY(x);
    if (mapGrid->IsCellInGrid(cellX, cellY) == false)
        return false;
    zOut = GetCellFloorZ(map, phaseMask, *mapGrid, cellX, cellY, x, y);
    return true;
}

float ROWOWMod::GetCellFloorZ(Map const* map, uint32 phaseMask, ROWOWMapGrid const& mapGrid, int32 cellX, int32 cellY, float x, float y) const
{
    // RO has a single ground a cell, the GAT's.  The floor is the collision surface nearest it, looked for from just over it, going down past the
    // surfaces of whatever stands on the cell.  A search from 3 yards over it took the first surface, which on prt_fild09 put a monster up to 3.5 yards
    // up a bush or tree on 2,388 walkable cells, and a standing one climbed further from there (the ground snap searched from its own height)
    float gatZ = mapGrid.GetCellGroundZ(cellX, cellY);
    float roGroundZ = gatZ - ROWOW_GAT_GROUND_Z_LIFT;
    float lowestZ = roGroundZ - ROWOW_CELL_FLOOR_SEARCH_BELOW;
    float searchZ = roGroundZ + ROWOW_CELL_FLOOR_SEARCH_ABOVE;
    float floorZ = INVALID_HEIGHT;
    for (int32 step = 0; step < ROWOW_CELL_FLOOR_SEARCH_STEPS && searchZ > lowestZ; step++)
    {
        float surfaceZ = map->GetHeight(phaseMask, x, y, searchZ, true, searchZ - lowestZ);
        if (surfaceZ <= INVALID_HEIGHT || surfaceZ < lowestZ || surfaceZ > searchZ)
            break;
        if (floorZ <= INVALID_HEIGHT || std::fabs(surfaceZ - roGroundZ) < std::fabs(floorZ - roGroundZ))
            floorZ = surfaceZ;
        if (surfaceZ <= roGroundZ)
            break; // Anything lower is further from the ground
        searchZ = surfaceZ - 0.05f; // Under this surface (the models' faces come in pairs a few hundredths apart)
    }
    return floorZ > INVALID_HEIGHT ? floorZ : gatZ;
}

namespace
{
    // path.cpp
    int32 constexpr MAX_WALKPATH = 32;
    int32 constexpr MOVE_COST = 10;
    int32 constexpr MOVE_DIAGONAL_COST = 14;
    uint64 constexpr MIN_MOBLINKTIME = 1000;

    // RO's directions (rAthena's dirx and diry): 0 north, then counter-clockwise
    int32 const DirectionX[ROWOW_RO_DIRECTION_COUNT] = { 0, -1, -1, -1, 0, 1, 1, 1 };
    int32 const DirectionY[ROWOW_RO_DIRECTION_COUNT] = { 1, 1, 0, -1, -1, -1, 0, 1 };

    enum PathNodeSet : uint8
    {
        PATH_SET_NONE,
        PATH_SET_OPEN,
        PATH_SET_CLOSED
    };

    struct PathNode
    {
        PathNode* Parent = nullptr;
        int16 X = 0;
        int16 Y = 0;
        int32 GCost = 0;
        int32 FCost = 0;
        PathNodeSet Set = PATH_SET_NONE;
    };

    struct PathNodeCompare
    {
        bool operator()(PathNode const* a, PathNode const* b) const { return a->FCost > b->FCost; }
    };

    using PathNodeHeap = std::vector<PathNode*>;

    int32 PathIndex(int32 x, int32 y)
    {
        return (x + y * MAX_WALKPATH) & (MAX_WALKPATH * MAX_WALKPATH - 1);
    }

    int32 PathHeuristic(int32 x0, int32 y0, int32 x1, int32 y1)
    {
        return MOVE_COST * (std::abs(x1 - x0) + std::abs(y1 - y0)); // Manhattan distance
    }

    // add_path: 1 when the node's index is taken by another cell (rAthena's array is too small to ensure all paths can be found)
    int32 AddPath(PathNodeHeap& heap, PathNode* nodes, int32 x, int32 y, int32 gCost, PathNode* parent, int32 hCost)
    {
        PathNode& node = nodes[PathIndex(x, y)];
        if (node.X == x && node.Y == y && node.Set != PATH_SET_NONE)
        {
            // We processed this node before.  New path to this node is better than old one: update costs and parent
            if (gCost < node.GCost)
            {
                node.GCost = gCost;
                node.Parent = parent;
                node.FCost = gCost + hCost;
                if (node.Set == PATH_SET_CLOSED)
                {
                    heap.push_back(&node); // Put it in open set again
                    std::push_heap(heap.begin(), heap.end(), PathNodeCompare());
                }
                else
                    std::make_heap(heap.begin(), heap.end(), PathNodeCompare());
                node.Set = PATH_SET_OPEN;
            }
            return 0;
        }
        if (node.Set != PATH_SET_NONE)
            return 1; // Index is already taken
        node.X = int16(x);
        node.Y = int16(y);
        node.GCost = gCost;
        node.Parent = parent;
        node.FCost = gCost + hCost;
        node.Set = PATH_SET_OPEN;
        heap.push_back(&node);
        std::push_heap(heap.begin(), heap.end(), PathNodeCompare());
        return 0;
    }

    uint8 GetDirection(int32 dx, int32 dy)
    {
        for (uint8 direction = 0; direction < ROWOW_RO_DIRECTION_COUNT; direction++)
            if (DirectionX[direction] == dx && DirectionY[direction] == dy)
                return direction;
        return 0;
    }
}

bool ROWOWMod::MapGetCellNoPass(Map* map, ROWOWMapGrid const& mapGrid, int32 x, int32 y) const
{
    // map_getcell(CELL_CHKNOPASS): not walkable, or an Ice Wall stands on it
    return mapGrid.IsCellWalkable(x, y) == false || MapGetCellIceWall(map, x, y) == true;
}

bool ROWOWMod::PathSearchLong(ROWOWMapGrid const& mapGrid, int32 x0, int32 y0, int32 x1, int32 y1) const
{
    // path_search_long with CELL_CHKWALL: is ranged attack from (x0,y0) to (x1,y1) possible?
    if (mapGrid.HasCells() == false)
        return true;
    int32 dx = x1 - x0;
    if (dx < 0)
    {
        std::swap(x0, x1);
        std::swap(y0, y1);
        dx = -dx;
    }
    int32 dy = y1 - y0;
    int32 wx = 0;
    int32 wy = 0;
    int32 weight = dx > std::abs(dy) ? dx : std::abs(y1 - y0);
    while (x0 != x1 || y0 != y1)
    {
        wx += dx;
        wy += dy;
        if (wx >= weight)
        {
            wx -= weight;
            x0++;
        }
        if (wy >= weight)
        {
            wy -= weight;
            y0++;
        }
        else if (wy < 0)
        {
            wy += weight;
            y0--;
        }
        if ((x0 != x1 || y0 != y1) && mapGrid.IsCellShootable(x0, y0) == false)
            return false;
    }
    return true;
}

bool ROWOWMod::PathSearch(std::vector<uint8>& pathOut, Map* map, ROWOWMapGrid const& mapGrid, int32 x0, int32 y0, int32 x1, int32 y1, uint8 cell) const
{
    // path_search with A* (the client's own search, which never cuts the corner of a cell nothing can walk on).  Its result is the directions to
    // walk, at most MAX_WALKPATH of them.  The start cell isn't checked, as that would get a unit stuck
    pathOut.clear();
    if (mapGrid.HasCells() == false || mapGrid.IsCellOnMap(x0, y0) == false || mapGrid.IsCellOnMap(x1, y1) == false)
        return false;
    auto isBlocked = [this, map, &mapGrid, cell](int32 x, int32 y)
    {
        if (cell == ROWOW_CELL_CHKWALL)
            return mapGrid.IsCellShootable(x, y) == false;
        return MapGetCellNoPass(map, mapGrid, x, y);
    };
    if (isBlocked(x1, y1) == true)
        return false;
    if (x0 == x1 && y0 == y1)
        return true;

    PathNode nodes[MAX_WALKPATH * MAX_WALKPATH];
    PathNodeHeap heap;
    heap.reserve(64);
    PathNode& startNode = nodes[PathIndex(x0, y0)];
    startNode.X = int16(x0);
    startNode.Y = int16(y0);
    startNode.FCost = PathHeuristic(x0, y0, x1, y1);
    startNode.Set = PATH_SET_OPEN;
    heap.push_back(&startNode);
    int32 xs = mapGrid.Width - 1;
    int32 ys = mapGrid.Height - 1;
    PathNode* current = nullptr;
    for (;;)
    {
        if (heap.empty() == true)
            return false;

        // Look for the lowest f_cost node in the 'open' set, and move it to the 'closed' set
        std::pop_heap(heap.begin(), heap.end(), PathNodeCompare());
        current = heap.back();
        heap.pop_back();
        int32 x = current->X;
        int32 y = current->Y;
        int32 gCost = current->GCost;
        current->Set = PATH_SET_CLOSED;
        if (x == x1 && y == y1)
            break;

        // The allowed directions for the current cell.  Diagonal directions are only allowed if both directions around it are allowed, to prevent
        // cutting corner of nearby wall
        bool isNorth = y < ys && isBlocked(x, y + 1) == false;
        bool isSouth = y > 0 && isBlocked(x, y - 1) == false;
        bool isEast = x < xs && isBlocked(x + 1, y) == false;
        bool isWest = x > 0 && isBlocked(x - 1, y) == false;
        int32 e = 0; // error flag
        if (isSouth == true && isEast == true && isBlocked(x + 1, y - 1) == false)
            e += AddPath(heap, nodes, x + 1, y - 1, gCost + MOVE_DIAGONAL_COST, current, PathHeuristic(x + 1, y - 1, x1, y1));
        if (isEast == true)
            e += AddPath(heap, nodes, x + 1, y, gCost + MOVE_COST, current, PathHeuristic(x + 1, y, x1, y1));
        if (isNorth == true && isEast == true && isBlocked(x + 1, y + 1) == false)
            e += AddPath(heap, nodes, x + 1, y + 1, gCost + MOVE_DIAGONAL_COST, current, PathHeuristic(x + 1, y + 1, x1, y1));
        if (isNorth == true)
            e += AddPath(heap, nodes, x, y + 1, gCost + MOVE_COST, current, PathHeuristic(x, y + 1, x1, y1));
        if (isNorth == true && isWest == true && isBlocked(x - 1, y + 1) == false)
            e += AddPath(heap, nodes, x - 1, y + 1, gCost + MOVE_DIAGONAL_COST, current, PathHeuristic(x - 1, y + 1, x1, y1));
        if (isWest == true)
            e += AddPath(heap, nodes, x - 1, y, gCost + MOVE_COST, current, PathHeuristic(x - 1, y, x1, y1));
        if (isSouth == true && isWest == true && isBlocked(x - 1, y - 1) == false)
            e += AddPath(heap, nodes, x - 1, y - 1, gCost + MOVE_DIAGONAL_COST, current, PathHeuristic(x - 1, y - 1, x1, y1));
        if (isSouth == true)
            e += AddPath(heap, nodes, x, y - 1, gCost + MOVE_COST, current, PathHeuristic(x, y - 1, x1, y1));
        if (e != 0)
            return false;
    }

    // Recreate path
    int32 length = 0;
    for (PathNode* node = current; node->Parent != nullptr; node = node->Parent)
        length++;
    if (length > MAX_WALKPATH)
        return false;
    pathOut.resize(size_t(length));
    int32 pathIndex = length - 1;
    for (PathNode* node = current; node->Parent != nullptr; node = node->Parent)
        pathOut[size_t(pathIndex--)] = GetDirection(node->X - node->Parent->X, node->Y - node->Parent->Y);
    return true;
}

bool ROWOWMod::IsMobMovementBlocked(Creature* md, bool isIgnoreDisableMove) const
{
    // Movement the core controls (fear, confuse, knockbacks, jumps, charges) or that's otherwise impossible right now.  Creatures that never move
    // (RO plants and the like) count as blocked unless that's ignored, since they still get put on the ground
    if (md->HasUnitState(UNIT_STATE_LOST_CONTROL) == true || md->HasUnitState(UNIT_STATE_NOT_MOVE) == true)
        return true;
    if (md->HasUnitState(UNIT_STATE_CASTING) == true)
        return true;
    if (isIgnoreDisableMove == false && md->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) == true)
        return true;
    return md->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE;
}

bool ROWOWMod::UnitCanMove(Creature* md, ROWOWMobData const* mob, uint64 tick) const
{
    // unit_can_move, for a monster
    if (mob->CanMoveTime > tick)
        return false;
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(md));
    if (sc != nullptr && sc->Cant.Move == true)
        return false; // Status changes that block movement
    return IsMobMovementBlocked(md) == false;
}

bool ROWOWMod::UnitCanReachBL(Creature* md, Unit const* tbl, int32 range, int32& xOut, int32& yOut) const
{
    // unit_can_reach_bl: a cell beside the target that it can walk to (the one on its side if it can stand there), within range when that's set
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    if (mapGrid == nullptr || md->GetMap() != tbl->GetMap())
        return false;
    int32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    GetUnitCell(md, x0, y0);
    GetUnitCell(tbl, x1, y1);
    xOut = x1;
    yOut = y1;
    if (x0 == x1 && y0 == y1)
        return true;
    if (range > 0 && CheckDistanceBL(md, tbl, range) == false)
        return false;

    // It judges whether it can adjoin or not.
    int32 dx = x1 - x0;
    int32 dy = y1 - y0;
    dx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    dy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    Map* map = md->GetMap();
    if (MapGetCellNoPass(map, *mapGrid, x1 - dx, y1 - dy) == true) // Look for a suitable cell to place in.
    {
        int32 direction = 0;
        for (; direction < ROWOW_RO_DIRECTION_COUNT && MapGetCellNoPass(map, *mapGrid, x1 - DirectionX[direction], y1 - DirectionY[direction]) == true; direction++)
            ;
        if (direction == ROWOW_RO_DIRECTION_COUNT)
            return false; // No valid cells.
        dx = DirectionX[direction];
        dy = DirectionY[direction];
    }
    xOut = x1 - dx;
    yOut = y1 - dy;
    std::vector<uint8> path;
    return PathSearch(path, map, *mapGrid, x0, y0, x1 - dx, y1 - dy, ROWOW_CELL_CHKNOPASS);
}

bool ROWOWMod::UnitCanReachCell(Creature* md, int32 x1, int32 y1, int32 range) const
{
    // unit_can_reach_bl for a floor item: a cell beside it, as for a unit on it
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    if (mapGrid == nullptr)
        return false;
    int32 x0 = 0, y0 = 0;
    GetUnitCell(md, x0, y0);
    if (x0 == x1 && y0 == y1)
        return true;
    if (range > 0 && std::max(std::abs(x1 - x0), std::abs(y1 - y0)) > range)
        return false;

    // It judges whether it can adjoin or not.
    int32 dx = x1 - x0;
    int32 dy = y1 - y0;
    dx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    dy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    Map* map = md->GetMap();
    if (MapGetCellNoPass(map, *mapGrid, x1 - dx, y1 - dy) == true) // Look for a suitable cell to place in.
    {
        int32 direction = 0;
        for (; direction < ROWOW_RO_DIRECTION_COUNT && MapGetCellNoPass(map, *mapGrid, x1 - DirectionX[direction], y1 - DirectionY[direction]) == true; direction++)
            ;
        if (direction == ROWOW_RO_DIRECTION_COUNT)
            return false; // No valid cells.
        dx = DirectionX[direction];
        dy = DirectionY[direction];
    }
    std::vector<uint8> path;
    return PathSearch(path, map, *mapGrid, x0, y0, x1 - dx, y1 - dy, ROWOW_CELL_CHKNOPASS);
}

uint32 ROWOWMod::UnitWalkPathPos(Creature* /*md*/, ROWOWMobData const* mob, uint64 tick) const
{
    // walkpath.path_pos: the cells of its path walked so far
    if (mob->IsWalking == false || mob->WalkPathTime == 0)
        return 0;
    return uint32(std::min<uint64>(mob->WalkPathLength, (tick - mob->WalkStartTime) * mob->WalkPathLength / mob->WalkPathTime));
}

bool ROWOWMod::UnitWalkPath(Creature* md, ROWOWMobData* mob, std::vector<uint8> const& path, int32 x0, int32 y0, bool isRun)
{
    // The path as one spline, over the ground under each cell's center.  The GAT's height says which floor: the ground is looked for from just above it
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    ROWOWStatus const* status = GetROStatus(md);
    if (mapGrid == nullptr || status == nullptr || path.empty() == true)
        return false;
    Movement::PointsArray nodes;
    nodes.reserve(path.size() + 1);
    nodes.emplace_back(md->GetPositionX(), md->GetPositionY(), md->GetPositionZ());
    int32 x = x0;
    int32 y = y0;
    uint32 pathTime = 0;
    for (uint8 direction : path)
    {
        x += DirectionX[direction];
        y += DirectionY[direction];
        float worldX = mapGrid->GetCellCenterWorldX(y);
        float worldY = mapGrid->GetCellCenterWorldY(x);
        nodes.emplace_back(worldX, worldY, GetCellFloorZ(md, *mapGrid, x, y, worldX, worldY));
        pathTime += uint32(status->Speed) * uint32((direction % 2) != 0 ? MOVE_DIAGONAL_COST : MOVE_COST) / uint32(MOVE_COST); // unit_get_walkpath_time
    }

    // A path of one cell is two points, which the core's escort movement turns into a MoveTo with the mmaps' path finding: its navmesh also has
    // polygons on the tree tops, and RO's own path is already the path.  A middle point keeps it a plain spline
    if (nodes.size() == 2 && (nodes[1] - nodes[0]).length() > 0.2f)
    {
        G3D::Vector3 middle = (nodes[0] + nodes[1]) * 0.5f;
        int32 middleCellX = mapGrid->GetCellX(middle.y);
        int32 middleCellY = mapGrid->GetCellY(middle.x);
        middle.z = mapGrid->IsCellWalkable(middleCellX, middleCellY) == true ? GetCellFloorZ(md, *mapGrid, middleCellX, middleCellY, middle.x, middle.y) :
            middle.z;
        nodes.insert(nodes.begin() + 1, middle);
    }
    md->GetMotionMaster()->MoveSplinePath(&nodes, isRun == true ? FORCED_MOVEMENT_RUN : FORCED_MOVEMENT_WALK);
    mob->IsWalking = true;
    mob->WalkStartTime = GetTick();
    mob->WalkPathTime = pathTime;
    mob->WalkPathLength = uint32(path.size());
    mob->WalkPath = path;
    mob->WalkPathX = x0;
    mob->WalkPathY = y0;
    return true;
}

bool ROWOWMod::UnitWalkToXY(Creature* md, int32 x, int32 y)
{
    // unit_walktoxy, the random walk's: the path must be found and no longer than max_walk_path
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    ROWOWStatus const* status = GetROStatus(md);
    if (mapGrid == nullptr || status == nullptr)
        return false;
    int32 x0 = 0, y0 = 0;
    GetUnitCell(md, x0, y0);
    std::vector<uint8> path;
    if (PathSearch(path, md->GetMap(), *mapGrid, x0, y0, x, y, ROWOW_CELL_CHKNOPASS) == false)
        return false;
    if (int32(path.size()) > BattleConfig.MaxWalkPath)
        return false;
    ROWOWMobData* mob = GetMobData(md);
    if ((status->Mode & ROWOW_MODE_CANMOVE) == 0 || UnitCanMove(md, mob, GetTick()) == false)
        return false;
    mob->AttackTargetID.Clear(); // unit_stop_attack: Sets target to 0

    // Randomize the target position
    if (GetSCE(md, ROWOW_SC_CONFUSION) != nullptr && MapRandomDir(md, x, y) == true)
        if (PathSearch(path, md->GetMap(), *mapGrid, x0, y0, x, y, ROWOW_CELL_CHKNOPASS) == false)
            return false;
    return UnitWalkPath(md, mob, path, x0, y0, false);
}

bool ROWOWMod::UnitCalcPos(Map* map, int32 x0, int32 y0, int32 tx, int32 ty, uint8 dir, int32& xOut, int32& yOut) const
{
    // unit_calc_pos: the cell 2 cells behind the master's facing, or 1, or the same at a random direction, or the master's own, that a pet (or homunculus, or mercenary) on x0, y0
    // can reach.  False when none is
    ROWOWMapGrid const* mapGrid = GetMapGrid(map->GetId());
    if (mapGrid == nullptr || dir >= ROWOW_RO_DIRECTION_COUNT)
        return false;
    auto canReach = [&](int32 x, int32 y)
    {
        if (x == x0 && y == y0) // Same place
            return true;
        std::vector<uint8> path;
        return PathSearch(path, map, *mapGrid, x0, y0, x, y, ROWOW_CELL_CHKNOPASS);
    };
    auto tryDirection = [&](uint8 direction)
    {
        // 2 cells from Master Position, or 1
        int32 dx = -DirectionX[direction] * 2;
        int32 dy = -DirectionY[direction] * 2;
        xOut = tx + dx;
        yOut = ty + dy;
        if (canReach(xOut, yOut) == true)
            return true;
        xOut += dx > 0 ? -1 : (dx < 0 ? 1 : 0);
        yOut += dy > 0 ? -1 : (dy < 0 ? 1 : 0);
        return canReach(xOut, yOut);
    };
    if (tryDirection(dir) == true)
        return true;
    for (int32 i = 0; i < 12; i++)
        if (tryDirection(uint8(urand(0, ROWOW_RO_DIRECTION_COUNT - 1))) == true) // Pick a Random Dir
            return true;
    xOut = tx; // Exactly Master Position
    yOut = ty;
    return canReach(xOut, yOut);
}

bool ROWOWMod::UnitWalkToBL(Creature* md, ROWOWMobData* mob, Unit* tbl, int32 range, bool isAttackContinue)
{
    // unit_walktobl: to a cell beside the target ("monsters always target an adjacent tile even if ranged"), which unit_update_chase ends when the
    // target comes in range
    ROWOWStatus const* status = GetROStatus(md);
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    if (status == nullptr || mapGrid == nullptr || (status->Mode & ROWOW_MODE_CANMOVE) == 0)
        return false;
    int32 toX = 0, toY = 0;
    if (UnitCanReachBL(md, tbl, DistanceBL(md, tbl) + 1, toX, toY) == false)
    {
        mob->TargetToID.Clear();
        return false;
    }
    if (range == 0)
        GetUnitCell(tbl, toX, toY); // Should walk on the same cell as target (for looters)
    mob->TargetToID = tbl->GetGUID();
    mob->ChaseRange = range;
    mob->IsAttackContinue = isAttackContinue; // Chase to attack.
    mob->AttackTargetID.Clear(); // unit_stop_attack: Sets target to 0
    if (GetSCE(md, ROWOW_SC_CONFUSION) != nullptr) // Randomize the target position
        MapRandomDir(md, toX, toY);
    uint64 tick = GetTick();
    if (mob->CanMoveTime > tick)
        return true; // Can't move, wait a bit before invoking the movement (the next think invokes it)
    if (UnitCanMove(md, mob, tick) == false)
        return false;
    int32 x0 = 0, y0 = 0;
    GetUnitCell(md, x0, y0);
    std::vector<uint8> path;
    if (PathSearch(path, md->GetMap(), *mapGrid, x0, y0, toX, toY, ROWOW_CELL_CHKNOPASS) == false)
        return false;
    if (path.empty() == true)
        return true;
    if (UnitWalkPath(md, mob, path, x0, y0, true) == false)
        return false;

    // Set mobstate here already as chase skills can be used on the first frame of movement
    if (isAttackContinue == true)
        MobSetState(md, mob, ROWOW_MSS_RUSH);
    return true;
}

bool ROWOWMod::UnitUpdateChase(Creature* md, ROWOWMobData* mob, uint64 tick)
{
    // unit_update_chase, each think rather than each cell: if target in attack range, attack; if target out of sight, drop target; otherwise update
    // the chase path one cell before reaching the path's end
    if (mob->IsWalking == false || mob->TargetToID.IsEmpty() == true)
        return false;
    ROWOWStatus const* status = GetROStatus(md);
    if (status == nullptr)
        return false;
    Unit* tbl = ObjectAccessor::GetUnit(*md, mob->TargetToID);
    bool isFullCheck = md->movespline->Finalized() == true || md->movespline->timeElapsed() <= status->Speed * MOVE_DIAGONAL_COST / MOVE_COST;
    uint32 pathPos = UnitWalkPathPos(md, mob, tick);

    // Reached destination, start attacking
    if (tbl != nullptr && tbl->IsAlive() == true && tbl->GetMap() == md->GetMap() && pathPos > 0 && CheckDistanceBL(md, tbl, mob->ChaseRange) == true)
    {
        bool isStop = true;
        if (mob->IsAttackContinue == true)
            isStop = UnitAttack(md, mob, tbl);

        // Stop, unless the attack was skipped
        if (isStop == true)
        {
            UnitStopWalking(md);
            mob->TargetToID.Clear();
            return true;
        }
    }
    else if (tbl == nullptr || (isFullCheck == true && MobCheckVisibility(md, tbl, true) == false)) // Cancel chase
    {
        MobUnlockTarget(md, tick); // Make sure monsters properly unlock their target, but still continue movement
        return false;
    }
    else if (isFullCheck == true && pathPos > 0 && mob->CanMoveTime <= tick)
    {
        // We call this only when we know there is no walk delay to prevent it pre-planning a chase.  If the call here fails, the unit should
        // continue its current path
        if (UnitWalkToBL(md, mob, tbl, mob->ChaseRange, mob->IsAttackContinue) == true)
            return true;
    }
    return false;
}

uint32 ROWOWMod::UnitEscape(Creature* md, Unit* target, int32 dist, uint8 flag)
{
    // unit_escape: a walk away from the target, the way the target looks at it.  flag&1 a straight line up to the first unreachable cell; else the
    // furthest reachable cell of the way
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    if (mapGrid == nullptr)
        return 0;
    int32 x = 0, y = 0, targetX = 0, targetY = 0;
    GetUnitCell(md, x, y);
    GetUnitCell(target, targetX, targetY);
    uint8 dir = x == targetX && y == targetY ? UnitGetDir(target) : GetRODirection(float(x - targetX), float(y - targetY)); // map_calc_dir
    if ((flag & 1) != 0)
    {
        // Straight line escape: keep moving until we hit an unreachable cell
        for (int32 i = 1; i <= dist; i++)
            if (MapGetCellNoPass(md->GetMap(), *mapGrid, x + i * DirectionX[dir], y + i * DirectionY[dir]) == true)
                dist = i - 1;
    }
    else
    {
        // Find the furthest reachable cell (then find a walkpath to it)
        while (dist > 0 && MapGetCellNoPass(md->GetMap(), *mapGrid, x + dist * DirectionX[dir], y + dist * DirectionY[dir]) == true)
            dist--;
    }
    if (dist > 0 && UnitWalkToXY(md, x + dist * DirectionX[dir], y + dist * DirectionY[dir]) == true)
        return GetMobData(md)->WalkPathTime; // unit_get_walkpath_time
    return 0;
}

bool ROWOWMod::UnitWalkToCell(Creature* md, ROWOWMobData* mob, int32 x, int32 y)
{
    // unit_walktobl with range 0, for a floor item: onto its cell
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    uint64 tick = GetTick();
    mob->AttackTargetID.Clear(); // unit_stop_attack: Sets target to 0
    if (mapGrid == nullptr)
        return false;
    if (mob->CanMoveTime > tick)
        return true; // Can't move, wait a bit before invoking the movement (the next think invokes it)
    if (UnitCanMove(md, mob, tick) == false)
        return false;
    int32 x0 = 0, y0 = 0;
    GetUnitCell(md, x0, y0);
    std::vector<uint8> path;
    if (PathSearch(path, md->GetMap(), *mapGrid, x0, y0, x, y, ROWOW_CELL_CHKNOPASS) == false)
        return false;
    return path.empty() == true || UnitWalkPath(md, mob, path, x0, y0, true);
}

void ROWOWMod::UnitStopWalkingSoon(Creature* md, uint64 tick)
{
    // unit_stop_walking_soon: the path is cut to the next cell, or the one after it once it's past the middle of this one, so it stops on a cell and not at once
    ROWOWMobData* mob = GetMobData(md);
    if (mob->IsWalking == false || mob->WalkPath.empty() == true || mob->WalkPathTime == 0)
    {
        UnitStopWalking(md);
        return;
    }
    uint64 walkedTime = tick - mob->WalkStartTime;
    uint32 pathPos = UnitWalkPathPos(md, mob, tick);
    uint32 pathRemain = (walkedTime * mob->WalkPathLength * 2 / mob->WalkPathTime) % 2 != 0 ? 2 : 1;
    if (pathPos >= mob->WalkPath.size())
    {
        UnitStopWalking(md);
        return;
    }
    int32 x = mob->WalkPathX;
    int32 y = mob->WalkPathY;
    for (uint32 i = 0; i < pathPos; i++)
    {
        x += DirectionX[mob->WalkPath[i]];
        y += DirectionY[mob->WalkPath[i]];
    }
    std::vector<uint8> path(mob->WalkPath.begin() + pathPos, mob->WalkPath.begin() + std::min<size_t>(pathPos + pathRemain, mob->WalkPath.size()));
    UnitWalkPath(md, mob, path, x, y, true);
}

void ROWOWMod::UnitStopWalking(Creature* md)
{
    // unit_stop_walking: its own path only, leaving any movement the core controls (like a fear) alone
    ROWOWMobData* mob = GetMobData(md);
    mob->IsWalking = false;
    if (md->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) != ESCORT_MOTION_TYPE)
        return;
    md->GetMotionMaster()->MovementExpiredOnSlot(MOTION_SLOT_ACTIVE, false);
    if (md->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == NULL_MOTION_TYPE)
        md->StopMoving();
}

void ROWOWMod::UnitSetWalkDelay(Creature* md, ROWOWMobData* mob, uint64 tick, int64 delay)
{
    // unit_set_walkdelay for a hit: trapped, or the legacy walk delay system off (damage_walk_delay), it stops on the closest cell center.  Otherwise
    // it's held for the delay, and immune to being stopped for double the flinch time
    if (delay <= 0)
        return;
    if (mob->IsCanEscape == true)
        return; // Mob needs to escape, don't stop it
    if (UnitCanMove(md, mob, tick) == false || (BattleConfig.DamageWalkDelay & ROWOW_BL_MOB) == 0)
    {
        // USW_MOVE_FULL_CELL: on to the next cell center, not where it stands (MergedWorlds ro-attack-range: at once, each hit cost a
        // chasing monster ground on a player backing away)
        UnitStopWalkingSoon(md, tick);
        return;
    }
    if (int64(mob->CanMoveTime) - (int64(tick) - delay) > 0)
        return;
    mob->CanMoveTime = tick + uint64(delay);
    UnitStopWalking(md);
}

bool ROWOWMod::UnitAttack(Creature* md, ROWOWMobData* mob, Unit* target)
{
    // unit_attack for a monster: it swings at the target when its attack delay allows (UnitAttackTimerSub).  The state is set regardless of whether the
    // attack is executed now or later
    if (MobRandomTarget(md, target) == false)
        return false; // Continue walking
    if (target == nullptr || target->IsAlive() == false)
    {
        UnitUnattackable(md, mob, GetTick());
        return true;
    }
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(md));
    if (sc != nullptr && sc->Cant.Attack == true) // unit_can_attack
    {
        mob->AttackTargetID.Clear();
        return false; // Continue walking
    }
    if (BattleCheckTarget(md, target, ROWOW_BCT_ENEMY) <= 0 || StatusCheckSkillUse(md, target, 0, 0) == false)
    {
        UnitUnattackable(md, mob, GetTick());
        return true;
    }
    mob->IsAttackContinue = true;
    mob->AttackTargetID = target->GetGUID();
    MobSetState(md, mob, ROWOW_MSS_BERSERK);
    return true;
}

void ROWOWMod::UnitUnattackable(Creature* md, ROWOWMobData* mob, uint64 tick)
{
    // unit_unattackable
    mob->AttackTargetID.Clear();
    mob->IsAttackContinue = false;
    MobUnlockTarget(md, tick);
}

void ROWOWMod::UnitAttackTimerSub(Creature* md, ROWOWMobData* mob, uint64 tick)
{
    // unit_attack_timer_sub for a monster, on WoW's attack timer (set to the monster's attack delay).  (Its timer call runs mob_ai_sub_hard first; the
    // think comes at most MIN_MOBTHINKTIME before this)
    if (mob->AttackTargetID.IsEmpty() == true || md->isAttackReady() == false || md->HasUnitState(UNIT_STATE_CASTING) == true)
        return;

    // mob_ai_sub_hard_attacktimer: the attack timer runs the AI, whose Berserk state checks its skills exactly when the attack delay ends (attackabletime).
    // The think of every 100 ms can't see that moment, as the swing below resets the timer in the same update.  A skill takes the swing's place
    if (mob->SkillState == ROWOW_MSS_BERSERK && MobSkillUse(md, tick, -1) == true)
        return;
    if (md->IsAlive() == false)
        return;
    Unit* target = ObjectAccessor::GetUnit(*md, mob->AttackTargetID);
    ROWOWStatus const* sstatus = GetROStatus(md);
    if (target == nullptr || sstatus == nullptr || target->GetMap() != md->GetMap() || target->IsAlive() == false)
    {
        UnitUnattackable(md, mob, tick);
        return;
    }

    // Monsters have a special visibility check at the end of their attack delay: if they still have a target, but it is not visible, they drop it
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(md));
    if (MobCheckVisibility(md, target, true) == false || BattleCheckTarget(md, target, ROWOW_BCT_ENEMY) <= 0 ||
        StatusCheckSkillUse(md, target, 0, 0) == false || (sc != nullptr && sc->Cant.Attack == true))
    {
        UnitUnattackable(md, mob, tick);
        return;
    }

    // Attacking when under cast delay has restrictions: we are in a combo-attack, delay this until your canact time is over. [Skotlex]
    if (BattleConfig.SDelayAttackEnable == 0 && GetSkillUnitData(md)->CanActTime > tick)
        return;
    int32 range = sstatus->Range;
    if (target->isMoving() == true)
        range++; // Extra range when chasing
    if (BattleCheckAttackRange(md, target, range) == false) // MergedWorlds ro-melee-range: melee reaches 50% further
    {
        mob->AttackTargetID.Clear(); // Monster: chase if required, which its next think does (or drops the target, if it can't move)
        return;
    }

    if (mob->IsWalking == true)
        UnitStopWalking(md);

    // Link monsters nearby [Skotlex]
    if ((sstatus->Mode & ROWOW_MODE_ASSIST) != 0 && tick - mob->LastLinkTime >= MIN_MOBLINKTIME)
    {
        mob->LastLinkTime = tick;
        MobLinkSearch(md, target, tick);
    }

    // The AI's unit_attack at the end of the attack delay: the state follows the aggressive bit, which a hit may have cleared
    MobSetState(md, mob, ROWOW_MSS_BERSERK);

    // Turn to the target first, as RO monsters do
    if (md->movespline->Finalized() == true && md->HasInArc(float(M_PI) / 4.0f, target) == false && IsMobMovementBlocked(md, true) == false)
        md->SetFacingToObject(target);
    md->AttackerStateUpdate(target);
    md->resetAttackTimer();
    if (ROWOWCreatureTemplate const* creatureTemplate = GetCreatureTemplate(md->GetEntry()); creatureTemplate != nullptr && creatureTemplate->AttackVisualSpellID != 0)
        md->CastSpell(target, creatureTemplate->AttackVisualSpellID, true);
    if (md->IsAlive() == true && mob->IsAttackContinue == false)
        mob->AttackTargetID.Clear();
}
