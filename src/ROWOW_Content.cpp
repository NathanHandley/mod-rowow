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

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Corpse.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include "ROWOW.h"

#include <cctype>
#include <cstring>
#include <fstream>

// Optional content (Nathan 2026-11-11): Ragnarok Online's content is an optional part of the client.  The launcher's checklist installs or deletes
// its patches (patch-<locale>-6.MPQ and -7.MPQ), and a client without them can't draw any of RO's models, looks or maps.  The client says which
// content it has as it logs in: the converter puts a marker in RO's own patches (MWContentLUA.cs), and wow.exe sends ROWOW_CONTENT_FLAG_* in the
// high byte of the client data version in CMSG_AUTH_SESSION (its MWContent::GetContentFlags; mod-everquest masks that byte off before it compares
// the version).  RecordClientContent keeps it by account, ApplyContentAtLogin puts it on the session's player, and .ro content overrides it (to
// test with two clients from one client folder).  For a player without the content:
// - Nothing it is sent shows an RO model.  Each unit's display, native display and mount display is patched for it as it's sent (OnPatchValuesUpdate,
//   per viewer): a player showing an RO job sprite (the Ragnarok identity outside Midgard, dead or alive) shows its own body instead (its native
//   display, else its race's; mod-everquest's EverQuest look isn't worked out here), a Peco shown in a ground mount's place shows the mount again,
//   and any other unit with an RO display (a companion pet, a homunculus or mercenary out of Midgard) shows the core's invisible model.  A corpse
//   lying as an RO sprite lies as its race's body, unclothed (its item fields name the sprite's head and headgear).  The auras that hang RO looks on
//   a unit (head, headgear, garment, hat effects, cart and falcon, the illusions) are taken out of the aura updates it is sent (CanPacketSend)
// - It can't go onto an RO map: every teleport there is refused with a message (ROWOW_IdentityPlayerScript::OnPlayerBeforeTeleport, StartWorldTravel
//   and the world travel spell before their cast), and a character that logs in on one is put at its way out (its identity's return point, else its
//   home) before it is placed on any map, its identity keeping where it was for its next way in
// - Its own client hides the rest (MW_CONTENT_RAGNAROK isn't set: the Ragnarok options pages, the Ragnarok identity, the character window's Ragnarok
//   tab, Adopt as Child, and RO_Identity, whose .toc says X-MWContent: Ragnarok), and draws no RO sprite or look should one reach it anyway
// What is decided where: a display is RO's when its CreatureModelData model is under Creature\Ragnarok\ (the client's own test, Unit_C.cpp
// IsRagnarokSpriteModel), read from the server's DBC files at startup

namespace
{
    // An account's client content, as CMSG_AUTH_SESSION reported it (a network thread writes, the world thread reads), and .ro content's override
    struct ROWOWAccountContent
    {
        bool HasLoggedIn = false; // Since the server started (an override alone makes the entry too)
        uint8 ClientFlags = 0;
        bool IsReported = false; // The client sent ROWOW_CONTENT_FLAG_REPORTED: an older client doesn't, and has every game's content
        int8 Override = ROWOW_CONTENT_OVERRIDE_AUTO;
    };

    std::mutex AccountContentMutex;
    std::unordered_map<uint32, ROWOWAccountContent> AccountContentByAccountID;

    char const* const ContentStateKey = "ROWOWContent";

    // The model paths of CreatureModelData.dbc (field 2), which the core doesn't load: by model ID
    bool ReadRagnarokModelIDs(std::string const& path, std::unordered_set<uint32>& modelIDsOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (file.is_open() == false)
            return false;
        char magic[4];
        uint32 header[4]; // record count, field count, record size, string block size
        file.read(magic, 4);
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        if (file.good() == false || memcmp(magic, "WDBC", 4) != 0 || header[2] < 12)
            return false;
        std::vector<char> records(size_t(header[0]) * header[2]);
        std::vector<char> strings(size_t(header[3]) + 1, '\0');
        file.read(records.data(), records.size());
        file.read(strings.data(), header[3]);
        if (file.good() == false)
            return false;
        // A model in a Ragnarok folder of the top one (Creature\Ragnarok\, World\Ragnarok\: the converter puts every RO model there, and
        // GetPatchContentGroup every file in such a folder in RO's patches)
        for (uint32 i = 0; i < header[0]; i++)
        {
            char const* record = records.data() + size_t(i) * header[2];
            uint32 modelID = 0;
            uint32 nameOffset = 0;
            memcpy(&modelID, record, 4);
            memcpy(&nameOffset, record + 8, 4);
            if (nameOffset >= header[3])
                continue;
            std::string name = strings.data() + nameOffset;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return c == '/' ? '\\' : char(std::tolower(c)); });
            size_t firstSlash = name.find('\\');
            if (firstSlash != std::string::npos && name.compare(firstSlash, 10, "\\ragnarok\\") == 0)
                modelIDsOut.insert(modelID);
        }
        return true;
    }

    // A valid spot off the RO world on a map the player can be put on directly (a continent, not an instance)
    bool IsWayOutUsable(uint32 mapID, float x, float y, float z)
    {
        if (ROWOW->IsROMap(mapID) == true || MapMgr::IsValidMapCoord(mapID, x, y, z) == false)
            return false;
        MapEntry const* mapEntry = sMapStore.LookupEntry(mapID);
        return mapEntry != nullptr && mapEntry->Instanceable() == false;
    }
}

