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
//  RO's party rules follow rAthena's party.cpp, int_party.cpp and pc_takeitem:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"

#include "ROWOW.h"
#include "ROWOW_Skill.h"

using namespace std;

// RO's parties (R11): a WoW group is the RO party.  Its window, invites, leader, party chat, member frames and loot method are the stock ones.  RO's
// own party settings are gone (Nathan 2026-11-10): exp is always RO's Even Share (within party_share_level base levels, else Each Take) and the items
// follow the group's WoW loot method (GetPartyItemShare).  What's kept here is the levels and jobs of the members in the RO world, which the map threads
// can't read off each other's identities.  So each member writes its own in PartiesByGroupGUID under PartyMutex from its own update (UpdateParty), which
// is rAthena's char server's party data with party_update_interval between its updates.  A member not in the RO form is offline to the party.
// PartyMutex is held only over that map, never across a call into the core

namespace
{
    // How often a member checks its group, level and job against what the party has (party_update_interval)
    constexpr uint32 PARTY_UPDATE_INTERVAL_IN_MS = 1000;
}

void ROWOWMod::LoadPartyData()
{
    std::lock_guard<std::mutex> lock(PartyMutex);
    PartiesByGroupGUID.clear();
}

uint8 ROWOWMod::GetPartyItemShare(Group const* group) const
{
    // The WoW loot method as RO's item share (Nathan 2026-11-10): free for all is RO's default (the loot priority, then anyone), round robin is RO's
    // round robin, group loot and need before greed a random member, master loot the master looter.  The loot threshold isn't used
    if (group == nullptr)
        return ROWOW_PARTY_ITEM_EACH_TAKE;
    switch (group->GetLootMethod())
    {
        case ROUND_ROBIN: return ROWOW_PARTY_ITEM_ROUND_ROBIN;
        case GROUP_LOOT:
        case NEED_BEFORE_GREED: return ROWOW_PARTY_ITEM_RANDOM;
        case MASTER_LOOT: return ROWOW_PARTY_ITEM_MASTER;
        default: return ROWOW_PARTY_ITEM_EACH_TAKE;
    }
}

bool ROWOWMod::IsPartyExpShared(Group const* group)
{
    // Even Share, always (Nathan 2026-11-10), while the members online to the party are within party_share_level base levels (int_party_check_lv).
    // Unlike rAthena's it isn't switched off for good past that: each kill looks again
    if (group == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(PartyMutex);
    auto partyIter = PartiesByGroupGUID.find(group->GetGUID().GetCounter());
    if (partyIter == PartiesByGroupGUID.end())
        return true;
    return PartyCheckExpShare(partyIter->second);
}

bool ROWOWMod::PartyCheckExpShare(ROWOWParty const& party) const
{
    // party_check_exp_share over int_party_check_lv's range: the members online within party_share_level base levels, or fewer than two online
    if (party.MembersByGUID.size() < 2)
        return true;
    uint32 minLevel = std::numeric_limits<uint32>::max();
    uint32 maxLevel = 0;
    for (auto const& member : party.MembersByGUID)
    {
        minLevel = std::min(minLevel, member.second.BaseLevel);
        maxLevel = std::max(maxLevel, member.second.BaseLevel);
    }
    return maxLevel - minLevel <= ROWOW_PARTY_SHARE_LEVEL;
}

void ROWOWMod::UpdateParty(Player* player, uint32 diff)
{
    // The char server's side of a member: joining, leaving, a level or job changing
    ROWOWIdentity* identity = GetIdentity(player);
    identity->PartyUpdateTimer += diff;
    if (identity->PartyUpdateTimer < PARTY_UPDATE_INTERVAL_IN_MS)
        return;
    identity->PartyUpdateTimer = 0;

    Group* group = player->GetGroup();
    uint32 groupGUID = group != nullptr ? group->GetGUID().GetCounter() : 0;
    if (groupGUID == identity->PartyGroupGUID && identity->BaseLevel == identity->PartyBaseLevel && identity->JobID == identity->PartyJobID)
        return;
    if (groupGUID != identity->PartyGroupGUID)
        PartyRemoveMember(player);
    identity->PartyGroupGUID = groupGUID;
    identity->PartyBaseLevel = identity->BaseLevel;
    identity->PartyJobID = identity->JobID;
    if (groupGUID != 0)
    {
        std::lock_guard<std::mutex> lock(PartyMutex);
        ROWOWPartyMember& member = PartiesByGroupGUID[groupGUID].MembersByGUID[player->GetGUID().GetCounter()];
        member.BaseLevel = identity->BaseLevel;
        member.JobID = identity->JobID;
    }
}

void ROWOWMod::PartyRemoveMember(Player* player)
{
    // A member going offline to the party (leaving the RO world or the game) or leaving it
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->PartyGroupGUID == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(PartyMutex);
        auto partyIter = PartiesByGroupGUID.find(identity->PartyGroupGUID);
        if (partyIter != PartiesByGroupGUID.end())
            partyIter->second.MembersByGUID.erase(player->GetGUID().GetCounter());
    }
    identity->PartyGroupGUID = 0;
}

void ROWOWMod::PartyDisband(Group const* group)
{
    // party_broken, on the world thread (or a map's, for a group the core disbands from an update)
    std::lock_guard<std::mutex> lock(PartyMutex);
    PartiesByGroupGUID.erase(group->GetGUID().GetCounter());
}

