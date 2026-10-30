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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/pc.cpp and skill.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Creature.h"
#include "Item.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// What skills do to items and players outside the skill engine: stealing (pc_steal_item), making items (skill_produce_mix for Holy Water,
// skill_arrow_create), breaking equipment (skill_break_equip), the invincibility after arriving (pc_setinvincibletimer), riding a Peco
// (pc_setriding), a Crusader's Devotion links (sd->devotion and the devoted's SC_DEVOTION) and the Merchant's prices (pc_modifybuyvalue,
// pc_modifysellvalue).  All of it runs in the player's own map thread, a Devotion link's two players being on one map whenever either
// reads the other

namespace
{
    // EQP_WEAPON, EQP_ARMOR, EQP_SHIELD, EQP_HELM, EQP_ACC, and the strip statuses (on monsters) and protections (Chemical Protection) of each
    uint32 const BreakWhereList[5] = { ROWOW_EQUIP_POSITION_HAND_R, ROWOW_EQUIP_POSITION_ARMOR, ROWOW_EQUIP_POSITION_HAND_L,
        ROWOW_EQUIP_POSITION_HEAD_LOW | ROWOW_EQUIP_POSITION_HEAD_MID | ROWOW_EQUIP_POSITION_HEAD_TOP, ROWOW_EQUIP_POSITION_ACC };
    int32 const BreakStripStatuses[5] = { ROWOW_SC_STRIPWEAPON, ROWOW_SC_STRIPARMOR, ROWOW_SC_STRIPSHIELD, ROWOW_SC_STRIPHELM, ROWOW_SC_NONE };
    int32 const BreakProtectStatuses[5] = { ROWOW_SC_CP_WEAPON, ROWOW_SC_CP_ARMOR, ROWOW_SC_CP_SHIELD, ROWOW_SC_CP_HELM, ROWOW_SC_NONE };

    int32 GetItemIDByAegisName(std::string const& aegisName)
    {
        std::string lowerName = aegisName;
        for (char& curChar : lowerName)
            curChar = char(tolower((unsigned char)curChar));
        auto itemIDIter = ROWOW->ItemIDsByAegisName.find(lowerName);
        return itemIDIter != ROWOW->ItemIDsByAegisName.end() ? int32(itemIDIter->second) : 0;
    }

    // The item in the bags (identified, not worn) of an item_db Id, or null (pc_search_inventory)
    Item* FindBagItem(Player* player, ROWOWIdentity const& identity, uint32 itemID)
    {
        auto itemTemplateIDIter = ROWOW->ItemTemplateIDsByItemID.find(itemID);
        if (itemTemplateIDIter == ROWOW->ItemTemplateIDsByItemID.end())
            return nullptr;
        for (uint8 bag = 0; bag <= 4; bag++)
        {
            for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
            {
                Item* item = ROWOWMod::GetItemByClientPosition(player, bag, slot);
                if (item != nullptr && item->GetEntry() == itemTemplateIDIter->second && ROWOW->GetIdentityEquipPosition(identity, item->GetGUID().GetCounter()) == 0)
                    return item;
            }
        }
        return nullptr;
    }

    // A deferred bonus a worn item has (bUnbreakableWeapon and the like, which take no value)
    bool HasDeferredBonus(ROWOWStatus const* status, char const* type)
    {
        if (status == nullptr)
            return false;
        return std::any_of(status->Bonus.Deferred.begin(), status->Bonus.Deferred.end(), [type](ROWOWDeferredBonus const& bonus) { return bonus.Type == type; });
    }
}