void ROWOWMod::LoadContentData()
{
    RagnarokDisplayIDs.clear();
    RagnarokLookSpellIDs.clear();
    if (IsEnabled == false)
        return;

    std::unordered_set<uint32> ragnarokModelIDs;
    std::string modelDataPath = sWorld->GetDataPath() + "dbc/CreatureModelData.dbc";
    if (ReadRagnarokModelIDs(modelDataPath, ragnarokModelIDs) == false)
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadContentData could not read '{}', so a client without Ragnarok Online's content may be sent RO models", modelDataPath);
    uint32 ragnarokDisplayCount = 0;
    if (ragnarokModelIDs.empty() == false)
    {
        RagnarokDisplayIDs.assign(sCreatureDisplayInfoStore.GetNumRows(), false);
        for (uint32 displayID = 0; displayID < sCreatureDisplayInfoStore.GetNumRows(); displayID++)
        {
            CreatureDisplayInfoEntry const* displayInfo = sCreatureDisplayInfoStore.LookupEntry(displayID);
            if (displayInfo != nullptr && ragnarokModelIDs.find(displayInfo->ModelId) != ragnarokModelIDs.end())
            {
                RagnarokDisplayIDs[displayID] = true;
                ragnarokDisplayCount++;
            }
        }
    }

    // The looks' auras: every one LoadIllusionData knows, and any other RO spell with the looks' icon (as the client tells them, Unit_C.cpp
    // IsRagnarokLookSpell)
    for (auto const& illusionBySpellID : IllusionsBySpellID)
        RagnarokLookSpellIDs.insert(illusionBySpellID.first);
    for (std::unordered_set<uint32> const* spellIDs : { &HeadSpellIDs, &HeadgearSpellIDs, &GarmentSpellIDs, &HatEffectSpellIDs, &OptionLookSpellIDs })
        RagnarokLookSpellIDs.insert(spellIDs->begin(), spellIDs->end());
    for (auto const& scriptLookSpellIDByKey : ScriptLookSpellIDsByKey)
        RagnarokLookSpellIDs.insert(scriptLookSpellIDByKey.second);
    uint32 lastSpellID = std::min<uint32>(ROWOW_RO_SPELL_ID_LAST, sSpellMgr->GetSpellInfoStoreSize() - 1);
    for (uint32 spellID = ROWOW_RO_SPELL_ID_FIRST; spellID <= lastSpellID; spellID++)
        if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID))
            if (spellInfo->SpellIconID == ROWOW_RO_LOOK_SPELL_ICON_ID)
                RagnarokLookSpellIDs.insert(spellID);
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadContentData found {} Ragnarok Online displays ({} models) and {} RO look auras", ragnarokDisplayCount, ragnarokModelIDs.size(),
        RagnarokLookSpellIDs.size());
}

void ROWOWMod::RecordClientContent(uint32 accountID, uint32 clientDataVersion)
{
    uint8 clientFlags = uint8(clientDataVersion >> ROWOW_CONTENT_FLAGS_SHIFT);
    {
        std::lock_guard<std::mutex> lock(AccountContentMutex);
        ROWOWAccountContent& accountContent = AccountContentByAccountID[accountID];
        accountContent.HasLoggedIn = true;
        accountContent.ClientFlags = clientFlags;
        accountContent.IsReported = (clientFlags & ROWOW_CONTENT_FLAG_REPORTED) != 0;
    }
    LOG_INFO("module.ROWOW", "Account {} logs in with client content flags 0x{:02X} (Ragnarok Online content: {})", accountID, clientFlags,
        (clientFlags & ROWOW_CONTENT_FLAG_REPORTED) == 0 ? "not reported, so assumed" : ((clientFlags & ROWOW_CONTENT_FLAG_RAGNAROK) != 0 ? "yes" : "no"));
}

