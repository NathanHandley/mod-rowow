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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/vending.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "CellImpl.h"
#include "Chat.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// Vending: a Merchant sits down with a shop of its cart's items (vending_openvending), which the players around list (vending_vendinglistreq,
// the RO_Identity addon's shop window on targeting the vendor) and buy from (vending_purchasereq); standing up or moving closes it
// (vending_closevending), and a vendor can't move.  The shop's sign is the rooms' (SetShopSign): each player in sight is told of it from its
// own update, and a click on it lists the shop.  The vendor and the buyer are on one map, and what they do comes as chat commands, which run
// in the world update with the maps idle, so no lock is needed here.  (Autotrade, the vending search and the shops' database rows are left
// out)

namespace
{
    constexpr int64 MAX_ZENY = 1000000000; // MAX_ZENY
    constexpr size_t VendMessageChunkLength = 200;

    void SendAddonMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, message);
        player->SendDirectMessage(&data);
    }

    // vending_calc_tax: vending_tax off a sale of vending_tax_min and up
    double VendingCalcTax(double zeny)
    {
        if (ROWOW->BattleConfig.VendingTax != 0 && zeny >= ROWOW->BattleConfig.VendingTaxMin)
            zeny -= zeny * (ROWOW->BattleConfig.VendingTax / 10000.);
        return zeny;
    }
}

void ROWOWMod::VendingOpenVendingReq(Player* player, uint8 skillLevel)
{
    // The Vending skill's part (prevend) and clif_openvendingreq: the shop setup lists the cart's items and takes 2 + the level of them
    ROWOWIdentity* identity = GetIdentity(player);
    identity->MenuSkillID = ROWOW_SKILL_MC_VENDING;
    identity->VendSkillLevel = skillLevel;
    SendCartToClient(player, *identity, true);
    SendAddonMessage(player, fmt::format("ROWOWVENDREQ\t{}", 2 + skillLevel));
}

bool ROWOWMod::VendingOpenVending(Player* player, std::string const& title, std::vector<ROWOWVendingItem> const& items)
{
    // vending_openvending: a shop of the cart items chosen, at a price each (at most vending_max_value).  It fails without the skill or the
    // cart, or with an item that can't be sold (unidentified, broken, untradeable)
    ROWOWIdentity* identity = GetIdentity(player);
    if (player->IsAlive() == false || identity->MenuSkillID != ROWOW_SKILL_MC_VENDING || identity->IsVending == true || PCCantAct(player) == true)
        return false; // can't open vendings lying dead || didn't use via the skill (wpe/hack) || can't have 2 shops at once
    identity->MenuSkillID = 0;
    int32 vendingSkillLevel = PCCheckSkill(player, ROWOW_SKILL_MC_VENDING);
    // skill level and cart check
    if (vendingSkillLevel == 0 || PCIsCartOn(player) == false)
    {
        ClifSkillFail(player, ROWOW_SKILL_MC_VENDING, ROWOW_USESKILL_FAIL_LEVEL);
        return false;
    }
    // check number of items in shop
    int32 count = int32(items.size());
    if (count < 1 || count > ROWOW_MAX_VENDING || count > 2 + vendingSkillLevel)
    {
        ClifSkillFail(player, ROWOW_SKILL_MC_VENDING, ROWOW_USESKILL_FAIL_LEVEL);
        return false;
    }
    // filter out invalid items
    std::vector<ROWOWVendingItem> vending;
    int64 total = 0;
    for (ROWOWVendingItem const& item : items)
    {
        if (item.CartSlot >= ROWOW_MAX_CART) // invalid position
            continue;
        ROWOWCartItem const& cartItem = identity->CartItems[item.CartSlot];
        auto roItemIter = ItemsByItemTemplateID.find(cartItem.ItemTemplateID);
        uint32 stateEnchantment = cartItem.Enchantments[PERM_ENCHANTMENT_SLOT];
        uint32 state = (stateEnchantment > ROWOW_ITEM_STATE_ENCHANTMENT_START && stateEnchantment < ROWOW_ITEM_STATE_ENCHANTMENT_START + ROWOW_ITEM_STATE_COUNT) ?
            stateEnchantment - ROWOW_ITEM_STATE_ENCHANTMENT_START : 0;
        if (cartItem.ItemTemplateID == 0 || item.Amount == 0 || cartItem.Count < item.Amount // invalid item or insufficient quantity
            || (state & ROWOW_ITEM_STATE_UNIDENTIFIED) != 0 // unidentified item
            || (state & ROWOW_ITEM_STATE_BROKEN) != 0 // broken item
            || roItemIter == ItemsByItemTemplateID.end() || roItemIter->second.IsNoTrade == true) // untradeable item
            continue;
        ROWOWVendingItem vendingItem = item;
        vendingItem.Value = std::min<uint32>(item.Value, uint32(BattleConfig.VendingMaxValue));
        total += int64(vendingItem.Value) * vendingItem.Amount;
        vending.push_back(vendingItem);
    }
    // check if the total value of the items plus the current zeny is over the limit
    if (BattleConfig.VendingOverMax == 0 && int64(player->GetMoney()) + total > MAX_ZENY)
    {
        ClifSkillFail(player, ROWOW_SKILL_MC_VENDING, ROWOW_USESKILL_FAIL_LEVEL);
        return false;
    }
    if (vending.size() != items.size())
    {
        ChatHandler(player->GetSession()).SendSysMessage("Some of your items cannot be vended and were removed from the shop.");
        ClifSkillFail(player, ROWOW_SKILL_MC_VENDING, ROWOW_USESKILL_FAIL_LEVEL); // custom reply packet
        return false;
    }
    identity->IsVending = true;
    identity->VendingItems = vending;
    identity->VendingTitle = title.substr(0, 79); // MESSAGE_SIZE
    // A vendor sits by its shop (the client's vending state)
    player->SetStandState(UNIT_STAND_STATE_SIT);
    SendAddonMessage(player, "ROWOWVENDOPEN\t" + identity->VendingTitle);
    player->StopMoving();
    player->AttackStop();
    SetShopSign(player, identity->VendingTitle, false);
    PCUpdateMoveLock(player);
    return true;
}

