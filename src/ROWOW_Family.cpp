//  Author: Nathan Handley (nathanhandley@protonmail.com)
//  Copyright (c) 2026 Nathan Handley
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//  Ported from rAthena (https://github.com/rathena/rathena), pc.cpp (pc_marriage, pc_divorce, pc_try_adopt, pc_adoption), clif.cpp
//  (clif_parse_Adopt_request, clif_parse_Adopt_reply), skill.cpp (UNT_CALLFAMILY) and skills/other (WE_*), pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

// Marriage and adoption (Nathan 2026-11-11), the only way to a Baby job in pre-renewal RO.  The family is the RO identity's (its save slot's: rAthena's
// char_id is the identity's ID), so a character's three slots each have their own, and a reset identity is a new RO character with none (the others'
// links to it go too).  Partner and child are other characters' save slots.  It's kept for every identity in one table under FamilyMutex
// (mod_rowow_identity_family, written at each change), not on the player: a marriage, divorce or adoption changes two or three identities at once.
// - Marriage: the Prontera church's official script (npc/other/marriage.txt: Marry Happy's application, then Bishop Vomars, groom first, in a party of
//   two) and its script commands (marriage, wedding, getpartnerid...).  The rings 2634 and 2635 grant WE_MALE, WE_FEMALE and WE_CALLPARTNER while worn
// - Divorce: Deviruchi in Niflheim (npc/other/divorce.txt, divorce).  Both lose their wedding ring: an away partner when that slot is next in Midgard
// - Adoption: a married couple, both base 70 or more, wearing their rings, in a party (a WoW group) with a Novice, first job or Super Novice who isn't
//   married or adopted, asks it (the unit menu's "Adopt as Child", .ro adopt <name>); it answers in a popup (.ro adopt accept|decline).  It becomes the
//   Baby job of its job, keeping its levels, and gets WE_BABY and WE_CALLPARENT, its parents WE_CALLBABY
// Commands run in the world thread (chat), with the maps idle, so they touch all three players; the skills run in the caster's map thread and only
// touch players on its map (a call to another map waits for the called's own update)

#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"
#include "ROWOW_JobMapIDs.h"

namespace
{
    constexpr uint64 ROWOW_ADOPT_INVITE_TIME_IN_MS = 65 * IN_MILLISECONDS; // The child's popup times out at 60 s
    constexpr uint64 ROWOW_FAMILY_CALL_TIME_IN_MS = 10 * IN_MILLISECONDS; // A call its player didn't take in this long (logged out, between maps) is dropped
    constexpr uint32 ROWOW_ADOPT_PARENT_BASE_LEVEL = 70;

    void SendAddonText(Player* player, std::string const& addonMessage)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
        player->SendDirectMessage(&data);
    }

    void SendText(Player* player, std::string const& text)
    {
        if (player != nullptr && player->GetSession() != nullptr)
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    // The family's row as it is now (FamilyMutex held): none left when it has no links
    void AppendFamilyRow(CharacterDatabaseTransaction trans, ObjectGuid::LowType identityID, ROWOWFamily const* family)
    {
        if (family == nullptr || family->IsEmpty() == true)
        {
            trans->Append("DELETE FROM `mod_rowow_identity_family` WHERE `guid` = {}", identityID);
            return;
        }
        trans->Append("REPLACE INTO `mod_rowow_identity_family` (`guid`, `partner_id`, `father_id`, `mother_id`, `child_id`, `ring_removal_pending`) VALUES ({}, {}, {}, {}, {}, {})",
            identityID, family->PartnerID, family->FatherID, family->MotherID, family->ChildID, family->IsRingRemovalPending == true ? 1 : 0);
    }

    // pc_try_adopt's answers, as the parent who asked is told (rAthena's are client messages; these say what's needed)
    std::string GetAdoptRefusalText(int32 response, std::string const& babyName)
    {
        switch (response)
        {
            case ROWOW_ADOPT_ALREADY_ADOPTED: return babyName + " already has parents, or is being asked to be someone's child right now.";
            case ROWOW_ADOPT_MARRIED_AND_PARTY: return "To adopt a child you must be married, and you, your partner and the child must all be in the same party.";
            case ROWOW_ADOPT_EQUIP_RINGS: return "You and your partner must both be wearing your wedding rings to adopt a child.";
            case ROWOW_ADOPT_NOT_NOVICE: return "Only a Novice, a first job or a Super Novice can be adopted: " + babyName + " is a second job, a transcendent job, a Baby or another job.";
            case ROWOW_ADOPT_CHARACTER_NOT_FOUND: return "You, your partner and " + babyName + " must all be online and in Midgard to adopt.";
            case ROWOW_ADOPT_MORE_CHILDREN: return "You can't adopt more than one child.";
            case ROWOW_ADOPT_LEVEL_70: return "You and your partner must both be at least base level 70 to adopt a child.";
            case ROWOW_ADOPT_MARRIED: return "You can't adopt a married character.";
            default: return "The adoption failed.";
        }
    }

    // The jobs that may be adopted (pc_try_adopt): JOB_NOVICE to JOB_THIEF, and the Super Novice
    bool IsAdoptableJob(uint16 jobID)
    {
        return jobID <= ROWOW_JOB_THIEF || jobID == ROWOW_JOB_SUPER_NOVICE;
    }
}

