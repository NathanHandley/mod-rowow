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
//  Ported from rAthena (src/map/pc.cpp, status.cpp, battle.cpp), pre-renewal code paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "QueryResult.h"
#include "WorldSession.h"

#include "ROWOW.h"
#include "ROWOW_Script.h"

// RO bonuses: the worn items' scripts, their cards' and the item combos' (status_calc_pc_sub), the bonus commands they call (pc_bonus to
// pc_bonus5), refine, compounding cards into items (pc_insert_card), and what the bonuses do to a normal attack (battle_calc_cardfix).
// The scripts run in the player's own thread, on data loaded at startup and only read after

namespace
{
    // The bonus types the rules take now, and those whose system is still to come (kept as ROWOWDeferredBonus).  Anything else is
    // logged at startup with its items
    enum ROWOWBonusType : uint8
    {
        BONUS_STR, BONUS_AGI, BONUS_VIT, BONUS_INT, BONUS_DEX, BONUS_LUK, BONUS_ALL_STATS, BONUS_AGI_VIT, BONUS_AGI_DEX_STR,
        BONUS_ATK1, BONUS_ATK2, BONUS_BASE_ATK, BONUS_DEF1, BONUS_DEF2, BONUS_MDEF1, BONUS_MDEF2, BONUS_HIT, BONUS_FLEE1, BONUS_FLEE2,
        BONUS_CRITICAL, BONUS_ATK_ELE, BONUS_DEF_ELE, BONUS_MAX_HP, BONUS_MAX_SP, BONUS_MAX_HP_RATE, BONUS_MAX_SP_RATE, BONUS_SP_RATE,
        BONUS_ATTACK_RANGE, BONUS_ASPD, BONUS_ASPD_RATE, BONUS_HP_RECOV_RATE, BONUS_SP_RECOV_RATE, BONUS_CRITICAL_DEF, BONUS_NEAR_ATK_DEF,
        BONUS_LONG_ATK_DEF, BONUS_DOUBLE_RATE, BONUS_DOUBLE_ADD_RATE, BONUS_MATK_RATE, BONUS_IGNORE_DEF_ELE, BONUS_IGNORE_DEF_RACE,
        BONUS_IGNORE_DEF_CLASS, BONUS_MAGIC_ATK_DEF, BONUS_MISC_ATK_DEF, BONUS_PERFECT_HIT_RATE, BONUS_PERFECT_HIT_ADD_RATE,
        BONUS_CRITICAL_RATE, BONUS_DEF_RATIO_ATK_ELE, BONUS_DEF_RATIO_ATK_RACE, BONUS_DEF_RATIO_ATK_CLASS, BONUS_HIT_RATE, BONUS_FLEE_RATE,
        BONUS_FLEE2_RATE, BONUS_DEF_RATE, BONUS_DEF2_RATE, BONUS_MDEF_RATE, BONUS_MDEF2_RATE, BONUS_CRIT_ATK_RATE, BONUS_LONG_ATK_RATE,
        BONUS_SHORT_ATK_RATE, BONUS_NO_SIZEFIX, BONUS_CASTRATE, BONUS_SPLASH_RANGE, BONUS_SPLASH_ADD_RANGE,
        BONUS2_ADD_ELE, BONUS2_ADD_RACE, BONUS2_ADD_CLASS, BONUS2_ADD_SIZE, BONUS2_SUB_ELE, BONUS2_SUB_RACE, BONUS2_SUB_CLASS, BONUS2_SUB_SIZE,
        BONUS2_ADD_DAMAGE_CLASS, BONUS2_ADD_DEF_MONSTER, BONUS2_WEAPON_ATK, BONUS2_WEAPON_DAMAGE_RATE, BONUS2_CRITICAL_ADDRACE,
        BONUS2_IGNORE_DEF_RACE_RATE, BONUS2_IGNORE_DEF_CLASS_RATE, BONUS2_EXP_ADDRACE, BONUS2_EXP_ADDCLASS, BONUS_CRITICAL_RANGEATK,
        BONUS_ADD_EFF, BONUS_ADD_EFF2, BONUS_ADD_EFF_WHEN_HIT, BONUS_ADD_EFF_ON_SKILL, BONUS2_RES_EFF, BONUS_AUTO_SPELL, BONUS_AUTO_SPELL_WHEN_HIT,
        BONUS_AUTO_SPELL_ON_SKILL, BONUS_NOT_PRE_RENEWAL, BONUS_DEFERRED,
    };

    std::unordered_map<std::string, ROWOWBonusType> const BonusTypesByName =
    {
        { "bStr", BONUS_STR }, { "bAgi", BONUS_AGI }, { "bVit", BONUS_VIT }, { "bInt", BONUS_INT }, { "bDex", BONUS_DEX }, { "bLuk", BONUS_LUK },
        { "bAllStats", BONUS_ALL_STATS }, { "bAgiVit", BONUS_AGI_VIT }, { "bAgiDexStr", BONUS_AGI_DEX_STR }, { "bAtk", BONUS_ATK1 }, { "bAtk2", BONUS_ATK2 },
        { "bBaseAtk", BONUS_BASE_ATK }, { "bDef", BONUS_DEF1 }, { "bDef2", BONUS_DEF2 }, { "bMdef", BONUS_MDEF1 }, { "bMdef2", BONUS_MDEF2 },
        { "bHit", BONUS_HIT }, { "bFlee", BONUS_FLEE1 }, { "bFlee2", BONUS_FLEE2 }, { "bCritical", BONUS_CRITICAL }, { "bAtkEle", BONUS_ATK_ELE },
        { "bDefEle", BONUS_DEF_ELE }, { "bMaxHP", BONUS_MAX_HP }, { "bMaxSP", BONUS_MAX_SP }, { "bMaxHPrate", BONUS_MAX_HP_RATE },
        { "bMaxSPrate", BONUS_MAX_SP_RATE }, { "bUseSPrate", BONUS_SP_RATE }, { "bAtkRange", BONUS_ATTACK_RANGE }, { "bAspd", BONUS_ASPD },
        { "bAspdRate", BONUS_ASPD_RATE }, { "bHPrecovRate", BONUS_HP_RECOV_RATE }, { "bSPrecovRate", BONUS_SP_RECOV_RATE },
        { "bCriticalDef", BONUS_CRITICAL_DEF }, { "bNearAtkDef", BONUS_NEAR_ATK_DEF }, { "bLongAtkDef", BONUS_LONG_ATK_DEF },
        { "bDoubleRate", BONUS_DOUBLE_RATE }, { "bDoubleAddRate", BONUS_DOUBLE_ADD_RATE }, { "bMatkRate", BONUS_MATK_RATE },
        { "bIgnoreDefEle", BONUS_IGNORE_DEF_ELE }, { "bIgnoreDefRace", BONUS_IGNORE_DEF_RACE }, { "bIgnoreDefClass", BONUS_IGNORE_DEF_CLASS },
        { "bMagicAtkDef", BONUS_MAGIC_ATK_DEF }, { "bMiscAtkDef", BONUS_MISC_ATK_DEF }, { "bPerfectHitRate", BONUS_PERFECT_HIT_RATE },
        { "bPerfectHitAddRate", BONUS_PERFECT_HIT_ADD_RATE }, { "bCriticalRate", BONUS_CRITICAL_RATE }, { "bDefRatioAtkEle", BONUS_DEF_RATIO_ATK_ELE },
        { "bDefRatioAtkRace", BONUS_DEF_RATIO_ATK_RACE }, { "bDefRatioAtkClass", BONUS_DEF_RATIO_ATK_CLASS }, { "bHitRate", BONUS_HIT_RATE },
        { "bFleeRate", BONUS_FLEE_RATE }, { "bFlee2Rate", BONUS_FLEE2_RATE }, { "bDefRate", BONUS_DEF_RATE }, { "bDef2Rate", BONUS_DEF2_RATE },
        { "bMdefRate", BONUS_MDEF_RATE }, { "bMdef2Rate", BONUS_MDEF2_RATE }, { "bCritAtkRate", BONUS_CRIT_ATK_RATE }, { "bLongAtkRate", BONUS_LONG_ATK_RATE },
        { "bShortAtkRate", BONUS_SHORT_ATK_RATE }, { "bNoSizeFix", BONUS_NO_SIZEFIX }, { "bCastrate", BONUS_CASTRATE }, { "bSplashRange", BONUS_SPLASH_RANGE },
        { "bSplashAddRange", BONUS_SPLASH_ADD_RANGE },
        { "bAddEle", BONUS2_ADD_ELE }, { "bAddRace", BONUS2_ADD_RACE }, { "bAddClass", BONUS2_ADD_CLASS }, { "bAddSize", BONUS2_ADD_SIZE },
        { "bSubEle", BONUS2_SUB_ELE }, { "bSubRace", BONUS2_SUB_RACE }, { "bSubClass", BONUS2_SUB_CLASS }, { "bSubSize", BONUS2_SUB_SIZE },
        { "bAddDamageClass", BONUS2_ADD_DAMAGE_CLASS }, { "bAddDefMonster", BONUS2_ADD_DEF_MONSTER }, { "bWeaponAtk", BONUS2_WEAPON_ATK },
        { "bWeaponDamageRate", BONUS2_WEAPON_DAMAGE_RATE }, { "bCriticalAddRace", BONUS2_CRITICAL_ADDRACE }, { "bIgnoreDefRaceRate", BONUS2_IGNORE_DEF_RACE_RATE },
        { "bIgnoreDefClassRate", BONUS2_IGNORE_DEF_CLASS_RATE }, { "bExpAddRace", BONUS2_EXP_ADDRACE }, { "bExpAddClass", BONUS2_EXP_ADDCLASS },
        { "bCriticalLong", BONUS_CRITICAL_RANGEATK }, { "bAtkRate", BONUS_NOT_PRE_RENEWAL },

        // Skills (R6): autospells and skill bonuses, casting, healing, SP and HP use
        { "bAutoSpell", BONUS_AUTO_SPELL }, { "bAutoSpellWhenHit", BONUS_AUTO_SPELL_WHEN_HIT }, { "bAutoSpellOnSkill", BONUS_AUTO_SPELL_ON_SKILL }, { "bSkillAtk", BONUS_DEFERRED },
        { "bSkillHeal", BONUS_DEFERRED }, { "bSkillHeal2", BONUS_DEFERRED }, { "bSkillUseSP", BONUS_DEFERRED }, { "bSkillUseSPrate", BONUS_DEFERRED },
        { "bSkillCooldown", BONUS_DEFERRED }, { "bSkillFixCast", BONUS_DEFERRED }, { "bSkillVariableCast", BONUS_DEFERRED }, { "bDelayRate", BONUS_DEFERRED },
        { "bSkillDelay", BONUS_DEFERRED }, { "bSubSkill", BONUS_DEFERRED }, { "bVariableCast", BONUS_DEFERRED }, { "bVariableCastrate", BONUS_DEFERRED },
        { "bHealPower", BONUS_DEFERRED }, { "bHealPower2", BONUS_DEFERRED }, { "bAddSkillBlow", BONUS_DEFERRED }, { "bNoCastCancel", BONUS_DEFERRED },
        { "bNoCastCancel2", BONUS_DEFERRED }, { "bNoGemStone", BONUS_DEFERRED }, { "bIntravision", BONUS_DEFERRED }, { "bNoKnockback", BONUS_DEFERRED },
        // Magic (R6)
        { "bMagicAddRace", BONUS_DEFERRED }, { "bMagicAddEle", BONUS_DEFERRED }, { "bMagicAddSize", BONUS_DEFERRED }, { "bMagicAddClass", BONUS_DEFERRED },
        { "bMagicAtkEle", BONUS_DEFERRED }, { "bIgnoreMdefRace", BONUS_DEFERRED }, { "bIgnoreMdefEle", BONUS_DEFERRED }, { "bIgnoreMdefClass", BONUS_DEFERRED },
        { "bIgnoreMdefRaceRate", BONUS_DEFERRED }, { "bIgnoreMdefClassRate", BONUS_DEFERRED }, { "bAddMagicDamageClass", BONUS_DEFERRED },
        { "bAddMDefMonster", BONUS_DEFERRED }, { "bMagicDamageReturn", BONUS_DEFERRED }, { "bNoMagicDamage", BONUS_DEFERRED }, { "bMatk", BONUS_DEFERRED },
        { "bMagicHPGainValue", BONUS_DEFERRED }, { "bMagicSPGainValue", BONUS_DEFERRED },
        // Monster race groups (race2, R10)
        { "bAddRace2", BONUS_DEFERRED }, { "bSubRace2", BONUS_DEFERRED },
        // Statuses: their chances on hit and when hit, resistances, and (R6.5) the like
        { "bAddEff", BONUS_ADD_EFF }, { "bAddEff2", BONUS_ADD_EFF2 }, { "bAddEffWhenHit", BONUS_ADD_EFF_WHEN_HIT }, { "bAddEffOnSkill", BONUS_ADD_EFF_ON_SKILL },
        { "bResEff", BONUS2_RES_EFF }, { "bComaRace", BONUS_DEFERRED }, { "bComaClass", BONUS_DEFERRED }, { "bWeaponComaRace", BONUS_DEFERRED },
        { "bWeaponComaEle", BONUS_DEFERRED }, { "bWeaponComaClass", BONUS_DEFERRED }, { "bBreakWeaponRate", BONUS_DEFERRED },
        { "bBreakArmorRate", BONUS_DEFERRED }, { "bUnbreakableWeapon", BONUS_DEFERRED }, { "bUnbreakableArmor", BONUS_DEFERRED },
        { "bUnbreakableHelm", BONUS_DEFERRED }, { "bUnbreakableShield", BONUS_DEFERRED }, { "bUnbreakableGarment", BONUS_DEFERRED },
        { "bUnbreakableShoes", BONUS_DEFERRED }, { "bUnstripable", BONUS_DEFERRED }, { "bUnstripableWeapon", BONUS_DEFERRED },
        { "bUnstripableArmor", BONUS_DEFERRED }, { "bUnstripableHelm", BONUS_DEFERRED }, { "bUnstripableShield", BONUS_DEFERRED },
        { "bShortWeaponDamageReturn", BONUS_DEFERRED }, { "bLongWeaponDamageReturn", BONUS_DEFERRED }, { "bClassChange", BONUS_DEFERRED },
        // HP and SP drain, gain, loss and regen on hits, kills and over time
        { "bHPDrainRate", BONUS_DEFERRED }, { "bSPDrainRate", BONUS_DEFERRED }, { "bHPDrainValue", BONUS_DEFERRED }, { "bSPDrainValue", BONUS_DEFERRED },
        { "bHPDrainValueRace", BONUS_DEFERRED }, { "bSPDrainValueRace", BONUS_DEFERRED }, { "bSPGainRace", BONUS_DEFERRED }, { "bHPGainValue", BONUS_DEFERRED },
        { "bSPGainValue", BONUS_DEFERRED }, { "bSPVanishRate", BONUS_DEFERRED }, { "bHPVanishRate", BONUS_DEFERRED }, { "bHPLossRate", BONUS_DEFERRED },
        { "bSPLossRate", BONUS_DEFERRED }, { "bHPRegenRate", BONUS_DEFERRED }, { "bSPRegenRate", BONUS_DEFERRED }, { "bNoRegen", BONUS_DEFERRED },
        { "bRestartFullRecover", BONUS_DEFERRED }, { "bAddItemHealRate", BONUS_DEFERRED }, { "bAddItemGroupHealRate", BONUS_DEFERRED },
        // Drops and zeny (R10), movement and the rest
        { "bAddMonsterDropItem", BONUS_DEFERRED }, { "bAddMonsterDropItemGroup", BONUS_DEFERRED }, { "bAddMonsterIdDropItem", BONUS_DEFERRED },
        { "bGetZenyNum", BONUS_DEFERRED }, { "bAddGetZenyNum", BONUS_DEFERRED }, { "bSpeedRate", BONUS_DEFERRED }, { "bSpeedAddRate", BONUS_DEFERRED },
        { "bAddStealRate", BONUS_DEFERRED }, { "bInfiniteEndure", BONUS_DEFERRED }, { "bFixedCast", BONUS_DEFERRED }, { "bNoWalkDelay", BONUS_DEFERRED },
        { "bUnbreakable", BONUS_DEFERRED }, { "bPerfectHide", BONUS_DEFERRED }, { "bNoMiscDamage", BONUS_DEFERRED }, { "bNoWeaponDamage", BONUS_DEFERRED },
        { "bIgnoreDefRaceRate2", BONUS_DEFERRED },
    };