bool ROWOWMod::PCStealItem(Player* player, Unit* bl, uint16 skillLevel)
{
    // pc_steal_item: one try at the monster's drops, each at its drop rate scaled by the steal's success rate ((DEX - the monster's DEX) / 2
    // + 6 a level + 4 + bAddStealRate); a monster can be stolen from once.  (Slaves, the treasure race group and nomobloot maps come with them)
    Creature* md = bl != nullptr ? bl->ToCreature() : nullptr;
    uint32 mobID = md != nullptr ? GetROMobID(md) : 0;
    if (md == nullptr || mobID == 0)
        return false;
    ROWOWStealState* stealState = md->CustomData.GetDefault<ROWOWStealState>("ROWOWStealState");
    ROWOWStatusChange const* mdsc = GetStatusChange(static_cast<Unit const*>(md));
    if (stealState->StealFlag == UCHAR_MAX || (mdsc != nullptr && mdsc->Opt1 != 0 && mdsc->Opt1 != ROWOW_OPT1_BURNING)) // already stolen from / status change check
        return false;
    ROWOWStatus const* sdStatus = GetROStatus(player);
    ROWOWStatus const* mdStatus = GetROStatus(md);
    if ((mdStatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0 ||
        (BattleConfig.SkillStealMaxTries != 0 && stealState->StealFlag++ >= BattleConfig.SkillStealMaxTries)) // Reached limit of steal attempts. [Lupus]
    {
        // Can't steal from
        stealState->StealFlag = UCHAR_MAX;
        return false;
    }
    // base skill success chance (percentual)
    double rate = (sdStatus->Stats[ROWOW_STAT_DEX] - mdStatus->Stats[ROWOW_STAT_DEX]) / 2 + skillLevel * 6 + 4;
    rate += GetDeferredBonus(player, "bAddStealRate");
    if (rate < 1)
        return false;

    // Try dropping one item.  Droprate is affected by the skill success rate.
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    int64 rowIndex = GetMobRowIndex(mobID);
    if (mobTable == nullptr || rowIndex < 0)
        return false;
    uint32 itemID = 0;
    for (auto const& drop : ROWOWDataTable::ParseMapList(mobTable->GetCell(size_t(rowIndex), mobTable->GetColumnIndex("Drops"))))
    {
        int32 dropItemID = 0;
        double dropRate = 0;
        bool isStealProtected = false;
        for (auto const& field : drop)
        {
            if (field.first == "Item")
                dropItemID = GetItemIDByAegisName(field.second);
            else if (field.first == "Rate")
                dropRate = atof(field.second.c_str());
            else if (field.first == "StealProtected")
                isStealProtected = field.second == "true";
        }
        if (isStealProtected == true || dropItemID == 0 || ItemTemplateIDsByItemID.find(uint32(dropItemID)) == ItemTemplateIDsByItemID.end())
            continue;
        if (double(urand(0, 9999)) < dropRate * rate / 100.)
        {
            itemID = uint32(dropItemID);
            break;
        }
    }
    if (itemID == 0)
        return false;
    stealState->StealFlag = UCHAR_MAX; // you can't steal from this mob any more
    // itemdb_isidentified: equipment is stolen unidentified
    auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
    auto roItemIter = ItemsByItemTemplateID.find(itemTemplateIDIter->second);
    bool isIdentified = roItemIter == ItemsByItemTemplateID.end() || (roItemIter->second.IsWeapon == false && roItemIter->second.IsArmor == false &&
        roItemIter->second.Type != ROWOW_ITEM_TYPE_PETARMOR);
    return GiveROItem(player, itemID, 1, isIdentified);
}

bool ROWOWMod::SkillProduceMix(Player* player, uint16 skillID, uint32 nameID, int32 qty)
{
    // skill_produce_mix, the parts the first jobs use (Holy Water, made for sure): the produce_db recipe of the item for the skill, its
    // materials taken from the bags, and the item made.  (Forging, brewing and their success rates come with the Blacksmith and Alchemist)
    ROWOWDataTable const* produceTable = GetDataTable("mod_rowow_db_produce");
    if (produceTable == nullptr)
        return false;
    int32 productColumnIndex = produceTable->GetColumnIndex("ProduceItemID");
    int32 skillColumnIndex = produceTable->GetColumnIndex("RequireSkill");
    int32 skillLevelColumnIndex = produceTable->GetColumnIndex("RequireSkillLevel");
    int32 materialsColumnIndex = produceTable->GetColumnIndex("Materials");
    if (qty < 1)
        qty = 1;
    for (size_t rowIndex = 0; rowIndex < produceTable->Rows.size(); rowIndex++)
    {
        // skill_can_produce_mix: the recipe, the skill at its level, and every material in the bags
        if (uint32(produceTable->GetInt(rowIndex, productColumnIndex, 0)) != nameID || produceTable->GetInt(rowIndex, skillColumnIndex, 0) != skillID)
            continue;
        if (PCCheckSkill(player, skillID) < produceTable->GetInt(rowIndex, skillLevelColumnIndex, 0))
            return false;
        std::vector<std::pair<uint32, uint32>> materials;
        for (auto const& material : ROWOWDataTable::ParseMapList(produceTable->GetCell(rowIndex, materialsColumnIndex)))
        {
            uint32 materialID = 0, amount = 0;
            for (auto const& field : material)
            {
                if (field.first == "ItemID")
                    materialID = uint32(strtoul(field.second.c_str(), nullptr, 10));
                else if (field.first == "Amount")
                    amount = uint32(strtoul(field.second.c_str(), nullptr, 10));
            }
            if (materialID == 0)
                continue;
            auto materialTemplateIDIter = ItemTemplateIDsByItemID.find(materialID);
            if (materialTemplateIDIter == ItemTemplateIDsByItemID.end() || player->GetItemCount(materialTemplateIDIter->second) < amount * uint32(qty))
                return false;
            materials.emplace_back(materialTemplateIDIter->second, amount * uint32(qty));
        }
        for (auto const& material : materials)
            player->DestroyItemCount(material.first, material.second, true);
        // AL_HOLYWATER: make_per = 100000, 100% success
        GiveROItem(player, nameID, uint32(qty), true);
        return true;
    }
    return false;
}

void ROWOWMod::ClifArrowCreateList(Player* player)
{
    // clif_arrow_create_list: the items in the bags (identified, not worn) that Arrow Crafting makes arrows of, for the RO_Identity addon to
    // list (ROWOWARROW "<item template>,..."); the one picked comes back as .ro makearrow (skill_arrow_create)
    ROWOWDataTable const* arrowTable = GetDataTable("mod_rowow_db_create_arrow");
    ROWOWIdentity* identity = GetIdentity(player);
    if (arrowTable == nullptr)
        return;
    int32 sourceColumnIndex = arrowTable->GetColumnIndex("Source");
    std::string itemTemplateIDs;
    int32 count = 0;
    for (size_t rowIndex = 0; rowIndex < arrowTable->Rows.size(); rowIndex++)
    {
        int32 itemID = GetItemIDByAegisName(arrowTable->GetCell(rowIndex, sourceColumnIndex));
        Item* item = itemID != 0 ? FindBagItem(player, *identity, uint32(itemID)) : nullptr;
        if (item == nullptr || IsItemIdentified(item) == false)
            continue;
        itemTemplateIDs += fmt::format("{}{}", itemTemplateIDs.empty() == true ? "" : ",", item->GetEntry());
        count++;
    }
    if (count > 0)
    {
        identity->MenuSkillID = ROWOW_SKILL_AC_MAKINGARROW;
        identity->MenuSkillValue = count;
    }
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWARROW\t" + itemTemplateIDs);
    player->SendDirectMessage(&data);
}

bool ROWOWMod::SkillArrowCreate(Player* player, uint32 nameID)
{
    // skill_arrow_create: one of the item picked becomes its arrows (create_arrow_db's Make list)
    ROWOWDataTable const* arrowTable = GetDataTable("mod_rowow_db_create_arrow");
    ROWOWIdentity* identity = GetIdentity(player);
    if (arrowTable == nullptr || nameID == 0 || identity->MenuSkillID != ROWOW_SKILL_AC_MAKINGARROW)
        return false;
    identity->MenuSkillID = 0;
    identity->MenuSkillValue = 0;
    int32 sourceColumnIndex = arrowTable->GetColumnIndex("Source");
    int32 makeColumnIndex = arrowTable->GetColumnIndex("Make");
    for (size_t rowIndex = 0; rowIndex < arrowTable->Rows.size(); rowIndex++)
    {
        if (uint32(GetItemIDByAegisName(arrowTable->GetCell(rowIndex, sourceColumnIndex))) != nameID)
            continue;
        Item* item = FindBagItem(player, *identity, nameID);
        if (item == nullptr)
            return false;
        uint32 count = 1;
        player->DestroyItemCount(item, count, true);
        for (auto const& made : ROWOWDataTable::ParseMapList(arrowTable->GetCell(rowIndex, makeColumnIndex)))
        {
            int32 madeItemID = 0;
            uint32 amount = 0;
            for (auto const& field : made)
            {
                if (field.first == "Item")
                    madeItemID = GetItemIDByAegisName(field.second);
                else if (field.first == "Amount")
                    amount = uint32(strtoul(field.second.c_str(), nullptr, 10));
            }
            if (madeItemID != 0 && amount != 0)
                GiveROItem(player, uint32(madeItemID), amount, true);
        }
        return true;
    }
    return false;
}

int32 ROWOWMod::SkillBreakEquip(Unit* src, Unit* bl, uint32 where, int32 rate, int32 flag)
{
    // skill_break_equip: breaks the equipment in the positions asked for, each at rate in 10000 (less the target's unbreakable bonuses, and
    // equip_skill_break_rate or equip_self_break_rate); a monster takes the strip status instead (break_mob_equip).  Chemical Protection
    // keeps its position.  Axes, maces, rods and books never break.  Returns the positions broken
    ROWOWStatusChange const* srcsc = GetStatusChange(static_cast<Unit const*>(src));
    // Grant player skills/items the ability to "break" non-player equipment.  WS_MELTDOWN is exempt from this check.
    if (BattleConfig.BreakMobEquip == 0 && bl->IsPlayer() == false && (srcsc == nullptr || srcsc->HasSCE(ROWOW_SC_MELTDOWN) == false))
        return 0;
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(bl));
    if (sc != nullptr && sc->IsEmpty() == true)
        sc = nullptr;
    Player* sd = bl->ToPlayer();
    if (sd != nullptr)
    {
        ROWOWStatus const* status = GetROStatus(sd);
        if (HasDeferredBonus(status, "bUnbreakableWeapon") == true)
            where &= ~ROWOW_EQUIP_POSITION_HAND_R;
        if (HasDeferredBonus(status, "bUnbreakableArmor") == true)
            where &= ~ROWOW_EQUIP_POSITION_ARMOR;
        if (HasDeferredBonus(status, "bUnbreakableHelm") == true)
            where &= ~(ROWOW_EQUIP_POSITION_HEAD_LOW | ROWOW_EQUIP_POSITION_HEAD_MID | ROWOW_EQUIP_POSITION_HEAD_TOP);
        if (HasDeferredBonus(status, "bUnbreakableShield") == true)
            where &= ~ROWOW_EQUIP_POSITION_HAND_L;
        if (HasDeferredBonus(status, "bUnbreakableGarment") == true)
            where &= ~ROWOW_EQUIP_POSITION_GARMENT;
        if (HasDeferredBonus(status, "bUnbreakableShoes") == true)
            where &= ~ROWOW_EQUIP_POSITION_SHOES;
        if (int32 unbreakable = GetDeferredBonus(sd, "bUnbreakable"))
            rate -= rate * unbreakable / 100;
        if ((where & ROWOW_EQUIP_POSITION_HAND_R) != 0)
        {
            switch (status->WeaponType)
            {
                case ROWOW_WEAPON_TYPE_FIST: // Bare fists should not break :P
                case ROWOW_WEAPON_TYPE_1HAXE:
                case ROWOW_WEAPON_TYPE_2HAXE:
                case ROWOW_WEAPON_TYPE_MACE: // Axes and Maces can't be broken [DracoRPG]
                case ROWOW_WEAPON_TYPE_2HMACE:
                case ROWOW_WEAPON_TYPE_STAFF:
                case ROWOW_WEAPON_TYPE_2HSTAFF:
                case ROWOW_WEAPON_TYPE_BOOK: // Rods and Books can't be broken [Skotlex]
                case ROWOW_WEAPON_TYPE_DOUBLE_AA: // Axe usage during dual wield should also prevent breaking [Neutral]
                case ROWOW_WEAPON_TYPE_DOUBLE_DA:
                case ROWOW_WEAPON_TYPE_DOUBLE_SA:
                    where &= ~ROWOW_EQUIP_POSITION_HAND_R;
                    break;
            }
        }
    }
    if ((flag & ROWOW_BCT_ENEMY) != 0)
    {
        if (BattleConfig.EquipSkillBreakRate != 100)
            rate = rate * BattleConfig.EquipSkillBreakRate / 100;
    }
    else if ((flag & (ROWOW_BCT_PARTY | ROWOW_BCT_SELF)) != 0)
    {
        if (BattleConfig.EquipSelfBreakRate != 100)
            rate = rate * BattleConfig.EquipSelfBreakRate / 100;
    }
    for (int32 i = 0; i < 5; i++)
    {
        if ((where & BreakWhereList[i]) == 0)
            continue;
        if (sc != nullptr && BreakProtectStatuses[i] != ROWOW_SC_NONE && sc->HasSCE(BreakProtectStatuses[i]) == true)
            where &= ~BreakWhereList[i];
        else if (int32(urand(0, 9999)) >= rate)
            where &= ~BreakWhereList[i];
        else if (sd == nullptr && BreakStripStatuses[i] != ROWOW_SC_NONE) // Cause Strip effect.
            SCStart(src, bl, BreakStripStatuses[i], 100, 0, SkillGetTime(StatusDBGetSkill(BreakStripStatuses[i]), 1));
    }
    if (where == 0) // Nothing to break.
        return 0;
    if (sd != nullptr)
    {
        // Each worn piece in a broken position (weapon or shield by what's held) is broken, and comes off
        ROWOWIdentity* identity = GetIdentity(sd);
        std::vector<std::pair<uint32, ObjectGuid::LowType>> wornItems(identity->ItemGUIDsByEquipPosition.begin(), identity->ItemGUIDsByEquipPosition.end());
        for (auto const& wornItem : wornItems)
        {
            Item* item = sd->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(wornItem.second));
            ROWOWItem const* roItem = GetROItem(item);
            if (roItem == nullptr || IsItemBroken(item) == true || (wornItem.first & ROWOW_EQUIP_POSITION_COSTUME) != 0 || wornItem.first == ROWOW_EQUIP_POSITION_AMMO)
                continue;
            bool isBroken = false;
            if ((wornItem.first & (ROWOW_EQUIP_POSITION_HAND_R | ROWOW_EQUIP_POSITION_HAND_L)) != 0)
                isBroken = ((where & ROWOW_EQUIP_POSITION_HAND_R) != 0 && roItem->IsWeapon == true) || ((where & ROWOW_EQUIP_POSITION_HAND_L) != 0 && roItem->IsArmor == true);
            else
                isBroken = (wornItem.first & where) != 0;
            if (isBroken == false)
                continue;
            SetItemState(sd, item, GetItemRefine(item), true, IsItemIdentified(item));
            PCUnequipItem(sd, wornItem.first, 3);
        }
        ChatHandler(sd->GetSession()).SendNotification("Your equipment broke.");
    }
    return int32(where); // Return list of pieces broken.
}

