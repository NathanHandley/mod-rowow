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

// RO's parties (R11): a WoW group is the RO party.  Its window, invites, leader, party chat and member frames are the stock ones.  What RO adds on top is
// here: the party's options (its share of exp and of items) and the levels and jobs of the members in the RO world, which the map threads can't read off
// each other's identities.  So each member writes its own in PartiesByGroupGUID under PartyMutex from its own update (UpdateParty), which is rAthena's
// char server's party data with party_update_interval between its updates.  A member not in the RO form is offline to the party.  PartyMutex is held only
// over that map, never across a call into the core

namespace
{
    // How often a member checks its group, level and job against what the party has (party_update_interval)
    constexpr uint32 PARTY_UPDATE_INTERVAL_IN_MS = 1000;

    void BroadcastToGroup(Group* group, uint32 language, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, Language(language), nullptr, nullptr, message);
        group->BroadcastPacket(&data, false);
    }

    void SendAddonMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, message);
        player->SendDirectMessage(&data);
    }

    std::string GetPartyOptionMessage(ROWOWPartyOption const& option)
    {
        return fmt::format("ROWOWPARTY\t{}\t{}\t{}", option.IsExpShare == true ? 1 : 0, (option.Item & 1) != 0 ? 1 : 0, (option.Item & 2) != 0 ? 1 : 0);
    }
}