    // The bonus types by their lowercase names, as rAthena's script constants aren't case sensitive
    std::unordered_map<std::string, ROWOWBonusType> const& GetBonusTypesByLowerName()
    {
        static std::unordered_map<std::string, ROWOWBonusType> const bonusTypesByLowerName = []()
        {
            std::unordered_map<std::string, ROWOWBonusType> lowerNames;
            for (auto const& bonusTypeByName : BonusTypesByName)
            {
                std::string lowerName = bonusTypeByName.first;
                for (char& curChar : lowerName)
                    curChar = char(tolower((unsigned char)curChar));
                lowerNames[lowerName] = bonusTypeByName.second;
            }
            return lowerNames;
        }();
        return bonusTypesByLowerName;
    }

    // rAthena's equipment indexes (e_equip_index) as getequipid and getequiprefinerycnt take them, and the positions each is
    std::pair<char const*, uint32> const EquipIndexes[] =
    {
        { "EQI_ACC_L", ROWOW_EQUIP_POSITION_ACC_L }, { "EQI_ACC_R", ROWOW_EQUIP_POSITION_ACC_R }, { "EQI_SHOES", ROWOW_EQUIP_POSITION_SHOES },
        { "EQI_GARMENT", ROWOW_EQUIP_POSITION_GARMENT }, { "EQI_HEAD_LOW", ROWOW_EQUIP_POSITION_HEAD_LOW }, { "EQI_HEAD_MID", ROWOW_EQUIP_POSITION_HEAD_MID },
        { "EQI_HEAD_TOP", ROWOW_EQUIP_POSITION_HEAD_TOP }, { "EQI_ARMOR", ROWOW_EQUIP_POSITION_ARMOR }, { "EQI_HAND_L", ROWOW_EQUIP_POSITION_HAND_L },
        { "EQI_HAND_R", ROWOW_EQUIP_POSITION_HAND_R }, { "EQI_COSTUME_HEAD_TOP", ROWOW_EQUIP_POSITION_COSTUME_HEAD_TOP },
        { "EQI_COSTUME_HEAD_MID", ROWOW_EQUIP_POSITION_COSTUME_HEAD_MID }, { "EQI_COSTUME_HEAD_LOW", ROWOW_EQUIP_POSITION_COSTUME_HEAD_LOW },
        { "EQI_COSTUME_GARMENT", ROWOW_EQUIP_POSITION_COSTUME_GARMENT }, { "EQI_AMMO", ROWOW_EQUIP_POSITION_AMMO },
    };

    // The enchantment slots an item's cards are kept in (the gem slots and the socket bonus slot, which RO items never use otherwise)
    EnchantmentSlot const CardEnchantmentSlots[ROWOW_MAX_SLOTS] = { SOCK_ENCHANTMENT_SLOT, SOCK_ENCHANTMENT_SLOT_2, SOCK_ENCHANTMENT_SLOT_3, BONUS_ENCHANTMENT_SLOT };

    std::string ToLower(std::string text)
    {
        for (char& curChar : text)
            curChar = char(tolower((unsigned char)curChar));
        return text;
    }

    // A bonus value that names a race, element, size or class, checked against the table's size (PC_BONUS_CHK_*)
    bool IsBonusIndexValid(int64 index, int32 maxIndex)
    {
        return index >= 0 && index < maxIndex;
    }

    // The first job of a job's line (BaseClass) and the job without its transcendence or Baby (BaseJob)
    uint16 GetBaseJobID(uint16 jobID)
    {
        if (jobID >= ROWOW_JOB_UPPER_START && jobID <= ROWOW_JOB_UPPER_END)
            return uint16(jobID - ROWOW_JOB_UPPER_START);
        if (jobID >= ROWOW_JOB_BABY && jobID <= ROWOW_JOB_SUPER_BABY)
            return ROWOW->GetNonBabyJobID(jobID);
        if (jobID == 4048) // Star_Gladiator2
            return 4047;
        return jobID;
    }

    uint16 GetBaseClassJobID(uint16 jobID)
    {
        static std::unordered_map<uint16, uint16> const firstJobsBySecondJob =
        {
            { 7, 1 }, { 13, 1 }, { 14, 1 }, { 21, 1 }, { 8, 4 }, { 15, 4 }, { 9, 2 }, { 16, 2 }, { 10, 5 }, { 18, 5 }, { 11, 3 }, { 19, 3 }, { 20, 3 },
            { 12, 6 }, { 17, 6 }, { 23, 0 }, { 4047, 4046 }, { 4049, 4046 },
        };
        uint16 baseJobID = GetBaseJobID(jobID);
        auto firstJobIter = firstJobsBySecondJob.find(baseJobID);
        return firstJobIter != firstJobsBySecondJob.end() ? firstJobIter->second : baseJobID;
    }

    // pc_bonus_autospell: an autospell, its battle flags filled in (both ranges, weapon, normal attacks or the skills for magic and misc);
    // the same autospell of the same card adds its rate only with autospell_stacking
    void AddAutoSpell(std::vector<ROWOWAutoSpell>& spells, uint16 id, uint16 lv, int16 rate, int16 battleFlag, uint32 cardID, uint8 flag)
    {
        if (rate == 0)
            return;
        if ((battleFlag & (ROWOW_BF_SHORT | ROWOW_BF_LONG)) == 0)
            battleFlag |= ROWOW_BF_SHORT | ROWOW_BF_LONG; // No range defined? Use both.
        if ((battleFlag & (ROWOW_BF_WEAPON | ROWOW_BF_MAGIC | ROWOW_BF_MISC)) == 0)
            battleFlag |= ROWOW_BF_WEAPON; // No attack type defined? Use weapon.
        if ((battleFlag & (ROWOW_BF_SKILL | ROWOW_BF_NORMAL)) == 0)
        {
            if ((battleFlag & (ROWOW_BF_MAGIC | ROWOW_BF_MISC)) != 0)
                battleFlag |= ROWOW_BF_SKILL; // These two would never trigger without BF_SKILL
            if ((battleFlag & ROWOW_BF_WEAPON) != 0)
                battleFlag |= ROWOW_BF_NORMAL; // By default autospells should only trigger on normal weapon attacks.
        }
        for (ROWOWAutoSpell& spell : spells)
        {
            if ((spell.CardID == cardID || spell.Rate < 0 || rate < 0) && spell.SkillID == id && spell.Level == lv && spell.BattleFlag == battleFlag && spell.Flag == flag)
            {
                if (ROWOW->BattleConfig.AutospellStacking == 0 && spell.Rate > 0 && rate > 0) // Stacking disabled
                    return;
                spell.Rate = int16(std::min<int32>(spell.Rate + rate, 1000));
                return;
            }
        }
        ROWOWAutoSpell entry;
        entry.SkillID = id;
        entry.Level = lv;
        entry.Rate = int16(std::clamp<int32>(rate, -1000, 1000));
        entry.BattleFlag = battleFlag;
        entry.CardID = cardID;
        entry.Flag = flag;
        spells.push_back(entry);
    }

    // A bonus's text argument: the skills' bonuses (bSkillAtk, bSkillHeal, the skill command...) name a skill, as rAthena's skill_name2id
    // reads it; any other text is 0
    int64 GetSkillValue(std::string const& text)
    {
        return int64(ROWOW->GetSkillIDByName(text));
    }
}