void ROWOWMod::PCSetInvincibleTimer(Player* player)
{
    // pc_setinvincibletimer: no monster or skill targets the player for player_invincible_time after it arrives on a map or revives (WoW's
    // immunity to NPCs keeps the monsters' AI off it meanwhile)
    if (BattleConfig.PCInvincibleTime <= 0 || IsInROForm(player) == false)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    identity->InvincibleEndTime = GetTick() + uint64(BattleConfig.PCInvincibleTime);
    identity->InvinciblePositionX = player->GetPositionX();
    identity->InvinciblePositionY = player->GetPositionY();
    if (player->IsImmuneToNPC() == false)
    {
        player->SetImmuneToNPC(true);
        identity->IsInvincibleSet = true;
    }
}

void ROWOWMod::PCDelInvincibleTimer(Player* player)
{
    // pc_delinvincibletimer: the player acted (walked, attacked, used a skill or an item); the units it stands in act on it now
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->InvincibleEndTime == 0)
        return;
    identity->InvincibleEndTime = 0;
    if (identity->IsInvincibleSet == true)
    {
        player->SetImmuneToNPC(false);
        identity->IsInvincibleSet = false;
    }
    std::vector<uint16> skillUnitCell;
    int32 x = 0, y = 0;
    GetUnitCell(player, x, y);
    SkillUnitMove(player, GetTick(), 1, x, y, skillUnitCell);
}