void ROWOWMod::LoadPartyData()
{
    std::lock_guard<std::mutex> lock(PartyMutex);
    PartiesByGroupGUID.clear();
    if (IsEnabled == false)
        return;
    if (QueryResult result = CharacterDatabase.Query("SELECT `GroupGUID`, `ExpShare`, `ItemShare` FROM `mod_rowow_party`"))
    {
        do
        {
            Field* fields = result->Fetch();
            ROWOWParty& party = PartiesByGroupGUID[fields[0].Get<uint32>()];
            party.Option.IsExpShare = fields[1].Get<uint8>() != 0;
            party.Option.Item = fields[2].Get<uint8>() & 3;
        } while (result->NextRow());
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadPartyData loaded the options of {} RO parties", PartiesByGroupGUID.size());
}

ROWOWPartyOption ROWOWMod::GetPartyOption(Group const* group)
{
    ROWOWPartyOption option;
    if (group == nullptr)
        return option;
    std::lock_guard<std::mutex> lock(PartyMutex);
    auto partyIter = PartiesByGroupGUID.find(group->GetGUID().GetCounter());
    if (partyIter != PartiesByGroupGUID.end())
        option = partyIter->second.Option;
    return option;
}

void ROWOWMod::SendPartyToClient(Player* player)
{
    // For the RO_Identity addon's party window, which only hears of a change
    SendAddonMessage(player, GetPartyOptionMessage(GetPartyOption(player->GetGroup())));
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
    // The char server's side of a member: joining, leaving, a level or job changing.  Its level range then may break the even share
    // (int_party_check_lv), which goes off by itself and doesn't come back
    ROWOWIdentity* identity = GetIdentity(player);
    identity->PartyUpdateTimer += diff;
    if (identity->PartyUpdateTimer < PARTY_UPDATE_INTERVAL_IN_MS)
        return;
    identity->PartyUpdateTimer = 0;

    Group* group = player->GetGroup();
    uint32 groupGUID = group != nullptr ? group->GetGUID().GetCounter() : 0;
    if (groupGUID == identity->PartyGroupGUID && identity->BaseLevel == identity->PartyBaseLevel && identity->JobID == identity->PartyJobID)
        return;
    bool isJoined = groupGUID != identity->PartyGroupGUID;
    if (isJoined == true)
        PartyRemoveMember(player);
    identity->PartyGroupGUID = groupGUID;
    identity->PartyBaseLevel = identity->BaseLevel;
    identity->PartyJobID = identity->JobID;

    ROWOWPartyOption option;
    bool isShareBroken = false;
    if (groupGUID != 0)
    {
        std::lock_guard<std::mutex> lock(PartyMutex);
        ROWOWParty& party = PartiesByGroupGUID[groupGUID];
        ROWOWPartyMember& member = party.MembersByGUID[player->GetGUID().GetCounter()];
        member.BaseLevel = identity->BaseLevel;
        member.JobID = identity->JobID;
        if (party.Option.IsExpShare == true && PartyCheckExpShare(party) == false)
        {
            party.Option.IsExpShare = false;
            isShareBroken = true;
        }
        option = party.Option;
    }
    if (isShareBroken == true)
    {
        SavePartyOption(groupGUID, option);
        BroadcastToGroup(group, LANG_ADDON, GetPartyOptionMessage(option));
        BroadcastToGroup(group, LANG_UNIVERSAL, "Party EXP sharing is now Each Take: the members are too many levels apart.");
    }
    else if (isJoined == true)
        SendAddonMessage(player, GetPartyOptionMessage(option));
}

void ROWOWMod::PartyRemoveMember(Player* player)
{
    // A member going offline to the party (leaving the RO world or the game) or leaving it.  Its range only gets narrower: the even share stays as it is
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
    // party_broken, on the world thread (or a map's, for a group the core disbands from an update): the options go with the group
    uint32 groupGUID = group->GetGUID().GetCounter();
    {
        std::lock_guard<std::mutex> lock(PartyMutex);
        PartiesByGroupGUID.erase(groupGUID);
    }
    CharacterDatabase.Execute("DELETE FROM `mod_rowow_party` WHERE `GroupGUID` = {}", groupGUID);
}

void ROWOWMod::SavePartyOption(uint32 groupGUID, ROWOWPartyOption const& option)
{
    CharacterDatabase.Execute("REPLACE INTO `mod_rowow_party` (`GroupGUID`, `ExpShare`, `ItemShare`) VALUES ({}, {}, {})", groupGUID, option.IsExpShare == true ? 1 : 0, uint32(option.Item));
}

void ROWOWMod::PartyChangeOption(Player* sd, bool isExpShare, uint8 item)
{
    // party_changeoption and mapif_parse_PartyChangeOption, the leader's (rAthena's client only offers the setting to the leader): even share is
    // refused with members too far apart, the item options always go through.  A chat command: the world thread, with the maps idle
    Group* group = sd->GetGroup();
    if (group == nullptr || group->IsLeader(sd->GetGUID()) == false)
    {
        ChatHandler(sd->GetSession()).SendSysMessage("Only the party leader can change the party's settings.");
        return;
    }
    uint32 groupGUID = group->GetGUID().GetCounter();
    bool isRefused = false;
    ROWOWPartyOption option;
    {
        std::lock_guard<std::mutex> lock(PartyMutex);
        ROWOWParty& party = PartiesByGroupGUID[groupGUID];
        if (isExpShare == true && PartyCheckExpShare(party) == false)
        {
            isExpShare = false;
            isRefused = true;
        }
        party.Option.IsExpShare = isExpShare;
        party.Option.Item = item & 3; // Filter out invalid values.
        option = party.Option;
    }
    SavePartyOption(groupGUID, option);
    BroadcastToGroup(group, LANG_ADDON, GetPartyOptionMessage(option));
    if (isRefused == true)
        ChatHandler(sd->GetSession()).PSendSysMessage("You can't set EXP sharing to Even Share: the party's members must be within {} base levels.", ROWOW_PARTY_SHARE_LEVEL);
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

bool ROWOWMod::PartyShareLoot(Player* sd, Group* group, ROWOWPartyOption const& option, ROWOWDropItem const& item, ObjectGuid const& firstCharID)
{
    // party_share_loot: with the even share, the first_charid (or any item, party_item_share_type &1 off) goes to a member on the map, alive, in turn
    // (&2) or at random, and to whoever picked it up when none has room.  False when that one has no room either.  Members on one map share its thread
    Player* target = nullptr;
    if (group != nullptr && (option.Item & 2) != 0 && (firstCharID.IsEmpty() == false || (BattleConfig.PartyShareType & 1) == 0))
    {
        std::vector<Player*> members;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member == nullptr || IsOnSameROMap(member, sd) == false || member->IsAlive() == false || IsInROForm(member) == false)
                continue;
            members.push_back(member);
        }
        if ((BattleConfig.PartyShareType & 2) != 0) // Round Robin
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