void ROWOWMod::LoadBonusData()
{
    CardItemIDsByEnchantmentID.clear();
    ItemCombos.clear();
    ItemComboIndexesByItemID.clear();
    UnknownBonusItemIDs.clear();
    AutoBonusScriptTexts.clear();
    for (auto& refineBonuses : RefineBonuses)
        for (ROWOWRefineBonus& refineBonus : refineBonuses)
            refineBonus = ROWOWRefineBonus();
    if (IsEnabled == false)
        return;

    // Refine bonuses: armor's, and each weapon level's (refine.yml's Armor and Weapon groups), and the normal refine's chance (Upgrade Weapon's)
    if (ROWOWDataTable const* refineTable = GetDataTable(ROWOW_DATA_TABLE_REFINE))
    {
        int32 groupColumnIndex = refineTable->GetColumnIndex("Group");
        int32 levelColumnIndex = refineTable->GetColumnIndex("Levels_Level");
        int32 refineColumnIndex = refineTable->GetColumnIndex("Levels_RefineLevels_Level");
        int32 bonusColumnIndex = refineTable->GetColumnIndex("Levels_RefineLevels_Bonus");
        int32 randomBonusColumnIndex = refineTable->GetColumnIndex("Levels_RefineLevels_RandomBonus");
        int32 chancesColumnIndex = refineTable->GetColumnIndex("Levels_RefineLevels_Chances");
        for (size_t rowIndex = 0; rowIndex < refineTable->Rows.size(); rowIndex++)
        {
            std::string const& group = refineTable->GetCell(rowIndex, groupColumnIndex);
            int64 level = refineTable->GetInt(rowIndex, levelColumnIndex, 1);
            int64 refine = refineTable->GetInt(rowIndex, refineColumnIndex, 0);
            int32 refineType = group == "Armor" ? 0 : (group == "Weapon" ? int32(level) : -1);
            if (refineType < 0 || refineType >= ROWOW_REFINE_TYPE_COUNT || refine < 1 || refine > ROWOW_MAX_REFINE)
                continue;
            RefineBonuses[refineType][refine].Bonus = int32(refineTable->GetInt(rowIndex, bonusColumnIndex, 0));
            RefineBonuses[refineType][refine].RandomBonus = int32(refineTable->GetInt(rowIndex, randomBonusColumnIndex, 0));
            for (auto const& chance : ROWOWDataTable::ParseMapList(refineTable->GetCell(rowIndex, chancesColumnIndex)))
            {
                bool isNormal = std::any_of(chance.begin(), chance.end(), [](auto const& field) { return field.first == "Type" && field.second == "Normal"; });
                for (auto const& field : chance)
                    if (isNormal == true && field.first == "Rate")
                        RefineBonuses[refineType][refine].Chance = int32(strtol(field.second.c_str(), nullptr, 10));

                // The refiners' cost of the type (the refiner NPCs' getequiprefinecost and getequippercentrefinery, R8)
                int32 costType = -1;
                for (auto const& field : chance)
                    if (field.first == "Type")
                        costType = field.second == "Normal" ? ROWOW_REFINE_COST_NORMAL : (field.second == "Over10" ? 1 : (field.second == "Enriched" ? ROWOW_REFINE_COST_ENRICHED : -1));
                if (costType < 0)
                    continue;
                ROWOWRefineBonus& refineBonus = RefineBonuses[refineType][refine];
                refineBonus.HasCost[costType] = true;
                for (auto const& field : chance)
                {
                    if (field.first == "Rate")
                        refineBonus.CostChances[costType] = int32(strtol(field.second.c_str(), nullptr, 10));
                    else if (field.first == "Price")
                        refineBonus.CostPrices[costType] = int32(strtol(field.second.c_str(), nullptr, 10));
                    else if (field.first == "Material")
                    {
                        auto materialIDIter = ItemIDsByAegisName.find(ToLower(field.second));
                        refineBonus.CostMaterialIDs[costType] = materialIDIter != ItemIDsByAegisName.end() ? materialIDIter->second : 0;
                    }
                }
            }
        }
    }

    // The cards' enchantments (written by the converter)
    QueryResult cardTableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_card'");
    if (cardTableResult)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `EnchantmentID`, `ItemID` FROM `mod_rowow_card`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 enchantmentID = fields[0].Get<uint32>();
                uint32 itemID = fields[1].Get<uint32>();
                auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
                if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
                    continue;
                CardItemIDsByEnchantmentID[enchantmentID] = itemID;
                ItemsByItemTemplateID[itemTemplateIDIter->second].CardEnchantmentID = enchantmentID;
            } while (result->NextRow());
        }
    }

    // The commands of worn items' scripts
    auto readBonusValues = [](std::vector<ROWOWScriptValue> const& arguments, std::string& typeOut, std::vector<int64>& valuesOut)
    {
        if (arguments.empty() == true)
            return false;
        typeOut = arguments[0].IsString == true ? arguments[0].Text : ROWOWSCRIPT->GetSymbolName(arguments[0].Number);
        for (size_t argumentIndex = 1; argumentIndex < arguments.size(); argumentIndex++)
            valuesOut.push_back(arguments[argumentIndex].IsString == true ? GetSkillValue(arguments[argumentIndex].Text) : arguments[argumentIndex].Number);
        return typeOut.empty() == false;
    };
    auto checkBonusType = [this](std::vector<ROWOWScriptValue> const& arguments)
    {
        // Known or not, a bonus never stops its item from working; an unknown one is only remembered for the startup log
        if (arguments.empty() == false)
        {
            std::string type = arguments[0].IsString == true ? arguments[0].Text : ROWOWSCRIPT->GetSymbolName(arguments[0].Number);
            if (GetBonusTypesByLowerName().find(ToLower(type)) == GetBonusTypesByLowerName().end())
                UnknownBonusItemIDs[type].insert(CompilingItemID);
        }
        return true;
    };
    for (char const* bonusCommandName : { "bonus", "bonus2", "bonus3", "bonus4", "bonus5" })
    {
        ROWOWSCRIPT->RegisterCommand(bonusCommandName, [this, readBonusValues](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
        {
            std::string type;
            std::vector<int64> values;
            if (context.BonusStatus != nullptr && readBonusValues(arguments, type, values) == true)
                PCBonus(context, type, values);
            return ROWOWScriptValue::FromNumber(0);
        }, checkBonusType);
    }
    for (char const* autoBonusCommandName : { "autobonus", "autobonus2", "autobonus3" })
    {
        // BUILDIN_FUNC(autobonus), autobonus2 and autobonus3: pc_addautobonus on the worn item's positions.  Their scripts are compiled once
        // at startup (script_add_autobonus), so the compile check keeps the texts the items name
        std::string commandName = autoBonusCommandName;
        ROWOWSCRIPT->RegisterCommand(autoBonusCommandName, [commandName](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
        {
            if (context.BonusStatus == nullptr || arguments.size() < 3 || arguments[0].IsString == false)
                return ROWOWScriptValue::FromNumber(0);
            uint32 pos = context.EquipPositions;
            int16 rate = int16(arguments[1].GetNumber());
            uint32 dur = uint32(std::max<int64>(0, arguments[2].GetNumber()));
            std::string const& bonusScript = arguments[0].Text;
            if (rate == 0 || dur == 0 || pos == 0 || bonusScript.empty() == true)
                return ROWOWScriptValue::FromNumber(0);
            bool isOnSkill = commandName == "autobonus3";
            uint16 flag = 0;
            if (arguments.size() >= 4)
                flag = uint16(arguments[3].IsString == true ? GetSkillValue(arguments[3].Text) : arguments[3].GetNumber());
            std::string otherScript = (arguments.size() >= 5 && arguments[4].IsString == true) ? arguments[4].Text : "";
            std::vector<ROWOWAutoBonus>& autoBonuses = commandName == "autobonus" ? context.BonusStatus->Bonus.AutoBonus :
                (commandName == "autobonus2" ? context.BonusStatus->Bonus.AutoBonus2 : context.BonusStatus->Bonus.AutoBonus3);
            // pc_addautobonus: one bonus per position and script
            for (ROWOWAutoBonus const& autoBonus : autoBonuses)
                if (autoBonus.Pos == pos && autoBonus.BonusScript == bonusScript)
                    return ROWOWScriptValue::FromNumber(0);
            if (isOnSkill == false)
            {
                if ((flag & (ROWOW_BF_SHORT | ROWOW_BF_LONG)) == 0)
                    flag |= ROWOW_BF_SHORT | ROWOW_BF_LONG; // No range defined? Use both.
                if ((flag & (ROWOW_BF_WEAPON | ROWOW_BF_MAGIC | ROWOW_BF_MISC)) == 0)
                    flag |= ROWOW_BF_WEAPON; // No attack type defined? Use weapon.
                if ((flag & (ROWOW_BF_SKILL | ROWOW_BF_NORMAL)) == 0)
                {
                    if ((flag & (ROWOW_BF_MAGIC | ROWOW_BF_MISC)) != 0)
                        flag |= ROWOW_BF_SKILL; // These two would never trigger without BF_SKILL
                    if ((flag & ROWOW_BF_WEAPON) != 0)
                        flag |= ROWOW_BF_NORMAL | ROWOW_BF_SKILL;
                }
            }
            ROWOWAutoBonus autoBonus;
            autoBonus.BonusScript = bonusScript;
            autoBonus.Rate = int16(std::clamp<int32>(rate, -10000, 10000));
            autoBonus.Duration = dur;
            autoBonus.AtkType = flag;
            autoBonus.OtherScript = otherScript;
            autoBonus.Pos = pos;
            autoBonuses.push_back(autoBonus);
            return ROWOWScriptValue::FromNumber(0);
        }, [this](std::vector<ROWOWScriptValue> const& arguments)
        {
            if (arguments.empty() == false && arguments[0].IsString == true)
                AutoBonusScriptTexts.insert(arguments[0].Text);
            if (arguments.size() >= 5 && arguments[4].IsString == true)
                AutoBonusScriptTexts.insert(arguments[4].Text);
            return true;
        });
    }
    for (char const* skillCommandName : { "skill", "addtoskill" })
    {
        // The skills equipment and cards grant (buildin_skill, pc_skill): ADDSKILL_TEMP by default, ADDSKILL_TEMP_ADDLEVEL for addtoskill.  An item
        // script's copy of the command, compiled before the NPC scripts' (ROWOW_ScriptCommands.cpp).  In a status calculation it's a part of it,
        // and StatusCalcPCItems clears them first
        bool isAddToSkill = strcmp(skillCommandName, "addtoskill") == 0;
        ROWOWSCRIPT->RegisterCommand(skillCommandName, [this, isAddToSkill](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
        {
            if (context.Identity == nullptr || arguments.empty() == true)
                return ROWOWScriptValue::FromNumber(0);
            uint16 skillID = uint16(arguments[0].IsString == true ? GetSkillIDByName(arguments[0].Text) : arguments[0].GetNumber());
            int64 flag = arguments.size() > 2 ? arguments[2].GetNumber() : (isAddToSkill == true ? ROWOW_ADDSKILL_TEMP_ADDLEVEL : ROWOW_ADDSKILL_TEMP);
            if (flag == ROWOW_ADDSKILL_TEMP || flag == ROWOW_ADDSKILL_TEMP_ADDLEVEL)
                PCAddTempSkill(*context.Identity, skillID, int32(arguments.size() > 1 ? arguments[1].GetNumber() : 1), flag == ROWOW_ADDSKILL_TEMP_ADDLEVEL);
            return ROWOWScriptValue::FromNumber(0);
        });
    }
    ROWOWSCRIPT->RegisterCommand("getrefine", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        return ROWOWScriptValue::FromNumber(context.EquipItem != nullptr ? int64(GetItemRefine(context.EquipItem)) : 0);
    });
    ROWOWSCRIPT->RegisterCommand("readparam", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // A base stat (bStr..bLuk), without the bonuses
        static char const* const StatParameterNames[ROWOW_STAT_COUNT] = { "bStr", "bAgi", "bVit", "bInt", "bDex", "bLuk" };
        if (context.Identity == nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        std::string parameterName = ROWOWSCRIPT->GetSymbolName(arguments[0].Number);
        for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
            if (parameterName == StatParameterNames[statIndex])
                return ROWOWScriptValue::FromNumber(int64(context.Identity->Stats[statIndex]));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("max", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        int64 result = arguments.empty() == true ? 0 : arguments[0].GetNumber();
        for (ROWOWScriptValue const& argument : arguments)
            result = std::max(result, argument.GetNumber());
        return ROWOWScriptValue::FromNumber(result);
    });
    ROWOWSCRIPT->RegisterCommand("min", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        int64 result = arguments.empty() == true ? 0 : arguments[0].GetNumber();
        for (ROWOWScriptValue const& argument : arguments)
            result = std::min(result, argument.GetNumber());
        return ROWOWScriptValue::FromNumber(result);
    });
    ROWOWSCRIPT->RegisterCommand("pow", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (arguments.size() < 2)
            return ROWOWScriptValue::FromNumber(0);
        return ROWOWScriptValue::FromNumber(int64(std::pow(double(arguments[0].GetNumber()), double(arguments[1].GetNumber()))));
    });
    for (auto const& equipIndex : EquipIndexes)
        ROWOWSCRIPT->RegisterConstant(equipIndex.first, int64(&equipIndex - &EquipIndexes[0]));
    auto getEquipIndexItem = [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments) -> Item*
    {
        if (context.AttachedPlayer == nullptr || arguments.empty() == true || arguments[0].GetNumber() < 0 || arguments[0].GetNumber() >= int64(std::size(EquipIndexes)))
            return nullptr;
        return GetIdentityEquippedItem(context.AttachedPlayer, EquipIndexes[arguments[0].GetNumber()].second);
    };
    ROWOWSCRIPT->RegisterCommand("getequipid", [this, getEquipIndexItem](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        ROWOWItem const* roItem = GetROItem(getEquipIndexItem(context, arguments));
        return ROWOWScriptValue::FromNumber(roItem != nullptr ? int64(roItem->ItemID) : -1);
    });
    ROWOWSCRIPT->RegisterCommand("getequiprefinerycnt", [getEquipIndexItem](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        Item* item = getEquipIndexItem(context, arguments);
        return ROWOWScriptValue::FromNumber(item != nullptr ? int64(GetItemRefine(item)) : 0);
    });
    // The refiners' commands (R8): what's worn in an equip index, its refine chance and cost (refine.yml, through RefineDatabase::findLevelInfo:
    // the costs of reaching the next level), and the refine's success or failure
    auto getRefineLevelInfo = [this](Item const* item) -> ROWOWRefineBonus const*
    {
        ROWOWItem const* roItem = GetROItem(item);
        if (roItem == nullptr || roItem->IsRefineable == false || (roItem->IsWeapon == false && roItem->IsArmor == false))
            return nullptr;
        uint32 refine = GetItemRefine(item);
        int32 refineType = roItem->IsWeapon == true ? roItem->WeaponLevel : 0;
        if (refine >= ROWOW_MAX_REFINE || refineType < 0 || refineType >= ROWOW_REFINE_TYPE_COUNT)
            return nullptr;
        return &RefineBonuses[refineType][refine + 1];
    };
    ROWOWSCRIPT->RegisterCommand("getequipisequiped", [getEquipIndexItem](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        return ROWOWScriptValue::FromNumber(getEquipIndexItem(context, arguments) != nullptr ? 1 : 0);
    });
    ROWOWSCRIPT->RegisterCommand("getequipname", [getEquipIndexItem](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // The item's name (ename), "" for nothing worn
        Item* item = getEquipIndexItem(context, arguments);
        return ROWOWScriptValue::FromText(item != nullptr ? item->GetTemplate()->Name1 : std::string());
    });
    ROWOWSCRIPT->RegisterCommand("getequipisenableref", [this, getEquipIndexItem](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        ROWOWItem const* roItem = GetROItem(getEquipIndexItem(context, arguments));
        return ROWOWScriptValue::FromNumber(roItem != nullptr && roItem->IsRefineable == true ? 1 : 0);
    });
    ROWOWSCRIPT->RegisterCommand("getequipweaponlv", [this, getEquipIndexItem](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getequipweaponlv(<equip index>): a weapon's level, 0 for none (the worn item's own, EQI_COMPOUND_ON, without one)
        Item const* item = arguments.empty() == true ? context.EquipItem : getEquipIndexItem(context, arguments);
        ROWOWItem const* roItem = GetROItem(item);
        return ROWOWScriptValue::FromNumber(roItem != nullptr && roItem->IsWeapon == true ? roItem->WeaponLevel : 0);
    });
    ROWOWSCRIPT->RegisterCommand("getequiparmorlv", [this, getEquipIndexItem](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // Armor's level: pre-renewal item_db has none, so every armor is level 1 (item_db's default)
        Item const* item = arguments.empty() == true ? context.EquipItem : getEquipIndexItem(context, arguments);
        ROWOWItem const* roItem = GetROItem(item);
        return ROWOWScriptValue::FromNumber(roItem != nullptr && roItem->IsArmor == true ? 1 : 0);
    });
    ROWOWSCRIPT->RegisterCommand("getequipcardid", [getEquipIndexItem](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getequipcardid(<equip index>,<slot>): the card (item_db Id) in a slot, 0 for none
        Item* item = getEquipIndexItem(context, arguments);
        int64 slot = arguments.size() > 1 ? arguments[1].GetNumber() : -1;
        return ROWOWScriptValue::FromNumber(item != nullptr && slot >= 0 && slot < ROWOW_MAX_SLOTS ? int64(GetItemCard(item, uint8(slot))) : 0);
    });
    ROWOWSCRIPT->RegisterCommand("getequippercentrefinery", [getEquipIndexItem, getRefineLevelInfo](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getequippercentrefinery(<equip index>{,<enriched>}): the chance in percent to refine it once more, 0 when it can't be
        ROWOWRefineBonus const* info = getRefineLevelInfo(getEquipIndexItem(context, arguments));
        int32 costType = arguments.size() > 1 && arguments[1].GetNumber() != 0 ? ROWOW_REFINE_COST_ENRICHED : ROWOW_REFINE_COST_NORMAL;
        return ROWOWScriptValue::FromNumber(info != nullptr && info->HasCost[costType] == true ? info->CostChances[costType] / 100 : 0);
    });
    ROWOWSCRIPT->RegisterCommand("getequiprefinecost", [getEquipIndexItem, getRefineLevelInfo](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getequiprefinecost(<equip index>,<REFINE_COST_*>,<REFINE_MATERIAL_ID or REFINE_ZENY_COST>): -1 when it can't be refined so
        int64 costType = arguments.size() > 1 ? arguments[1].GetNumber() : -1;
        if (costType < 0 || costType >= ROWOW_REFINE_COST_COUNT)
            return ROWOWScriptValue::FromNumber(-1);
        ROWOWRefineBonus const* info = getRefineLevelInfo(getEquipIndexItem(context, arguments));
        if (info == nullptr || info->HasCost[costType] == false)
            return ROWOWScriptValue::FromNumber(-1);
        switch (arguments.size() > 2 ? arguments[2].GetNumber() : -1)
        {
            case 0: return ROWOWScriptValue::FromNumber(int64(info->CostMaterialIDs[costType])); // REFINE_MATERIAL_ID
            case 1: return ROWOWScriptValue::FromNumber(info->CostPrices[costType]); // REFINE_ZENY_COST
            default: return ROWOWScriptValue::FromNumber(-1);
        }
    });
    ROWOWSCRIPT->RegisterCommand("successrefitem", [this, getEquipIndexItem](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // successrefitem <equip index>{,<count>}: refined count levels more (capped at MAX_REFINE), worn again with the new bonuses
        Item* item = getEquipIndexItem(context, arguments);
        if (item == nullptr)
            return ROWOWScriptValue::FromNumber(-1);
        uint32 refine = GetItemRefine(item);
        if (refine >= ROWOW_MAX_REFINE)
            return ROWOWScriptValue::FromNumber(ROWOW_MAX_REFINE);
        refine = uint32(std::clamp<int64>(int64(refine) + (arguments.size() > 1 ? arguments[1].GetNumber() : 1), 0, ROWOW_MAX_REFINE));
        SetItemState(context.AttachedPlayer, item, refine, IsItemBroken(item), IsItemIdentified(item));
        RefreshROForm(context.AttachedPlayer);
        SendItemCardsToClient(context.AttachedPlayer, *context.Identity, true);
        ChatHandler(context.AttachedPlayer->GetSession()).SendSysMessage(fmt::format("Upgrade success!! ({} is now +{})", item->GetTemplate()->Name1, refine)); // clif_refine success
        if (int32(refine) == BattleConfig.BlacksmithFameRefineThreshold)
            PCFameRefine(context.AttachedPlayer, item);
        return ROWOWScriptValue::FromNumber(int64(refine));
    });
    ROWOWSCRIPT->RegisterCommand("failedrefitem", [this, getEquipIndexItem](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // failedrefitem <equip index>: the refine failed, and the item is gone (pc_unequipitem, pc_delitem)
        Item* item = getEquipIndexItem(context, arguments);
        if (item == nullptr)
        {
            LOG_ERROR("module.ROWOW", "failedrefitem: no item equipped at pos {} (character {})", arguments.empty() == true ? -1 : arguments[0].GetNumber(),
                context.AttachedPlayer != nullptr ? context.AttachedPlayer->GetGUID().GetCounter() : 0);
            return ROWOWScriptValue::FromNumber(0);
        }
        std::string itemName = item->GetTemplate()->Name1;
        PCUnequipItem(context.AttachedPlayer, EquipIndexes[arguments[0].GetNumber()].second, 1);
        uint32 count = 1;
        context.AttachedPlayer->DestroyItemCount(item, count, true);
        ChatHandler(context.AttachedPlayer->GetSession()).SendSysMessage(fmt::format("Upgrade failed!! {} was destroyed.", itemName)); // clif_refine failure
        return ROWOWScriptValue::FromNumber(1);
    });
    ROWOWSCRIPT->RegisterCommand("isequipped", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // Whether every item named is worn
        if (context.AttachedPlayer == nullptr || context.Identity == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        for (ROWOWScriptValue const& argument : arguments)
        {
            bool isWorn = false;
            for (auto const& itemGUIDByEquipPosition : context.Identity->ItemGUIDsByEquipPosition)
            {
                ROWOWItem const* roItem = GetROItem(context.AttachedPlayer->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDByEquipPosition.second)));
                if (roItem != nullptr && int64(roItem->ItemID) == argument.GetNumber())
                    isWorn = true;
            }
            if (isWorn == false)
                return ROWOWScriptValue::FromNumber(0);
        }
        return ROWOWScriptValue::FromNumber(1);
    });
    ROWOWSCRIPT->RegisterParameter("BaseJob", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        return ROWOWScriptValue::FromNumber(context.Identity != nullptr ? int64(GetBaseJobID(context.Identity->JobID)) : 0);
    });
    ROWOWSCRIPT->RegisterParameter("BaseClass", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        return ROWOWScriptValue::FromNumber(context.Identity != nullptr ? int64(GetBaseClassJobID(context.Identity->JobID)) : 0);
    });
    ROWOWSCRIPT->RegisterConstant("RC_All", ROWOW_RACE_ALL);
    ROWOWSCRIPT->RegisterConstant("Ele_All", ROWOW_ELEMENT_ALL);
    ROWOWSCRIPT->RegisterConstant("Size_All", ROWOW_SIZE_ALL);
    ROWOWSCRIPT->RegisterConstant("Class_Normal", ROWOW_CLASS_NORMAL);
    ROWOWSCRIPT->RegisterConstant("Class_Boss", ROWOW_CLASS_BOSS);
    ROWOWSCRIPT->RegisterConstant("Class_Guardian", ROWOW_CLASS_GUARDIAN);
    ROWOWSCRIPT->RegisterConstant("Class_All", ROWOW_CLASS_ALL);
    static char const* const WeaponTypeConstantNames[ROWOW_WEAPON_TYPE_MAX] =
    {
        "W_FIST", "W_DAGGER", "W_1HSWORD", "W_2HSWORD", "W_1HSPEAR", "W_2HSPEAR", "W_1HAXE", "W_2HAXE", "W_MACE", "W_2HMACE", "W_STAFF", "W_BOW",
        "W_KNUCKLE", "W_MUSICAL", "W_WHIP", "W_BOOK", "W_KATAR", "W_REVOLVER", "W_RIFLE", "W_GATLING", "W_SHOTGUN", "W_GRENADE", "W_HUUMA", "W_2HSTAFF",
    };
    for (int64 weaponType = 0; weaponType < ROWOW_WEAPON_TYPE_MAX; weaponType++)
        ROWOWSCRIPT->RegisterConstant(WeaponTypeConstantNames[weaponType], weaponType);

    // Compile the worn items' scripts: Script for every status calculation, EquipScript and UnEquipScript for putting on and taking off
    uint32 bonusScriptCount = 0;
    std::map<std::string, uint32> unavailableCommandCounts;
    if (ROWOWDataTable const* itemTable = GetDataTable(ROWOW_DATA_TABLE_ITEM))
    {
        int32 idColumnIndex = itemTable->GetColumnIndex("Id");
        int32 slotsColumnIndex = itemTable->GetColumnIndex("Slots");
        int32 scriptColumnIndex = itemTable->GetColumnIndex("Script");
        int32 equipScriptColumnIndex = itemTable->GetColumnIndex("EquipScript");
        int32 unEquipScriptColumnIndex = itemTable->GetColumnIndex("UnEquipScript");
        for (size_t rowIndex = 0; rowIndex < itemTable->Rows.size(); rowIndex++)
        {
            uint32 itemID = uint32(itemTable->GetInt(rowIndex, idColumnIndex, 0));
            auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
            if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
                continue;
            auto itemIter = ItemsByItemTemplateID.find(itemTemplateIDIter->second);
            if (itemIter == ItemsByItemTemplateID.end())
                continue;
            ROWOWItem& item = itemIter->second;
            item.Slots = uint8(std::clamp<int64>(itemTable->GetInt(rowIndex, slotsColumnIndex, 0), 0, ROWOW_MAX_SLOTS));
            if (item.Type != ROWOW_ITEM_TYPE_WEAPON && item.Type != ROWOW_ITEM_TYPE_ARMOR && item.Type != ROWOW_ITEM_TYPE_CARD && item.Type != ROWOW_ITEM_TYPE_AMMO)
                continue;
            CompilingItemID = itemID;
            std::string sourceName = "item " + std::to_string(itemID);
            auto compile = [&](int32 columnIndex, std::shared_ptr<ROWOWScript>& scriptOut)
            {
                std::string source = ROWOWDataTable::UnescapeText(itemTable->GetCell(rowIndex, columnIndex));
                if (source.empty() == true)
                    return;
                scriptOut = ROWOWSCRIPT->Compile(source, sourceName);
                if (scriptOut->Error.empty() == false)
                    LOG_ERROR("module.ROWOW", "ROWOWMod::LoadBonusData: a script of RO item {} didn't compile: {}", itemID, scriptOut->Error);
                for (std::string const& unavailableCommand : scriptOut->UnavailableCommands)
                    unavailableCommandCounts[unavailableCommand]++;
            };
            compile(scriptColumnIndex, item.BonusScript);
            compile(equipScriptColumnIndex, item.EquipScript);
            compile(unEquipScriptColumnIndex, item.UnEquipScript);
            if (item.BonusScript != nullptr)
                bonusScriptCount++;
        }
    }

    // Item combos: their items by AegisName, and their scripts
    if (ROWOWDataTable const* comboTable = GetDataTable(ROWOW_DATA_TABLE_ITEM_COMBO))
    {
        int32 scriptColumnIndex = comboTable->GetColumnIndex("Script");
        int32 comboColumnIndex = comboTable->GetColumnIndex("Combos_Combo");
        for (size_t rowIndex = 0; rowIndex < comboTable->Rows.size(); rowIndex++)
        {
            ROWOWItemCombo combo;
            bool isComplete = true;
            for (std::string const& aegisName : ROWOWDataTable::ParseTextList(comboTable->GetCell(rowIndex, comboColumnIndex)))
            {
                auto itemIDIter = ItemIDsByAegisName.find(ToLower(aegisName));
                if (itemIDIter == ItemIDsByAegisName.end())
                    isComplete = false;
                else
                    combo.ItemIDs.push_back(itemIDIter->second);
            }
            if (isComplete == false || combo.ItemIDs.size() < 2)
                continue;
            CompilingItemID = combo.ItemIDs[0];
            combo.Script = ROWOWSCRIPT->Compile(ROWOWDataTable::UnescapeText(comboTable->GetCell(rowIndex, scriptColumnIndex)), "combo " + std::to_string(rowIndex));
            for (uint32 itemID : combo.ItemIDs)
                ItemComboIndexesByItemID[itemID].push_back(ItemCombos.size());
            ItemCombos.push_back(combo);
        }
    }
    CompilingItemID = 0;

    std::string unknownBonusesText;
    for (auto const& unknownBonus : UnknownBonusItemIDs)
    {
        std::string itemIDsText;
        for (uint32 itemID : unknownBonus.second)
            itemIDsText += fmt::format("{}{}", itemIDsText.empty() == true ? "" : " ", itemID);
        unknownBonusesText += fmt::format("{}{} ({})", unknownBonusesText.empty() == true ? "" : ", ", unknownBonus.first, itemIDsText);
    }
    std::string unavailableCommandsText;
    for (auto const& unavailableCommandCount : unavailableCommandCounts)
        unavailableCommandsText += fmt::format("{}{} ({})", unavailableCommandsText.empty() == true ? "" : ", ", unavailableCommandCount.first, unavailableCommandCount.second);
    // The autobonus scripts the items name (script_add_autobonus, which rAthena does as an autobonus is first added)
    AutoBonusScriptsByText.clear();
    for (std::string const& text : AutoBonusScriptTexts)
    {
        std::shared_ptr<ROWOWScript> script = ROWOWSCRIPT->Compile(text, "autobonus");
        if (script->Error.empty() == false)
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadBonusData: the autobonus script '{}' didn't compile: {}", text, script->Error);
        AutoBonusScriptsByText[text] = script;
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadBonusData loaded {} equipment, card and ammo scripts, {} item combos and {} card enchantments", bonusScriptCount,
        ItemCombos.size(), CardItemIDsByEnchantmentID.size());
    if (unknownBonusesText.empty() == false)
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadBonusData: bonus types no rule takes yet (with their items): {}", unknownBonusesText);
    if (unavailableCommandsText.empty() == false)
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadBonusData: commands in worn items' scripts that do nothing yet: {}", unavailableCommandsText);
}

