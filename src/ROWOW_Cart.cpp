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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/pc.cpp and status.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Player.h"
#include "QueryResult.h"
#include "Tokenize.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// The Merchant's cart (rAthena's NEW_CARTS cart: the SC_PUSH_CART status, its look in val1): up to 100 slots of items and 8000 of weight
// (in tenths), filled from the bags and back.  A cart item isn't an item in the bags, so it's kept as what makes it (its template, count and
// enchantments: the refine, broken and unidentified state, and the cards), in mod_rowow_identity_cart.  The cart window is the RO_Identity
// addon's (ROWOWCART).  Only the player's own thread touches its cart

static_assert(ROWOW_ITEM_ENCHANTMENT_SLOT_COUNT == MAX_ENCHANTMENT_SLOT, "A cart item keeps every enchantment slot of a WoW item");

namespace
{
    // e_additem_result
    constexpr int32 ADDITEM_SUCCESS = 0;
    constexpr int32 ADDITEM_INVALID = 1;
    constexpr int32 ADDITEM_OVERWEIGHT = 2;
    constexpr int32 ADDITEM_OVERAMOUNT = 4;
    constexpr int32 MAX_AMOUNT = 30000; // MAX_AMOUNT: the most of an item in one slot

    // The longest item list one addon message carries (the client drops longer addon messages)
    constexpr size_t CartMessageChunkLength = 200;

    void SendCartMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWCART\t" + message);
        player->SendDirectMessage(&data);
    }

    ROWOWItem const* GetCartROItem(ROWOWCartItem const& cartItem)
    {
        auto itemIter = ROWOW->ItemsByItemTemplateID.find(cartItem.ItemTemplateID);
        return itemIter != ROWOW->ItemsByItemTemplateID.end() ? &itemIter->second : nullptr;
    }

    // itemdb_isstackable2: equipment, pet eggs and pet armor never stack
    bool IsStackable(ROWOWItem const& roItem)
    {
        return roItem.Type != ROWOW_ITEM_TYPE_WEAPON && roItem.Type != ROWOW_ITEM_TYPE_ARMOR && roItem.Type != ROWOW_ITEM_TYPE_PETEGG && roItem.Type != ROWOW_ITEM_TYPE_PETARMOR;
    }
}

bool ROWOWMod::PCIsCartOn(Unit const* unit) const
{
    return unit != nullptr && unit->IsPlayer() == true && GetSCE(unit, ROWOW_SC_PUSH_CART) != nullptr;
}

bool ROWOWMod::PCSetCart(Player* player, int32 type)
{
    // pc_setcart (NEW_CARTS): a cart of a look (1-5), or none (0).  Push Cart is required
    if (type < 0 || type > ROWOW_MAX_CARTS)
        return false; // Never trust the values sent by the client! [Skotlex]
    if (PCCheckSkill(player, ROWOW_SKILL_MC_PUSHCART) <= 0 && type != 0)
        return false; // Push cart is required
    ROWOWIdentity* identity = GetIdentity(player);
    if (type == 0)
    {
        if (PCIsCartOn(player) == false)
            return false;
        if (identity->IsVending == true)
            VendingCloseVending(player);
        StatusChangeEnd(player, ROWOW_SC_PUSH_CART);
    }
    else
    {
        if (PCIsCartOn(player) == false) // first time, so fill cart data
            StatusCalcCartWeight(player, *identity);
        SCStart(player, player, ROWOW_SC_PUSH_CART, 100, type, ROWOW_INFINITE_TICK);
    }
    SendCartToClient(player, *identity, true);
    return true;
}

void ROWOWMod::StatusCalcCartWeight(Player* player, ROWOWIdentity& identity) const
{
    // status_calc_cart_weight (CALCWT_ITEM | CALCWT_MAXBONUS | CALCWT_CARTSTATE): the cart's weight, its maximum and its slots in use
    identity.CartWeightMax = BattleConfig.MaxCartWeight + PCCheckSkill(player, ROWOW_SKILL_GN_REMODELING_CART) * 5000;
    identity.CartWeight = 0;
    identity.CartNum = 0;
    for (ROWOWCartItem const& cartItem : identity.CartItems)
    {
        if (cartItem.ItemTemplateID == 0)
            continue;
        ROWOWItem const* roItem = GetCartROItem(cartItem);
        identity.CartWeight += (roItem != nullptr ? roItem->Weight : 0) * int32(cartItem.Count);
        identity.CartNum++;
    }
}

