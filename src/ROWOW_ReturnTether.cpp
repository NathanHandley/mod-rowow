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

// Return-to-save teleports (Nathan 2026-11-10).  Two things that aren't RO's own:
// - Where the save point is: the RO_Identity addon adds "Your save is in <place>" to the tooltips of the items whose use goes to the save point (a
//   Butterfly Wing) and of Teleport's levels that can (AL_TELEPORT 2 and up), from the ROWOWSAVE message: the place (the save point's area, up to its
//   first comma: "Prontera City"), then those items' templates, spells and skills.  The area lookup may load terrain, so it's only made in the world
//   thread: on the addon's .ro sync (every loading screen and interface reload), and once a second for a client whose save point changed (a Kafra)
// - The return tether, a hearthstone's gate in RO: a Butterfly Wing or Teleport's save point (ReturnToSavePoint) in the RO world leaves a 30 minute
//   aura (the converter's Special_ReturnTether marker spell).  Clicking it off goes back to where the teleport was used.  The identity (its save slot)
//   keeps it (mod_rowow_identity_tether, the time it has left); the next return-to-save replaces it.  Its rules:
//   - Its 30 minutes count down only while its save slot is played in the RO world (Nathan 2026-11-11): leaving Midgard, a change of save slot or a
//     logout hide its aura and stop its time, which goes on when the slot is back in Midgard (the aura then too)
//   - Not given for a spot that can't be warped to (a nowarpto map)
//   - Clicked off while dead, in combat, unable to act (a status that stops casting) or on a noreturn map it isn't used: the aura comes back with the time
//     it had left, and the player is told why
//   - A spot that's no longer on an RO map (a map gone from a new conversion) or that became nowarpto ends it, saying so

#include "Chat.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MapMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include "ROWOW.h"

#include <regex>

namespace
{
    constexpr uint32 ROWOW_RETURN_TETHER_DURATION_IN_MS = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 ROWOW_SAVE_PLACE_UPDATE_INTERVAL_IN_MS = 1000;

    bool IsSamePosition(ROWOWPosition const& first, ROWOWPosition const& second)
    {
        return first.IsSet == second.IsSet && first.MapID == second.MapID && first.X == second.X && first.Y == second.Y && first.Z == second.Z;
    }

    bool IsReturnTetherCounting(Player const* player, ROWOWIdentity const& identity)
    {
        // The identity's tether shows and its time counts down only while the identity (its save slot) is played in the RO world
        return identity.ReturnTetherOrigin.IsSet == true && identity.ReturnTetherRemainingMS > 0 && identity.IsInForm == true && ROWOW->IsROMap(player->GetMapId()) == true;
    }

    void SendAddonText(Player* player, std::string const& addonMessage)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
        player->SendDirectMessage(&data);
    }

    std::string GetAreaPlaceName(uint32 areaID)
    {
        // An area's name as the client shows it, up to its first comma ("Prontera City, Capitol of Rune-Midgarts" is "Prontera City"); empty for none
        std::string name;
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaID))
            name = area->area_name[sWorld->GetDefaultDbcLocale()];
        size_t commaPosition = name.find(',');
        if (commaPosition != std::string::npos)
            name = name.substr(0, commaPosition);
        for (char& character : name)
            if (character == '\t')
                character = ' ';
        return name;
    }
}

void ROWOWMod::ReturnToSavePoint(Player* player)
{
    // TeleportToSavePoint, leaving the tether at the spot it went from.  The place it went from is named now, while the player stands there (its own
    // area, kept by the core: no lookup that could load terrain off this map's thread)
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWPosition savePoint = identity->SavePoint;
    if (savePoint.IsSet == false && GetCellPosition(StartMapID, StartCellX, StartCellY, savePoint) == false)
        return;
    ROWOWPosition origin;
    origin.IsSet = true;
    origin.MapID = player->GetMapId();
    origin.X = player->GetPositionX();
    origin.Y = player->GetPositionY();
    origin.Z = player->GetPositionZ();
    origin.O = player->GetOrientation();
    std::string originPlaceName = GetAreaPlaceName(player->GetAreaId());
    if (originPlaceName.empty() == true)
    {
        int32 cellX = 0, cellY = 0;
        GetUnitCell(player, cellX, cellY);
        originPlaceName = GetMapNameAt(origin.MapID, cellX, cellY);
    }
    if (player->TeleportTo(savePoint.MapID, savePoint.X, savePoint.Y, savePoint.Z, savePoint.O) == false)
        return;
    PlayTeleportLooks(player); // pc_setpos(..., CLR_TELEPORT): it's still at the spot it leaves (the move waits for the client)
    if (identity->IsInForm == true && IsROMap(origin.MapID) == true && HasMapFlag(GetPositionMapKey(origin), "nowarpto") == false)
        SetReturnTether(player, origin, originPlaceName);
}