void ROWOWMod::PCBonus(ROWOWScriptContext& context, std::string const& type, std::vector<int64> const& values) const
{
    // pc_bonus..pc_bonus5, by the bonus type's name.  lr_flag is whose the bonus is (a left-hand weapon's, the ammo's, a shield's)
    ROWOWStatus& status = *context.BonusStatus;
    ROWOWBonus& bonus = status.Bonus;
    uint8 lrFlag = context.BonusLRFlag;
    bool isArrow = lrFlag == ROWOW_LR_FLAG_ARROW;
    auto typeIter = GetBonusTypesByLowerName().find(ToLower(type));
    if (typeIter == GetBonusTypesByLowerName().end() || typeIter->second == BONUS_DEFERRED)
    {
        ROWOWDeferredBonus deferredBonus;
        deferredBonus.Type = type;
        deferredBonus.Values = values;
        deferredBonus.LRFlag = lrFlag;
        bonus.Deferred.push_back(deferredBonus);
        return;
    }
    int64 val = values.empty() == false ? values.back() : 0; // The last value is the amount (pc_bonus2's val, after its type2)
    int64 type2 = values.size() >= 2 ? values[0] : 0;
    ROWOWWeaponBonus& weaponBonus = lrFlag == ROWOW_LR_FLAG_WEAPON ? bonus.LeftWeapon : bonus.RightWeapon;
    auto capValue = [](int64 value, int64 minimum, int64 maximum) { return int32(std::clamp(value, minimum, maximum)); };
    auto addParam = [&](uint8 statIndex) { if (isArrow == false) bonus.ParamBonus[statIndex] += int32(val); };
    // pc_bonus_addeff and pc_bonus_addeff_onskill (PC_BONUS_CHK_SC: a status the status database has).  Without a duration, the duration is
    // the status's skill's level 7 Duration2
    auto isStatus = [this](int64 type) { return GetStatusChangeDB(int32(type)) != nullptr; };
    auto addEffect = [this](std::vector<ROWOWAddEffect>& effects, int32 type, int32 rate, int32 arrowRate, uint8 flag, uint32 duration)
    {
        if ((flag & (ROWOW_ATF_SHORT | ROWOW_ATF_LONG)) == 0)
            flag |= ROWOW_ATF_SHORT | ROWOW_ATF_LONG; // Default range: both
        if ((flag & (ROWOW_ATF_TARGET | ROWOW_ATF_SELF)) == 0)
            flag |= ROWOW_ATF_TARGET; // Default target: enemy.
        if ((flag & (ROWOW_ATF_WEAPON | ROWOW_ATF_MAGIC | ROWOW_ATF_MISC)) == 0)
            flag |= ROWOW_ATF_WEAPON; // Default type: weapon.
        if (duration == 0)
            duration = uint32(std::max<int64>(0, SkillGetTime2(StatusDBGetSkill(type), 7)));
        for (ROWOWAddEffect& effect : effects)
        {
            if (effect.Type == type && effect.Flag == flag)
            {
                effect.Rate = int32(std::min<int64>(int64(effect.Rate) + rate, INT_MAX));
                effect.ArrowRate = int32(std::min<int64>(int64(effect.ArrowRate) + arrowRate, SHRT_MAX));
                effect.Duration = std::max(effect.Duration, duration);
                return;
            }
        }
        effects.push_back({ type, rate, arrowRate, flag, duration });
    };
    auto addEffectOnSkill = [this](int32 type, int32 rate, uint16 skillID, uint8 target, uint32 duration, ROWOWBonus& onSkillBonus)
    {
        if (duration == 0)
            duration = uint32(std::max<int64>(0, SkillGetTime2(StatusDBGetSkill(type), 7)));
        for (ROWOWAddEffectOnSkill& effect : onSkillBonus.AddEffOnSkill)
        {
            if (effect.Type == type && effect.SkillID == skillID && effect.Target == target)
            {
                effect.Rate = int32(std::min<int64>(int64(effect.Rate) + rate, INT_MAX));
                effect.Duration = std::max(effect.Duration, duration);
                return;
            }
        }
        onSkillBonus.AddEffOnSkill.push_back({ type, rate, skillID, target, duration });
    };
    switch (typeIter->second)
    {
        case BONUS_ADD_EFF:
            // bonus2 bAddEff,eff,n; bonus3 bAddEff,eff,n,y; bonus4 bAddEff,eff,n,y,t
            if (values.size() >= 2 && isStatus(values[0]) == true)
            {
                int32 rate = int32(values[1]);
                uint8 flag = values.size() >= 3 ? uint8(values[2]) : 0;
                uint32 duration = values.size() >= 4 ? uint32(std::max<int64>(0, values[3])) : 0;
                addEffect(bonus.AddEff, int32(values[0]), isArrow == false ? rate : 0, isArrow == true ? rate : 0, flag, duration);
            }
            break;
        case BONUS_ADD_EFF2:
            // bonus2 bAddEff2,eff,n
            if (values.size() >= 2 && isStatus(values[0]) == true)
                addEffect(bonus.AddEff, int32(values[0]), isArrow == false ? int32(values[1]) : 0, isArrow == true ? int32(values[1]) : 0, ROWOW_ATF_SELF, 0);
            break;
        case BONUS_ADD_EFF_WHEN_HIT:
            // bonus2 bAddEffWhenHit,eff,n; bonus3 bAddEffWhenHit,eff,n,y; bonus4 bAddEffWhenHit,eff,n,y,t
            if (values.size() >= 2 && isStatus(values[0]) == true && isArrow == false)
                addEffect(bonus.AddEffAtked, int32(values[0]), int32(values[1]), 0, values.size() >= 3 ? uint8(values[2]) : 0,
                    values.size() >= 4 ? uint32(std::max<int64>(0, values[3])) : 0);
            break;
        case BONUS_ADD_EFF_ON_SKILL:
            // bonus3 bAddEffOnSkill,sk,eff,n; bonus4 bAddEffOnSkill,sk,eff,n,y; bonus5 bAddEffOnSkill,sk,eff,n,y,t
            if (values.size() >= 3 && isStatus(values[1]) == true && isArrow == false)
                addEffectOnSkill(int32(values[1]), int32(values[2]), uint16(values[0]), values.size() >= 4 ? uint8(values[3]) : uint8(ROWOW_ATF_TARGET),
                    values.size() >= 5 ? uint32(std::max<int64>(0, values[4])) : 0, bonus);
            break;
        case BONUS2_RES_EFF:
            // bonus2 bResEff,eff,n (pc_bonus_itembonus: they add up)
            if (values.size() >= 2 && isArrow == false)
                bonus.ResEff[int32(values[0])] += int32(values[1]);
            break;
        case BONUS_AUTO_SPELL:
        case BONUS_AUTO_SPELL_WHEN_HIT:
        {
            // bonus3 bAutoSpell,sk,y,n; bonus4 bAutoSpell,sk,y,n,i; bonus5 bAutoSpell,sk,y,n,bf,i (and the same of bAutoSpellWhenHit, triggered by
            // normal attacks and skills).  Without i, a support or self skill (that isn't a combo) goes on the caster, others on the target
            if (values.size() < 3 || isArrow == true)
                break;
            uint16 skillID = uint16(values[0]);
            uint8 flag = 0;
            int16 battleFlag = typeIter->second == BONUS_AUTO_SPELL_WHEN_HIT ? int16(ROWOW_BF_NORMAL | ROWOW_BF_SKILL) : 0;
            if (values.size() == 3)
            {
                ROWOWSkill const* autoSkill = GetSkill(skillID);
                bool isSelf = autoSkill != nullptr && ((autoSkill->Inf & ROWOW_INF_SUPPORT_SKILL) != 0 ||
                    ((autoSkill->Inf & ROWOW_INF_SELF_SKILL) != 0 && autoSkill->Inf2[ROWOW_INF2_NOTARGETSELF] == false)); // Support or Self (non-auto-target) skills should pick self.
                flag = isSelf == true ? ROWOW_AUTOSPELL_FORCE_SELF : ROWOW_AUTOSPELL_FORCE_TARGET;
            }
            else
            {
                flag = uint8(values.back() & ROWOW_AUTOSPELL_FORCE_ALL);
                if (values.size() >= 5)
                    battleFlag = int16(values[3]);
            }
            AddAutoSpell(typeIter->second == BONUS_AUTO_SPELL ? bonus.AutoSpell : bonus.AutoSpell2, skillID, uint16(values[1]), int16(values[2]), battleFlag,
                context.EquipCardID, flag);
            break;
        }
        case BONUS_AUTO_SPELL_ON_SKILL:
        {
            // bonus4 bAutoSpellOnSkill,sk,x,y,n; bonus5 bAutoSpellOnSkill,sk,x,y,n,i: when using sk, skill x at level y (n in 1000).  Without i, a
            // support or self skill goes on the target of sk, others on the caster
            if (values.size() < 4 || isArrow == true)
                break;
            uint8 flag = 0;
            if (values.size() == 4)
            {
                ROWOWSkill const* autoSkill = GetSkill(uint16(values[1]));
                bool isSupport = autoSkill != nullptr && ((autoSkill->Inf & ROWOW_INF_SUPPORT_SKILL) != 0 ||
                    ((autoSkill->Inf & ROWOW_INF_SELF_SKILL) != 0 && autoSkill->Inf2[ROWOW_INF2_NOTARGETSELF] == false));
                flag = isSupport == true ? ROWOW_AUTOSPELL_FORCE_TARGET : ROWOW_AUTOSPELL_FORCE_SELF;
            }
            else
                flag = uint8(values[4] & ROWOW_AUTOSPELL_FORCE_ALL);
            // pc_bonus_autospell_onskill
            if (values[3] == 0)
                break;
            ROWOWAutoSpell autoSpell;
            autoSpell.TriggerSkillID = uint16(values[0]);
            autoSpell.SkillID = uint16(values[1]);
            autoSpell.Level = uint16(values[2]);
            autoSpell.Rate = int16(std::clamp<int64>(values[3], -1000, 1000));
            autoSpell.CardID = context.EquipCardID;
            autoSpell.Flag = flag;
            bonus.AutoSpell3.push_back(autoSpell);
            break;
        }
        case BONUS_STR: addParam(ROWOW_STAT_STR); break;
        case BONUS_AGI: addParam(ROWOW_STAT_AGI); break;
        case BONUS_VIT: addParam(ROWOW_STAT_VIT); break;
        case BONUS_INT: addParam(ROWOW_STAT_INT); break;
        case BONUS_DEX: addParam(ROWOW_STAT_DEX); break;
        case BONUS_LUK: addParam(ROWOW_STAT_LUK); break;
        case BONUS_ALL_STATS:
            for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
                addParam(statIndex);
            break;
        case BONUS_AGI_VIT: addParam(ROWOW_STAT_AGI); addParam(ROWOW_STAT_VIT); break;
        case BONUS_AGI_DEX_STR: addParam(ROWOW_STAT_AGI); addParam(ROWOW_STAT_DEX); addParam(ROWOW_STAT_STR); break;
        case BONUS_ATK1:
            if (lrFlag == ROWOW_LR_FLAG_NONE)
                status.WeaponAttack = capValue(status.WeaponAttack + val, 0, USHRT_MAX);
            else if (lrFlag == ROWOW_LR_FLAG_WEAPON)
                status.LeftWeaponAttack = capValue(status.LeftWeaponAttack + val, 0, USHRT_MAX);
            break;
        case BONUS_ATK2:
            if (lrFlag == ROWOW_LR_FLAG_NONE)
                status.WeaponRefineAttack = capValue(status.WeaponRefineAttack + val, 0, USHRT_MAX);
            else if (lrFlag == ROWOW_LR_FLAG_WEAPON)
                status.LeftWeaponRefineAttack = capValue(status.LeftWeaponRefineAttack + val, 0, USHRT_MAX);
            break;
        case BONUS_BASE_ATK: if (isArrow == false) status.BaseAttack += int32(val); break;
        case BONUS_DEF1: if (isArrow == false) status.Def = capValue(status.Def + val, CHAR_MIN, CHAR_MAX); break;
        case BONUS_DEF2: if (isArrow == false) status.Def2 = capValue(status.Def2 + val, SHRT_MIN, SHRT_MAX); break;
        case BONUS_MDEF1: if (isArrow == false) status.MDef = capValue(status.MDef + val, CHAR_MIN, CHAR_MAX); break;
        case BONUS_MDEF2: if (isArrow == false) status.MDef2 = capValue(status.MDef2 + val, SHRT_MIN, SHRT_MAX); break;
        case BONUS_HIT:
            if (isArrow == false)
                status.Hit = capValue(status.Hit + val, SHRT_MIN, SHRT_MAX);
            else
                bonus.ArrowHit += int32(val);
            break;
        case BONUS_FLEE1: if (isArrow == false) status.Flee = capValue(status.Flee + val, SHRT_MIN, SHRT_MAX); break;
        case BONUS_FLEE2: if (isArrow == false) status.Flee2 = capValue(status.Flee2 + val * 10, SHRT_MIN, SHRT_MAX); break;
        case BONUS_CRITICAL:
            if (isArrow == false)
                status.Critical = capValue(status.Critical + val * 10, SHRT_MIN, SHRT_MAX);
            else
                bonus.ArrowCri += int32(val * 10);
            break;
        case BONUS_ATK_ELE:
            if (IsBonusIndexValid(val, ROWOW_ELEMENT_COUNT) == false)
                break;
            if (lrFlag == ROWOW_LR_FLAG_ARROW)
            {
                // A bow or gun takes its ammo's element, and any other weapon gets it for its arrow attacks
                if (status.IsArrowAttack == true)
                    status.WeaponElement = uint8(val);
                else
                    status.ArrowElement = int32(val);
            }
            else if (lrFlag == ROWOW_LR_FLAG_WEAPON)
                status.LeftWeaponElement = uint8(val);
            else
                status.WeaponElement = uint8(val);
            break;
        case BONUS_DEF_ELE: if (isArrow == false && IsBonusIndexValid(val, ROWOW_ELEMENT_COUNT) == true) status.DefElement = uint8(val); break;
        case BONUS_MAX_HP: if (isArrow == false) bonus.HP += int32(val); break;
        case BONUS_MAX_SP: if (isArrow == false) bonus.SP += int32(val); break;
        case BONUS_MAX_HP_RATE: if (isArrow == false) bonus.HPRate += int32(val); break;
        case BONUS_MAX_SP_RATE: if (isArrow == false) bonus.SPRate += int32(val); break;
        case BONUS_SP_RATE: if (isArrow == false) bonus.UseSPRate += int32(val); break;
        case BONUS_ATTACK_RANGE:
            if (lrFlag == ROWOW_LR_FLAG_ARROW)
            {
                if (status.IsArrowAttack == true)
                    status.Range += int32(val);
            }
            else
                status.Range += int32(val); // (rhw and lhw ranges are one here; the longer counts)
            break;
        case BONUS_ASPD: if (isArrow == false) bonus.AspdAdd -= int32(10 * val); break;
        case BONUS_ASPD_RATE: if (isArrow == false) status.AspdRate -= int32(10 * val); break;
        case BONUS_HP_RECOV_RATE: if (isArrow == false) bonus.HPRecovRate += int32(val); break;
        case BONUS_SP_RECOV_RATE: if (isArrow == false) bonus.SPRecovRate += int32(val); break;
        case BONUS_CRITICAL_DEF: if (isArrow == false) bonus.CriticalDef += int32(val); break;
        case BONUS_NEAR_ATK_DEF: if (isArrow == false) bonus.NearAttackDefRate += int32(val); break;
        case BONUS_LONG_ATK_DEF: if (isArrow == false) bonus.LongAttackDefRate += int32(val); break;
        case BONUS_DOUBLE_RATE: if (lrFlag == ROWOW_LR_FLAG_NONE && bonus.DoubleRate < val) bonus.DoubleRate = int32(val); break;
        case BONUS_DOUBLE_ADD_RATE: if (lrFlag == ROWOW_LR_FLAG_NONE) bonus.DoubleAddRate += int32(val); break;
        case BONUS_MATK_RATE: if (isArrow == false) bonus.MatkRate += int32(val); break;
        case BONUS_IGNORE_DEF_ELE:
        case BONUS_IGNORE_DEF_RACE:
        case BONUS_IGNORE_DEF_CLASS:
        case BONUS_DEF_RATIO_ATK_ELE:
        case BONUS_DEF_RATIO_ATK_RACE:
        case BONUS_DEF_RATIO_ATK_CLASS:
        {
            ROWOWBonusType bonusType = typeIter->second;
            int32 maxIndex = (bonusType == BONUS_IGNORE_DEF_ELE || bonusType == BONUS_DEF_RATIO_ATK_ELE) ? ROWOW_ELEMENT_MAX
                : ((bonusType == BONUS_IGNORE_DEF_RACE || bonusType == BONUS_DEF_RATIO_ATK_RACE) ? ROWOW_RACE_MAX : ROWOW_CLASS_MAX);
            if (IsBonusIndexValid(val, maxIndex) == false || (lrFlag != ROWOW_LR_FLAG_NONE && lrFlag != ROWOW_LR_FLAG_WEAPON))
                break;
            uint32 bit = uint32(1) << val;
            switch (bonusType)
            {
                case BONUS_IGNORE_DEF_ELE: weaponBonus.IgnoreDefEle |= bit; break;
                case BONUS_IGNORE_DEF_RACE: weaponBonus.IgnoreDefRace |= bit; break;
                case BONUS_IGNORE_DEF_CLASS: weaponBonus.IgnoreDefClass |= bit; break;
                case BONUS_DEF_RATIO_ATK_ELE: weaponBonus.DefRatioAtkEle |= bit; break;
                case BONUS_DEF_RATIO_ATK_RACE: weaponBonus.DefRatioAtkRace |= bit; break;
                default: weaponBonus.DefRatioAtkClass |= bit; break;
            }
            break;
        }
        case BONUS_MAGIC_ATK_DEF: if (isArrow == false) bonus.MagicDefRate += int32(val); break;
        case BONUS_MISC_ATK_DEF: if (isArrow == false) bonus.MiscDefRate += int32(val); break;
        case BONUS_PERFECT_HIT_RATE: if (isArrow == false && bonus.PerfectHit < val) bonus.PerfectHit = int32(val); break;
        case BONUS_PERFECT_HIT_ADD_RATE: if (isArrow == false) bonus.PerfectHitAdd += int32(val); break;
        case BONUS_CRITICAL_RATE: if (isArrow == false) bonus.CriticalRate += int32(val); break;
        case BONUS_HIT_RATE: if (isArrow == false) bonus.HitRate += int32(val); break;
        case BONUS_FLEE_RATE: if (isArrow == false) bonus.FleeRate += int32(val); break;
        case BONUS_FLEE2_RATE: if (isArrow == false) bonus.Flee2Rate += int32(val); break;
        case BONUS_DEF_RATE: if (isArrow == false) bonus.DefRate += int32(val); break;
        case BONUS_DEF2_RATE: if (isArrow == false) bonus.Def2Rate += int32(val); break;
        case BONUS_MDEF_RATE: if (isArrow == false) bonus.MdefRate += int32(val); break;
        case BONUS_MDEF2_RATE: if (isArrow == false) bonus.Mdef2Rate += int32(val); break;
        case BONUS_CRIT_ATK_RATE: if (isArrow == false) bonus.CritAtkRate += int32(val); break;
        case BONUS_LONG_ATK_RATE: if (isArrow == false) bonus.LongAttackAtkRate += int32(val); break;
        case BONUS_SHORT_ATK_RATE: if (isArrow == false) bonus.ShortAttackAtkRate += int32(val); break;
        case BONUS_NO_SIZEFIX: if (isArrow == false) bonus.NoSizeFix = true; break;
        case BONUS_CASTRATE:
            if (values.size() >= 2)
            {
                // bonus2 bCastrate,<skill>,<rate> is a skill's own, which skill_castfix reads
                ROWOWDeferredBonus deferredBonus;
                deferredBonus.Type = type;
                deferredBonus.Values = values;
                deferredBonus.LRFlag = lrFlag;
                bonus.Deferred.push_back(deferredBonus);
            }
            else if (isArrow == false)
                bonus.CastRate += int32(val);
            break;
        case BONUS_SPLASH_RANGE: if (bonus.SplashRange < val) bonus.SplashRange = int32(val); break;
        case BONUS_SPLASH_ADD_RANGE: bonus.SplashAddRange += int32(val); break;
        case BONUS2_ADD_ELE:
        case BONUS2_ADD_RACE:
        case BONUS2_ADD_CLASS:
        case BONUS2_ADD_SIZE:
        {
            // A shield's go to the right hand, the ammo's to its own arrow tables
            ROWOWBonusType bonusType = typeIter->second;
            int32 maxIndex = bonusType == BONUS2_ADD_ELE ? ROWOW_ELEMENT_MAX : (bonusType == BONUS2_ADD_RACE ? ROWOW_RACE_MAX : (bonusType == BONUS2_ADD_CLASS ? ROWOW_CLASS_MAX : ROWOW_SIZE_MAX));
            if (IsBonusIndexValid(type2, maxIndex) == false)
                break;
            ROWOWWeaponBonus& handBonus = lrFlag == ROWOW_LR_FLAG_WEAPON ? bonus.LeftWeapon : bonus.RightWeapon;
            int32* table = nullptr;
            switch (bonusType)
            {
                case BONUS2_ADD_ELE: table = isArrow == true ? bonus.ArrowAddEle : handBonus.AddEle; break;
                case BONUS2_ADD_RACE: table = isArrow == true ? bonus.ArrowAddRace : handBonus.AddRace; break;
                case BONUS2_ADD_CLASS: table = isArrow == true ? bonus.ArrowAddClass : handBonus.AddClass; break;
                default: table = isArrow == true ? bonus.ArrowAddSize : handBonus.AddSize; break;
            }
            table[type2] += int32(val);
            break;
        }
        case BONUS2_SUB_ELE: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_ELEMENT_MAX) == true) bonus.SubEle[type2] += int32(val); break;
        case BONUS2_SUB_RACE: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_RACE_MAX) == true) bonus.SubRace[type2] += int32(val); break;
        case BONUS2_SUB_CLASS: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_CLASS_MAX) == true) bonus.SubClass[type2] += int32(val); break;
        case BONUS2_SUB_SIZE: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_SIZE_MAX) == true) bonus.SubSize[type2] += int32(val); break;
        case BONUS2_ADD_DAMAGE_CLASS:
        {
            // pc_bonus_itembonus: the same monster's rates add up
            ROWOWWeaponBonus& handBonus = lrFlag != ROWOW_LR_FLAG_NONE ? bonus.LeftWeapon : bonus.RightWeapon;
            auto damageIter = std::find_if(handBonus.AddDamageClass.begin(), handBonus.AddDamageClass.end(), [type2](auto const& entry) { return int64(entry.first) == type2; });
            if (damageIter != handBonus.AddDamageClass.end())
                damageIter->second += int32(val);
            else
                handBonus.AddDamageClass.push_back(std::make_pair(uint32(type2), int32(val)));
            break;
        }
        case BONUS2_ADD_DEF_MONSTER:
        {
            if (isArrow == true)
                break;
            auto defIter = std::find_if(bonus.AddDefMonster.begin(), bonus.AddDefMonster.end(), [type2](auto const& entry) { return int64(entry.first) == type2; });
            if (defIter != bonus.AddDefMonster.end())
                defIter->second += int32(val);
            else
                bonus.AddDefMonster.push_back(std::make_pair(uint32(type2), int32(val)));
            break;
        }
        case BONUS2_WEAPON_ATK: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_WEAPON_TYPE_MAX) == true) bonus.WeaponAtk[type2] += int32(val); break;
        case BONUS2_WEAPON_DAMAGE_RATE: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_WEAPON_TYPE_MAX) == true) bonus.WeaponDamageRate[type2] += int32(val); break;
        case BONUS2_CRITICAL_ADDRACE: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_RACE_MAX) == true) bonus.CriticalAddRace[type2] += int32(val * 10); break;
        case BONUS2_IGNORE_DEF_RACE_RATE: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_RACE_MAX) == true) bonus.IgnoreDefByRace[type2] += int32(val); break;
        case BONUS2_IGNORE_DEF_CLASS_RATE: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_CLASS_MAX) == true) bonus.IgnoreDefByClass[type2] += int32(val); break;
        case BONUS2_EXP_ADDRACE: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_RACE_MAX) == true) bonus.ExpAddRace[type2] += int32(val); break;
        case BONUS2_EXP_ADDCLASS: if (isArrow == false && IsBonusIndexValid(type2, ROWOW_CLASS_MAX) == true) bonus.ExpAddClass[type2] += int32(val); break;
        case BONUS_CRITICAL_RANGEATK:
            if (isArrow == false)
                bonus.CriticalRangeAtk += int32(val * 10);
            else
                bonus.ArrowCri += int32(val * 10);
            break;
        case BONUS_NOT_PRE_RENEWAL: // pc_bonus: not supported in Pre-Renewal mode
        default:
            break;
    }
}

