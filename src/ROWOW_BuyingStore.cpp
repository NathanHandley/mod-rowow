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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/buyingstore.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// Buying stores: a player opens a store of the items it wants to buy, at a price each, up to a zeny limit (Open Buying Store, or a Bulk Buyer Shop
// License's script), and the players around sell to it.  The setup and the store are the RO_Identity addon's windows (ROWOWBUYREQ, ROWOWBUYLIST),
// its sign the rooms' (SetShopSign); the windows' picks come back as .ro buyopen, buyclose, buylist and buysell, chat commands that run in the
// world update with the maps idle, so the buyer and seller, on one map, need no lock.  A buyer can't move, and sits as a vendor does.  An item
// with a maker, a card or a refine can't be sold (buyingstore_blankslots).  (Autotrade, the store search and the database rows are left out)

namespace
{
    constexpr int64 BuyingStoreMaxPrice = 99990000; // BUYINGSTORE_MAX_PRICE
    constexpr uint32 BuyingStoreMaxAmount = 9999; // BUYINGSTORE_MAX_AMOUNT
    constexpr size_t BuyingStoreMessageChunkLength = 200;

    void SendAddonMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, message);
        player->SendDirectMessage(&data);
    }

    // The sellable stacks of an item in the seller's bags: no enchantment, no maker, not worn (buyingstore_blankslots, the inventory's checks)
    std::vector<Item*> GetSellableItems(ROWOWMod const* mod, Player* player, ROWOWIdentity const& identity, uint32 itemTemplateID)
    {
        std::vector<Item*> items;
        for (uint8 bag = 0; bag <= 4; bag++)
        {
            for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
            {
                Item* item = mod->GetItemByClientPosition(player, bag, slot);
                if (item == nullptr || item->GetEntry() != itemTemplateID || mod->GetIdentityEquipPosition(identity, item->GetGUID().GetCounter()) != 0 ||
                    item->GetGuidValue(ITEM_FIELD_CREATOR).IsEmpty() == false)
                    continue;
                bool isBlank = true;
                for (uint8 enchantmentSlot = 0; enchantmentSlot < MAX_ENCHANTMENT_SLOT; enchantmentSlot++)
                    if (item->GetEnchantmentId(EnchantmentSlot(enchantmentSlot)) != 0)
                        isBlank = false;
                if (isBlank == true)
                    items.push_back(item);
            }
        }
        return items;
    }
}

bool ROWOWMod::BuyingStoreSetup(Player* player, uint8 slots)
{
    // buyingstore_setup: the setup window, for up to MAX_BUYINGSTORE_SLOTS items
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsVending == true || identity->BuyingStore != nullptr || player->GetTradeData() != nullptr || slots == 0)
        return false;
    auto mapIDsIter = MapIDsByMapFlag.find("nobuyingstore");
    if (mapIDsIter != MapIDsByMapFlag.end() && mapIDsIter->second.find(player->GetMapId()) != mapIDsIter->second.end())
    {
        ChatHandler(player->GetSession()).SendSysMessage("You can't open a shop on this map.");
        return false;
    }
    identity->BuyingStoreSlots = std::min<uint8>(slots, ROWOW_MAX_BUYINGSTORE_SLOTS);
    SendAddonMessage(player, fmt::format("ROWOWBUYREQ\t{}\t{}", identity->BuyingStoreSlots, player->GetMoney()));
    return true;
}