int32 ROWOWMod::PCCartAddItem(Player* player, ROWOWIdentity& identity, ROWOWCartItem const& item, int32 amount)
{
    // pc_cart_additem: onto a slot of the same item (same enchantments and cards) when it stacks, else into a free slot.  (Item stack limits,
    // rentals and bound items come with their systems)
    ROWOWItem const* roItem = GetCartROItem(item);
    if (item.ItemTemplateID == 0 || amount <= 0 || roItem == nullptr)
        return ADDITEM_INVALID;
    if (roItem->IsNoCart == true)
    {
        // Check item trade restrictions [Skotlex]
        ChatHandler(player->GetSession()).SendSysMessage("This item cannot be stored.");
        return ADDITEM_INVALID;
    }
    int32 w = roItem->Weight * amount;
    if (w + identity.CartWeight > identity.CartWeightMax)
        return ADDITEM_OVERWEIGHT;
    int32 i = ROWOW_MAX_CART;
    if (IsStackable(*roItem) == true)
    {
        for (i = 0; i < ROWOW_MAX_CART; i++)
            if (identity.CartItems[i].ItemTemplateID == item.ItemTemplateID && identity.CartItems[i].Enchantments == item.Enchantments && identity.CartItems[i].CreatorGUID == item.CreatorGUID)
                break;
    }
    if (i < ROWOW_MAX_CART)
    {
        // item already in cart, stack it
        if (amount > MAX_AMOUNT - int32(identity.CartItems[i].Count))
            return ADDITEM_OVERAMOUNT; // no slot
        identity.CartItems[i].Count += uint32(amount);
    }
    else
    {
        // item not stackable or not present, add it
        for (i = 0; i < ROWOW_MAX_CART && identity.CartItems[i].ItemTemplateID != 0; i++)
            ;
        if (i == ROWOW_MAX_CART)
            return ADDITEM_OVERAMOUNT; // no slot
        identity.CartItems[i] = item;
        identity.CartItems[i].Count = uint32(amount);
        identity.CartNum++;
    }
    identity.CartWeight += w;
    identity.AreCartItemsChanged = true;
    return ADDITEM_SUCCESS;
}

void ROWOWMod::PCCartDelItem(Player* /*player*/, ROWOWIdentity& identity, int32 slot, int32 amount)
{
    // pc_cart_delitem
    if (slot < 0 || slot >= ROWOW_MAX_CART)
        return;
    ROWOWCartItem& cartItem = identity.CartItems[slot];
    if (cartItem.ItemTemplateID == 0 || int32(cartItem.Count) < amount)
        return;
    ROWOWItem const* roItem = GetCartROItem(cartItem);
    cartItem.Count -= uint32(amount);
    identity.CartWeight -= (roItem != nullptr ? roItem->Weight : 0) * amount;
    if (cartItem.Count == 0)
    {
        cartItem = ROWOWCartItem();
        identity.CartNum--;
    }
    identity.AreCartItemsChanged = true;
}

void ROWOWMod::PCPutItemToCart(Player* player, Item* item, int32 amount)
{
    // pc_putitemtocart: an RO item from the bags into the cart (a worn one must come off first, and not while a shop is open)
    ROWOWIdentity* identity = GetIdentity(player);
    if (item == nullptr || PCIsCartOn(player) == false || GetROItem(item) == nullptr || amount < 1 || int32(item->GetCount()) < amount || identity->IsVending == true)
        return;
    if (GetIdentityEquipPosition(*identity, item->GetGUID().GetCounter()) != 0)
    {
        ChatHandler(player->GetSession()).SendNotification("Take it off before putting it in the cart.");
        return;
    }
    ROWOWCartItem cartItem;
    cartItem.ItemTemplateID = item->GetEntry();
    for (uint8 slot = 0; slot < MAX_ENCHANTMENT_SLOT; slot++)
        cartItem.Enchantments[slot] = item->GetEnchantmentId(EnchantmentSlot(slot));
    cartItem.CreatorGUID = item->GetGuidValue(ITEM_FIELD_CREATOR).GetCounter();
    int32 flag = PCCartAddItem(player, *identity, cartItem, amount);
    if (flag == ADDITEM_SUCCESS)
    {
        uint32 count = uint32(amount);
        player->DestroyItemCount(item, count, true);
    }
    else if (flag == ADDITEM_OVERAMOUNT)
        ChatHandler(player->GetSession()).SendNotification("The cart is full."); // clif_cart_additem_ack ADDITEM_TO_CART_FAIL_COUNT
    else if (flag == ADDITEM_OVERWEIGHT)
        ChatHandler(player->GetSession()).SendNotification("The cart is too heavy for that."); // ADDITEM_TO_CART_FAIL_WEIGHT
    SendCartToClient(player, *identity, false);
}

bool ROWOWMod::PCGetItemFromCart(Player* player, int32 slot, int32 amount)
{
    // pc_getitemfromcart: a cart item back into the bags (with its enchantments and cards), when they have room
    ROWOWIdentity* identity = GetIdentity(player);
    if (slot < 0 || slot >= ROWOW_MAX_CART)
        return false; // Invalid index check [Skotlex]
    ROWOWCartItem cartItem = identity->CartItems[slot];
    if (cartItem.ItemTemplateID == 0 || amount < 1 || int32(cartItem.Count) < amount || identity->IsVending == true)
        return false;
    if (CanStoreROItem(player, cartItem.ItemTemplateID, uint32(amount), cartItem.CreatorGUID) == false)
    {
        ChatHandler(player->GetSession()).SendNotification("Your bags are full.");
        return true;
    }
    if (StoreROItem(player, cartItem.ItemTemplateID, uint32(amount), &cartItem.Enchantments, cartItem.CreatorGUID) == nullptr)
        return true;
    PCCartDelItem(player, *identity, slot, amount);
    SendCartToClient(player, *identity, false);
    return true;
}