bool ROWOWMod::AccountHasRagnarokContent(uint32 accountID) const
{
    std::lock_guard<std::mutex> lock(AccountContentMutex);
    auto accountContentIter = AccountContentByAccountID.find(accountID);
    if (accountContentIter == AccountContentByAccountID.end())
        return true; // Not reported (a cluster's login, which skips the client data version check): as before
    ROWOWAccountContent const& accountContent = accountContentIter->second;
    if (accountContent.Override != ROWOW_CONTENT_OVERRIDE_AUTO)
        return accountContent.Override == ROWOW_CONTENT_OVERRIDE_ON;
    return accountContent.HasLoggedIn == false || accountContent.IsReported == false || (accountContent.ClientFlags & ROWOW_CONTENT_FLAG_RAGNAROK) != 0;
}

int8 ROWOWMod::GetContentOverride(uint32 accountID) const
{
    std::lock_guard<std::mutex> lock(AccountContentMutex);
    auto accountContentIter = AccountContentByAccountID.find(accountID);
    return accountContentIter != AccountContentByAccountID.end() ? accountContentIter->second.Override : int8(ROWOW_CONTENT_OVERRIDE_AUTO);
}

void ROWOWMod::SetContentOverride(uint32 accountID, int8 contentOverride)
{
    // Until the server stops (or .ro content auto), through relogs: a test's second client logs out and in under it
    std::lock_guard<std::mutex> lock(AccountContentMutex);
    AccountContentByAccountID[accountID].Override = contentOverride;
}

std::string ROWOWMod::GetContentStatusText(uint32 accountID) const
{
    std::string clientText = "not logged in since the server started";
    int8 contentOverride = ROWOW_CONTENT_OVERRIDE_AUTO;
    {
        std::lock_guard<std::mutex> lock(AccountContentMutex);
        auto accountContentIter = AccountContentByAccountID.find(accountID);
        if (accountContentIter != AccountContentByAccountID.end())
        {
            ROWOWAccountContent const& accountContent = accountContentIter->second;
            contentOverride = accountContent.Override;
            if (accountContent.HasLoggedIn == false)
                clientText = "not logged in since the server started";
            else if (accountContent.IsReported == false)
                clientText = fmt::format("not reported (flags 0x{:02X}, an older client: has it)", accountContent.ClientFlags);
            else
                clientText = fmt::format("{} (flags 0x{:02X})", (accountContent.ClientFlags & ROWOW_CONTENT_FLAG_RAGNAROK) != 0 ? "installed" : "not installed",
                    accountContent.ClientFlags);
        }
    }
    std::string accountName;
    AccountMgr::GetName(accountID, accountName);
    char const* overrideText = contentOverride == ROWOW_CONTENT_OVERRIDE_ON ? "on" : (contentOverride == ROWOW_CONTENT_OVERRIDE_OFF ? "off" : "auto");
    return fmt::format("Ragnarok Online content of account {} ({}): client {}; override {}; in effect: {}.", accountID, accountName, clientText, overrideText,
        AccountHasRagnarokContent(accountID) == true ? "shown" : "not shown");
}

bool ROWOWMod::HasRagnarokContent(Player const* player) const
{
    if (player == nullptr)
        return true;
    ROWOWContentState const* state = player->CustomData.Get<ROWOWContentState>(ContentStateKey);
    return state == nullptr || state->HasRagnarok == true;
}

void ROWOWMod::SendRagnarokContentMissingMessage(Player* player) const
{
    if (player != nullptr && player->GetSession() != nullptr)
        ChatHandler(player->GetSession()).SendSysMessage("You don't have the Ragnarok Online content installed, so you can't go to Midgard. Turn it on in the launcher to play there.");
}