bool ROWOWMod::BuyingStoreCreate(Player* player, int64 zenyLimit, std::string const& title, std::vector<ROWOWBuyingStoreItem> const& items)
{
    // buyingstore_create: the zeny limit no more than the zeny held, and each item one the buyer holds at least one of, that a store may buy
    // (BuyingStore in its flags), wanted once, at a price
    ROWOWIdentity* identity = GetIdentity(player);
    ChatHandler handler(player->GetSession());
    uint8 slots = identity->BuyingStoreSlots;
    identity->BuyingStoreSlots = 0;
    auto failed = [&handler]()
    {
        handler.SendNotification("Failed to open the buying store."); // clif_buyingstore_open_failed
        return false;
    };
    if (items.empty() == true)
        return false; // canceled, or no items
    if (slots == 0 || PCCantAct(player) == true || items.size() > slots || zenyLimit <= 0 || zenyLimit > int64(player->GetMoney()) || title.empty() == true)
        return failed(); // disabled or invalid input
    for (size_t index = 0; index < items.size(); index++)
    {
        ROWOWBuyingStoreItem const& item = items[index];
        ROWOWItem const* roItem = GetROItemByItemTemplateID(item.ItemTemplateID);
        if (roItem == nullptr || item.Amount == 0)
            return failed(); // invalid input
        if (item.Price == 0 || item.Price > BuyingStoreMaxPrice)
            return failed(); // invalid price: unlike vending, items cannot be bought at 0 Zeny
        if (roItem->IsBuyingStore == false || roItem->IsNoTrade == true)
            return failed(); // restrictions: allowed and no character-bound items
        uint32 held = player->GetItemCount(item.ItemTemplateID);
        if (held == 0)
            return failed(); // At least one must be owned
        if (held + item.Amount > BuyingStoreMaxAmount)
            return failed(); // too many items of same kind
        for (size_t earlier = 0; earlier < index; earlier++)
            if (items[earlier].ItemTemplateID == item.ItemTemplateID)
                return failed(); // duplicate
    }

    // success
    identity->BuyingStore = std::make_unique<ROWOWBuyingStore>();
    identity->BuyingStore->BuyerID = ++BuyingStoreNextID;
    identity->BuyingStore->ZenyLimit = zenyLimit;
    identity->BuyingStore->Title = title.substr(0, 79); // MESSAGE_SIZE
    identity->BuyingStore->Items = items;
    player->StopMoving();
    player->AttackStop();
    player->SetStandState(UNIT_STAND_STATE_SIT);
    SetShopSign(player, identity->BuyingStore->Title, true);
    PCUpdateMoveLock(player);
    SendAddonMessage(player, "ROWOWBUYOPEN\t" + identity->BuyingStore->Title);
    return true;
}

void ROWOWMod::BuyingStoreClose(Player* player)
{
    // buyingstore_close
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->BuyingStore == nullptr)
        return;
    identity->BuyingStore.reset();
    RemoveShopSign(player);
    PCUpdateMoveLock(player);
    SendAddonMessage(player, "ROWOWBUYOPEN\t");
}

void ROWOWMod::BuyingStoreOpen(Player* player, Player* buyer)
{
    // buyingstore_open and clif_buyingstore_itemlist: what the store still buys, for the seller's window ("B<tab><buyer GUID><tab><buyer id><tab>
    // <zeny limit><tab><title>", "S<tab><template>:<amount>:<price>|...", "E"), or an empty list for a store that's gone or out of sight
    ROWOWIdentity* buyerIdentity = buyer != nullptr ? buyer->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
    if (buyerIdentity == nullptr || buyerIdentity->BuyingStore == nullptr || buyer == player || PCCantAct(player) == true || buyer->GetMap() != player->GetMap() ||
        DistanceBL(player, buyer) > BattleConfig.AreaSize)
    {
        SendAddonMessage(player, "ROWOWBUYLIST\tB\t0\t0\t0\t");
        SendAddonMessage(player, "ROWOWBUYLIST\tE");
        return;
    }
    ROWOWBuyingStore const& store = *buyerIdentity->BuyingStore;
    SendAddonMessage(player, fmt::format("ROWOWBUYLIST\tB\t{}\t{}\t{}\t{}", buyer->GetGUID().GetCounter(), store.BuyerID, store.ZenyLimit, store.Title));
    std::string chunk;
    for (ROWOWBuyingStoreItem const& item : store.Items)
    {
        if (item.Amount == 0)
            continue;
        std::string itemText = fmt::format("{}:{}:{}", item.ItemTemplateID, item.Amount, item.Price);
        if (chunk.empty() == false && chunk.size() + itemText.size() + 1 > BuyingStoreMessageChunkLength)
        {
            SendAddonMessage(player, "ROWOWBUYLIST\tS\t" + chunk);
            chunk.clear();
        }
        chunk += (chunk.empty() == true ? "" : "|") + itemText;
    }
    if (chunk.empty() == false)
        SendAddonMessage(player, "ROWOWBUYLIST\tS\t" + chunk);
    SendAddonMessage(player, "ROWOWBUYLIST\tE");
}