void ROWOWMod::UpdateInvincibleTimer(Player* player)
{
    // pc_invincible_timer running out, or the player moving off its spot (clif_parse_WalkToXY) or attacking (clif_parse_ActionRequest)
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->InvincibleEndTime == 0)
        return;
    if (GetTick() >= identity->InvincibleEndTime || identity->IsInForm == false || player->GetVictim() != nullptr ||
        player->GetExactDist2d(identity->InvinciblePositionX, identity->InvinciblePositionY) > 0.5f)
        PCDelInvincibleTimer(player);
}

bool ROWOWMod::PCIsInvincible(Unit const* unit) const
{
    if (unit == nullptr || unit->IsPlayer() == false)
        return false;
    ROWOWIdentity const* identity = unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr && identity->InvincibleEndTime != 0;
}

bool ROWOWMod::PCIsRiding(Unit const* unit) const
{
    // pc_isriding: a player in the form on its Peco
    if (unit == nullptr || unit->IsPlayer() == false)
        return false;
    ROWOWIdentity const* identity = unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr && identity->IsInForm == true && (identity->Option & ROWOW_OPTION_RIDING) != 0;
}

void ROWOWMod::PCSetRiding(Player* player, bool isRiding)
{
    // pc_setriding: mounts a Peco (only with Riding learned) or dismounts it, and pc_setoption's status calculation follows (the speed, the
    // ASPD and Cavalier Mastery, the spear against medium monsters).  The look stays the job's own: the mounted job sprites (Peco Knight,
    // Peco Crusader...) aren't converted
    ROWOWIdentity* identity = GetIdentity(player);
    uint32 option = identity->Option;
    if (isRiding == true)
    {
        if (PCCheckSkill(*identity, ROWOW_SKILL_KN_RIDING) > 0) // add peco
            option |= ROWOW_OPTION_RIDING;
    }
    else
        option &= ~ROWOW_OPTION_RIDING;
    if (option == identity->Option)
        return;
    identity->Option = option;
    std::bitset<ROWOW_SCB_MAX> calcFlag;
    calcFlag.set(ROWOW_SCB_BASE);
    calcFlag.set(ROWOW_SCB_SPEED);
    StatusCalcBL(player, calcFlag);
}