bool ROWOWMod::PartySkillCheckJob(Player const* sd, uint16 jobID)
{
    // party_check_state's snovice and tk: a member online anywhere of the job.  Read from the party's table, as another map's identities aren't this thread's
    Group const* group = sd->GetGroup();
    if (group == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(PartyMutex);
    auto partyIter = PartiesByGroupGUID.find(group->GetGUID().GetCounter());
    if (partyIter == PartiesByGroupGUID.end())
        return false;
    for (auto const& member : partyIter->second.MembersByGUID)
        if (GetNonBabyJobID(member.second.JobID) == jobID)
            return true;
    return false;
}

void ROWOWMod::PartyExpShare(Group* group, Creature* src, uint64 baseExp, uint64 jobExp)
{
    // party_exp_share: evenly among the members on the monster's map (on a stitched map, its part), alive (so its distance doesn't matter, as in
    // rAthena), more for each with party_even_share_bonus.  Members on the monster's map share its thread.  (idle_no_share isn't in: no idle time is kept; it's off)
    std::vector<Player*> members;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (member == nullptr || IsOnSameROMap(member, src) == false || member->IsAlive() == false || IsInROForm(member) == false)
            continue;
        members.push_back(member);
    }
    if (members.empty() == true)
        return;
    uint64 count = members.size();
    baseExp /= count;
    jobExp /= count;
    if (BattleConfig.PartyEvenShareBonus != 0 && count > 1)
    {
        double bonus = 100 + BattleConfig.PartyEvenShareBonus * (count - 1);
        if (baseExp != 0)
            baseExp = uint64(std::clamp(double(baseExp) * bonus / 100, 0.0, double(std::numeric_limits<uint32>::max())));
        if (jobExp != 0)
            jobExp = uint64(std::clamp(double(jobExp) * bonus / 100, 0.0, double(std::numeric_limits<uint32>::max())));
    }
    for (Player* member : members)
        PCGainExp(member, baseExp, jobExp, src);
}

bool ROWOWMod::PartyShareLoot(Player* sd, Group* group, ROWOWDropItem const& item, ObjectGuid const& firstCharID)
{
    // party_share_loot: with a shared loot method (GetPartyItemShare), the first_charid (or any item, party_item_share_type &1 off) goes to a member on
    // the map, alive: in turn, at random or the master looter, and to whoever picked it up when none has room.  False when that one has no room
    // either.  Members on one map share its thread
    Player* target = nullptr;
    uint8 itemShare = GetPartyItemShare(group);
    if (group != nullptr && itemShare != ROWOW_PARTY_ITEM_EACH_TAKE && (firstCharID.IsEmpty() == false || (BattleConfig.PartyShareType & 1) == 0))
    {
        std::vector<Player*> members;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member == nullptr || IsOnSameROMap(member, sd) == false || member->IsAlive() == false || IsInROForm(member) == false)
                continue;
            members.push_back(member);
        }
        if (itemShare == ROWOW_PARTY_ITEM_MASTER) // The master looter, when on the map, alive and with room
        {
            ObjectGuid masterLooterGUID = group->GetMasterLooterGuid();
            for (Player* member : members)
            {
                if (member->GetGUID() == masterLooterGUID && PCAddItem(member, item.ItemTemplateID, item.Amount, item.IsIdentified) == true)
                {
                    target = member;
                    break;
                }
            }
        }
        else if (itemShare == ROWOW_PARTY_ITEM_ROUND_ROBIN) // Round Robin
        {
            uint32 groupGUID = group->GetGUID().GetCounter();
            uint32 turn = 0;
            {
                std::lock_guard<std::mutex> lock(PartyMutex);
                turn = PartiesByGroupGUID[groupGUID].ItemTurn++;
            }
            for (size_t i = 0; i < members.size() && target == nullptr; i++)
            {
                Player* member = members[(turn + i) % members.size()];
                if (PCAddItem(member, item.ItemTemplateID, item.Amount, item.IsIdentified) == true)
                    target = member;
            }
        }
        else // Random pick
        {
            while (members.empty() == false)
            {
                size_t pick = urand(0, uint32(members.size() - 1)); // Pick a random member.
                if (PCAddItem(members[pick], item.ItemTemplateID, item.Amount, item.IsIdentified) == true)
                {
                    target = members[pick];
                    break;
                }
                members[pick] = members.back(); // Discard this receiver.
                members.pop_back();
            }
        }
    }
    if (target == nullptr)
    {
        target = sd; // Give it to the char that picked it up
        if (PCAddItem(sd, item.ItemTemplateID, item.Amount, item.IsIdentified) == false)
            return false;
    }

    // clif_party_show_picker: the party on the map sees who got a weapon, armor or card (show_picker.item_type)
    ROWOWItem const* roItem = GetROItemByItemTemplateID(item.ItemTemplateID);
    if (group != nullptr && BattleConfig.PartyShowSharePicker != 0 && roItem != nullptr && (BattleConfig.ShowPickerItemType & (1 << roItem->Type)) != 0)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(item.ItemTemplateID);
        std::string message = fmt::format("{} has got {}.", target->GetName(), itemTemplate != nullptr ? itemTemplate->Name1 : "an item");
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member != nullptr && member != target && IsOnSameROMap(member, sd) == true && IsInROForm(member) == true)
                ChatHandler(member->GetSession()).SendSysMessage(message.c_str());
        }
    }
    return true;
}

class ROWOW_PartyGroupScript : public GroupScript
{
public:
    ROWOW_PartyGroupScript() : GroupScript("ROWOW_PartyGroupScript") {}

    void OnDisband(Group* group) override
    {
        if (ROWOW->IsEnabled == true && group != nullptr)
            ROWOW->PartyDisband(group);
    }
};

void AddROWOWPartyScripts()
{
    new ROWOW_PartyGroupScript();
}