bool ROWOWMod::GetContentWayOut(Player* player, uint32& mapIDOut, float& xOut, float& yOut, float& zOut, float& oOut)
{
    // Where a character without the content is put when it's in the RO world: its identity's return point (where it came in from), else its home,
    // else where its race and class start.  Before the identity is loaded (a login, OnPlayerLoadFromDB) the return point is read from its rows
    ROWOWPosition returnPosition;
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity != nullptr && identity->IsLoaded == true)
        returnPosition = identity->ReturnPosition;
    else
    {
        ObjectGuid::LowType guid = player->GetGUID().GetCounter();
        uint32 identityID = guid; // Slot 1's identity is the character's own GUID
        if (QueryResult result = CharacterDatabase.Query("SELECT `slot` FROM `mod_rowow_identity_character` WHERE `guid` = {}", guid))
        {
            uint8 slot = result->Fetch()[0].Get<uint8>();
            if (slot > 1)
                if (QueryResult slotResult = CharacterDatabase.Query("SELECT `identity` FROM `mod_rowow_identity_slot` WHERE `guid` = {} AND `slot` = {}", guid, slot))
                    identityID = slotResult->Fetch()[0].Get<uint32>();
        }
        if (QueryResult result = CharacterDatabase.Query("SELECT `return_map`, `return_x`, `return_y`, `return_z`, `return_o` FROM `mod_rowow_identity` WHERE `guid` = {}", identityID))
        {
            Field* fields = result->Fetch();
            int32 mapID = fields[0].Get<int32>();
            returnPosition.IsSet = mapID >= 0;
            returnPosition.MapID = mapID >= 0 ? uint32(mapID) : 0;
            returnPosition.X = fields[1].Get<float>();
            returnPosition.Y = fields[2].Get<float>();
            returnPosition.Z = fields[3].Get<float>();
            returnPosition.O = fields[4].Get<float>();
        }
    }
    if (returnPosition.IsSet == true && IsWayOutUsable(returnPosition.MapID, returnPosition.X, returnPosition.Y, returnPosition.Z) == true)
    {
        mapIDOut = returnPosition.MapID;
        xOut = returnPosition.X;
        yOut = returnPosition.Y;
        zOut = returnPosition.Z;
        oOut = returnPosition.O;
        return true;
    }
    if (IsWayOutUsable(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ) == true)
    {
        mapIDOut = player->m_homebindMapId;
        xOut = player->m_homebindX;
        yOut = player->m_homebindY;
        zOut = player->m_homebindZ;
        oOut = player->GetOrientation();
        return true;
    }
    if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(player->getRace(true), player->getClass()))
    {
        if (IsWayOutUsable(info->mapId, info->positionX, info->positionY, info->positionZ) == true)
        {
            mapIDOut = info->mapId;
            xOut = info->positionX;
            yOut = info->positionY;
            zOut = info->positionZ;
            oOut = info->orientation;
            return true;
        }
    }
    return false;
}

void ROWOWMod::ApplyContentAtLogin(Player* player)
{
    // The world thread, in Player::LoadFromDB: the player has its map but isn't on it, and no client has been sent it yet
    if (IsEnabled == false || player == nullptr || player->GetSession() == nullptr)
        return;
    ROWOWContentState* state = player->CustomData.GetDefault<ROWOWContentState>(ContentStateKey);
    state->HasRagnarok = AccountHasRagnarokContent(player->GetSession()->GetAccountId());
    state->WasLoginRelocated = false;
    if (state->HasRagnarok == true || IsROMap(player->GetMapId()) == false)
        return;

    // Logged out in the RO world: put at its way out before it's placed (its client couldn't load the RO map), its identity keeping where it was
    // (OnPlayerLogin, ROWOW_ContentPlayerScript).  Saved in the form, it comes back out of it as from a server stop in the RO world
    // (CompleteROFormChange's WasInFormAtLoad)
    uint32 mapID = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float o = 0.0f;
    if (GetContentWayOut(player, mapID, x, y, z, o) == false)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::ApplyContentAtLogin found no way out of the RO world for {} (GUID {}), whose client has no Ragnarok Online content: left on map {}",
            player->GetName(), player->GetGUID().GetCounter(), player->GetMapId());
        return;
    }
    Map* map = sMapMgr->CreateMap(mapID, player);
    if (map == nullptr)
        return;
    state->RelocatedFrom.IsSet = true;
    state->RelocatedFrom.MapID = player->GetMapId();
    state->RelocatedFrom.X = player->GetPositionX();
    state->RelocatedFrom.Y = player->GetPositionY();
    state->RelocatedFrom.Z = player->GetPositionZ();
    state->RelocatedFrom.O = player->GetOrientation();
    state->WasLoginRelocated = true;
    LOG_INFO("module.ROWOW", "{} (GUID {}) logs in on RO map {} with a client that has no Ragnarok Online content: put on map {} ({}, {}, {})", player->GetName(),
        player->GetGUID().GetCounter(), player->GetMapId(), mapID, x, y, z);
    player->ResetMap();
    player->Relocate(x, y, z, o);
    player->SetMap(map);
    player->UpdatePositionData();
}