void ROWOWMod::LoadFamilyData()
{
    // Once, with FamilyMutex held: every family, and the characters whose save slots they name (a slot past the first has its own ID)
    if (IsFamilyLoaded == true)
        return;
    IsFamilyLoaded = true;
    FamiliesByIdentityID.clear();
    CharacterGUIDsByIdentityID.clear();
    QueryResult tableResult = CharacterDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_identity_family'");
    if (!tableResult)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadFamilyData found no mod_rowow_identity_family table, so no RO identity can marry or adopt (apply mod_rowow_identity_family.sql)");
        return;
    }
    if (QueryResult result = CharacterDatabase.Query("SELECT `guid`, `partner_id`, `father_id`, `mother_id`, `child_id`, `ring_removal_pending` FROM `mod_rowow_identity_family`"))
    {
        do
        {
            Field* fields = result->Fetch();
            ROWOWFamily& family = FamiliesByIdentityID[fields[0].Get<uint32>()];
            family.PartnerID = fields[1].Get<uint32>();
            family.FatherID = fields[2].Get<uint32>();
            family.MotherID = fields[3].Get<uint32>();
            family.ChildID = fields[4].Get<uint32>();
            family.IsRingRemovalPending = fields[5].Get<uint8>() != 0;
        } while (result->NextRow());
    }
    if (QueryResult result = CharacterDatabase.Query("SELECT `identity`, `guid` FROM `mod_rowow_identity_slot`"))
    {
        do
        {
            Field* fields = result->Fetch();
            CharacterGUIDsByIdentityID[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
        } while (result->NextRow());
    }
    uint32 pendingCount = 0;
    for (auto const& familyByIdentityID : FamiliesByIdentityID)
        if (familyByIdentityID.second.IsRingRemovalPending == true)
            pendingCount++;
    FamilyPendingCount = pendingCount + uint32(FamilyCallsByCharacterGUID.size());
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadFamilyData loaded {} RO identity families", FamiliesByIdentityID.size());
}

ROWOWFamily ROWOWMod::GetFamily(ObjectGuid::LowType identityID)
{
    if (identityID == 0)
        return ROWOWFamily();
    std::lock_guard<std::mutex> lock(FamilyMutex);
    LoadFamilyData();
    auto familyIter = FamiliesByIdentityID.find(identityID);
    return familyIter != FamiliesByIdentityID.end() ? familyIter->second : ROWOWFamily();
}

ROWOWFamily ROWOWMod::GetFamily(Player* player)
{
    ROWOWIdentity const* identity = player != nullptr ? player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
    if (identity == nullptr || identity->Exists == false)
        return ROWOWFamily();
    return GetFamily(identity->IdentityID);
}

ObjectGuid::LowType ROWOWMod::GetIdentityCharacterGUID(ObjectGuid::LowType identityID)
{
    // Slot 1's identity is the character's GUID; the others' are in mod_rowow_identity_slot (cached when loaded and when linked)
    if (identityID == 0)
        return 0;
    if (identityID < ROWOW_IDENTITY_ID_SLOT_START)
        return identityID;
    {
        std::lock_guard<std::mutex> lock(FamilyMutex);
        LoadFamilyData();
        auto guidIter = CharacterGUIDsByIdentityID.find(identityID);
        if (guidIter != CharacterGUIDsByIdentityID.end())
            return guidIter->second;
    }
    QueryResult result = CharacterDatabase.Query("SELECT `guid` FROM `mod_rowow_identity_slot` WHERE `identity` = {}", identityID);
    if (!result)
        return 0;
    ObjectGuid::LowType characterGUID = result->Fetch()[0].Get<uint32>();
    std::lock_guard<std::mutex> lock(FamilyMutex);
    CharacterGUIDsByIdentityID[identityID] = characterGUID;
    return characterGUID;
}

std::string ROWOWMod::GetIdentityCharacterName(ObjectGuid::LowType identityID)
{
    ObjectGuid::LowType characterGUID = GetIdentityCharacterGUID(identityID);
    std::string name;
    if (characterGUID == 0 || sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(characterGUID), name) == false)
        return std::string();
    return name;
}

Player* ROWOWMod::FindIdentityPlayer(ObjectGuid::LowType identityID, bool isInFormRequired)
{
    // map_charid2sd: the identity's character online with that save slot in use (and in the RO world, when asked: rAthena's map server only has those)
    ObjectGuid::LowType characterGUID = GetIdentityCharacterGUID(identityID);
    if (characterGUID == 0)
        return nullptr;
    Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(characterGUID));
    if (player == nullptr || player->IsInWorld() == false)
        return nullptr;
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->Exists == false || identity->IdentityID != identityID || (isInFormRequired == true && identity->IsInForm == false))
        return nullptr;
    return player;
}

Player* ROWOWMod::FindFamilyPlayerOnMap(Player* player, ObjectGuid::LowType identityID)
{
    // The family member on the player's RO map (the same thread), in the form
    if (player == nullptr || identityID == 0)
        return nullptr;
    ObjectGuid::LowType characterGUID = GetIdentityCharacterGUID(identityID);
    if (characterGUID == 0)
        return nullptr;
    Player* member = ObjectAccessor::GetPlayer(player->GetMap(), ObjectGuid::Create<HighGuid::Player>(characterGUID));
    if (member == nullptr || IsOnSameROMap(player, member) == false)
        return nullptr;
    ROWOWIdentity const* identity = member->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->IdentityID != identityID || identity->IsInForm == false)
        return nullptr;
    return member;
}