void ROWOWMod::SendCartToClient(Player* player, ROWOWIdentity& identity, bool isForced)
{
    // clif_cartlist and clif_updatestatus(SP_CARTINFO): the cart window's look, weight and items, in parts as the skill tree is sent
    // ("B\t<cart look 0-5>,<weight>,<max weight>,<slots used>,<Change Cart allowed>", "S\t<slot>:<template>:<count>:<enchantment>:..|...", "E")
    if (identity.IsInForm == false)
        return;
    ROWOWStatusChangeEntry const* cart = GetSCE(player, ROWOW_SC_PUSH_CART);
    std::vector<std::string> itemTexts;
    for (int32 slot = 0; slot < ROWOW_MAX_CART; slot++)
    {
        ROWOWCartItem const& cartItem = identity.CartItems[slot];
        if (cartItem.ItemTemplateID == 0)
            continue;
        std::string itemText = fmt::format("{}:{}:{}", slot, cartItem.ItemTemplateID, cartItem.Count);
        for (uint32 enchantmentID : cartItem.Enchantments)
            itemText += fmt::format(":{}", enchantmentID);
        itemTexts.push_back(itemText);
    }
    std::string message = fmt::format("{},{},{},{},{}", cart != nullptr ? cart->Val1 : 0, identity.CartWeight, identity.CartWeightMax, identity.CartNum,
        PCCheckSkill(player, ROWOW_SKILL_MC_CHANGECART) > 0 || PCCheckSkill(player, ROWOW_SKILL_MC_CARTDECORATE) > 0 ? 1 : 0);
    for (std::string const& itemText : itemTexts)
        message += "\t" + itemText;
    if (isForced == false && message == identity.SentCartMessage)
        return;
    identity.SentCartMessage = message;
    SendCartMessage(player, "B\t" + message.substr(0, message.find('\t')));
    std::string chunk;
    for (std::string const& itemText : itemTexts)
    {
        if (chunk.empty() == false && chunk.size() + itemText.size() + 1 > CartMessageChunkLength)
        {
            SendCartMessage(player, "S\t" + chunk);
            chunk.clear();
        }
        chunk += (chunk.empty() == true ? "" : "|") + itemText;
    }
    if (chunk.empty() == false)
        SendCartMessage(player, "S\t" + chunk);
    SendCartMessage(player, "E");
}

void ROWOWMod::LoadIdentityCart(Player* player, ROWOWIdentity& identity) const
{
    if (QueryResult cartResult = CharacterDatabase.Query("SELECT `slot`, `item_template`, `count`, `enchantments`, `creator` FROM `mod_rowow_identity_cart` WHERE `guid` = {}",
        player->GetGUID().GetCounter()))
    {
        do
        {
            Field* fields = cartResult->Fetch();
            uint16 slot = fields[0].Get<uint16>();
            if (slot >= ROWOW_MAX_CART)
                continue;
            ROWOWCartItem& cartItem = identity.CartItems[slot];
            cartItem.ItemTemplateID = fields[1].Get<uint32>();
            cartItem.Count = fields[2].Get<uint32>();
            std::string enchantmentIDs = fields[3].Get<std::string>();
            std::vector<std::string_view> enchantmentTokens = Acore::Tokenize(enchantmentIDs, ' ', false);
            for (size_t enchantmentSlot = 0; enchantmentSlot < enchantmentTokens.size() && enchantmentSlot < MAX_ENCHANTMENT_SLOT; enchantmentSlot++)
                cartItem.Enchantments[enchantmentSlot] = uint32(strtoul(std::string(enchantmentTokens[enchantmentSlot]).c_str(), nullptr, 10));
            cartItem.CreatorGUID = fields[4].Get<uint32>();
        } while (cartResult->NextRow());
    }
    StatusCalcCartWeight(player, identity);
}

void ROWOWMod::SaveIdentityCart(Player* player, ROWOWIdentity& identity, CharacterDatabaseTransaction trans) const
{
    if (identity.AreCartItemsChanged == false)
        return;
    uint32 guid = player->GetGUID().GetCounter();
    trans->Append("DELETE FROM `mod_rowow_identity_cart` WHERE `guid` = {}", guid);
    for (int32 slot = 0; slot < ROWOW_MAX_CART; slot++)
    {
        ROWOWCartItem const& cartItem = identity.CartItems[slot];
        if (cartItem.ItemTemplateID == 0)
            continue;
        std::string enchantmentIDs;
        for (uint32 enchantmentID : cartItem.Enchantments)
            enchantmentIDs += fmt::format("{}{}", enchantmentIDs.empty() == true ? "" : " ", enchantmentID);
        trans->Append("INSERT INTO `mod_rowow_identity_cart` (`guid`, `slot`, `item_template`, `count`, `enchantments`, `creator`) VALUES ({}, {}, {}, {}, '{}', {})", guid, slot,
            cartItem.ItemTemplateID, cartItem.Count, enchantmentIDs, cartItem.CreatorGUID);
    }
    identity.AreCartItemsChanged = false;
}
