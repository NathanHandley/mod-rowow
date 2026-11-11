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
//   aura (the converter's Special_ReturnTether marker spell).  Clicking it off goes back to where the teleport was used.  The identity keeps it
//   (mod_rowow_identity_tether), so it lasts through a relog (its 30 minutes are real time, offline too); the next return-to-save replaces it.  Its rules:
//   - Only in the form, in the RO world: leaving Midgard ends it (the travel back in goes to where it left anyway)
//   - Not given for a spot that can't be warped to (a nowarpto map)
//   - Clicked off while dead, in combat, unable to act (a status that stops casting) or on a noreturn map it isn't used: the aura comes back with the time
//     it had left, and the player is told why
//   - A spot that's no longer on an RO map (a map gone from a new conversion) or that became nowarpto ends it, saying so

#include "Chat.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
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
    constexpr uint32 ROWOW_RETURN_TETHER_DURATION_IN_SECONDS = 30 * MINUTE;
    constexpr uint32 ROWOW_SAVE_PLACE_UPDATE_INTERVAL_IN_MS = 1000;

    bool IsSamePosition(ROWOWPosition const& first, ROWOWPosition const& second)
    {
        return first.IsSet == second.IsSet && first.MapID == second.MapID && first.X == second.X && first.Y == second.Y && first.Z == second.Z;
    }

    void SendAddonText(Player* player, std::string const& addonMessage)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
        player->SendDirectMessage(&data);
    }
}

void ROWOWMod::ReturnToSavePoint(Player* player)
{
    // TeleportToSavePoint, leaving the tether at the spot it went from
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
    if (player->TeleportTo(savePoint.MapID, savePoint.X, savePoint.Y, savePoint.Z, savePoint.O) == false)
        return;
    if (identity->IsInForm == true && IsROMap(origin.MapID) == true && HasMapFlag(GetPositionMapKey(origin), "nowarpto") == false)
        SetReturnTether(player, origin);
}

void ROWOWMod::SetReturnTether(Player* player, ROWOWPosition const& origin)
{
    if (ReturnTetherSpellID == 0)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    identity->ReturnTetherOrigin = origin;
    identity->ReturnTetherExpireTime = uint32(GameTime::GetGameTime().count()) + ROWOW_RETURN_TETHER_DURATION_IN_SECONDS;
    player->RemoveAurasDueToSpell(ReturnTetherSpellID); // A new one replaces the last, with its full time
    RefreshReturnTether(player);
    ChatHandler(player->GetSession()).SendSysMessage("A return tether holds the way back for 30 minutes: click it off your buffs to return to where you teleported from.");
}

void ROWOWMod::ClearReturnTether(Player* player)
{
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr)
        return;
    identity->ReturnTetherOrigin = ROWOWPosition();
    identity->ReturnTetherExpireTime = 0;
    identity->IsReturnTetherUsePending = false;
    if (ReturnTetherSpellID != 0 && player->HasAura(ReturnTetherSpellID) == true)
        player->RemoveAurasDueToSpell(ReturnTetherSpellID);
}

void ROWOWMod::RefreshReturnTether(Player* player)
{
    // After every map arrival and form change (CompleteROFormChange), and when it's set or refused.  The core saves the aura with the character, so one
    // loaded with it is set to the identity's time here, or taken off
    if (ReturnTetherSpellID == 0 || player == nullptr)
        return;
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr)
        return;
    uint32 now = uint32(GameTime::GetGameTime().count());
    if (identity->ReturnTetherOrigin.IsSet == true && (identity->ReturnTetherExpireTime <= now || identity->IsInForm == false))
    {
        identity->ReturnTetherOrigin = ROWOWPosition();
        identity->ReturnTetherExpireTime = 0;
    }
    bool isWanted = identity->ReturnTetherOrigin.IsSet == true && identity->IsInForm == true && IsROMap(player->GetMapId()) == true;
    if (isWanted == false)
    {
        if (player->HasAura(ReturnTetherSpellID) == true)
            player->RemoveAurasDueToSpell(ReturnTetherSpellID);
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
    int32 durationInMS = int32(std::min(identity->ReturnTetherExpireTime - now, ROWOW_RETURN_TETHER_DURATION_IN_SECONDS) * IN_MILLISECONDS);
    aura->SetMaxDuration(int32(ROWOW_RETURN_TETHER_DURATION_IN_SECONDS * IN_MILLISECONDS));
    aura->SetDuration(durationInMS);
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
        identity->ReturnTetherExpireTime = 0;
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
    if (identity->ReturnTetherExpireTime <= uint32(GameTime::GetGameTime().count()) || identity->IsInForm == false || IsROMap(player->GetMapId()) == false)
    {
        ClearReturnTether(player);
        return;
    }
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
        player->TeleportTo(origin.MapID, origin.X, origin.Y, origin.Z, origin.O);
    }
}

void ROWOWMod::LoadIdentityReturnTether(ROWOWIdentity& identity)
{
    identity.ReturnTetherOrigin = ROWOWPosition();
    identity.ReturnTetherExpireTime = 0;
    QueryResult result = CharacterDatabase.Query("SELECT `map`, `x`, `y`, `z`, `o`, `expire_time` FROM `mod_rowow_identity_tether` WHERE `guid` = {}", identity.IdentityID);
    if (!result)
        return;
    Field* fields = result->Fetch();
    uint32 expireTime = fields[5].Get<uint32>();
    if (expireTime <= uint32(GameTime::GetGameTime().count()))
        return;
    identity.ReturnTetherOrigin.IsSet = true;
    identity.ReturnTetherOrigin.MapID = fields[0].Get<uint32>();
    identity.ReturnTetherOrigin.X = fields[1].Get<float>();
    identity.ReturnTetherOrigin.Y = fields[2].Get<float>();
    identity.ReturnTetherOrigin.Z = fields[3].Get<float>();
    identity.ReturnTetherOrigin.O = fields[4].Get<float>();
    identity.ReturnTetherExpireTime = expireTime;
}

void ROWOWMod::SaveIdentityReturnTether(ROWOWIdentity const& identity, CharacterDatabaseTransaction trans)
{
    trans->Append("DELETE FROM `mod_rowow_identity_tether` WHERE `guid` = {}", identity.IdentityID);
    ROWOWPosition const& origin = identity.ReturnTetherOrigin;
    if (origin.IsSet == false || identity.ReturnTetherExpireTime <= uint32(GameTime::GetGameTime().count()))
        return;
    trans->Append("INSERT INTO `mod_rowow_identity_tether` (`guid`, `map`, `x`, `y`, `z`, `o`, `expire_time`) VALUES ({}, {}, {}, {}, {}, {}, {})", identity.IdentityID,
        origin.MapID, origin.X, origin.Y, origin.Z, origin.O, identity.ReturnTetherExpireTime);
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

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity != nullptr && identity->IsReturnTetherUsePending == true)
            ROWOW->UseReturnTether(player);
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
