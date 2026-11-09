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
#include "DatabaseEnv.h"
#include "Guild.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// Kafra storage (R8.2, R12): RO's storage is account wide, and so is the Ragnarok tab of the bank window, which a Kafra opens (plan section 8
// items 7 and 14, RONotes\world-bank-tabs.md).  The tab takes RO items only, in its item slots and in general bags of any game in its bag slots,
// the core holds them (account_bank_item, world 1), its corner shows zeny, and any banker in any world reaches it.  Its bag slots are bought with
// gold, out of the form, at a stock banker, and each comes with an Adventurer's Bag that can't leave it (StorageFillBagSlots).  The Character and Account tabs are beside it, and the guild's bank when the guild has bought a tab, in the stock guild
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

uint32 ROWOWMod::StorageGetZeny(Player* player)
{
    // In the form the character's money is its zeny, and out of it the money is gold
    ROWOWIdentity* identity = GetIdentity(player);
    return identity->IsInForm == true ? player->GetMoney() : identity->Zeny;
}

std::string ROWOWMod::StorageCurrencyText(Player* player)
{
    uint32 zeny = StorageGetZeny(player);
    GetIdentity(player)->StorageZenyShown = zeny;
    std::string text = std::to_string(zeny);
    for (int32 i = int32(text.size()) - 3; i > 0; i -= 3)
        text.insert(i, ",");
    return text + " z";
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

void ROWOWMod::StorageFillBagSlots(Player* player)
{
    // The Ragnarok tab's bag slots each hold an Adventurer's Bag, which stays there (Nathan 2026-11-09): a bought slot that's empty gets one, at login
    // (slots bought before this, a bag taken out), and while the tab's bags are in the slots (a slot bought there, a bag of another game taken out of
    // one).  A general bag of another game that was put in a slot before stays until it's taken out.  The account's bags, so they're written at once,
    // as the slot's purchase is.  The player's own thread, with only its own items
    if (StartBagItemTemplateID == 0 || player->IsAccountBankLoaded() == false || Player::IsWorldBankEnabled(WORLD_BANK_RAGNAROK) == false)
        return;
    uint8 slotCount = std::min<uint8>(player->GetAccountBankBagSlotCount(WORLD_BANK_RAGNAROK), MAX_ACCOUNT_BANK_BAG_SLOT_COUNT);
    if (slotCount == 0)
        return;
    uint8 shownPage = player->GetBankBagPage();
    if (shownPage != WORLD_BANK_RAGNAROK)
        player->SetBankBagPage(WORLD_BANK_RAGNAROK);
    uint32 bagsGiven = 0;
    for (uint8 slot = ACCOUNT_BANK_BAG_SLOT_START; slot < ACCOUNT_BANK_BAG_SLOT_START + slotCount; slot++)
    {
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) != nullptr)
            continue;
        Item* bag = Item::CreateItem(StartBagItemTemplateID, 1, player);
        if (bag == nullptr)
            break;
        ItemPosCountVec bagDestination;
        bagDestination.push_back(ItemPosCount((uint16(INVENTORY_SLOT_BAG_0) << 8) | slot, 1));
        player->StoreItem(bagDestination, bag, true);
        bagsGiven++;
    }
    if (shownPage != WORLD_BANK_RAGNAROK)
        player->SetBankBagPage(shownPage);
    if (bagsGiven == 0)
        return;
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    player->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    LOG_INFO("module.ROWOW", "Storage of account {}: {} Adventurer's Bags put in the Ragnarok tab's bag slots ({} bought), by {}", player->GetSession()->GetAccountId(),
        bagsGiven, slotCount, player->GetName());
}

bool ROWOWMod::StorageIsFixedBag(Player* player, Item const* item) const
{
    // The bag slots hold the Ragnarok tab's bags only while its page is in them
    if (item == nullptr || StartBagItemTemplateID == 0 || item->GetEntry() != StartBagItemTemplateID || player->GetBankBagPage() != WORLD_BANK_RAGNAROK)
        return false;
    if (item->GetBagSlot() != INVENTORY_SLOT_BAG_0 || item->GetSlot() < ACCOUNT_BANK_BAG_SLOT_START || item->GetSlot() >= ACCOUNT_BANK_BAG_SLOT_END)
        return false;
    return player->GetItemByPos(INVENTORY_SLOT_BAG_0, item->GetSlot()) == item;
}

bool ROWOWMod::IsStorageBagMoveRefused(Player* player, uint8 sourceBag, uint8 sourceSlot, uint8 destinationBag, uint8 destinationSlot) const
{
    // A move, a swap, a destroy or a store elsewhere that would take the Ragnarok tab's Adventurer's Bag out of its slot, or put something in its place.
    // What goes in or out of the bag itself names the bag as its bag, not as a slot, so it isn't refused here
    auto isFixedBagAt = [player, this](uint8 bag, uint8 slot)
    {
        return bag == INVENTORY_SLOT_BAG_0 && slot >= ACCOUNT_BANK_BAG_SLOT_START && slot < ACCOUNT_BANK_BAG_SLOT_END &&
            StorageIsFixedBag(player, player->GetItemByPos(bag, slot)) == true;
    };
    return isFixedBagAt(sourceBag, sourceSlot) == true || isFixedBagAt(destinationBag, destinationSlot) == true;
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
    ROWOW_StoragePlayerScript() : PlayerScript("ROWOW_StoragePlayerScript", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_CAN_BANK_ITEM,
        PLAYERHOOK_CAN_BUY_BANK_SLOT, PLAYERHOOK_ON_BANK_CURRENCY_TEXT, PLAYERHOOK_CAN_GUILD_BANK_ITEM }) {}

    void OnPlayerLogin(Player* player) override
    {
        // The account's Ragnarok tab, loaded with the character: its bought bag slots each get an Adventurer's Bag they lack
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->StorageFillBagSlots(player);
    }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        // While the Ragnarok tab's bags are in the slots: a slot bought there, or a slot a bag of another game was taken out of, gets its bag
        if (player->GetBankBagPage() == WORLD_BANK_RAGNAROK)
            ROWOW->StorageFillBagSlots(player);
        // Zeny changes with the tab showing, a Kafra's fee or a shop's sale, and every path ends in the money or the identity, so this watches them.  The
        // player's own map thread, and it only writes to its own session and identity
        if (player->GetBankTabShowing() != WORLD_BANK_RAGNAROK + 1)
            return;
        if (ROWOW->StorageGetZeny(player) != ROWOW->GetIdentity(player)->StorageZenyShown)
            player->SendBankCurrencyText(WORLD_BANK_RAGNAROK);
    }

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

    void OnPlayerCanBuyBankSlot(Player* player, bool& result) override
    {
        // Bag slots cost gold on every tab, and zeny never pays one (plan section 8 item 13).  The form can't change with the bank open: leaving it
        // changes map, which closes the bank
        if (ROWOW->IsEnabled == true && ROWOW->IsInROForm(player) == true)
            result = false;
    }

    void OnPlayerBankCurrencyText(Player* player, uint8 world, std::string& text) override
    {
        if (ROWOW->IsEnabled == true && world == WORLD_BANK_RAGNAROK)
            text = ROWOW->StorageCurrencyText(player);
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
