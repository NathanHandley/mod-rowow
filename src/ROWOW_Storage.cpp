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
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// Kafra storage (R8.2, R12): RO's storage is account wide, and so is the Ragnarok tab of the bank window, which a Kafra opens (plan section 8
// items 7 and 14, RONotes\world-bank-tabs.md).  The tab takes RO items only, the core holds them (account_bank_item, world 1), and any banker
// in any world reaches it.  The Character and Account tabs are beside it, and the guild's bank when the guild has bought a tab, in the stock guild
// bank window.  Neither needs a banker in reach: the player is its own banker, as AzerothCore's bank command (.character check bank) does for the
// bank and the core's SendShowGuildBank (MergedWorlds) does for the guild bank.  The client opens the guild bank window on a list it didn't ask for
// on an RO map (MergedWorldClient ro-guild-storage).  Zeny never moves into or out of the guild bank (plan section 8 item 5)

void ROWOWMod::StorageStorageOpen(Player* player)
{
    // storage_storageopen: the bank window on the Ragnarok tab, with the player as the banker (rAthena keeps it open until the client closes it;
    // here until the player leaves the map)
    if (player == nullptr || player->GetSession() == nullptr)
        return;
    GetIdentity(player)->StorageFlag = 1;
    player->GetSession()->SendShowBank(player->GetGUID(), WORLD_BANK_RAGNAROK);
}

InventoryResult ROWOWMod::StorageCanAddItem(Player* player, Item const* item, uint8 world)
{
    // The Ragnarok tab takes what storage_additem does: an RO item (a companion pet's carrier isn't one) that RO's storage takes (itemdb_canstore).
    // A Kafra reaches the Character and Account tabs too, so itemdb_canstore holds on them, and on every tab worn equipment stays out: it sits in the
    // bags with its place in the identity, in the form or not, until it's taken off
    if (IsROItemTemplate(item->GetEntry()) == false)
        return world == WORLD_BANK_RAGNAROK ? EQUIP_ERR_WORLD_BANK_ITEM_NOT_ALLOWED : EQUIP_ERR_OK;
    if (GetIdentityEquipPosition(*GetIdentity(player), item->GetGUID().GetCounter()) != 0)
        return EQUIP_ERR_WORLD_BANK_ITEM_WORN;
    if (GetROItem(item) == nullptr || ItemDBIsRestricted(item, &ROWOWItem::IsNoStorage) == true)
        return EQUIP_ERR_ACCOUNT_BANK_ITEM_NOT_ALLOWED;
    return EQUIP_ERR_OK;
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
    // storage_storageclose and storage_guild_storageclose: leaving the map ends both (pc_setpos closes them in rAthena).  The client's windows
    // may stay up, but neither bank takes the player as its banker any more.  A banker of AzerothCore's bank command is left alone: only a
    // Kafra's ends here
    if (player == nullptr || player->GetSession() == nullptr)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->StorageFlag != 0)
        player->GetSession()->ClearPlayerBanker();
    identity->StorageFlag = 0;
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

// Kafra storage's hooks: the guild bank windows asked for, the end of storage on leaving a map, what the bank tabs take, and no zeny in or
// out of the guild bank
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
    ROWOW_StoragePlayerScript() : PlayerScript("ROWOW_StoragePlayerScript", { PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_CAN_BANK_ITEM, PLAYERHOOK_CAN_GUILD_BANK_ITEM }) {}

    void OnPlayerMapChanged(Player* player) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->StorageClose(player);
    }

    void OnPlayerCanBankItem(Player* player, Item const* item, uint8 world, InventoryResult& result) override
    {
        // The player's own thread: its session's packets, which also read its identity.  Other worlds' tabs are their modules' to answer, and on
        // the Character and Account tabs this only ever refuses
        if (ROWOW->IsEnabled == false || (world != WORLD_BANK_ACCOUNT && world != WORLD_BANK_RAGNAROK))
            return;
        InventoryResult storageResult = ROWOW->StorageCanAddItem(player, item, world);
        if (world == WORLD_BANK_RAGNAROK || storageResult != EQUIP_ERR_OK)
            result = storageResult;
    }

    void OnPlayerCanGuildBankItem(Player* player, Item const* item, InventoryResult& result) override
    {
        // storage_guild_additem's itemdb_canguildstore, and worn equipment stays out.  The world thread: the guild bank's packets
        if (ROWOW->IsEnabled == false || ROWOW->IsROItemTemplate(item->GetEntry()) == false)
            return;
        if (ROWOW->GetIdentityEquipPosition(*ROWOW->GetIdentity(player), item->GetGUID().GetCounter()) != 0)
            result = EQUIP_ERR_WORLD_BANK_ITEM_WORN;
        else if (ROWOW->ItemDBIsRestricted(item, &ROWOWItem::IsNoGuildStorage) == true)
        {
            ChatHandler(player->GetSession()).SendSysMessage("This item cannot be stored in the guild's storage."); // msg_txt 264
            result = EQUIP_ERR_ITEMS_CANT_BE_SWAPPED;
        }
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