void ROWOWMod::ApplyContentChange(Player* player)
{
    // .ro content changed what the account's player is treated as having (the world thread, the maps idle)
    if (IsEnabled == false || player == nullptr || player->GetSession() == nullptr)
        return;
    ROWOWContentState* state = player->CustomData.GetDefault<ROWOWContentState>(ContentStateKey);
    bool hasRagnarok = AccountHasRagnarokContent(player->GetSession()->GetAccountId());
    if (state->HasRagnarok == hasRagnarok)
        return;
    state->HasRagnarok = hasRagnarok;

    // What its client shows made right now: everything it sees is sent its looks again (patched for it, OnPatchValuesUpdate, the corpse hook), and the
    // RO look auras on them are taken off its client, or sent again
    std::vector<WorldObject*> objects;
    objects.push_back(player);
    for (auto const& visibleObject : *player->GetObjectVisibilityContainer().GetVisibleWorldObjectsMap())
        if (visibleObject.second != nullptr)
            objects.push_back(visibleObject.second);
    for (WorldObject* object : objects)
    {
        if (Unit* unit = object->ToUnit())
        {
            unit->ForceValuesUpdateAtIndex(UNIT_FIELD_DISPLAYID);
            unit->ForceValuesUpdateAtIndex(UNIT_FIELD_NATIVEDISPLAYID);
            unit->ForceValuesUpdateAtIndex(UNIT_FIELD_MOUNTDISPLAYID);
            if (hasRagnarok == true)
            {
                player->GetAurasForTarget(unit, true);
                continue;
            }
            for (auto const& visibleAura : *unit->GetVisibleAuras())
            {
                if (visibleAura.second == nullptr || IsRagnarokLookSpell(visibleAura.second->GetBase()->GetId()) == false)
                    continue;
                WorldPacket data(SMSG_AURA_UPDATE, 8 + 1 + 4);
                data << unit->GetPackGUID();
                data << uint8(visibleAura.first);
                data << uint32(0);
                player->SendDirectMessage(&data);
            }
        }
        else if (Corpse* corpse = object->ToCorpse())
        {
            corpse->ForceValuesUpdateAtIndex(CORPSE_FIELD_DISPLAY_ID);
            for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; slot++)
                corpse->ForceValuesUpdateAtIndex(CORPSE_FIELD_ITEM + slot);
        }
    }

    // Out of the RO world
    if (hasRagnarok == false && IsROMap(player->GetMapId()) == true)
    {
        uint32 mapID = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float o = 0.0f;
        if (GetContentWayOut(player, mapID, x, y, z, o) == true)
        {
            ChatHandler(player->GetSession()).SendSysMessage("Your client is treated as having no Ragnarok Online content, so you are taken out of Midgard.");
            player->TeleportTo(mapID, x, y, z, o);
        }
    }
}

bool ROWOWMod::HandleContentCommand(ChatHandler* handler, std::string const& modeText, std::string const& nameText)
{
    // .ro content: the selected player's (or your own) account.  .ro content <on|off|auto> [player or account]: treat its client as having Ragnarok
    // Online's content, as not having it, or as its client says (until the server stops).  .ro content <player or account>: its state
    std::string mode = modeText;
    std::string name = nameText;
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    int8 contentOverride = ROWOW_CONTENT_OVERRIDE_AUTO;
    bool isSet = true;
    if (mode == "on")
        contentOverride = ROWOW_CONTENT_OVERRIDE_ON;
    else if (mode == "off")
        contentOverride = ROWOW_CONTENT_OVERRIDE_OFF;
    else if (mode == "auto")
        contentOverride = ROWOW_CONTENT_OVERRIDE_AUTO;
    else
    {
        isSet = false;
        if (mode.empty() == false && mode != "status" && name.empty() == true)
            name = modeText;
    }

    Player* target = nullptr;
    uint32 accountID = 0;
    if (name.empty() == false)
    {
        target = ObjectAccessor::FindPlayerByName(name, false);
        if (target != nullptr && target->GetSession() != nullptr)
            accountID = target->GetSession()->GetAccountId();
        else
        {
            target = nullptr;
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
            accountID = guid.IsEmpty() == false ? sCharacterCache->GetCharacterAccountIdByGuid(guid) : AccountMgr::GetId(name);
        }
    }
    else
    {
        target = handler->getSelectedPlayerOrSelf();
        if (target != nullptr && target->GetSession() != nullptr)
            accountID = target->GetSession()->GetAccountId();
    }
    if (accountID == 0)
    {
        handler->SendSysMessage("Use .ro content [on|off|auto] [player or account], on the selected player or yourself without a name.");
        return true;
    }
    if (isSet == true)
    {
        SetContentOverride(accountID, contentOverride);
        if (target == nullptr)
            if (WorldSession* session = sWorldSessionMgr->FindSession(accountID))
                target = session->GetPlayer();
        if (target != nullptr && target->IsInWorld() == true)
            ApplyContentChange(target);
    }
    handler->SendSysMessage(GetContentStatusText(accountID));
    return true;
}