bool ROWOWMod::IsWearingWeddingRing(Player* player)
{
    // pc_isequipped(sd, WEDDING_RING_M) || pc_isequipped(sd, WEDDING_RING_F): RO's equipment is in the bags, by the positions it's worn in
    ROWOWIdentity* identity = GetIdentity(player);
    for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
    {
        ROWOWItem const* roItem = GetROItem(player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDByEquipPosition.second)));
        if (roItem != nullptr && (roItem->ItemID == ROWOW_ITEM_WEDDING_RING_M || roItem->ItemID == ROWOW_ITEM_WEDDING_RING_F))
            return true;
    }
    return false;
}

void ROWOWMod::RemoveWeddingRings(Player* player)
{
    // pc_divorce's pc_delitem of every WEDDING_RING_M and WEDDING_RING_F in the inventory, worn ones too (taken off first, so their skills go)
    ROWOWIdentity* identity = GetIdentity(player);
    std::vector<uint32> wornPositions;
    for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
    {
        ROWOWItem const* roItem = GetROItem(player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDByEquipPosition.second)));
        if (roItem != nullptr && (roItem->ItemID == ROWOW_ITEM_WEDDING_RING_M || roItem->ItemID == ROWOW_ITEM_WEDDING_RING_F))
            wornPositions.push_back(itemGUIDByEquipPosition.first);
    }
    for (uint32 position : wornPositions)
        PCUnequipItem(player, position, 1);
    for (uint32 ringItemID : { ROWOW_ITEM_WEDDING_RING_M, ROWOW_ITEM_WEDDING_RING_F })
    {
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(ringItemID);
        if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
            continue;
        uint32 count = player->GetItemCount(itemTemplateIDIter->second);
        if (count > 0)
            player->DestroyItemCount(itemTemplateIDIter->second, count, true);
    }
    if (wornPositions.empty() == false)
        SendEquipmentToClient(player, *identity, false);
}

bool ROWOWMod::PCMarriage(Player* sd, Player* dstsd)
{
    // pc_marriage: both RO characters in the RO world, unmarried, neither a Baby; and a man and a woman (marriage.txt: "a man can only marry a woman and
    // vice versa"; the church's script checks it, this too)
    if (sd == nullptr || dstsd == nullptr || sd == dstsd || IsInROForm(sd) == false || IsInROForm(dstsd) == false)
        return false;
    ROWOWIdentity* identity = GetIdentity(sd);
    ROWOWIdentity* dstIdentity = GetIdentity(dstsd);
    ROWOWJob const* job = GetJob(identity->JobID);
    ROWOWJob const* dstJob = GetJob(dstIdentity->JobID);
    if (identity->Exists == false || dstIdentity->Exists == false || (job != nullptr && job->IsBaby == true) || (dstJob != nullptr && dstJob->IsBaby == true) ||
        sd->getGender() == dstsd->getGender())
        return false;
    {
        std::lock_guard<std::mutex> lock(FamilyMutex);
        LoadFamilyData();
        ROWOWFamily& family = FamiliesByIdentityID[identity->IdentityID];
        ROWOWFamily& dstFamily = FamiliesByIdentityID[dstIdentity->IdentityID];
        if (family.PartnerID != 0 || dstFamily.PartnerID != 0)
        {
            if (family.IsEmpty() == true)
                FamiliesByIdentityID.erase(identity->IdentityID);
            if (dstFamily.IsEmpty() == true)
                FamiliesByIdentityID.erase(dstIdentity->IdentityID);
            return false;
        }
        family.PartnerID = dstIdentity->IdentityID;
        dstFamily.PartnerID = identity->IdentityID;
        CharacterGUIDsByIdentityID[identity->IdentityID] = identity->CharacterGUID;
        CharacterGUIDsByIdentityID[dstIdentity->IdentityID] = dstIdentity->CharacterGUID;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        AppendFamilyRow(trans, identity->IdentityID, &family);
        AppendFamilyRow(trans, dstIdentity->IdentityID, &dstFamily);
        CharacterDatabase.CommitTransaction(trans);
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::PCMarriage: {} (RO identity {}) married {} (RO identity {})", sd->GetName(), identity->IdentityID, dstsd->GetName(), dstIdentity->IdentityID);
    SendFamilyToClient(sd);
    SendFamilyToClient(dstsd);
    return true;
}

bool ROWOWMod::PCDivorce(Player* sd)
{
    // pc_divorce: both are single again and lose their wedding rings (the partner's when its save slot is next in the RO world, if it isn't now: rAthena's
    // char server does an offline partner's at once)
    if (sd == nullptr)
        return false;
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->Exists == false)
        return false;
    ObjectGuid::LowType partnerID = 0;
    {
        std::lock_guard<std::mutex> lock(FamilyMutex);
        LoadFamilyData();
        auto familyIter = FamiliesByIdentityID.find(identity->IdentityID);
        if (familyIter == FamiliesByIdentityID.end() || familyIter->second.PartnerID == 0)
            return false;
        partnerID = familyIter->second.PartnerID;
        familyIter->second.PartnerID = 0;
        if (identity->IsInForm == false)
            familyIter->second.IsRingRemovalPending = true;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        auto partnerIter = FamiliesByIdentityID.find(partnerID);
        if (partnerIter != FamiliesByIdentityID.end() && partnerIter->second.PartnerID == identity->IdentityID)
        {
            partnerIter->second.PartnerID = 0;
            partnerIter->second.IsRingRemovalPending = true; // Taken in its own update, whatever thread it's in now
            AppendFamilyRow(trans, partnerID, &partnerIter->second);
            if (partnerIter->second.IsEmpty() == true)
                FamiliesByIdentityID.erase(partnerIter);
        }
        familyIter = FamiliesByIdentityID.find(identity->IdentityID);
        AppendFamilyRow(trans, identity->IdentityID, &familyIter->second);
        if (familyIter->second.IsEmpty() == true)
            FamiliesByIdentityID.erase(familyIter);
        CharacterDatabase.CommitTransaction(trans);
        uint32 pendingCount = 0;
        for (auto const& familyByIdentityID : FamiliesByIdentityID)
            if (familyByIdentityID.second.IsRingRemovalPending == true)
                pendingCount++;
        FamilyPendingCount = pendingCount + uint32(FamilyCallsByCharacterGUID.size());
    }
    if (identity->IsInForm == true)
        RemoveWeddingRings(sd);
    std::string partnerName = GetIdentityCharacterName(partnerID);
    LOG_INFO("module.ROWOW", "ROWOWMod::PCDivorce: {} (RO identity {}) divorced {} (RO identity {})", sd->GetName(), identity->IdentityID, partnerName, partnerID);
    // clif_divorced
    SendText(sd, "You and " + (partnerName.empty() == false ? partnerName : std::string("your partner")) + " are divorced.");
    SendFamilyToClient(sd);
    if (Player* partner = FindIdentityPlayer(partnerID, false))
    {
        SendText(partner, sd->GetName() + " has divorced you.");
        SendFamilyToClient(partner);
    }
    return true;
}