void ROWOWMod::VendingCloseVending(Player* player)
{
    // vending_closevending
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->IsVending == false)
        return;
    identity->IsVending = false;
    identity->VendingItems.clear();
    SendAddonMessage(player, "ROWOWVENDOPEN\t");
    RemoveShopSign(player);
    PCUpdateMoveLock(player);
}

void ROWOWMod::VendingVendingListReq(Player* player, Player* vendor)
{
    // vending_vendinglistreq: the shop's items for the buyer's shop window ("B\t<vendor>\t<title>", "S\t<index>:<template>:<amount>:<price>:
    // <enchantment>:..|...", "E"), or an empty list when it's no shop
    ROWOWIdentity* vendorIdentity = vendor != nullptr ? vendor->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
    if (vendorIdentity == nullptr || vendorIdentity->IsVending == false || vendor == player || vendor->GetMap() != player->GetMap())
    {
        SendAddonMessage(player, fmt::format("ROWOWVENDLIST\tB\t{}\t", vendor != nullptr ? vendor->GetGUID().GetCounter() : 0));
        SendAddonMessage(player, "ROWOWVENDLIST\tE");
        return;
    }
    if (DistanceBL(player, vendor) > BattleConfig.AreaSize)
    {
        SendAddonMessage(player, fmt::format("ROWOWVENDLIST\tB\t{}\t", vendor->GetGUID().GetCounter()));
        SendAddonMessage(player, "ROWOWVENDLIST\tE");
        return;
    }
    SendAddonMessage(player, fmt::format("ROWOWVENDLIST\tB\t{}\t{}", vendor->GetGUID().GetCounter(), vendorIdentity->VendingTitle));
    std::string chunk;
    for (size_t index = 0; index < vendorIdentity->VendingItems.size(); index++)
    {
        ROWOWVendingItem const& vendingItem = vendorIdentity->VendingItems[index];
        ROWOWCartItem const& cartItem = vendorIdentity->CartItems[vendingItem.CartSlot];
        std::string itemText = fmt::format("{}:{}:{}:{}", index, cartItem.ItemTemplateID, vendingItem.Amount, vendingItem.Value);
        for (uint32 enchantmentID : cartItem.Enchantments)
            itemText += fmt::format(":{}", enchantmentID);
        if (chunk.empty() == false && chunk.size() + itemText.size() + 1 > VendMessageChunkLength)
        {
            SendAddonMessage(player, "ROWOWVENDLIST\tS\t" + chunk);
            chunk.clear();
        }
        chunk += (chunk.empty() == true ? "" : "|") + itemText;
    }
    if (chunk.empty() == false)
        SendAddonMessage(player, "ROWOWVENDLIST\tS\t" + chunk);
    SendAddonMessage(player, "ROWOWVENDLIST\tE");
}