// The client's report, as it logs in
class ROWOW_ContentAccountScript : public AccountScript
{
public:
    ROWOW_ContentAccountScript() : AccountScript("ROWOW_ContentAccountScript", { ACCOUNTHOOK_CAN_ACCOUNT_LOGIN_WITH_CLIENT_DATA_VERSION }) {}

    bool CanAccountLoginWithClientDataVersion(uint32 accountId, AccountTypes /*security*/, uint32 clientDataVersion, uint32& /*expectedClientDataVersion*/) override
    {
        // A network thread (WorldSocket::HandleAuthSession): only the account map, under its lock
        if (ROWOW->IsEnabled == true)
            ROWOW->RecordClientContent(accountId, clientDataVersion);
        return true;
    }
};

class ROWOW_ContentPlayerScript : public PlayerScript
{
public:
    ROWOW_ContentPlayerScript() : PlayerScript("ROWOW_ContentPlayerScript", { PLAYERHOOK_ON_LOAD_FROM_DB, PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_SEND_INITIAL_PACKETS_BEFORE_ADD_TO_MAP, PLAYERHOOK_ON_UPDATE }) {}

    void OnPlayerLoadFromDB(Player* player) override
    {
        ROWOW->ApplyContentAtLogin(player);
    }

    void OnPlayerSendInitialPacketsBeforeAddToMap(Player* player, WorldPacket& /*data*/) override
    {
        // Every arrival, and a login onto the character still in the world (its client closed moments before: no LoadFromDB then), which may
        // be from a client with other content: the session's content again, and off an RO map at the next update without it (that client was
        // already told the RO map; it waits for the way out without loading it, Client.cpp Packet_SMSG_LOGIN_VERIFY_WORLD)
        if (ROWOW->IsEnabled == false || player == nullptr || player->GetSession() == nullptr)
            return;
        ROWOWContentState* state = player->CustomData.GetDefault<ROWOWContentState>(ContentStateKey);
        state->HasRagnarok = ROWOW->AccountHasRagnarokContent(player->GetSession()->GetAccountId());
        state->IsWayOutPending = state->HasRagnarok == false && ROWOW->IsROMap(player->GetMapId()) == true;
    }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        if (ROWOW->IsEnabled == false || player == nullptr || player->IsInWorld() == false || player->IsBeingTeleported() == true)
            return;
        ROWOWContentState* state = player->CustomData.Get<ROWOWContentState>(ContentStateKey);
        if (state == nullptr || state->IsWayOutPending == false)
            return;
        state->IsWayOutPending = false;
        uint32 mapID = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float o = 0.0f;
        if (ROWOW->IsROMap(player->GetMapId()) == true && ROWOW->GetContentWayOut(player, mapID, x, y, z, o) == true)
        {
            ChatHandler(player->GetSession()).SendSysMessage("You don't have the Ragnarok Online content installed, so you are taken out of Midgard. Turn it on in the launcher to play there.");
            player->TeleportTo(mapID, x, y, z, o);
        }
    }

    void OnPlayerLogin(Player* player) override
    {
        // Put out of the RO world as it logged in: told why, and its identity's way back in is where it was
        if (ROWOW->IsEnabled == false || player == nullptr)
            return;
        ROWOWContentState* state = player->CustomData.Get<ROWOWContentState>(ContentStateKey);
        if (state == nullptr || state->WasLoginRelocated == false)
            return;
        state->WasLoginRelocated = false;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        if (identity->Exists == true && state->RelocatedFrom.IsSet == true)
            identity->LastPosition = state->RelocatedFrom;
        ChatHandler(player->GetSession()).SendSysMessage("You logged out in Midgard, but you don't have the Ragnarok Online content installed, so you were brought back. Turn it on in the launcher to play there.");
    }
};

// What a unit looks like to a player without the content (see the top of this file)
class ROWOW_ContentUnitScript : public UnitScript
{
public:
    ROWOW_ContentUnitScript() : UnitScript("ROWOW_ContentUnitScript", true, { UNITHOOK_SHOULD_TRACK_VALUES_UPDATE_POS_BY_INDEX, UNITHOOK_ON_PATCH_VALUES_UPDATE }) {}

    bool ShouldTrackValuesUpdatePosByIndex(Unit const* /*unit*/, uint8 /*updateType*/, uint16 index) override
    {
        return ROWOW->IsEnabled == true && (index == UNIT_FIELD_NATIVEDISPLAYID || index == UNIT_FIELD_MOUNTDISPLAYID);
    }