int32 ROWOWMod::PCTryAdopt(Player* p1, Player* p2, Player* baby, bool isAnswer)
{
    // pc_try_adopt, in its order.  isAnswer: the child's own yes (its adopt_invite is the one asked, cleared before pc_adoption)
    if (p1 == nullptr || p2 == nullptr || baby == nullptr || IsInROForm(p1) == false || IsInROForm(p2) == false || IsInROForm(baby) == false)
        return ROWOW_ADOPT_CHARACTER_NOT_FOUND;
    ROWOWIdentity* identity1 = GetIdentity(p1);
    ROWOWIdentity* identity2 = GetIdentity(p2);
    ROWOWIdentity* babyIdentity = GetIdentity(baby);
    if (identity1->Exists == false || identity2->Exists == false || babyIdentity->Exists == false)
        return ROWOW_ADOPT_CHARACTER_NOT_FOUND;
    ROWOWFamily family1 = GetFamily(identity1->IdentityID);
    ROWOWFamily family2 = GetFamily(identity2->IdentityID);
    ROWOWFamily babyFamily = GetFamily(babyIdentity->IdentityID);
    bool isAsked = false;
    {
        std::lock_guard<std::mutex> lock(FamilyMutex);
        auto inviteIter = AdoptInvitesByCharacterGUID.find(baby->GetGUID().GetCounter());
        isAsked = inviteIter != AdoptInvitesByCharacterGUID.end() && inviteIter->second.ExpireTime > uint64(GameTime::GetGameTimeMS().count());
    }
    if (babyFamily.FatherID != 0 || babyFamily.MotherID != 0 || (isAsked == true && isAnswer == false))
        return ROWOW_ADOPT_ALREADY_ADOPTED; // already adopted baby / in adopt request
    Group* group1 = p1->GetGroup();
    if (family1.PartnerID == 0 || group1 == nullptr || group1 != baby->GetGroup())
        return ROWOW_ADOPT_MARRIED_AND_PARTY; // You need to be married and in party with baby to adopt
    if (family1.PartnerID != identity2->IdentityID || family2.PartnerID != identity1->IdentityID)
        return ROWOW_ADOPT_MARRIED_AND_PARTY; // Not married, wrong married
    if (p2->GetGroup() != group1)
        return ROWOW_ADOPT_MARRIED_AND_PARTY; // Both parents need to be in the same party
    // Parents need to have their ring equipped
    if (IsWearingWeddingRing(p1) == false || IsWearingWeddingRing(p2) == false)
        return ROWOW_ADOPT_EQUIP_RINGS;
    // Already adopted a baby
    if (family1.ChildID != 0 || family2.ChildID != 0)
        return ROWOW_ADOPT_MORE_CHILDREN;
    // Parents need at least lvl 70 to adopt
    if (identity1->BaseLevel < ROWOW_ADOPT_PARENT_BASE_LEVEL || identity2->BaseLevel < ROWOW_ADOPT_PARENT_BASE_LEVEL)
        return ROWOW_ADOPT_LEVEL_70;
    if (babyFamily.PartnerID != 0)
        return ROWOW_ADOPT_MARRIED;
    if (IsAdoptableJob(babyIdentity->JobID) == false)
        return ROWOW_ADOPT_NOT_NOVICE;
    return ROWOW_ADOPT_ALLOWED;
}

void ROWOWMod::SendAdoptRefusal(Player* player, Player* baby, int32 response)
{
    SendText(player, GetAdoptRefusalText(response, baby != nullptr ? baby->GetName() : std::string("The child")));
}

