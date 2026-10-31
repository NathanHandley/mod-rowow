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
//  Kafra storage follows rAthena's storage.cpp (storage_storageopen, storage_guild_storageopen, storage_storageclose):
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "Guild.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// Kafra storage (R8.2): there is no RO storage of its own (plan section 8 item 7).  A Kafra's storage is the character's WoW bank, in the
// stock bank window, and its guild's bank when the guild has bought a tab, in the stock guild bank window.  Neither needs a banker in reach:
// the player is its own banker, as AzerothCore's bank command (.character check bank) does for the bank and the core's SendShowGuildBank
// (MergedWorlds) does for the guild bank.  The client opens the guild bank window on a list it didn't ask for on an RO map (MergedWorldClient
// ro-guild-storage).  Zeny never moves into or out of the guild bank (plan section 8 item 5)

void ROWOWMod::StorageStorageOpen(Player* player)
{
    // storage_storageopen: the bank window, with the player as the banker (rAthena keeps it open until the client closes it; here until the
    // player leaves the map)
    if (player == nullptr || player->GetSession() == nullptr)
        return;
    GetIdentity(player)->StorageFlag = 1;
    player->GetSession()->SendShowBank(player->GetGUID());
}

int32 ROWOWMod::StorageGuildStorageOpen(Player* player)
{
    // storage_guild_storageopen: a guild with a bank, then its window at the next world update (the list reads the guild, which the world
    // thread changes).  rAthena's "already open" answers need the client's close, which the WoW client doesn't send; both windows may be open
    if (player == nullptr || player->GetSession() == nullptr)
        return ROWOW_GSTORAGE_NO_GUILD;
    Guild* guild = player->GetGuild();
    if (guild == nullptr)
        return ROWOW_GSTORAGE_NO_GUILD;
    if (guild->GetPurchasedBankTabCount() == 0)
        return ROWOW_GSTORAGE_NO_STORAGE;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    PendingGuildStorageOpens.insert(player->GetGUID());
    return ROWOW_GSTORAGE_OPEN;
}

void ROWOWMod::StorageClose(Player* player)
{
    // storage_storageclose and storage_guild_storageclose: leaving the map ends both (pc_setpos closes them in rAthena).  The client's guild
    // bank window may stay up, but the guild bank no longer takes the player as its banker.  (The bank does until another banker is used, as
    // with AzerothCore's bank command)
    if (player == nullptr || player->GetSession() == nullptr)
        return;
    GetIdentity(player)->StorageFlag = 0;
    player->GetSession()->SendShowGuildBank(ObjectGuid::Empty);
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    PendingGuildStorageOpens.erase(player->GetGUID());
}

void ROWOWMod::OpenPendingGuildStorages()
{
    // The world update, with the maps idle: each guild bank asked for is shown to its player, if it's still in the form and its guild still
    // has a bank
    std::set<ObjectGuid> playerGUIDs;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        playerGUIDs.swap(PendingGuildStorageOpens);
    }
    for (ObjectGuid const& playerGUID : playerGUIDs)
    {
        Player* player = ObjectAccessor::FindPlayer(playerGUID);
        if (player == nullptr || player->GetSession() == nullptr || player->IsInWorld() == false || IsInROForm(player) == false)
            continue;
        Guild* guild = player->GetGuild();
        if (guild == nullptr || guild->GetPurchasedBankTabCount() == 0)
            continue;
        player->GetSession()->SendShowGuildBank(player->GetGUID());
    }
}

// Kafra storage's hooks: the guild bank windows asked for, the end of storage on leaving a map, and no zeny in or out of the guild bank
class ROWOW_StorageWorldScript : public WorldScript
{
public:
    ROWOW_StorageWorldScript() : WorldScript("ROWOW_StorageWorldScript") {}

    void OnUpdate(uint32 /*diff*/) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->OpenPendingGuildStorages();
    }
};

class ROWOW_StoragePlayerScript : public PlayerScript
{
public:
    ROWOW_StoragePlayerScript() : PlayerScript("ROWOW_StoragePlayerScript", { PLAYERHOOK_ON_MAP_CHANGED }) {}

    void OnPlayerMapChanged(Player* player) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->StorageClose(player);
    }
};

class ROWOW_StorageGuildScript : public GuildScript
{
public:
    ROWOW_StorageGuildScript() : GuildScript("ROWOW_StorageGuildScript", { GUILDHOOK_ON_MEMBER_WITDRAW_MONEY, GUILDHOOK_ON_MEMBER_DEPOSIT_MONEY }) {}

    void OnMemberWitdrawMoney(Guild* /*guild*/, Player* player, uint32& amount, bool /*isRepair*/) override
    {
        // In the form the character's money is its zeny, which never comes from the guild's gold
        if (ROWOW->IsEnabled == false || player == nullptr || ROWOW->IsInROForm(player) == false)
            return;
        amount = 0;
        ChatHandler(player->GetSession()).SendNotification("Zeny can't come out of the guild bank.");
    }

    void OnMemberDepositMoney(Guild* /*guild*/, Player* player, uint32& amount) override
    {
        // ... and never goes into it
        if (ROWOW->IsEnabled == false || player == nullptr || ROWOW->IsInROForm(player) == false)
            return;
        amount = 0;
        ChatHandler(player->GetSession()).SendNotification("Zeny can't go into the guild bank.");
    }
};

void AddROWOWStorageScripts()
{
    new ROWOW_StorageWorldScript();
    new ROWOW_StoragePlayerScript();
    new ROWOW_StorageGuildScript();
}