    void OnPatchValuesUpdate(Unit const* unit, ByteBuffer& valuesUpdateBuf, BuildValuesCachePosPointers& posPointers, Player* target) override
    {
        // The map thread building the unit's update for the target, which is on the same map.  Cheap until a display is RO's: most units' aren't
        if (ROWOW->IsEnabled == false || unit == nullptr || target == nullptr || ROWOW->RagnarokDisplayIDs.empty() == true)
            return;
        int32 displayPos = posPointers.UnitFieldDisplayPos;
        auto nativeIter = posPointers.other.find(UNIT_FIELD_NATIVEDISPLAYID);
        auto mountIter = posPointers.other.find(UNIT_FIELD_MOUNTDISPLAYID);
        bool hasNative = nativeIter != posPointers.other.end();
        bool hasMount = mountIter != posPointers.other.end();
        if (displayPos < 0 && hasNative == false && hasMount == false)
            return;
        // On an RO map whoever sees the unit has the content (a player without it is never let onto one)
        if (ROWOW->IsROMap(unit->GetMapId()) == true)
            return;
        uint32 displayID = displayPos >= 0 ? valuesUpdateBuf.read<uint32>(displayPos) : 0;
        uint32 nativeDisplayID = hasNative == true ? valuesUpdateBuf.read<uint32>(nativeIter->second) : 0;
        uint32 mountDisplayID = hasMount == true ? valuesUpdateBuf.read<uint32>(mountIter->second) : 0;
        bool isDisplayRagnarok = displayPos >= 0 && ROWOW->IsRagnarokDisplay(displayID) == true;
        bool isNativeRagnarok = hasNative == true && ROWOW->IsRagnarokDisplay(nativeDisplayID) == true;
        bool isMountRagnarok = hasMount == true && (mountDisplayID == ROWOW_MOUNT_DISPLAY_ID_NONE || ROWOW->IsRagnarokDisplay(mountDisplayID) == true);
        if (isDisplayRagnarok == false && isNativeRagnarok == false && isMountRagnarok == false)
            return;
        if (ROWOW->HasRagnarokContent(target) == true)
            return;

        uint32 shownDisplayID = GetNonRagnarokDisplayID(unit);
        if (isDisplayRagnarok == true)
            valuesUpdateBuf.put<uint32>(displayPos, shownDisplayID);
        if (isNativeRagnarok == true)
            valuesUpdateBuf.put<uint32>(nativeIter->second, shownDisplayID);
        if (isMountRagnarok == true)
            valuesUpdateBuf.put<uint32>(mountIter->second, GetNonRagnarokMountDisplayID(unit, mountDisplayID));
    }

private:
    // A player's own body (its native display, or its race's while that is the sprite too: mod-everquest's corpse handling makes it so while dead);
    // any other unit the core's invisible model
    static uint32 GetNonRagnarokDisplayID(Unit const* unit)
    {
        Player const* player = unit->ToPlayer();
        if (player == nullptr)
            return CreatureModel::DefaultInvisibleModel.CreatureDisplayID;
        uint32 nativeDisplayID = player->GetNativeDisplayId();
        if (nativeDisplayID != 0 && ROWOW->IsRagnarokDisplay(nativeDisplayID) == false)
            return nativeDisplayID;
        if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(player->getRace(true), player->getClass()))
            return player->getGender() == GENDER_FEMALE ? info->displayId_f : info->displayId_m;
        return CreatureModel::DefaultInvisibleModel.CreatureDisplayID;
    }

    // The WoW mount a Peco was shown in place of (RefreshWorldIdentityMount keeps it), else its aura's creature's; an RO mount none (no model, still
    // mounted, as for the Peco)
    static uint32 GetNonRagnarokMountDisplayID(Unit const* unit, uint32 mountDisplayID)
    {
        Player const* player = unit->ToPlayer();
        if (mountDisplayID == ROWOW_MOUNT_DISPLAY_ID_NONE && player != nullptr)
        {
            ROWOWWorldIdentityState const* state = player->CustomData.Get<ROWOWWorldIdentityState>("ROWOWWorldIdentity");
            if (state != nullptr && state->HiddenMountDisplayID != 0 && ROWOW->IsRagnarokDisplay(state->HiddenMountDisplayID) == false)
                return state->HiddenMountDisplayID;
            Unit::AuraEffectList const& mountAuras = player->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
            if (mountAuras.empty() == false)
                if (CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(mountAuras.front()->GetMiscValue()))
                    if (CreatureModel const* model = creatureTemplate->GetFirstValidModel())
                        if (ROWOW->IsRagnarokDisplay(model->CreatureDisplayID) == false)
                            return model->CreatureDisplayID;
        }
        return ROWOW_MOUNT_DISPLAY_ID_NONE;
    }
};