bool ROWOWMod::PCAdoptRequest(Player* parent, Player* baby)
{
    // clif_parse_Adopt_request: the parent who asks, its partner (online in the RO world), and the child.  Allowed, the child is asked; its answer is
    // .ro adopt accept or decline (PCAdoptReply)
    if (parent == nullptr || IsInROForm(parent) == false)
        return false;
    if (baby == nullptr || baby == parent)
    {
        SendText(parent, "Right-click the character you want to adopt (or type .ro adopt <name>): it must be in your party, online and in Midgard.");
        return false;
    }
    ROWOWFamily family = GetFamily(parent);
    Player* partner = family.PartnerID != 0 ? FindIdentityPlayer(family.PartnerID, true) : nullptr;
    int32 response = family.PartnerID == 0 ? ROWOW_ADOPT_MARRIED_AND_PARTY : PCTryAdopt(parent, partner, baby, false);
    if (response != ROWOW_ADOPT_ALLOWED)
    {
        SendAdoptRefusal(parent, baby, response);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(FamilyMutex);
        ROWOWAdoptInvite& invite = AdoptInvitesByCharacterGUID[baby->GetGUID().GetCounter()];
        invite.ParentGUID = parent->GetGUID().GetCounter();
        invite.PartnerGUID = partner->GetGUID().GetCounter();
        invite.ExpireTime = uint64(GameTime::GetGameTimeMS().count()) + ROWOW_ADOPT_INVITE_TIME_IN_MS;
    }
    // clif_Adopt_request
    SendAddonText(baby, fmt::format("ROWOWADOPT\tQ\t{}\t{}", parent->GetName(), partner->GetName()));
    SendText(baby, parent->GetName() + " and " + partner->GetName() + " want to adopt you.");
    SendText(parent, "You asked " + baby->GetName() + " to be your child.");
    SendText(partner, parent->GetName() + " asked " + baby->GetName() + " to be your child.");
    return true;
}

void ROWOWMod::PCAdoptReply(Player* baby, bool isAccepted)
{
    // clif_parse_Adopt_reply: the invite is used up whatever the answer; both parents must still be online
    ROWOWAdoptInvite invite;
    bool hasInvite = false;
    {
        std::lock_guard<std::mutex> lock(FamilyMutex);
        auto inviteIter = AdoptInvitesByCharacterGUID.find(baby->GetGUID().GetCounter());
        if (inviteIter != AdoptInvitesByCharacterGUID.end())
        {
            invite = inviteIter->second;
            hasInvite = invite.ExpireTime > uint64(GameTime::GetGameTimeMS().count());
            AdoptInvitesByCharacterGUID.erase(inviteIter);
        }
    }
    SendAddonText(baby, "ROWOWADOPT\tC");
    if (hasInvite == false)
    {
        SendText(baby, "No one is asking to adopt you now.");
        return;
    }
    Player* parent = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(invite.ParentGUID));
    Player* partner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(invite.PartnerGUID));
    if (isAccepted == false)
    {
        SendText(baby, "You declined to be adopted.");
        SendText(parent, baby->GetName() + " declined to be your child.");
        SendText(partner, baby->GetName() + " declined to be your child.");
        return;
    }
    if (parent == nullptr || partner == nullptr)
    {
        SendText(baby, "Both of your new parents must still be online for the adoption.");
        return;
    }
    PCAdoption(parent, partner, baby);
}