void ROWOWMod::SetReturnTether(Player* player, ROWOWPosition const& origin, std::string const& placeName)
{
    if (ReturnTetherSpellID == 0)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    identity->ReturnTetherOrigin = origin;
    identity->ReturnTetherRemainingMS = ROWOW_RETURN_TETHER_DURATION_IN_MS;
    identity->ReturnTetherPlaceName = placeName;
    player->RemoveAurasDueToSpell(ReturnTetherSpellID); // A new one replaces the last, with its full time
    RefreshReturnTether(player);
    SendReturnTetherToClient(player, false);
    if (placeName.empty() == true)
        ChatHandler(player->GetSession()).SendSysMessage("A return tether holds the way back for 30 minutes: click it off your buffs to return to where you teleported from.");
    else
        ChatHandler(player->GetSession()).PSendSysMessage("A return tether holds the way back to {} for 30 minutes: click it off your buffs to return there.", placeName);
}

void ROWOWMod::SendReturnTetherToClient(Player* player, bool isNameLookupAllowed)
{
    // ROWOWTETHER\t<the tether's aura spell>\t<the place it goes back to>, the place empty when there's no tether (the RO_Identity addon puts it on the
    // aura's tooltip)
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (ReturnTetherSpellID == 0 || identity == nullptr)
        return;
    if (identity->ReturnTetherOrigin.IsSet == true && identity->ReturnTetherPlaceName.empty() == true && isNameLookupAllowed == true)
        identity->ReturnTetherPlaceName = GetSavePlaceName(identity->ReturnTetherOrigin); // Loaded with the identity: named the way the save point is
    SendAddonText(player, fmt::format("ROWOWTETHER\t{}\t{}", ReturnTetherSpellID, identity->ReturnTetherOrigin.IsSet == true ? identity->ReturnTetherPlaceName : ""));
}

void ROWOWMod::ClearReturnTether(Player* player)
{
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr)
        return;
    identity->ReturnTetherOrigin = ROWOWPosition();
    identity->ReturnTetherRemainingMS = 0;
    identity->IsReturnTetherUsePending = false;
    identity->ReturnTetherPlaceName.clear();
    if (ReturnTetherSpellID != 0 && player->HasAura(ReturnTetherSpellID) == true)
        player->RemoveAurasDueToSpell(ReturnTetherSpellID);
    SendReturnTetherToClient(player, false);
}

void ROWOWMod::RefreshReturnTether(Player* player)
{
    // After every map arrival and form change (CompleteROFormChange), and when it's set or refused.  The core saves the aura with the character, so one
    // loaded with it is set to the identity's time here, or taken off.  Out of the RO world (or the form) the tether is kept, its aura hidden: the
    // identity's time stands still until it's back (UpdateReturnTether)
    if (ReturnTetherSpellID == 0 || player == nullptr)
        return;
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr)
        return;
    if (identity->ReturnTetherOrigin.IsSet == true && identity->ReturnTetherRemainingMS == 0)
        identity->ReturnTetherOrigin = ROWOWPosition();
    if (IsReturnTetherCounting(player, *identity) == false)
    {
        if (player->HasAura(ReturnTetherSpellID) == true)
            player->RemoveAurasDueToSpell(ReturnTetherSpellID); // Not AURA_REMOVE_BY_CANCEL nor by expiry: the tether stays (OnReturnTetherAuraRemoved)
        return;
    }
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(ReturnTetherSpellID);
    if (spellInfo == nullptr)
        return;
    Aura* aura = player->GetAura(ReturnTetherSpellID);
    if (aura == nullptr)
        aura = player->AddAura(spellInfo, MAX_EFFECT_MASK, player); // The overload that also takes a dead target
    if (aura == nullptr)
        return;
    aura->SetMaxDuration(int32(ROWOW_RETURN_TETHER_DURATION_IN_MS));
    aura->SetDuration(int32(std::min(identity->ReturnTetherRemainingMS, ROWOW_RETURN_TETHER_DURATION_IN_MS)));
}

