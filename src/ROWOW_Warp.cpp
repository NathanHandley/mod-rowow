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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/pc.cpp, clif.cpp and skill.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "Player.h"
#include "QueryResult.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// Warp Portal's memo points (pc_memo, /memo: the RO_Identity addon's /memo and .ro memo) and the warp point menu that Warp Portal and
// Teleport level 2 open (clif_skill_warppoint, ROWOWWARP), whose pick comes back as .ro warp (skill_castend_map).  The memo points are kept
// in mod_rowow_identity_memo.  All of it runs in the player's own thread

namespace
{
    void SendWarpMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWWARP\t" + message);
        player->SendDirectMessage(&data);
    }

    // The RO name of a map key the warp list shows (prontera, prt_fild08): its own (a stitched map's part's), else the shortest of rAthena's names for it
    std::string GetROMapName(uint32 mapID)
    {
        std::string ownName = ROWOW->GetMapNameOfMapKey(mapID);
        if (ownName.empty() == false)
            return ownName;
        std::string mapName;
        for (auto const& mapIDByName : ROWOW->MapIDsByMapName)
            if (mapIDByName.second == mapID && (mapName.empty() == true || mapIDByName.first.size() < mapName.size()))
                mapName = mapIDByName.first;
        return mapName;
    }
}

bool ROWOWMod::PCMemo(Player* player, int32 pos)
{
    // pc_memo: the spot becomes a memo point (pos -1: the newest, pushing the others down, the same map's replaced)
    ROWOWIdentity* identity = GetIdentity(player);
    ChatHandler handler(player->GetSession());
    // check mapflags
    if (HasMapFlag(player, "nomemo") == true || HasMapFlag(player, "nowarpto") == true || IsROMap(player->GetMapId()) == false)
    {
        handler.SendNotification("Saved point cannot be memorized."); // clif_skill_teleportmessage NOTIFY_MAPINFO_CANT_MEMO
        return false;
    }
    // check inputs
    if (pos < -1 || pos >= ROWOW_MAX_MEMOPOINTS)
        return false; // invalid input
    // check required skill level
    int32 skill = PCCheckSkill(player, ROWOW_SKILL_AL_WARP);
    if (skill < 1)
    {
        handler.SendNotification("You haven't learned Warp."); // WARPPOINT_NOT_LEARNED
        return false;
    }
    if (skill < 2 || skill - 2 < pos)
    {
        handler.SendNotification("Skill Level is not high enough."); // WARPPOINT_LOW_LEVEL
        return false;
    }
    if (pos == -1)
    {
        // prevent memo-ing the same map multiple times (by map key: each part of a stitched map is a map of its own)
        uint32 mapKey = GetUnitMapKey(player);
        int32 i = 0;
        while (i < ROWOW_MAX_MEMOPOINTS && (identity->MemoPoints[i].MapID == 0 ||
            GetMapKey(identity->MemoPoints[i].MapID, identity->MemoPoints[i].CellX, identity->MemoPoints[i].CellY) != mapKey))
            i++;
        for (int32 j = std::min(i, ROWOW_MAX_MEMOPOINTS - 1); j > 0; j--)
            identity->MemoPoints[j] = identity->MemoPoints[j - 1];
        pos = 0;
    }
    if (player->GetMap()->Instanceable() == true)
    {
        handler.SendSysMessage("You cannot create a memo in an instance.");
        return false;
    }
    ROWOWMemoPoint& memo = identity->MemoPoints[pos];
    memo.MapID = player->GetMapId();
    GetUnitCell(player, memo.CellX, memo.CellY);
    identity->AreMemoPointsChanged = true;
    handler.SendSysMessage("Saved location as a Memo Point for Warp skill."); // WARPPOINT_SUCCESS
    return true;
}

void ROWOWMod::ClifSkillWarpPoint(Player* player, uint16 skillID, uint16 skillLevel, std::vector<uint32> const& mapIDs)
{
    // clif_skill_warppoint: the destinations to pick from ("<skill>\t<map ID>:<map name>,..."; Teleport's map ID 0 is "Random"), and the menu
    // skill open with its value (Warp Portal's cell was kept by the cast; Teleport keeps its level)
    std::string entries;
    for (size_t index = 0; index < mapIDs.size(); index++)
    {
        uint32 mapID = mapIDs[index];
        std::string name = (skillID == ROWOW_SKILL_AL_TELEPORT && index == 0) ? "Random" : GetROMapName(mapID);
        if (name.empty() == true)
            continue;
        entries += fmt::format("{}{}:{}", entries.empty() == true ? "" : ",", (skillID == ROWOW_SKILL_AL_TELEPORT && index == 0) ? 0 : mapID, name);
    }
    if (entries.empty() == true)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    identity->MenuSkillID = skillID;
    if (skillID != ROWOW_SKILL_AL_WARP)
        identity->MenuSkillValue = skillLevel;
    SendWarpMessage(player, fmt::format("{}\t{}", skillID, entries));
}