void ROWOWMod::VendingPurchaseReq(Player* player, Player* vendor, std::vector<std::pair<uint16, uint32>> const& purchases)
{
    // vending_purchasereq: the buyer pays (the vendor gets it less the tax), and each item goes from the vendor's cart into the buyer's bags
    ROWOWIdentity* vendorIdentity = vendor != nullptr ? vendor->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
    if (vendorIdentity == nullptr || vendorIdentity->IsVending == false || vendor == player)
        return; // invalid shop
    if (vendor->GetMap() != player->GetMap() || DistanceBL(player, vendor) > BattleConfig.AreaSize)
        return; // shop too far away
    int32 count = int32(purchases.size());
    if (count < 1 || count > ROWOW_MAX_VENDING || count > int32(vendorIdentity->VendingItems.size()))
        return; // invalid amount of purchased items
    std::vector<ROWOWVendingItem> vending = vendorIdentity->VendingItems; // copy vending list
    double z = 0.; // zeny counter
    for (std::pair<uint16, uint32> const& purchase : purchases)
    {
        uint32 amount = purchase.second;
        if (amount == 0 || purchase.first >= vending.size())
            return; // picked non-existing item
        ROWOWVendingItem& vendingItem = vending[purchase.first];
        ROWOWCartItem const& cartItem = vendorIdentity->CartItems[vendingItem.CartSlot];
        z += double(vendingItem.Value) * double(amount);
        if (z > double(player->GetMoney()) || z < 0. || z > double(MAX_ZENY))
        {
            ChatHandler(player->GetSession()).SendNotification("You don't have enough zeny."); // clif_buyvending PURCHASEMC_NO_ZENY
            return;
        }
        if (z + double(vendor->GetMoney()) > double(MAX_ZENY) && BattleConfig.VendingOverMax == 0)
        {
            ChatHandler(player->GetSession()).SendNotification("The item is out of stock."); // too much zeny = overflow
            return;
        }
        if (vendingItem.Amount > cartItem.Count)
            vendingItem.Amount = cartItem.Count;
        if (vendingItem.Amount < amount)
        {
            ChatHandler(player->GetSession()).SendNotification("The item is out of stock."); // PURCHASEMC_OUT_OF_STOCK
            return;
        }
        vendingItem.Amount -= amount;
        if (CanStoreROItem(player, cartItem.ItemTemplateID, amount, cartItem.CreatorGUID) == false)
        {
            ChatHandler(player->GetSession()).SendNotification("Your bags are full."); // Buyer has no space in his inventory
            return;
        }
    }
    player->ModifyMoney(-int32(z));
    z = VendingCalcTax(z);
    vendor->ModifyMoney(int32(std::min<double>(z, double(MAX_ZENY) - double(vendor->GetMoney()))));

    for (std::pair<uint16, uint32> const& purchase : purchases)
    {
        uint32 amount = purchase.second;
        ROWOWVendingItem& vendingItem = vendorIdentity->VendingItems[purchase.first];
        ROWOWCartItem cartItem = vendorIdentity->CartItems[vendingItem.CartSlot];
        StoreROItem(player, cartItem.ItemTemplateID, amount, &cartItem.Enchantments, cartItem.CreatorGUID);
        vendingItem.Amount -= amount;
        double itemZeny = VendingCalcTax(double(vendingItem.Value) * double(amount));
        PCCartDelItem(vendor, *vendorIdentity, vendingItem.CartSlot, int32(amount));
        // clif_vendingreport
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(cartItem.ItemTemplateID);
        ChatHandler(vendor->GetSession()).PSendSysMessage("{} bought {} x {} for {} zeny.", player->GetName(), itemTemplate != nullptr ? itemTemplate->Name1 : "an item",
            amount, int64(itemZeny));
    }

    // The sold-out items leave the shop
    vendorIdentity->VendingItems.erase(std::remove_if(vendorIdentity->VendingItems.begin(), vendorIdentity->VendingItems.end(),
        [](ROWOWVendingItem const& vendingItem) { return vendingItem.Amount == 0; }), vendorIdentity->VendingItems.end());
    SendCartToClient(vendor, *vendorIdentity, false);
    VendingVendingListReq(player, vendor);
    if (vendorIdentity->VendingItems.empty() == true)
        VendingCloseVending(vendor); // Close Vending (this was automatically done by the client) [Skotlex]
}