Player* ROWOWMod::GetDevotionSource(Unit const* bl, bool isRangeChecked) const
{
    // The Crusader of a devoted unit, as rAthena checks it before each use of SC_DEVOTION: the player of val1, on the unit's map, whose slot
    // val2 still names the unit, and (check_distance_bl) within the link's reach, val3
    ROWOWStatusChangeEntry const* sce = GetSCE(bl, ROWOW_SC_DEVOTION);
    if (sce == nullptr || sce->Val2 < 0 || sce->Val2 >= ROWOW_MAX_DEVOTION || bl->IsInWorld() == false)
        return nullptr;
    Player* d_bl = ObjectAccessor::GetPlayer(bl->GetMap(), ObjectGuid::Create<HighGuid::Player>(uint32(sce->Val1)));
    ROWOWIdentity const* identity = d_bl != nullptr ? d_bl->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
    if (identity == nullptr || identity->DevotionGUIDs[sce->Val2] != bl->GetGUID())
        return nullptr;
    if (isRangeChecked == true && DistanceBL(bl, d_bl) > sce->Val3)
        return nullptr;
    return d_bl;
}

Player* ROWOWMod::GetDevotedPlayer(Player* sd, int32 slot)
{
    // sd->devotion[slot]: the player there, on the Crusader's map, whose Devotion still names the Crusader and the slot.  A slot whose player
    // left or lost the status (its status_change_end may have run where the Crusader's map isn't) is let go here
    ROWOWIdentity* identity = GetIdentity(sd);
    ObjectGuid& devotionGUID = identity->DevotionGUIDs[slot];
    if (devotionGUID.IsEmpty() == true)
        return nullptr;
    Player* tsd = ObjectAccessor::GetPlayer(sd->GetMap(), devotionGUID);
    ROWOWStatusChangeEntry const* sce = tsd != nullptr ? GetSCE(tsd, ROWOW_SC_DEVOTION) : nullptr;
    if (sce == nullptr || uint32(sce->Val1) != sd->GetGUID().GetCounter() || sce->Val2 != slot)
    {
        devotionGUID.Clear();
        return nullptr;
    }
    return tsd;
}