void ROWOWMod::SkillCastendMap(Player* player, uint16 skillID, std::string const& mapName)
{
    // skill_castend_map: the destination picked from the warp point menu ("Random", "cancel", or a map ID of the menu)
    ROWOWIdentity* identity = GetIdentity(player);
    auto skillFailed = [identity]()
    {
        identity->MenuSkillID = 0;
        identity->MenuSkillValue = 0;
    };
    if (skillID != identity->MenuSkillID)
        return;
    if (player->IsInWorld() == false || player->IsAlive() == false)
    {
        skillFailed();
        return;
    }
    if (ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player)); sc != nullptr && sc->Cant.Cast == true)
    {
        skillFailed();
        return;
    }
    player->AttackStop();
    if (mapName == "cancel")
    {
        skillFailed();
        return;
    }
    switch (skillID)
    {
        case ROWOW_SKILL_AL_TELEPORT:
            if (mapName == "Random" || mapName == "0")
                TeleportToRandomSpot(player);
            else if (identity->MenuSkillValue > 1) // Need lv2 to be able to warp here.
                ReturnToSavePoint(player); // With the return tether (ROWOW_ReturnTether.cpp)
            break;
        case ROWOW_SKILL_AL_WARP:
        {
            uint32 mapID = uint32(strtoul(mapName.c_str(), nullptr, 10));
            int32 maxcount = 0;
            ROWOWSkill const* skill = GetSkill(skillID);
            int32 lv = PCCheckSkill(player, skillID);
            if (lv <= 0)
                return;
            if ((maxcount = ROWOWSkill::GetLevelValue(skill->MaxCount, uint16(lv))) > 0)
            {
                // unit_skillunit_maxcount
                for (int32 groupID : GetSkillUnitData(player)->SkillUnitGroupIDs)
                {
                    std::shared_ptr<ROWOWSkillUnitGroup> group = SkillID2Group(player->GetMap(), groupID);
                    if (group != nullptr && group->SkillID == skillID && --maxcount == 0)
                        break;
                }
                if (maxcount == 0)
                {
                    ClifSkillFail(player, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                    skillFailed();
                    return;
                }
            }
            int32 wx = identity->MenuSkillValue >> 16;
            int32 wy = int32(int16(identity->MenuSkillValue & 0xFFFF));
            if (lv > 4)
                lv = 4; // crash prevention
            // check if the chosen map exists in the memo list (the save point, then the memo points the level allows)
            ROWOWPosition savePoint = identity->SavePoint;
            if (savePoint.IsSet == false)
                GetCellPosition(StartMapID, StartCellX, StartCellY, savePoint);
            // (the menu's map IDs are map keys: a part of a stitched map is a destination of its own)
            int32 x = 0, y = 0;
            bool isFound = false;
            if (GetPositionMapKey(savePoint) == mapID)
            {
                GetWorldCell(savePoint.MapID, savePoint.X, savePoint.Y, x, y);
                isFound = true;
            }
            for (int32 i = 0; isFound == false && i < lv - 1 && i < ROWOW_MAX_MEMOPOINTS; i++)
            {
                ROWOWMemoPoint const& memo = identity->MemoPoints[i];
                if (memo.MapID != 0 && GetMapKey(memo.MapID, memo.CellX, memo.CellY) == mapID)
                {
                    x = identity->MemoPoints[i].CellX;
                    y = identity->MemoPoints[i].CellY;
                    isFound = true;
                }
            }
            if (isFound == false || mapID == 0)
            {
                skillFailed();
                return;
            }
            if (SkillCheckConditionCastEnd(player, identity->MenuSkillID, uint16(lv)) == false)
            {
                // This checks versus skill_id/skill_lv...
                skillFailed();
                return;
            }
            SkillConsumeRequirement(player, identity->MenuSkillID, uint16(lv), 2);
            GetSkillUnitData(player)->SkillZ = identity->MenuSkillZ;
            std::shared_ptr<ROWOWSkillUnitGroup> group = SkillUnitSetting(player, skillID, uint16(lv), wx, wy, 0);
            if (group == nullptr)
            {
                skillFailed();
                return;
            }
            group->Val1 = (group->Val1 << 16) | int16(0);
            // record the destination coordinates
            group->Val2 = (x << 16) | (y & 0xFFFF);
            group->Val3 = int32(GetMapIDOfMapKey(mapID));
            break;
        }
    }
    identity->MenuSkillID = 0;
    identity->MenuSkillValue = 0;
}

void ROWOWMod::LoadIdentityMemo(Player* /*player*/, ROWOWIdentity& identity) const
{
    if (QueryResult memoResult = CharacterDatabase.Query("SELECT `slot`, `map`, `x`, `y` FROM `mod_rowow_identity_memo` WHERE `guid` = {}", identity.IdentityID))
    {
        do
        {
            Field* fields = memoResult->Fetch();
            uint8 slot = fields[0].Get<uint8>();
            if (slot >= ROWOW_MAX_MEMOPOINTS)
                continue;
            identity.MemoPoints[slot].MapID = fields[1].Get<uint32>();
            identity.MemoPoints[slot].CellX = fields[2].Get<int32>();
            identity.MemoPoints[slot].CellY = fields[3].Get<int32>();
        } while (memoResult->NextRow());
    }
}

void ROWOWMod::SaveIdentityMemo(Player* /*player*/, ROWOWIdentity& identity, CharacterDatabaseTransaction trans) const
{
    if (identity.AreMemoPointsChanged == false)
        return;
    uint32 guid = identity.IdentityID;
    trans->Append("DELETE FROM `mod_rowow_identity_memo` WHERE `guid` = {}", guid);
    for (uint8 slot = 0; slot < ROWOW_MAX_MEMOPOINTS; slot++)
    {
        ROWOWMemoPoint const& memo = identity.MemoPoints[slot];
        if (memo.MapID != 0)
            trans->Append("INSERT INTO `mod_rowow_identity_memo` (`guid`, `slot`, `map`, `x`, `y`) VALUES ({}, {}, {}, {}, {})", guid, slot, memo.MapID, memo.CellX, memo.CellY);
    }
    identity.AreMemoPointsChanged = false;
}