bool ROWOWMod::PCAdoption(Player* p1, Player* p2, Player* baby)
{
    // pc_adoption: the child's job becomes the Baby job of its job (pc_jobchange), keeping its job level and exp (and its base level, stats and skills, as a
    // job change does); father and mother by their sex (rAthena takes the one who asked as the father); WE_BABY and WE_CALLPARENT for the child, WE_CALLBABY
    // for the parents, ADDSKILL_PERMANENT.  (rAthena also grants WE_CHEERUP, a renewal skill that pre-renewal's skill_db leaves empty: not here)
    int32 response = PCTryAdopt(p1, p2, baby, true);
    if (response != ROWOW_ADOPT_ALLOWED)
    {
        SendAdoptRefusal(p1, baby, response);
        SendText(baby, "The adoption failed: " + GetAdoptRefusalText(response, baby->GetName()));
        return false;
    }
    ROWOWIdentity* babyIdentity = GetIdentity(baby);
    int64 jobClass = PCJobIDToMapID(int32(babyIdentity->JobID));
    int32 babyJobID = jobClass >= 0 ? PCMapIDToJobID(uint64(jobClass) | ROWOW_JOBL_BABY, baby->getGender() == GENDER_MALE) : -1;
    if (babyJobID < 0 || GetJob(uint16(babyJobID)) == nullptr)
    {
        SendText(p1, "The adoption failed: " + baby->GetName() + "'s job has no Baby job.");
        return false;
    }

    // Preserve current job levels and progress
    uint32 jobLevel = babyIdentity->JobLevel;
    uint32 jobExp = babyIdentity->JobExp;
    uint16 oldJobID = babyIdentity->JobID;
    PCJobChange(baby, uint16(babyJobID));
    if (babyIdentity->JobID != uint16(babyJobID))
    {
        SendText(p1, "The adoption failed: " + baby->GetName() + "'s job couldn't change.");
        return false;
    }
    babyIdentity->JobLevel = jobLevel;
    babyIdentity->JobExp = jobExp;

    Player* father = p1->getGender() == GENDER_MALE ? p1 : p2;
    Player* mother = father == p1 ? p2 : p1;
    ROWOWIdentity* fatherIdentity = GetIdentity(father);
    ROWOWIdentity* motherIdentity = GetIdentity(mother);
    {
        std::lock_guard<std::mutex> lock(FamilyMutex);
        LoadFamilyData();
        ROWOWFamily& fatherFamily = FamiliesByIdentityID[fatherIdentity->IdentityID];
        ROWOWFamily& motherFamily = FamiliesByIdentityID[motherIdentity->IdentityID];
        ROWOWFamily& babyFamily = FamiliesByIdentityID[babyIdentity->IdentityID];
        fatherFamily.ChildID = babyIdentity->IdentityID;
        motherFamily.ChildID = babyIdentity->IdentityID;
        babyFamily.FatherID = fatherIdentity->IdentityID;
        babyFamily.MotherID = motherIdentity->IdentityID;
        CharacterGUIDsByIdentityID[fatherIdentity->IdentityID] = fatherIdentity->CharacterGUID;
        CharacterGUIDsByIdentityID[motherIdentity->IdentityID] = motherIdentity->CharacterGUID;
        CharacterGUIDsByIdentityID[babyIdentity->IdentityID] = babyIdentity->CharacterGUID;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        AppendFamilyRow(trans, fatherIdentity->IdentityID, &fatherFamily);
        AppendFamilyRow(trans, motherIdentity->IdentityID, &motherFamily);
        AppendFamilyRow(trans, babyIdentity->IdentityID, &babyFamily);
        CharacterDatabase.CommitTransaction(trans);
    }

    // Baby skills, and the parents': learned as a quest NPC teaches (SKILL_FLAG_PERMANENT), on the Baby and Novice trees
    auto grantSkill = [this](Player* player, uint16 skillID)
    {
        SetIdentityVariable(*GetIdentity(player), fmt::format(GrantedSkillVariableFormat, skillID), 0);
        PCSkill(player, skillID, 1);
    };
    grantSkill(baby, ROWOW_SKILL_WE_BABY);
    grantSkill(baby, ROWOW_SKILL_WE_CALLPARENT);
    grantSkill(p1, ROWOW_SKILL_WE_CALLBABY);
    grantSkill(p2, ROWOW_SKILL_WE_CALLBABY);
    RefreshROForm(baby); // The job level back, the Baby's size, stat cap and look

    // chrif_save of all three
    for (Player* player : { p1, p2, baby })
        player->SaveToDB(false, false);

    ROWOWJob const* oldJob = GetJob(oldJobID);
    ROWOWJob const* newJob = GetJob(uint16(babyJobID));
    LOG_INFO("module.ROWOW", "ROWOWMod::PCAdoption: {} ({} -> {}, RO identity {}) adopted by {} and {}", baby->GetName(), oldJob != nullptr ? oldJob->Name : "?",
        newJob != nullptr ? newJob->Name : "?", babyIdentity->IdentityID, father->GetName(), mother->GetName());
    std::string text = baby->GetName() + " is now the child of " + father->GetName() + " and " + mother->GetName() + ".";
    for (Player* player : { p1, p2, baby })
    {
        SendText(player, text);
        SendFamilyToClient(player);
    }
    std::string newJobName = newJob != nullptr ? newJob->Name : "Baby";
    std::replace(newJobName.begin(), newJobName.end(), '_', ' '); // Baby_Swordman as Baby Swordman
    SendText(baby, "You are now a " + newJobName + ", with your levels, stats and skills.");
    return true;
}