void ROWOWMod::StatusCalcPCItems(Player* player, ROWOWIdentity const& identity, ROWOWStatus& status) const
{
    // The item part of status_calc_pc_sub: each worn item's DEF, weapon ATK, refine and script (an item worn in two positions counts
    // once, as pc_is_same_equip_index skips it), then the ammo's, the combos', and the cards'.  Costumes only change the look
    const_cast<ROWOWIdentity&>(identity).ItemSkillLevelsByID.clear(); // pc_calc_skilltree's skill_flag clean: the scripts below give them again
    ROWOWScriptContext context;
    context.AttachedPlayer = player;
    context.Identity = const_cast<ROWOWIdentity*>(&identity);
    context.BonusStatus = &status;
    auto runScript = [&](ROWOWScript const* script, Item const* item, uint8 lrFlag, uint32 equipPositions, uint32 cardID)
    {
        if (script == nullptr || script->Error.empty() == false)
            return;
        context.EquipItem = item;
        context.BonusLRFlag = lrFlag;
        context.EquipPositions = equipPositions;
        context.EquipCardID = cardID;
        context.Stack.clear();
        context.InstructionIndex = 0;
        context.InstructionCount = 0;
        context.IsStopped = false;
        context.Error.clear();
        ROWOWSCRIPT->Run(*script, context, true);
    };

    struct WornItem
    {
        uint32 Positions = 0;
        Item const* ItemObject = nullptr;
        ROWOWItem const* RO = nullptr;
    };
    std::vector<WornItem> wornItems;
    std::unordered_set<ObjectGuid::LowType> countedItemGUIDs;
    Item const* ammoItem = nullptr;
    for (auto const& itemGUIDByEquipPosition : identity.ItemGUIDsByEquipPosition)
    {
        if ((itemGUIDByEquipPosition.first & ROWOW_EQUIP_POSITION_COSTUME) != 0)
            continue;
        if (countedItemGUIDs.insert(itemGUIDByEquipPosition.second).second == false)
            continue;
        Item const* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDByEquipPosition.second));
        ROWOWItem const* roItem = GetROItem(item);
        if (roItem == nullptr)
            continue;
        if (itemGUIDByEquipPosition.first == ROWOW_EQUIP_POSITION_AMMO)
        {
            ammoItem = item;
            continue;
        }
        WornItem wornItem;
        wornItem.Positions = itemGUIDByEquipPosition.first;
        wornItem.ItemObject = item;
        wornItem.RO = roItem;
        wornItems.push_back(wornItem);
    }

    int32 refineDef = 0;
    int32 rightWeaponRange = 0;
    int32 leftWeaponRange = 0;
    for (WornItem const& wornItem : wornItems)
    {
        ROWOWItem const& roItem = *wornItem.RO;
        status.Def += roItem.Defense;
        uint32 refine = std::min<uint32>(GetItemRefine(wornItem.ItemObject), ROWOW_MAX_REFINE);
        if (roItem.IsWeapon == true)
        {
            int32 weaponLevel = std::clamp(roItem.WeaponLevel, 1, ROWOW_REFINE_TYPE_COUNT - 1);
            ROWOWRefineBonus const& refineBonus = RefineBonuses[weaponLevel][refine];
            bool isLeftHand = wornItem.Positions == ROWOW_EQUIP_POSITION_HAND_L;
            if (isLeftHand == true)
            {
                // Left-hand weapon
                status.LeftWeaponAttack += roItem.Attack;
                status.LeftWeaponRefineAttack += refineBonus.Bonus / 100;
                status.Bonus.LeftWeapon.Overrefine = refineBonus.RandomBonus / 100;
                leftWeaponRange += roItem.Range;
                status.LeftWeaponType = roItem.WeaponType;
                status.LeftWeaponLevel = roItem.WeaponLevel;
            }
            else
            {
                status.WeaponAttack += roItem.Attack;
                status.WeaponRefineAttack += refineBonus.Bonus / 100;
                status.Bonus.RightWeapon.Overrefine = refineBonus.RandomBonus / 100;
                rightWeaponRange += roItem.Range;
                if ((wornItem.Positions & ROWOW_EQUIP_POSITION_HAND_R) != 0)
                {
                    status.RightWeaponType = roItem.WeaponType;
                    status.WeaponLevel = roItem.WeaponLevel;
                }
            }
        }
        else if (roItem.IsArmor == true)
            refineDef += RefineBonuses[0][refine].Bonus;
    }
    status.WeaponType = (status.LeftWeaponType != ROWOW_WEAPON_TYPE_FIST && status.RightWeaponType != ROWOW_WEAPON_TYPE_FIST) ? ROWOW_WEAPON_TYPE_MAX : status.RightWeaponType;
    status.Range = std::max(rightWeaponRange, leftWeaponRange);
    status.IsArrowAttack = status.RightWeaponType == ROWOW_WEAPON_TYPE_BOW || (status.RightWeaponType >= ROWOW_WEAPON_TYPE_REVOLVER && status.RightWeaponType <= ROWOW_WEAPON_TYPE_GRENADE);

    // The scripts run once the weapons are known (an ammo's element goes to a bow or gun), each item's in its hand's
    for (WornItem const& wornItem : wornItems)
    {
        uint8 lrFlag = ROWOW_LR_FLAG_NONE;
        if (wornItem.Positions == ROWOW_EQUIP_POSITION_HAND_L)
            lrFlag = wornItem.RO->IsWeapon == true ? ROWOW_LR_FLAG_WEAPON : ROWOW_LR_FLAG_SHIELD;
        runScript(wornItem.RO->BonusScript.get(), wornItem.ItemObject, lrFlag, wornItem.Positions, 0);

        // A forged weapon (CARD0_FORGE): its Star Crumbs' ATK, a famous smith's +10, and its element unless its script gave it one
        int32 starCrumbs = 0, forgeElement = ROWOW_ELEMENT_NEUTRAL;
        if (wornItem.RO->IsWeapon == true && GetItemForge(wornItem.ItemObject, starCrumbs, forgeElement) == true)
        {
            bool isLeftHand = lrFlag == ROWOW_LR_FLAG_WEAPON;
            ROWOWWeaponBonus& wd = isLeftHand == true ? status.Bonus.LeftWeapon : status.Bonus.RightWeapon;
            wd.Star += starCrumbs * 5;
            if (wd.Star >= 15)
                wd.Star = 40; // 3 Star Crumbs now give +40 dmg
            if (PCFameRank(wornItem.ItemObject->GetGuidValue(ITEM_FIELD_CREATOR).GetCounter(), ROWOW_FAME_BLACKSMITH) != 0)
                wd.Star += 10;
            uint8& weaponElement = isLeftHand == true ? status.LeftWeaponElement : status.WeaponElement;
            if (weaponElement == ROWOW_ELEMENT_NEUTRAL) // Do not overwrite element from previous bonuses.
                weaponElement = uint8(forgeElement);
        }
    }

    // The ammo: its ATK, and its script as the ammo's
    if (ROWOWItem const* roAmmo = GetROItem(ammoItem))
    {
        status.ArrowAttack += roAmmo->Attack;
        status.AmmoType = roAmmo->AmmoType;
        runScript(roAmmo->BonusScript.get(), ammoItem, ROWOW_LR_FLAG_ARROW, ROWOW_EQUIP_POSITION_AMMO, 0);
    }

    // Pet Bonus: the script of the pet that's out, which reads its intimacy.  (status_calc_pc_sub's pd->bonus is petskillbonus, a SupportScript's)
    if (identity.Pet != nullptr && identity.Pet->DB != nullptr)
        runScript(identity.Pet->DB->BonusScript.get(), nullptr, ROWOW_LR_FLAG_NONE, 0, 0);

    // Item combos: every combo whose items are all worn
    std::unordered_set<uint32> wornItemIDs;
    for (WornItem const& wornItem : wornItems)
        wornItemIDs.insert(wornItem.RO->ItemID);
    std::unordered_set<size_t> ranComboIndexes;
    for (uint32 wornItemID : wornItemIDs)
    {
        auto comboIndexesIter = ItemComboIndexesByItemID.find(wornItemID);
        if (comboIndexesIter == ItemComboIndexesByItemID.end())
            continue;
        for (size_t comboIndex : comboIndexesIter->second)
        {
            ROWOWItemCombo const& combo = ItemCombos[comboIndex];
            if (ranComboIndexes.insert(comboIndex).second == false)
                continue;
            bool isComplete = std::all_of(combo.ItemIDs.begin(), combo.ItemIDs.end(), [&wornItemIDs](uint32 itemID) { return wornItemIDs.count(itemID) != 0; });
            if (isComplete == false)
                continue;
            // current_equip_combo_pos: where the combo's items are worn
            uint32 comboPositions = 0;
            for (WornItem const& wornItem : wornItems)
                if (std::find(combo.ItemIDs.begin(), combo.ItemIDs.end(), wornItem.RO->ItemID) != combo.ItemIDs.end())
                    comboPositions |= wornItem.Positions;
            runScript(combo.Script.get(), nullptr, ROWOW_LR_FLAG_NONE, comboPositions, 0);
        }
    }

    // What the equipment gave the stats so far is the equipment's (param_equip), and the cards' come after it
    std::copy(std::begin(status.Bonus.ParamBonus), std::end(status.Bonus.ParamBonus), std::begin(status.Bonus.ParamEquip));
    std::fill(std::begin(status.Bonus.ParamBonus), std::end(status.Bonus.ParamBonus), 0);
    status.Def += (refineDef + 50) / 100;

    // The cards, each in its item's hand
    for (WornItem const& wornItem : wornItems)
    {
        for (uint8 slot = 0; slot < ROWOW_MAX_SLOTS; slot++)
        {
            uint32 cardItemID = GetItemCard(wornItem.ItemObject, slot);
            if (cardItemID == 0)
                continue;
            auto cardTemplateIDIter = ItemTemplateIDsByItemID.find(cardItemID);
            auto cardIter = cardTemplateIDIter != ItemTemplateIDsByItemID.end() ? ItemsByItemTemplateID.find(cardTemplateIDIter->second) : ItemsByItemTemplateID.end();
            if (cardIter == ItemsByItemTemplateID.end())
                continue;
            uint8 lrFlag = (wornItem.Positions == ROWOW_EQUIP_POSITION_HAND_L && wornItem.RO->IsWeapon == true) ? ROWOW_LR_FLAG_WEAPON : ROWOW_LR_FLAG_NONE;
            runScript(cardIter->second.BonusScript.get(), wornItem.ItemObject, lrFlag, wornItem.Positions, cardItemID);
        }
    }

    // The autobonuses going (pc_delautobonus's restore)
    StatusCalcPCAutoBonuses(player, identity, status);

    // The statuses' scripts (status.yml's Script, like the foods' bonus bStr, getstatus(SC_STRFOOD, 1)), and the usable item an
    // SC_ITEMSCRIPT runs the script of
    if (ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player)))
    {
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_ITEMSCRIPT))
        {
            auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(uint32(sce->Val1));
            auto itemIter = itemTemplateIDIter != ItemTemplateIDsByItemID.end() ? ItemsByItemTemplateID.find(itemTemplateIDIter->second) : ItemsByItemTemplateID.end();
            if (itemIter != ItemsByItemTemplateID.end())
                runScript(itemIter->second.UseScript.get(), nullptr, ROWOW_LR_FLAG_NONE, 0, 0);
        }
        for (auto const& entryByType : sc->EntriesByType)
            if (ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(entryByType.first))
                runScript(scdb->Script.get(), nullptr, ROWOW_LR_FLAG_NONE, 0, 0);
    }
}