void ROWOWMod::UpdateReturnTether(Player* player, uint32 diffInMS)
{
    // The player's update (its map's thread; none while it's between maps or logged out): the tether's time goes down only while its identity is
    // played in the RO world, the aura's with it.  Run out, it's over (the aura's own expiry, a moment apart, ends it the same way)
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || player->IsInWorld() == false || player->IsBeingTeleported() == true || IsReturnTetherCounting(player, *identity) == false)
        return;
    if (identity->ReturnTetherRemainingMS > diffInMS)
    {
        identity->ReturnTetherRemainingMS -= diffInMS;
        return;
    }
    ClearReturnTether(player);
    ChatHandler(player->GetSession()).SendSysMessage("Your return tether has faded.");
}

void ROWOWMod::OnReturnTetherAuraRemoved(Player* player, bool isCancelled, bool isExpired)
{
    // The aura's removal (the core's, from the player's map thread).  Clicked off: the next update goes back.  Run out: the tether is over.  Taken off by
    // the module (or any other way) the tether stays as it is
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->ReturnTetherOrigin.IsSet == false)
        return;
    if (isCancelled == true)
        identity->IsReturnTetherUsePending = true;
    else if (isExpired == true)
    {
        identity->ReturnTetherOrigin = ROWOWPosition();
        identity->ReturnTetherRemainingMS = 0;
        identity->ReturnTetherPlaceName.clear();
        SendReturnTetherToClient(player, false);
        ChatHandler(player->GetSession()).SendSysMessage("Your return tether has faded.");
    }
}

void ROWOWMod::UseReturnTether(Player* player)
{
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->IsReturnTetherUsePending == false)
        return;
    if (player->IsInWorld() == false || player->IsBeingTeleported() == true)
        return; // Once it's arrived
    identity->IsReturnTetherUsePending = false;
    if (identity->ReturnTetherOrigin.IsSet == false)
        return;
    if (identity->ReturnTetherRemainingMS == 0)
    {
        ClearReturnTether(player);
        return;
    }
    if (IsReturnTetherCounting(player, *identity) == false)
        return; // Clicked off as it left the RO world: kept for the identity's return (RefreshReturnTether)
    auto refuse = [this, player](char const* text)
    {
        ChatHandler(player->GetSession()).SendNotification(text);
        RefreshReturnTether(player); // Back on the buff bar with the time it had left
    };
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player));
    if (player->IsAlive() == false)
        refuse("You can't follow your return tether while dead.");
    else if (player->IsInCombat() == true)
        refuse("You can't follow your return tether while in combat.");
    else if (sc != nullptr && sc->Cant.Cast == true)
        refuse("You can't follow your return tether right now.");
    else if (HasMapFlag(player, "noreturn") == true)
        refuse("You can't follow your return tether from this area.");
    else
    {
        ROWOWPosition origin = identity->ReturnTetherOrigin;
        ClearReturnTether(player);
        if (IsROMap(origin.MapID) == false || sMapStore.LookupEntry(origin.MapID) == nullptr || HasMapFlag(GetPositionMapKey(origin), "nowarpto") == true)
        {
            ChatHandler(player->GetSession()).SendNotification("The place your return tether led to can't be reached any more.");
            return;
        }
        if (player->TeleportTo(origin.MapID, origin.X, origin.Y, origin.Z, origin.O) == true)
            PlayTeleportLooks(player); // As a wing's teleport looks
    }
}

void ROWOWMod::LoadIdentityReturnTether(ROWOWIdentity& identity)
{
    // The identity's (its save slot's) tether, with the time it had left when it was last saved
    identity.ReturnTetherOrigin = ROWOWPosition();
    identity.ReturnTetherRemainingMS = 0;
    identity.ReturnTetherPlaceName.clear(); // Named on the next .ro sync (SendReturnTetherToClient, the world thread)
    QueryResult result = CharacterDatabase.Query("SELECT `map`, `x`, `y`, `z`, `o`, `remaining_ms` FROM `mod_rowow_identity_tether` WHERE `guid` = {}", identity.IdentityID);
    if (!result)
        return;
    Field* fields = result->Fetch();
    uint32 remainingMS = std::min(fields[5].Get<uint32>(), ROWOW_RETURN_TETHER_DURATION_IN_MS);
    if (remainingMS == 0)
        return;
    identity.ReturnTetherOrigin.IsSet = true;
    identity.ReturnTetherOrigin.MapID = fields[0].Get<uint32>();
    identity.ReturnTetherOrigin.X = fields[1].Get<float>();
    identity.ReturnTetherOrigin.Y = fields[2].Get<float>();
    identity.ReturnTetherOrigin.Z = fields[3].Get<float>();
    identity.ReturnTetherOrigin.O = fields[4].Get<float>();
    identity.ReturnTetherRemainingMS = remainingMS;
}