int32 ROWOWMod::PCDevotionSlot(Player* sd, ObjectGuid targetGUID, int32 count)
{
    // The slot Devotion uses for a target among the level's: the one devoting the Crusader to it already, else the first free one, else -1
    // (no free slots, the skill fails)
    ROWOWIdentity const* identity = GetIdentity(sd);
    for (int32 i = 0; i < count; i++)
        GetDevotedPlayer(sd, i);
    for (int32 i = 0; i < count; i++)
        if (identity->DevotionGUIDs[i] == targetGUID)
            return i;
    for (int32 i = 0; i < count; i++)
        if (identity->DevotionGUIDs[i].IsEmpty() == true)
            return i;
    return -1;
}

void ROWOWMod::PCDelDevotions(Player* sd)
{
    // pc_dead: the Crusader's devotions end with its death
    ROWOWIdentity* identity = GetIdentity(sd);
    for (int32 k = 0; k < ROWOW_MAX_DEVOTION; k++)
    {
        if (Player* devsd = GetDevotedPlayer(sd, k))
            StatusChangeEnd(devsd, ROWOW_SC_DEVOTION);
        identity->DevotionGUIDs[k].Clear();
    }
}

void ROWOWMod::StatusChangeStartDevoted(Unit* src, Player* sd, int32 type, int32 val1, int32 val2, int32 val3, int32 val4, int64 tick, uint8 flag)
{
    // status_change_start's devotion loops: a Crusader's Auto Guard, Defender, Reflect Shield and Endure go to each player it devotes itself to
    for (int32 i = 0; i < ROWOW_MAX_DEVOTION; i++)
        if (Player* tsd = GetDevotedPlayer(sd, i))
            StatusChangeStart(src, tsd, type, 10000, val1, val2, val3, val4, tick, flag);
}