uint32 ROWOWMod::GetItemCard(Item const* item, uint8 slot)
{
    if (item == nullptr || slot >= ROWOW_MAX_SLOTS)
        return 0;
    auto cardItemIDIter = ROWOW->CardItemIDsByEnchantmentID.find(item->GetEnchantmentId(CardEnchantmentSlots[slot]));
    return cardItemIDIter != ROWOW->CardItemIDsByEnchantmentID.end() ? cardItemIDIter->second : 0;
}

void ROWOWMod::SetItemCard(Player* player, Item* item, uint8 slot, uint32 cardItemID) const
{
    if (slot >= ROWOW_MAX_SLOTS)
        return;
    uint32 enchantmentID = 0;
    auto cardTemplateIDIter = ItemTemplateIDsByItemID.find(cardItemID);
    if (cardTemplateIDIter != ItemTemplateIDsByItemID.end())
    {
        auto cardIter = ItemsByItemTemplateID.find(cardTemplateIDIter->second);
        if (cardIter != ItemsByItemTemplateID.end())
            enchantmentID = cardIter->second.CardEnchantmentID;
    }
    item->SetEnchantment(CardEnchantmentSlots[slot], enchantmentID, 0, 0);
    item->SetState(ITEM_CHANGED, player);
}

bool ROWOWMod::PCInsertCard(Player* player, Item* card, Item* equipment)
{
    // pc_insert_card
    ROWOWItem const* roEquipment = GetROItem(equipment);
    ROWOWItem const* roCard = GetROItem(card);
    if (roEquipment == nullptr || roCard == nullptr)
        return false;
    if (roEquipment->Type != ROWOW_ITEM_TYPE_WEAPON && roEquipment->Type != ROWOW_ITEM_TYPE_ARMOR)
        return false; // only weapons and armor are allowed
    if (roCard->Type != ROWOW_ITEM_TYPE_CARD || roCard->CardEnchantmentID == 0)
        return false; // must be a card
    if (IsItemIdentified(equipment) == false)
        return false; // target must be identified
    int32 starCrumbs = 0, forgeElement = 0;
    if (GetItemForge(equipment, starCrumbs, forgeElement) == true)
        return false; // itemdb_isspecial: a forged weapon's card slots hold its forge
    if ((roEquipment->Locations & roCard->Locations) == 0)
        return false; // card cannot be compounded on this item type
    if (roEquipment->Type == ROWOW_ITEM_TYPE_WEAPON && roCard->Locations == ROWOW_EQUIP_POSITION_HAND_L)
        return false; // attempted to place shield card on left-hand weapon.
    if (roEquipment->Type == ROWOW_ITEM_TYPE_ARMOR && (roCard->Locations & ROWOW_EQUIP_POSITION_ACC) != 0 && (roCard->Locations & ROWOW_EQUIP_POSITION_ACC) != ROWOW_EQUIP_POSITION_ACC
        && (roEquipment->Locations & ROWOW_EQUIP_POSITION_ACC) != (roCard->Locations & ROWOW_EQUIP_POSITION_ACC))
        return false; // specific accessory-card can only be inserted to specific accessory.
    if (GetIdentityEquipPosition(*GetIdentity(player), equipment->GetGUID().GetCounter()) != 0)
        return false; // item must be unequipped
    uint8 slot = 0;
    while (slot < roEquipment->Slots && GetItemCard(equipment, slot) != 0)
        slot++;
    if (slot >= roEquipment->Slots)
        return false; // no free slots

    uint32 cardItemID = roCard->ItemID;
    uint32 count = 1;
    player->DestroyItemCount(card, count, true);
    SetItemCard(player, equipment, slot, cardItemID);
    ChatHandler(player->GetSession()).SendSysMessage("The card was compounded.");
    SendItemCardsToClient(player, *GetIdentity(player), true);
    return true;
}