void ROWOWMod::SaveIdentityReturnTether(ROWOWIdentity const& identity, CharacterDatabaseTransaction trans)
{
    trans->Append("DELETE FROM `mod_rowow_identity_tether` WHERE `guid` = {}", identity.IdentityID);
    ROWOWPosition const& origin = identity.ReturnTetherOrigin;
    if (origin.IsSet == false || identity.ReturnTetherRemainingMS == 0)
        return;
    trans->Append("INSERT INTO `mod_rowow_identity_tether` (`guid`, `map`, `x`, `y`, `z`, `o`, `remaining_ms`) VALUES ({}, {}, {}, {}, {}, {}, {})", identity.IdentityID,
        origin.MapID, origin.X, origin.Y, origin.Z, origin.O, identity.ReturnTetherRemainingMS);
}

void ROWOWMod::LoadReturnToSaveData()
{
    // The items whose use goes to the save point: item_db scripts with Teleport's save point (itemskill AL_TELEPORT at level 2 or 3: the Butterfly Wings)
    // or a warp to it.  A script that only might (the Gift Box?'s rand) isn't one.  Then Teleport's spells that offer the save point (level 2 and up)
    IsReturnToSaveDataLoaded = true;
    ReturnToSaveItemTemplateIDsText.clear();
    ReturnToSaveSpellIDsText.clear();
    std::set<uint32> itemTemplateIDs;
    if (ROWOWDataTable const* itemTable = GetDataTable(ROWOW_DATA_TABLE_ITEM))
    {
        int32 idColumnIndex = itemTable->GetColumnIndex("Id");
        int32 scriptColumnIndex = itemTable->GetColumnIndex("Script");
        std::regex teleportRegex("itemskill\\s*\"?AL_TELEPORT\"?\\s*,\\s*([0-9]+)", std::regex::icase);
        std::regex warpRegex("\\bwarp\\s*\"(SavePoint|Save)\"", std::regex::icase);
        for (size_t rowIndex = 0; idColumnIndex >= 0 && scriptColumnIndex >= 0 && rowIndex < itemTable->Rows.size(); rowIndex++)
        {
            std::string script = ROWOWDataTable::UnescapeText(itemTable->GetCell(rowIndex, scriptColumnIndex));
            if (script.empty() == true || script.find("rand(") != std::string::npos)
                continue;
            std::smatch match;
            bool isReturnToSave = std::regex_search(script, warpRegex);
            if (isReturnToSave == false && std::regex_search(script, match, teleportRegex) == true)
                isReturnToSave = std::stoi(match[1].str()) >= 2;
            if (isReturnToSave == false)
                continue;
            auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(uint32(itemTable->GetInt(rowIndex, idColumnIndex, 0)));
            if (itemTemplateIDIter != ItemTemplateIDsByItemID.end())
                itemTemplateIDs.insert(itemTemplateIDIter->second);
        }
    }
    for (uint32 itemTemplateID : itemTemplateIDs)
        ReturnToSaveItemTemplateIDsText += (ReturnToSaveItemTemplateIDsText.empty() == true ? "" : ",") + std::to_string(itemTemplateID);
    for (uint16 skillLevel = 2; skillLevel <= 10; skillLevel++)
        if (uint32 spellID = GetSkillSpellID(ROWOW_SKILL_AL_TELEPORT, skillLevel))
            ReturnToSaveSpellIDsText += (ReturnToSaveSpellIDsText.empty() == true ? "" : ",") + std::to_string(spellID);
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadReturnToSaveData: return-to-save items {}, spells {}, return tether spell {}", ReturnToSaveItemTemplateIDsText,
        ReturnToSaveSpellIDsText, ReturnTetherSpellID);
}

std::string ROWOWMod::GetSavePlaceName(ROWOWPosition const& savePoint) const
{
    // The area's name as the client shows it, up to its first comma ("Prontera City, Capitol of Rune-Midgarts" is "Prontera City"); else the RO map's name
    std::string name;
    // The ground of an RO map is a WMO, so its area comes from the vmap, which Map::GetAreaId asks before it creates the grid (map and vmap tiles):
    // with no one near the save point (a login or reload away from town on a stitched map) it answered with the map tile's zone, "Midgard".  So
    // the grid is created first
    if (Map* map = sMapMgr->CreateBaseMap(savePoint.MapID))
        map->GetGridTerrainData(savePoint.X, savePoint.Y);
    uint32 areaID = sMapMgr->GetAreaId(PHASEMASK_NORMAL, savePoint.MapID, savePoint.X, savePoint.Y, savePoint.Z);
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaID))
        name = area->area_name[sWorld->GetDefaultDbcLocale()];
    size_t commaPosition = name.find(',');
    if (commaPosition != std::string::npos)
        name = name.substr(0, commaPosition);
    if (name.empty() == true)
    {
        int32 cellX = 0, cellY = 0;
        GetWorldCell(savePoint.MapID, savePoint.X, savePoint.Y, cellX, cellY);
        name = GetMapNameAt(savePoint.MapID, cellX, cellY);
    }
    for (char& character : name)
        if (character == '\t')
            character = ' ';
    return name;
}