void ROWOWMod::BuyingStoreTrade(Player* player, Player* buyer, uint32 buyerID, std::vector<std::pair<uint32, uint32>> const& sales)
{
    // buyingstore_trade: all of a sale is checked first, then each item moves from the seller's bags to the buyer's, and the buyer pays.  The
    // store closes once it wants nothing more or has no zeny left
    ChatHandler handler(player->GetSession());
    auto failed = [&handler]()
    {
        handler.SendNotification("Failed to sell the item."); // clif_buyingstore_trade_failed_seller
    };
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWIdentity* buyerIdentity = buyer != nullptr ? buyer->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
    if (sales.empty() == true)
        return;
    if (PCCantAct(player) == true || buyerIdentity == nullptr || buyerIdentity->BuyingStore == nullptr || buyerIdentity->BuyingStore->BuyerID != buyerID || buyer == player ||
        buyer->GetMap() != player->GetMap() || DistanceBL(player, buyer) > BattleConfig.AreaSize)
        return failed(); // not online, not buying or not same store; out of view range
    ROWOWBuyingStore& store = *buyerIdentity->BuyingStore;

    // buyer lost zeny in the mean time? fix the limit
    store.ZenyLimit = std::min<int64>(store.ZenyLimit, buyer->GetMoney());
    std::vector<ROWOWStoreItem> storeItems;
    int64 zeny = 0;
    for (size_t index = 0; index < sales.size(); index++)
    {
        uint32 itemTemplateID = sales[index].first;
        uint32 amount = sales[index].second;
        for (size_t earlier = 0; earlier < index; earlier++)
            if (sales[earlier].first == itemTemplateID)
                return failed(); // duplicate
        uint32 held = 0;
        for (Item* item : GetSellableItems(this, player, *identity, itemTemplateID))
            held += item->GetCount();
        if (amount == 0 || held < amount)
            return failed(); // invalid input
        ROWOWItem const* roItem = GetROItemByItemTemplateID(itemTemplateID);
        if (roItem == nullptr || roItem->IsNoTrade == true)
            return failed(); // non-tradable item
        auto wanted = std::find_if(store.Items.begin(), store.Items.end(), [itemTemplateID](ROWOWBuyingStoreItem const& item) { return item.ItemTemplateID == itemTemplateID; });
        if (wanted == store.Items.end() || wanted->Amount == 0)
            return failed(); // there is no such item or the buyer has already bought all of them
        if (wanted->Amount < amount)
        {
            handler.SendNotification("The store doesn't want that many."); // BUYINGSTORE_TRADE_SELLER_COUNT
            return;
        }
        storeItems.push_back(ROWOWStoreItem{ itemTemplateID, amount, 0 });
        if (int64(amount) * wanted->Price > store.ZenyLimit - zeny)
        {
            handler.SendNotification("The store has run out of zeny."); // BUYINGSTORE_TRADE_SELLER_ZENY
            return;
        }
        zeny += int64(amount) * wanted->Price;
    }
    if (CanStoreROItems(buyer, storeItems) == false)
        return failed(); // buyer does not have enough space for these items

    // process item list
    for (std::pair<uint32, uint32> const& sale : sales)
    {
        uint32 itemTemplateID = sale.first;
        auto wanted = std::find_if(store.Items.begin(), store.Items.end(), [itemTemplateID](ROWOWBuyingStoreItem const& item) { return item.ItemTemplateID == itemTemplateID; });
        int64 saleZeny = int64(sale.second) * wanted->Price;
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemTemplateID);
        std::string itemName = itemTemplate != nullptr ? itemTemplate->Name1 : std::to_string(itemTemplateID);

        // move item
        if (StoreROItem(buyer, itemTemplateID, sale.second, nullptr, 0) == nullptr)
            continue; // Never, with the room checked: nothing moves or is paid for it
        uint32 left = sale.second;
        for (Item* item : GetSellableItems(this, player, *identity, itemTemplateID))
        {
            uint32 count = std::min(left, item->GetCount());
            left -= count;
            player->DestroyItemCount(item, count, true);
            if (left == 0)
                break;
        }
        wanted->Amount -= sale.second;

        // pay up
        buyer->ModifyMoney(-int32(saleZeny));
        player->ModifyMoney(int32(saleZeny));
        store.ZenyLimit -= saleZeny;

        // notify clients (clif_buyingstore_delete_item, clif_buyingstore_update_item)
        handler.PSendSysMessage("You sold {} x {} for {} zeny.", itemName, sale.second, saleZeny);
        ChatHandler(buyer->GetSession()).PSendSysMessage("{} sold you {} x {} for {} zeny.", player->GetName(), itemName, sale.second, saleZeny);
    }

    // check whether or not there is still something to buy
    BuyingStoreOpen(player, buyer);
    bool isDone = std::all_of(store.Items.begin(), store.Items.end(), [](ROWOWBuyingStoreItem const& item) { return item.Amount == 0; });
    if (isDone == true)
        ChatHandler(buyer->GetSession()).SendSysMessage("Your buying store has bought everything it wanted."); // BUYINGSTORE_TRADE_BUYER_NO_ITEMS
    else if (store.ZenyLimit == 0)
        ChatHandler(buyer->GetSession()).SendSysMessage("Your buying store has spent its zeny."); // BUYINGSTORE_TRADE_BUYER_ZENY
    else
        return; // continue buying
    BuyingStoreClose(buyer); // cannot continue buying
    SendAddonMessage(player, "ROWOWBUYLIST\tB\t0\t0\t0\t");
    SendAddonMessage(player, "ROWOWBUYLIST\tE");
}