void ROWOWMod::SendCardTargetsToClient(Player* player, Item* card)
{
    // clif_use_card: the identified, unworn equipment in the bags with a free slot the card fits ("ROWOWCARDLIST <card bag>:<card slot>\t
    // <bag>:<slot>,...")
    uint32 cardBag = 0;
    uint32 cardSlot = 0;
    ROWOWItem const* roCard = GetROItem(card);
    if (roCard == nullptr || roCard->Type != ROWOW_ITEM_TYPE_CARD || GetClientItemPosition(card, cardBag, cardSlot) == false)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    std::string positions;
    for (uint8 bag = 0; bag <= 4; bag++)
    {
        for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
        {
            Item* item = GetItemByClientPosition(player, bag, slot);
            ROWOWItem const* roItem = GetROItem(item);
            int32 starCrumbs = 0, forgeElement = 0;
            if (roItem == nullptr || (roItem->Type != ROWOW_ITEM_TYPE_WEAPON && roItem->Type != ROWOW_ITEM_TYPE_ARMOR) || IsItemIdentified(item) == false
                || (roItem->Locations & roCard->Locations) == 0 || GetIdentityEquipPosition(*identity, item->GetGUID().GetCounter()) != 0 ||
                GetItemForge(item, starCrumbs, forgeElement) == true)
                continue;
            if (roItem->Type == ROWOW_ITEM_TYPE_WEAPON && roCard->Locations == ROWOW_EQUIP_POSITION_HAND_L)
                continue;
            if (roItem->Type == ROWOW_ITEM_TYPE_ARMOR && (roCard->Locations & ROWOW_EQUIP_POSITION_ACC) != 0 && (roCard->Locations & ROWOW_EQUIP_POSITION_ACC) != ROWOW_EQUIP_POSITION_ACC
                && (roItem->Locations & ROWOW_EQUIP_POSITION_ACC) != (roCard->Locations & ROWOW_EQUIP_POSITION_ACC))
                continue;
            uint8 freeSlot = 0;
            while (freeSlot < roItem->Slots && GetItemCard(item, freeSlot) != 0)
                freeSlot++;
            if (freeSlot >= roItem->Slots)
                continue;
            positions += fmt::format("{}{}:{}", positions.empty() == true ? "" : ",", bag, slot);
        }
    }
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, fmt::format("ROWOWCARDLIST\t{}:{}\t{}", cardBag, cardSlot, positions));
    player->SendDirectMessage(&data);
}

void ROWOWMod::SendItemCardsToClient(Player* player, ROWOWIdentity& identity, bool isForced)
{
    // ROWOWCARDS: the cards (item template IDs) in each item of the bags that has any, "<bag>:<slot>:<card>:<card>:<card>:<card>,..."
    std::string message = "ROWOWCARDS\t";
    bool isFirst = true;
    for (uint8 bag = 0; bag <= 4; bag++)
    {
        for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
        {
            Item* item = GetItemByClientPosition(player, bag, slot);
            if (item == nullptr || GetROItem(item) == nullptr)
                continue;
            uint32 cardTemplateIDs[ROWOW_MAX_SLOTS] = { };
            bool hasCards = false;
            for (uint8 cardSlot = 0; cardSlot < ROWOW_MAX_SLOTS; cardSlot++)
            {
                auto cardTemplateIDIter = ItemTemplateIDsByItemID.find(GetItemCard(item, cardSlot));
                if (cardTemplateIDIter == ItemTemplateIDsByItemID.end())
                    continue;
                cardTemplateIDs[cardSlot] = cardTemplateIDIter->second;
                hasCards = true;
            }
            if (hasCards == false)
                continue;
            message += fmt::format("{}{}:{}:{}:{}:{}:{}", isFirst == true ? "" : ",", bag, slot, cardTemplateIDs[0], cardTemplateIDs[1], cardTemplateIDs[2], cardTemplateIDs[3]);
            isFirst = false;
        }
    }
    if (isForced == false && message == identity.SentCardsMessage)
        return;
    identity.SentCardsMessage = message;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, message);
    player->SendDirectMessage(&data);
}