void ROWOWMod::ClearIdentityFamily(ObjectGuid::LowType identityID, CharacterDatabaseTransaction trans)
{
    // A reset or deleted identity is a new RO character (or none): it has no family, and the others' links to it go.  Its partner is divorced and loses its
    // ring as a divorce takes it (char_delete's char_divorce_char_sql); its parents no longer have a child; its child no longer has that parent (it stays a
    // Baby: rAthena keeps an adopted child's job)
    std::vector<ObjectGuid::LowType> changedIDs;
    {
        std::lock_guard<std::mutex> lock(FamilyMutex);
        LoadFamilyData();
        auto familyIter = FamiliesByIdentityID.find(identityID);
        if (familyIter == FamiliesByIdentityID.end())
            return;
        ROWOWFamily family = familyIter->second;
        FamiliesByIdentityID.erase(familyIter);
        AppendFamilyRow(trans, identityID, nullptr);
        auto unlink = [&](ObjectGuid::LowType otherID, std::function<void(ROWOWFamily&)> const& change)
        {
            auto otherIter = otherID != 0 ? FamiliesByIdentityID.find(otherID) : FamiliesByIdentityID.end();
            if (otherIter == FamiliesByIdentityID.end())
                return;
            change(otherIter->second);
            AppendFamilyRow(trans, otherID, &otherIter->second);
            if (otherIter->second.IsEmpty() == true)
                FamiliesByIdentityID.erase(otherIter);
            changedIDs.push_back(otherID);
        };
        unlink(family.PartnerID, [identityID](ROWOWFamily& other)
        {
            if (other.PartnerID == identityID)
            {
                other.PartnerID = 0;
                other.IsRingRemovalPending = true;
            }
        });
        for (ObjectGuid::LowType parentID : { family.FatherID, family.MotherID })
            unlink(parentID, [identityID](ROWOWFamily& other)
            {
                if (other.ChildID == identityID)
                    other.ChildID = 0;
            });
        unlink(family.ChildID, [identityID](ROWOWFamily& other)
        {
            if (other.FatherID == identityID)
                other.FatherID = 0;
            if (other.MotherID == identityID)
                other.MotherID = 0;
        });
        uint32 pendingCount = 0;
        for (auto const& familyByIdentityID : FamiliesByIdentityID)
            if (familyByIdentityID.second.IsRingRemovalPending == true)
                pendingCount++;
        FamilyPendingCount = pendingCount + uint32(FamilyCallsByCharacterGUID.size());
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::ClearIdentityFamily: RO identity {} has no family now ({} others changed)", identityID, changedIDs.size());
    for (ObjectGuid::LowType changedID : changedIDs)
        if (Player* player = FindIdentityPlayer(changedID, false))
            SendFamilyToClient(player);
}

void ROWOWMod::RequestFamilyCall(Player* caster, ObjectGuid::LowType identityID)
{
    // UNT_CALLFAMILY (skill_unit_timer_sub): the called comes to the caster's cell, unless its map is nowarp.  It's moved in its own update (it may be on
    // another map, another thread)
    if (caster == nullptr || identityID == 0)
        return;
    ObjectGuid::LowType characterGUID = GetIdentityCharacterGUID(identityID);
    if (characterGUID == 0)
        return;
    ROWOWFamilyCall call;
    call.IdentityID = identityID;
    call.Destination.IsSet = true;
    call.Destination.MapID = caster->GetMapId();
    call.Destination.X = caster->GetPositionX();
    call.Destination.Y = caster->GetPositionY();
    call.Destination.Z = caster->GetPositionZ();
    call.Destination.O = caster->GetOrientation();
    call.RequestTime = uint64(GameTime::GetGameTimeMS().count());
    std::lock_guard<std::mutex> lock(FamilyMutex);
    FamilyCallsByCharacterGUID[characterGUID] = call;
    uint32 pendingCount = 0;
    for (auto const& familyByIdentityID : FamiliesByIdentityID)
        if (familyByIdentityID.second.IsRingRemovalPending == true)
            pendingCount++;
    FamilyPendingCount = pendingCount + uint32(FamilyCallsByCharacterGUID.size());
}

void ROWOWMod::UpdateFamily(Player* player)
{
    // The player's own update: a family call to answer, and the ring a divorce took while this save slot was away
    if (FamilyPendingCount.load() == 0)
        return;
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->Exists == false || player->IsInWorld() == false || player->IsBeingTeleported() == true)
        return;
    bool isRingRemoved = false;
    ROWOWFamilyCall call;
    {
        std::lock_guard<std::mutex> lock(FamilyMutex);
        uint64 now = uint64(GameTime::GetGameTimeMS().count());
        auto callIter = FamilyCallsByCharacterGUID.find(player->GetGUID().GetCounter());
        if (callIter != FamilyCallsByCharacterGUID.end())
        {
            // Only for the save slot called, in the RO world; dropped either way
            if (callIter->second.IdentityID == identity->IdentityID && identity->IsInForm == true)
                call = callIter->second;
            FamilyCallsByCharacterGUID.erase(callIter);
        }
        for (auto staleIter = FamilyCallsByCharacterGUID.begin(); staleIter != FamilyCallsByCharacterGUID.end();)
        {
            if (staleIter->second.RequestTime + ROWOW_FAMILY_CALL_TIME_IN_MS < now)
                staleIter = FamilyCallsByCharacterGUID.erase(staleIter);
            else
                ++staleIter;
        }
        auto familyIter = FamiliesByIdentityID.find(identity->IdentityID);
        if (familyIter != FamiliesByIdentityID.end() && familyIter->second.IsRingRemovalPending == true && identity->IsInForm == true)
        {
            familyIter->second.IsRingRemovalPending = false;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            AppendFamilyRow(trans, identity->IdentityID, &familyIter->second);
            CharacterDatabase.CommitTransaction(trans);
            if (familyIter->second.IsEmpty() == true)
                FamiliesByIdentityID.erase(familyIter);
            isRingRemoved = true;
        }
        uint32 pendingCount = 0;
        for (auto const& familyByIdentityID : FamiliesByIdentityID)
            if (familyByIdentityID.second.IsRingRemovalPending == true)
                pendingCount++;
        FamilyPendingCount = pendingCount + uint32(FamilyCallsByCharacterGUID.size());
    }
    if (isRingRemoved == true)
    {
        RemoveWeddingRings(player);
        SendText(player, "Your marriage is over: your wedding ring is gone.");
        SendFamilyToClient(player);
    }
    if (call.Destination.IsSet == true && player->IsAlive() == true && HasMapFlag(player, "nowarp") == false)
        player->TeleportTo(call.Destination.MapID, call.Destination.X, call.Destination.Y, call.Destination.Z, call.Destination.O);
}

void ROWOWMod::SendFamilyToClient(Player* player)
{
    // ROWOWFAMILY<tab><partner><tab><father><tab><mother><tab><child>: the names (the characters' whose save slots they are), "" for none
    if (player == nullptr || player->GetSession() == nullptr)
        return;
    ROWOWFamily family = GetFamily(player);
    SendAddonText(player, fmt::format("ROWOWFAMILY\t{}\t{}\t{}\t{}", GetIdentityCharacterName(family.PartnerID), GetIdentityCharacterName(family.FatherID),
        GetIdentityCharacterName(family.MotherID), GetIdentityCharacterName(family.ChildID)));
}