// The RO looks' auras left out of what a player without the content is sent
class ROWOW_ContentServerScript : public ServerScript
{
public:
    ROWOW_ContentServerScript() : ServerScript("ROWOW_ContentServerScript", { SERVERHOOK_CAN_PACKET_SEND }) {}

    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        // Whatever thread sends it; aura updates are sent by the map thread of the unit's map, the receiving player's own
        if (packet.GetOpcode() != SMSG_AURA_UPDATE && packet.GetOpcode() != SMSG_AURA_UPDATE_ALL)
            return true;
        if (ROWOW->IsEnabled == false || session == nullptr || ROWOW->RagnarokLookSpellIDs.empty() == true)
            return true;
        Player* player = session->GetPlayer();
        if (player == nullptr || ROWOW->HasRagnarokContent(player) == true)
            return true;
        try
        {
            // <packed unit GUID> then entries: <slot uint8><spell uint32>, and for a spell <flags uint8><caster level uint8><stack uint8>
            // [<effects uint16> with AFLAG_EXTENDED_EFFECTS][<packed caster GUID> without AFLAG_CASTER][<max duration int32><duration int32> with AFLAG_DURATION]
            WorldPacket reader(packet);
            reader.rpos(0);
            uint64 unitGUID = 0;
            reader.readPackGUID(unitGUID);
            WorldPacket filtered(packet.GetOpcode(), packet.size());
            filtered.appendPackGUID(unitGUID);
            bool isRemoved = false;
            bool hasEntry = false;
            while (reader.rpos() < reader.size())
            {
                size_t entryStart = reader.rpos();
                uint8 slot = 0;
                uint32 spellID = 0;
                reader >> slot >> spellID;
                if (spellID != 0)
                {
                    uint8 flags = 0;
                    uint8 casterLevel = 0;
                    uint8 stackAmount = 0;
                    reader >> flags >> casterLevel >> stackAmount;
                    if ((flags & AFLAG_EXTENDED_EFFECTS) != 0)
                        reader.read_skip<uint16>();
                    if ((flags & AFLAG_CASTER) == 0)
                    {
                        uint64 casterGUID = 0;
                        reader.readPackGUID(casterGUID);
                    }
                    if ((flags & AFLAG_DURATION) != 0)
                        reader.read_skip(8);
                }
                if (spellID != 0 && ROWOW->IsRagnarokLookSpell(spellID) == true)
                {
                    isRemoved = true;
                    continue;
                }
                filtered.append(packet.contents() + entryStart, reader.rpos() - entryStart);
                hasEntry = true;
            }
            if (isRemoved == false)
                return true;
            if (hasEntry == true || packet.GetOpcode() == SMSG_AURA_UPDATE_ALL)
                session->SendPacket(&filtered);
            return false;
        }
        catch (ByteBufferException const&)
        {
            return true;
        }
    }
};

// A corpse lying as an RO sprite, to a player without the content
class ROWOW_ContentWorldObjectScript : public WorldObjectScript
{
public:
    ROWOW_ContentWorldObjectScript() : WorldObjectScript("ROWOW_ContentWorldObjectScript", { WORLDOBJECTHOOK_ON_CORPSE_BUILD_VALUES_UPDATE_FIELD }) {}

    void OnCorpseBuildValuesUpdateField(Corpse const* corpse, Player const* target, uint16 index, uint32& value) override
    {
        if (ROWOW->IsEnabled == false || corpse == nullptr || target == nullptr)
            return;
        if (ROWOW->IsRagnarokDisplay(corpse->GetUInt32Value(CORPSE_FIELD_DISPLAY_ID)) == false || ROWOW->HasRagnarokContent(target) == true)
            return;
        if (index != CORPSE_FIELD_DISPLAY_ID)
        {
            value = 0; // The sprite's head and headgear spells (DressSpriteCorpse), not the character's gear
            return;
        }
        // Its race's body (the corpse's bytes have the race and gender, which the client dresses it by)
        uint32 bytes = corpse->GetUInt32Value(CORPSE_FIELD_BYTES_1);
        uint8 race = uint8(bytes >> 8);
        uint8 gender = uint8(bytes >> 16);
        if (ChrRacesEntry const* raceEntry = sChrRacesStore.LookupEntry(race))
            value = gender == GENDER_FEMALE ? raceEntry->model_f : raceEntry->model_m;
        else
            value = CreatureModel::DefaultInvisibleModel.CreatureDisplayID;
    }
};

void AddROWOWContentScripts()
{
    new ROWOW_ContentAccountScript();
    new ROWOW_ContentPlayerScript();
    new ROWOW_ContentUnitScript();
    new ROWOW_ContentServerScript();
    new ROWOW_ContentWorldObjectScript();
}