void ROWOWMod::RunItemScript(Player* player, ROWOWScript const* script, Item const* item)
{
    // An EquipScript or UnEquipScript (pc_equipitem and pc_unequipitem run them), with what it can't do yet left out
    if (script == nullptr || script->Error.empty() == false)
        return;
    ROWOWScriptContext context;
    context.AttachedPlayer = player;
    context.Identity = GetIdentity(player);
    context.EquipItem = item;
    ROWOWSCRIPT->Run(*script, context, true);
}

int64 ROWOWMod::BattleCalcCardFix(int32 attackType, Unit* src, Unit* target, std::bitset<ROWOW_NK_MAX> nk, uint16 skillID, int32 rhEle, int32 lhEle, int64 damage,
    int32 left, int32 flag) const
{
    // battle_calc_cardfix: for a weapon attack, left&2 is the attacker's cards (left&1 the left hand), otherwise the target's; a magic or misc
    // attack takes both.  Returns the change.  (The race2 groups, the element bonuses by attack kind (subele2) and the debuff statuses
    // aren't in yet)
    if (damage == 0)
        return 0;
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (sstatus == nullptr || tstatus == nullptr)
        return 0;
    int64 originalDamage = damage;
    int32 cardfix = 1000;
    // Official servers apply the cardfix value on a base of 1000 and round down the reduction/increase
    auto applyCardFix = [&damage](int32 fix) { damage = damage - (damage * (1000 - std::max(0, fix))) / 1000; };
    uint32 sourceMobID = src->IsCreature() == true ? GetROMobID(src->ToCreature()) : 0;
    uint32 targetMobID = target->IsCreature() == true ? GetROMobID(target->ToCreature()) : 0;

    switch (attackType)
    {
        case ROWOW_BF_MAGIC:
        {
            // Affected by attacker ATK bonuses
            if (sstatus->IsPlayer == true && nk[ROWOW_NK_IGNOREATKCARD] == false)
            {
                cardfix = cardfix * (100 + GetDeferredBonus2(src, "bMagicAddRace", tstatus->Race) + GetDeferredBonus2(src, "bMagicAddRace", ROWOW_RACE_ALL)) / 100;
                if (nk[ROWOW_NK_IGNOREELEMENT] == false)
                {
                    // Affected by Element modifier bonuses
                    cardfix = cardfix * (100 + GetDeferredBonus2(src, "bMagicAddEle", tstatus->DefElement) + GetDeferredBonus2(src, "bMagicAddEle", ROWOW_ELEMENT_ALL)) / 100;
                    cardfix = cardfix * (100 + GetDeferredBonus2(src, "bMagicAtkEle", rhEle) + GetDeferredBonus2(src, "bMagicAtkEle", ROWOW_ELEMENT_ALL)) / 100;
                }
                cardfix = cardfix * (100 + GetDeferredBonus2(src, "bMagicAddSize", tstatus->Size) + GetDeferredBonus2(src, "bMagicAddSize", ROWOW_SIZE_ALL)) / 100;
                cardfix = cardfix * (100 + GetDeferredBonus2(src, "bMagicAddClass", tstatus->Class) + GetDeferredBonus2(src, "bMagicAddClass", ROWOW_CLASS_ALL)) / 100;
                int32 addMagicDamage = GetDeferredBonus2(src, "bAddMagicDamageClass", targetMobID);
                if (targetMobID != 0 && addMagicDamage != 0)
                    cardfix = cardfix * (100 + addMagicDamage) / 100;
                applyCardFix(cardfix);
            }

            // Affected by target DEF bonuses
            if (tstatus->IsPlayer == true && nk[ROWOW_NK_IGNOREDEFCARD] == false)
            {
                ROWOWBonus const& bonus = tstatus->Bonus;
                cardfix = 1000; // reset var for target
                if (nk[ROWOW_NK_IGNOREELEMENT] == false) // Affected by Element modifier bonuses
                    cardfix = cardfix * (100 - bonus.SubEle[rhEle] - bonus.SubEle[ROWOW_ELEMENT_ALL]) / 100;
                cardfix = cardfix * (100 - bonus.SubSize[sstatus->Size] - bonus.SubSize[ROWOW_SIZE_ALL]) / 100;
                cardfix = cardfix * (100 - bonus.SubRace[sstatus->Race] - bonus.SubRace[ROWOW_RACE_ALL]) / 100;
                cardfix = cardfix * (100 - bonus.SubClass[sstatus->Class] - bonus.SubClass[ROWOW_CLASS_ALL]) / 100;
                int32 addMagicDefense = GetDeferredBonus2(target, "bAddMDefMonster", sourceMobID);
                if (sourceMobID != 0 && addMagicDefense != 0)
                    cardfix = cardfix * (100 - addMagicDefense) / 100;

                // It was discovered that ranged defense also counts vs magic! [Skotlex]
                if ((flag & ROWOW_BF_SHORT) != 0)
                    cardfix = cardfix * (100 - bonus.NearAttackDefRate) / 100;
                else if (nk[ROWOW_NK_IGNORELONGCARD] == false)
                    cardfix = cardfix * (100 - bonus.LongAttackDefRate) / 100;
                cardfix = cardfix * (100 - bonus.MagicDefRate) / 100;
                applyCardFix(cardfix);
            }
            break;
        }
        case ROWOW_BF_WEAPON:
        {
            if (sstatus->IsPlayer == true && nk[ROWOW_NK_IGNOREATKCARD] == false && (left & 2) != 0)
            {
                // Affected by attacker ATK bonuses
                ROWOWBonus const& bonus = sstatus->Bonus;
                int32 cardfixLeft = 1000;
                if (IsSkillUsingArrow(src, skillID) == true)
                {
                    // Ranged attack
                    cardfix = cardfix * (100 + bonus.RightWeapon.AddRace[tstatus->Race] + bonus.ArrowAddRace[tstatus->Race] + bonus.RightWeapon.AddRace[ROWOW_RACE_ALL]
                        + bonus.ArrowAddRace[ROWOW_RACE_ALL]) / 100;
                    if (nk[ROWOW_NK_IGNOREELEMENT] == false)
                    {
                        // Affected by Element modifier bonuses
                        int32 eleFix = bonus.RightWeapon.AddEle[tstatus->DefElement] + bonus.ArrowAddEle[tstatus->DefElement] + bonus.RightWeapon.AddEle[ROWOW_ELEMENT_ALL]
                            + bonus.ArrowAddEle[ROWOW_ELEMENT_ALL];
                        cardfix = cardfix * (100 + eleFix) / 100;
                    }
                    cardfix = cardfix * (100 + bonus.RightWeapon.AddSize[tstatus->Size] + bonus.ArrowAddSize[tstatus->Size] + bonus.RightWeapon.AddSize[ROWOW_SIZE_ALL]
                        + bonus.ArrowAddSize[ROWOW_SIZE_ALL]) / 100;
                    cardfix = cardfix * (100 + bonus.RightWeapon.AddClass[tstatus->Class] + bonus.ArrowAddClass[tstatus->Class] + bonus.RightWeapon.AddClass[ROWOW_CLASS_ALL]
                        + bonus.ArrowAddClass[ROWOW_CLASS_ALL]) / 100;
                }
                else
                {
                    // Melee attack: each hand's bonuses on their own (left_cardfix_to_right is off)
                    cardfix = cardfix * (100 + bonus.RightWeapon.AddRace[tstatus->Race] + bonus.RightWeapon.AddRace[ROWOW_RACE_ALL]) / 100;
                    if (nk[ROWOW_NK_IGNOREELEMENT] == false)
                        cardfix = cardfix * (100 + bonus.RightWeapon.AddEle[tstatus->DefElement] + bonus.RightWeapon.AddEle[ROWOW_ELEMENT_ALL]) / 100;
                    cardfix = cardfix * (100 + bonus.RightWeapon.AddSize[tstatus->Size] + bonus.RightWeapon.AddSize[ROWOW_SIZE_ALL]) / 100;
                    cardfix = cardfix * (100 + bonus.RightWeapon.AddClass[tstatus->Class] + bonus.RightWeapon.AddClass[ROWOW_CLASS_ALL]) / 100;
                    if ((left & 1) != 0)
                    {
                        // Left-handed weapon
                        cardfixLeft = cardfixLeft * (100 + bonus.LeftWeapon.AddRace[tstatus->Race] + bonus.LeftWeapon.AddRace[ROWOW_RACE_ALL]) / 100;
                        if (nk[ROWOW_NK_IGNOREELEMENT] == false)
                            cardfixLeft = cardfixLeft * (100 + bonus.LeftWeapon.AddEle[tstatus->DefElement] + bonus.LeftWeapon.AddEle[ROWOW_ELEMENT_ALL]) / 100;
                        cardfixLeft = cardfixLeft * (100 + bonus.LeftWeapon.AddSize[tstatus->Size] + bonus.LeftWeapon.AddSize[ROWOW_SIZE_ALL]) / 100;
                        cardfixLeft = cardfixLeft * (100 + bonus.LeftWeapon.AddClass[tstatus->Class] + bonus.LeftWeapon.AddClass[ROWOW_CLASS_ALL]) / 100;
                    }
                    // Adv. Katar Mastery functions similar to a +%ATK card on official [helvetica]
                    int32 skill = PCCheckSkill(src, ROWOW_SKILL_ASC_KATAR);
                    if (sstatus->WeaponType == ROWOW_WEAPON_TYPE_KATAR && skill > 0)
                        cardfix = cardfix * (100 + (10 + 2 * skill)) / 100;
                }
                for (auto const& addDamage : bonus.RightWeapon.AddDamageClass)
                {
                    if (addDamage.first == targetMobID)
                    {
                        cardfix = cardfix * (100 + addDamage.second) / 100;
                        break;
                    }
                }
                if ((left & 1) != 0)
                {
                    for (auto const& addDamage : bonus.LeftWeapon.AddDamageClass)
                    {
                        if (addDamage.first == targetMobID)
                        {
                            cardfixLeft = cardfixLeft * (100 + addDamage.second) / 100;
                            break;
                        }
                    }
                }
                if ((flag & ROWOW_BF_SHORT) != 0)
                    cardfix = cardfix * (100 + bonus.ShortAttackAtkRate) / 100;
                if ((flag & ROWOW_BF_LONG) != 0)
                    cardfix = cardfix * (100 + bonus.LongAttackAtkRate) / 100;
                applyCardFix((left & 1) != 0 ? cardfixLeft : cardfix);
            }
            else if (tstatus->IsPlayer == true && nk[ROWOW_NK_IGNOREDEFCARD] == false && (left & 2) == 0)
            {
                // Affected by target DEF bonuses
                ROWOWBonus const& bonus = tstatus->Bonus;
                if (nk[ROWOW_NK_IGNOREELEMENT] == false)
                {
                    // Affected by Element modifier bonuses
                    cardfix = cardfix * (100 - bonus.SubEle[rhEle] - bonus.SubEle[ROWOW_ELEMENT_ALL]) / 100;
                    if ((left & 1) != 0 && lhEle != rhEle)
                        cardfix = cardfix * (100 - bonus.SubEle[lhEle] - bonus.SubEle[ROWOW_ELEMENT_ALL]) / 100;
                }
                cardfix = cardfix * (100 - bonus.SubSize[sstatus->Size] - bonus.SubSize[ROWOW_SIZE_ALL]) / 100;
                cardfix = cardfix * (100 - bonus.SubRace[sstatus->Race] - bonus.SubRace[ROWOW_RACE_ALL]) / 100;
                cardfix = cardfix * (100 - bonus.SubClass[sstatus->Class] - bonus.SubClass[ROWOW_CLASS_ALL]) / 100;
                for (auto const& addDef : bonus.AddDefMonster)
                {
                    if (addDef.first == sourceMobID)
                    {
                        cardfix = cardfix * (100 - addDef.second) / 100;
                        break;
                    }
                }
                if ((flag & ROWOW_BF_SHORT) != 0)
                    cardfix = cardfix * (100 - bonus.NearAttackDefRate) / 100;
                else if (nk[ROWOW_NK_IGNORELONGCARD] == false) // BF_LONG (there's no other choice)
                    cardfix = cardfix * (100 - bonus.LongAttackDefRate) / 100;
                applyCardFix(cardfix);
            }
            break;
        }
        case ROWOW_BF_MISC:
        {
            // Affected by target DEF bonuses
            if (tstatus->IsPlayer == true && nk[ROWOW_NK_IGNOREDEFCARD] == false)
            {
                ROWOWBonus const& bonus = tstatus->Bonus;
                if (nk[ROWOW_NK_IGNOREELEMENT] == false) // Affected by Element modifier bonuses
                    cardfix = cardfix * (100 - bonus.SubEle[rhEle] - bonus.SubEle[ROWOW_ELEMENT_ALL]) / 100;
                cardfix = cardfix * (100 - bonus.SubRace[sstatus->Race] - bonus.SubRace[ROWOW_RACE_ALL]) / 100;
                cardfix = cardfix * (100 - bonus.SubSize[sstatus->Size] - bonus.SubSize[ROWOW_SIZE_ALL]) / 100;
                cardfix = cardfix * (100 - bonus.SubClass[sstatus->Class] - bonus.SubClass[ROWOW_CLASS_ALL]) / 100;
                cardfix = cardfix * (100 - bonus.MiscDefRate) / 100;
                if ((flag & ROWOW_BF_SHORT) != 0)
                    cardfix = cardfix * (100 - bonus.NearAttackDefRate) / 100;
                else if (nk[ROWOW_NK_IGNORELONGCARD] == false) // BF_LONG (there's no other choice)
                    cardfix = cardfix * (100 - bonus.LongAttackDefRate) / 100;
                applyCardFix(cardfix);
            }
            break;
        }
    }
    return damage - originalDamage;
}