// The wedding skills (rAthena's skills/other).  They run in the caster's map thread
namespace
{
    // SkillIWillProtectYou (WE_MALE) and SkillILookUpToYou (WE_FEMALE): at the partner (UnitSkillTarget), the caster gives it the share of its maximum HP
    // (or SP) that the skill's HP (SP) rate cost takes from the caster, while the caster has more than that
    class ROWOWSkillWeddingGift : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillWeddingGift(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            if (skill == nullptr || target == src)
                return;
            bool isHP = SkillID == ROWOW_SKILL_WE_MALE;
            int32 rate = std::abs(ROWOWSkill::GetLevelValue(isHP == true ? skill->HPRateCost : skill->SPRateCost, skillLevel));
            if (rate == 0)
                return;
            uint32 current = isHP == true ? src->GetHealth() : src->GetPower(POWER_MANA);
            uint32 maximum = isHP == true ? src->GetMaxHealth() : src->GetMaxPower(POWER_MANA);
            if (current <= maximum / uint32(rate))
                return;
            int64 gain = int64(isHP == true ? target->GetMaxHealth() : target->GetMaxPower(POWER_MANA)) * rate / 100; // The earned is the same % of the target HP than it costed the caster. [Skotlex]
            ROWOW->StatusHeal(target, isHP == true ? gain : 0, isHP == true ? 0 : gain, 0, src, ROWOW->GetSkillSpellID(SkillID, skillLevel));
            ROWOW->ClifSkillNoDamage(src, target, SkillID, skillLevel);
        }
    };

    // SkillBaby (WE_BABY): "Mom, Dad, I love you!": the child is stunned for a moment and its parents in its party (the screen around it) get Protect Exp
    // (no exp lost at death) for the skill's time
    class ROWOWSkillBaby : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBaby() : ROWOWSkillImpl(ROWOW_SKILL_WE_BABY) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            ROWOWFamily family = ROWOW->GetFamily(sd);
            Player* fatherSD = ROWOW->FindFamilyPlayerOnMap(sd, family.FatherID);
            Player* motherSD = ROWOW->FindFamilyPlayerOnMap(sd, family.MotherID);
            // Neither was found
            if (fatherSD == nullptr && motherSD == nullptr)
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            // Not in same party
            Group* group = sd->GetGroup();
            if (group != nullptr && (fatherSD == nullptr || fatherSD->GetGroup() != group) && (motherSD == nullptr || motherSD->GetGroup() != group))
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            // Not in same screen
            if ((fatherSD == nullptr || ROWOW->DistanceBL(sd, fatherSD) > ROWOW_AREA_SIZE) && (motherSD == nullptr || ROWOW->DistanceBL(sd, motherSD) > ROWOW_AREA_SIZE))
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            ROWOW->StatusChangeStart(src, target, ROWOW_SC_STUN, 10000, skillLevel, 0, 0, 0, ROWOW->SkillGetTime2(SkillID, skillLevel), ROWOW_SCSTART_NORATEDEF);
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            int32 type = skill != nullptr ? skill->StatusID : ROWOW_SC_NONE;
            if (type <= ROWOW_SC_NONE)
                return;
            for (Player* parent : { fatherSD, motherSD })
                if (parent != nullptr)
                    ROWOW->SCStart(src, parent, type, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillIMissYou (WE_CALLPARTNER), SkillCallParent (WE_CALLPARENT) and SkillCallBaby (WE_CALLBABY): the ground unit UNT_CALLFAMILY at the caster's cell
    // (skill_unitsetting's val1 and val2: skill_unitsetting's WE_ cases) brings the family member to it at its first tick.  Here at once: the called
    // comes in its own update (RequestFamilyCall)
    class ROWOWSkillCallFamily : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillCallFamily(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendPos2(Unit* src, int32 /*x*/, int32 /*y*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& flag) const override
        {
            flag |= 1; // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            ROWOWFamily family = ROWOW->GetFamily(sd);
            switch (SkillID)
            {
                case ROWOW_SKILL_WE_CALLPARTNER:
                    ROWOW->RequestFamilyCall(sd, family.PartnerID);
                    break;
                case ROWOW_SKILL_WE_CALLPARENT:
                    ROWOW->RequestFamilyCall(sd, family.FatherID);
                    ROWOW->RequestFamilyCall(sd, family.MotherID);
                    break;
                case ROWOW_SKILL_WE_CALLBABY:
                    if (family.PartnerID != 0) // pc_get_child: only while married
                        ROWOW->RequestFamilyCall(sd, family.ChildID);
                    break;
            }
        }
    };
}

void AddROWOWSkillsWedding()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillWeddingGift(ROWOW_SKILL_WE_MALE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillWeddingGift(ROWOW_SKILL_WE_FEMALE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillCallFamily(ROWOW_SKILL_WE_CALLPARTNER));
    ROWOW->RegisterSkillImpl(new ROWOWSkillBaby());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCallFamily(ROWOW_SKILL_WE_CALLPARENT));
    ROWOW->RegisterSkillImpl(new ROWOWSkillCallFamily(ROWOW_SKILL_WE_CALLBABY));
}

class ROWOW_FamilyWorldScript : public WorldScript
{
public:
    ROWOW_FamilyWorldScript() : WorldScript("ROWOW_FamilyWorldScript", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override
    {
        if (ROWOW->IsEnabled == false)
            return;
        std::lock_guard<std::mutex> lock(ROWOW->FamilyMutex);
        ROWOW->LoadFamilyData();
    }
};

class ROWOW_FamilyPlayerScript : public PlayerScript
{
public:
    ROWOW_FamilyPlayerScript() : PlayerScript("ROWOW_FamilyPlayerScript", { PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->UpdateFamily(player);
    }
};

void AddROWOWFamilyScripts()
{
    new ROWOW_FamilyWorldScript();
    new ROWOW_FamilyPlayerScript();
}