Unit* ROWOWMod::BattleCheckDevotion(Unit* bl) const
{
    // battle_check_devotion: damage reflected onto a devoted attacker goes to its Crusader instead at devotion_rdamage's chance
    if (BattleConfig.DevotionRDamage != 0 && BattleConfig.DevotionRDamage > int32(urand(0, 99)))
    {
        if (ROWOWStatusChangeEntry const* sce = GetSCE(bl, ROWOW_SC_DEVOTION); sce != nullptr && bl->IsInWorld() == true)
            return ObjectAccessor::GetPlayer(bl->GetMap(), ObjectGuid::Create<HighGuid::Player>(uint32(sce->Val1)));
    }
    return nullptr;
}

void ROWOWMod::BattleFixDamage(Unit* src, Unit* target, int64 damage, uint16 skillID)
{
    // battle_fix_damage (status_fix_damage): the damage alone, with no attack of its own: shown on the target (clif_damage of the target on
    // itself), as the skill's spell, and dealt from src
    if (src == nullptr || target == nullptr || target->IsAlive() == false || damage <= 0)
        return;
    uint32 spellID = GetSkillSpellID(skillID, 1);
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID != 0 ? spellID : GetWeaponHitSoundSpellID(ROWOW_WEAPON_TYPE_FIST));
    uint32 dealt = uint32(std::clamp<int64>(damage, 0, std::numeric_limits<uint32>::max()));
    if (spellInfo != nullptr)
    {
        SpellNonMeleeDamage log(target, target, spellInfo, SPELL_SCHOOL_MASK_NORMAL);
        log.damage = dealt;
        target->SendSpellNonMeleeDamageLog(&log);
    }
    Unit::DealDamage(src, target, dealt, nullptr, SPELL_DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, spellInfo, false);
}

int32 ROWOWMod::PCModifyBuyValue(Player const* player, int32 origValue) const
{
    // pc_modifybuyvalue: Discount (5% + 2% a level, 24% at 10) or the Rogue's Compulsion Discount (5% + 4% a level), the better
    int32 skill = 0, val = origValue, rate1 = 0, rate2 = 0;
    if ((skill = PCCheckSkill(player, ROWOW_SKILL_MC_DISCOUNT)) > 0) // merchant discount
        rate1 = 5 + skill * 2 - ((skill == 10) ? 1 : 0);
    if ((skill = PCCheckSkill(player, ROWOW_SKILL_RG_COMPULSION)) > 0) // rogue discount
        rate2 = 5 + skill * 4;
    if (rate1 < rate2)
        rate1 = rate2;
    if (rate1 != 0)
        val = int32(double(origValue) * double(100 - rate1) / 100.);
    if (val < 0)
        val = 0;
    if (origValue > 0 && val < 1)
        val = 1;
    return val;
}

int32 ROWOWMod::PCModifySellValue(Player const* player, int32 origValue) const
{
    // pc_modifysellvalue: Overcharge (5% + 2% a level, 24% at 10)
    int32 skill = 0, val = origValue, rate = 0;
    if ((skill = PCCheckSkill(player, ROWOW_SKILL_MC_OVERCHARGE)) > 0) // OverCharge
        rate = 5 + skill * 2 - ((skill == 10) ? 1 : 0);
    if (rate != 0)
        val = int32(double(origValue) * double(100 + rate) / 100.);
    if (val < 0)
        val = 0;
    if (origValue > 0 && val < 1)
        val = 1;
    return val;
}