void ROWOWMod::SendSavePlaceToClient(Player* player, bool isForced)
{
    // ROWOWSAVE <place>\t<item templates>\t<spells>\t<skills>, each list comma separated
    if (IsEnabled == false || player == nullptr)
        return;
    if (IsReturnToSaveDataLoaded == false)
        LoadReturnToSaveData();
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->Exists == false)
        return;
    ROWOWPosition savePoint = identity->SavePoint;
    if (savePoint.IsSet == false && GetCellPosition(StartMapID, StartCellX, StartCellY, savePoint) == false)
        return;
    if (isForced == false && identity->IsSavePlaceSent == true && IsSamePosition(identity->SentSavePlacePoint, savePoint) == true)
        return;
    identity->SentSavePlacePoint = savePoint;
    identity->IsSavePlaceSent = true;
    SendAddonText(player, fmt::format("ROWOWSAVE\t{}\t{}\t{}\t{}", GetSavePlaceName(savePoint), ReturnToSaveItemTemplateIDsText, ReturnToSaveSpellIDsText,
        ROWOW_SKILL_AL_TELEPORT));
}

void ROWOWMod::UpdateSavePlaces(uint32 diffInMS)
{
    // The world thread (OnWorldUpdate, the maps idle): a save point changed by a Kafra (a map thread's script) is told once a second
    SavePlaceUpdateTimer += diffInMS;
    if (SavePlaceUpdateTimer < ROWOW_SAVE_PLACE_UPDATE_INTERVAL_IN_MS)
        return;
    SavePlaceUpdateTimer = 0;
    for (auto const& sessionByID : sWorldSessionMgr->GetAllSessions())
    {
        Player* player = sessionByID.second != nullptr ? sessionByID.second->GetPlayer() : nullptr;
        if (player == nullptr || player->IsInWorld() == false)
            continue;
        ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity == nullptr || identity->IsLoaded == false || identity->Exists == false)
            continue;
        ROWOWPosition savePoint = identity->SavePoint;
        if (savePoint.IsSet == false && GetCellPosition(StartMapID, StartCellX, StartCellY, savePoint) == false)
            continue;
        if (identity->IsSavePlaceSent == true && IsSamePosition(identity->SentSavePlacePoint, savePoint) == true)
            continue;
        SendSavePlaceToClient(player, true);
    }
}

class ROWOW_ReturnTetherWorldScript : public WorldScript
{
public:
    ROWOW_ReturnTetherWorldScript() : WorldScript("ROWOW_ReturnTetherWorldScript") {}

    void OnUpdate(uint32 diff) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->UpdateSavePlaces(diff);
    }
};

class ROWOW_ReturnTetherPlayerScript : public PlayerScript
{
public:
    ROWOW_ReturnTetherPlayerScript() : PlayerScript("ROWOW_ReturnTetherPlayerScript", { PLAYERHOOK_ON_UPDATE }) {}

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity == nullptr)
            return;
        if (identity->IsReturnTetherUsePending == true)
            ROWOW->UseReturnTether(player);
        ROWOW->UpdateReturnTether(player, p_time);
    }
};

class ROWOW_ReturnTetherUnitScript : public UnitScript
{
public:
    ROWOW_ReturnTetherUnitScript() : UnitScript("ROWOW_ReturnTetherUnitScript", true, { UNITHOOK_ON_AURA_REMOVE }) {}

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (ROWOW->ReturnTetherSpellID == 0 || unit == nullptr || unit->IsPlayer() == false || aurApp == nullptr || aurApp->GetBase()->GetId() != ROWOW->ReturnTetherSpellID)
            return;
        ROWOW->OnReturnTetherAuraRemoved(unit->ToPlayer(), mode == AURA_REMOVE_BY_CANCEL, mode == AURA_REMOVE_BY_EXPIRE);
    }
};

void AddROWOWReturnTetherScripts()
{
    new ROWOW_ReturnTetherWorldScript();
    new ROWOW_ReturnTetherPlayerScript();
    new ROWOW_ReturnTetherUnitScript();
}
