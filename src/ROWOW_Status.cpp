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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/status.cpp, pc.cpp and mob.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Creature.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// The RO identity's status, experience and natural regen: rAthena's pre-renewal rules, keeping its function names (in the comments
// beside each) and structure so the port can be checked against it.  All of it runs in the player's own thread, or the map's for a
// monster and the players near it

namespace
{
    // rAthena's weapon type names (W_* without the prefix), as item_db's SubType and the job database's BaseASPD write them
    char const* const WeaponTypeNames[ROWOW_WEAPON_TYPE_MAX] =
    {
        "Fist", "Dagger", "1hSword", "2hSword", "1hSpear", "2hSpear", "1hAxe", "2hAxe", "Mace", "2hMace", "Staff", "Bow", "Knuckle",
        "Musical", "Whip", "Book", "Katar", "Revolver", "Rifle", "Gatling", "Shotgun", "Grenade", "Huuma", "2hStaff",
    };

    // The job database's names for the six stats
    char const* const StatNames[ROWOW_STAT_COUNT] = { "Str", "Agi", "Vit", "Int", "Dex", "Luk" };

    // item_db's ammo SubType names, by rAthena's e_ammo_type (AMMO_NONE has none)
    char const* const AmmoTypeNames[] = { "", "Arrow", "Dagger", "Bullet", "Shell", "Grenade", "Shuriken", "Kunai", "Cannonball", "Throwweapon" };

    // item_db's Locations names, and the equipment positions (EQP_*) each is.  The costume positions are for the post-renewal looks
    std::pair<char const*, uint32> const LocationNames[] =
    {
        { "Head_Low", ROWOW_EQUIP_POSITION_HEAD_LOW }, { "Right_Hand", ROWOW_EQUIP_POSITION_HAND_R }, { "Garment", ROWOW_EQUIP_POSITION_GARMENT },
        { "Left_Accessory", ROWOW_EQUIP_POSITION_ACC_L }, { "Armor", ROWOW_EQUIP_POSITION_ARMOR }, { "Left_Hand", ROWOW_EQUIP_POSITION_HAND_L },
        { "Shoes", ROWOW_EQUIP_POSITION_SHOES }, { "Right_Accessory", ROWOW_EQUIP_POSITION_ACC_R }, { "Head_Top", ROWOW_EQUIP_POSITION_HEAD_TOP },
        { "Head_Mid", ROWOW_EQUIP_POSITION_HEAD_MID }, { "Ammo", ROWOW_EQUIP_POSITION_AMMO },
        { "Costume_Head_Top", ROWOW_EQUIP_POSITION_COSTUME_HEAD_TOP }, { "Costume_Head_Mid", ROWOW_EQUIP_POSITION_COSTUME_HEAD_MID },
        { "Costume_Head_Low", ROWOW_EQUIP_POSITION_COSTUME_HEAD_LOW }, { "Costume_Garment", ROWOW_EQUIP_POSITION_COSTUME_GARMENT },
        { "Both_Hand", ROWOW_EQUIP_POSITION_HAND_R | ROWOW_EQUIP_POSITION_HAND_L }, { "Both_Accessory", ROWOW_EQUIP_POSITION_ACC_L | ROWOW_EQUIP_POSITION_ACC_R },
    };

    // item_db's Classes names (e_item_job), as the pre-renewal jobs use them
    std::pair<char const*, uint8> const ItemClassNames[] =
    {
        { "Normal", ROWOW_ITEM_CLASS_NORMAL }, { "Upper", ROWOW_ITEM_CLASS_UPPER }, { "Baby", ROWOW_ITEM_CLASS_BABY },
        { "All_Upper", ROWOW_ITEM_CLASS_UPPER }, { "All_Baby", ROWOW_ITEM_CLASS_BABY },
    };

    // item_db's Type names, by rAthena's e_item_types
    std::pair<char const*, uint8> const ItemTypeNames[] =
    {
        { "Healing", ROWOW_ITEM_TYPE_HEALING }, { "Usable", ROWOW_ITEM_TYPE_USABLE }, { "Etc", ROWOW_ITEM_TYPE_ETC },
        { "Armor", ROWOW_ITEM_TYPE_ARMOR }, { "Weapon", ROWOW_ITEM_TYPE_WEAPON }, { "Card", ROWOW_ITEM_TYPE_CARD },
        { "Petegg", ROWOW_ITEM_TYPE_PETEGG }, { "Petarmor", ROWOW_ITEM_TYPE_PETARMOR }, { "Ammo", ROWOW_ITEM_TYPE_AMMO },
        { "Delayconsume", ROWOW_ITEM_TYPE_DELAYCONSUME }, { "Cash", ROWOW_ITEM_TYPE_CASH },
    };

    // size_fix.yml's names for the sizes
    char const* const SizeFixNames[ROWOW_SIZE_COUNT] = { "Small", "Medium", "Large" };

    int32 GetWeaponTypeByName(std::string const& name)
    {
        for (int32 weaponType = 0; weaponType < ROWOW_WEAPON_TYPE_MAX; weaponType++)
            if (name == WeaponTypeNames[weaponType])
                return weaponType;
        return -1;
    }

    // "yes"/"no", hexadecimal or decimal, as rAthena reads its battle config values
    int32 ParseBattleConfigValue(std::string const& text)
    {
        if (text == "yes" || text == "on")
            return 1;
        if (text == "no" || text == "off")
            return 0;
        return int32(strtol(text.c_str(), nullptr, 0));
    }

    // PC_STATUS_POINT_COST: what raising a stat from this value by one costs, pre-renewal
    int32 GetStatusPointCost(int32 low)
    {
        return 1 + (low + 9) / 10;
    }

    // Bows, instruments, whips and guns put DEX where STR goes in the base attack, and are the ranged weapons
    bool IsRangedWeaponType(uint8 weaponType)
    {
        return weaponType == ROWOW_WEAPON_TYPE_BOW || weaponType == ROWOW_WEAPON_TYPE_MUSICAL || weaponType == ROWOW_WEAPON_TYPE_WHIP ||
            (weaponType >= ROWOW_WEAPON_TYPE_REVOLVER && weaponType <= ROWOW_WEAPON_TYPE_GRENADE);
    }

    // pc_calcweapontype: the weapon type of the hands together (a dual wield type when both hold a weapon)
    uint8 GetCombinedWeaponType(uint8 rightWeaponType, uint8 leftWeaponType)
    {
        if (leftWeaponType == ROWOW_WEAPON_TYPE_FIST)
            return rightWeaponType;
        if (rightWeaponType == ROWOW_WEAPON_TYPE_FIST)
            return leftWeaponType;
        uint8 combinedWeaponType = ROWOW_WEAPON_TYPE_FIST;
        switch (rightWeaponType)
        {
            case ROWOW_WEAPON_TYPE_DAGGER:
                switch (leftWeaponType)
                {
                    case ROWOW_WEAPON_TYPE_DAGGER: combinedWeaponType = ROWOW_WEAPON_TYPE_DOUBLE_DD; break;
                    case ROWOW_WEAPON_TYPE_1HSWORD: combinedWeaponType = ROWOW_WEAPON_TYPE_DOUBLE_DS; break;
                    case ROWOW_WEAPON_TYPE_1HAXE: combinedWeaponType = ROWOW_WEAPON_TYPE_DOUBLE_DA; break;
                }
                break;
            case ROWOW_WEAPON_TYPE_1HSWORD:
                switch (leftWeaponType)
                {
                    case ROWOW_WEAPON_TYPE_DAGGER: combinedWeaponType = ROWOW_WEAPON_TYPE_DOUBLE_DS; break;
                    case ROWOW_WEAPON_TYPE_1HSWORD: combinedWeaponType = ROWOW_WEAPON_TYPE_DOUBLE_SS; break;
                    case ROWOW_WEAPON_TYPE_1HAXE: combinedWeaponType = ROWOW_WEAPON_TYPE_DOUBLE_SA; break;
                }
                break;
            case ROWOW_WEAPON_TYPE_1HAXE:
                switch (leftWeaponType)
                {
                    case ROWOW_WEAPON_TYPE_DAGGER: combinedWeaponType = ROWOW_WEAPON_TYPE_DOUBLE_DA; break;
                    case ROWOW_WEAPON_TYPE_1HSWORD: combinedWeaponType = ROWOW_WEAPON_TYPE_DOUBLE_SA; break;
                    case ROWOW_WEAPON_TYPE_1HAXE: combinedWeaponType = ROWOW_WEAPON_TYPE_DOUBLE_AA; break;
                }
                break;
        }
        return combinedWeaponType != ROWOW_WEAPON_TYPE_FIST ? combinedWeaponType : rightWeaponType;
    }

    void CastLevelUpEffect(Player* player, uint32 spellID)
    {
        // A visual spell, seen by the players nearby as well (missing until the converter has made it)
        if (sSpellMgr->GetSpellInfo(spellID) != nullptr)
            player->CastSpell(player, spellID, true);
    }
}

char const* const ROWOWItemJobNames[ROWOW_ITEM_JOB_COUNT] =
{
    "Novice", "Swordman", "Mage", "Archer", "Acolyte", "Merchant", "Thief", "Knight", "Priest", "Wizard", "Blacksmith", "Hunter",
    "Assassin", "Crusader", "Monk", "Sage", "Rogue", "Alchemist", "BardDancer", "Taekwon", "StarGladiator", "SoulLinker", "Gunslinger",
    "Ninja", "SuperNovice",
};

void ROWOWMod::LoadStatusData()
{
    ItemsByItemTemplateID.clear();
    BattleConfig = ROWOWBattleConfig();
    NoPenaltyMapIDs.clear();
    if (IsEnabled == false)
        return;

    // The battle config values the rules use
    if (ROWOWDataTable const* battleConfigTable = GetDataTable(ROWOW_DATA_TABLE_BATTLE_CONFIG))
    {
        std::unordered_map<std::string, std::string> valuesByKey;
        int32 keyColumnIndex = battleConfigTable->GetColumnIndex("Key");
        int32 valueColumnIndex = battleConfigTable->GetColumnIndex("Value");
        for (size_t rowIndex = 0; rowIndex < battleConfigTable->Rows.size(); rowIndex++)
            valuesByKey[battleConfigTable->GetCell(rowIndex, keyColumnIndex)] = battleConfigTable->GetCell(rowIndex, valueColumnIndex);
        auto readValue = [&valuesByKey](char const* key, int32& valueOut)
        {
            auto valueIter = valuesByKey.find(key);
            if (valueIter != valuesByKey.end() && valueIter->second.empty() == false)
                valueOut = ParseBattleConfigValue(valueIter->second);
        };
        int32 multiLevelUp = BattleConfig.MultiLevelUp == true ? 1 : 0;
        int32 maxAspd = 190;
        readValue("base_exp_rate", BattleConfig.BaseExpRate);
        readValue("job_exp_rate", BattleConfig.JobExpRate);
        readValue("multi_level_up", multiLevelUp);
        readValue("multi_level_up_base", BattleConfig.MultiLevelUpBase);
        readValue("multi_level_up_job", BattleConfig.MultiLevelUpJob);
        readValue("max_exp_gain_rate", BattleConfig.MaxExpGainRate);
        readValue("exp_calc_type", BattleConfig.ExpCalcType);
        readValue("exp_bonus_attacker", BattleConfig.ExpBonusAttacker);
        readValue("exp_bonus_max_attacker", BattleConfig.ExpBonusMaxAttacker);
        readValue("death_penalty_type", BattleConfig.DeathPenaltyType);
        readValue("death_penalty_base", BattleConfig.DeathPenaltyBase);
        readValue("death_penalty_job", BattleConfig.DeathPenaltyJob);
        readValue("death_penalty_maxlv", BattleConfig.DeathPenaltyMaxLevel);
        readValue("hp_rate", BattleConfig.HPRate);
        readValue("sp_rate", BattleConfig.SPRate);
        readValue("restart_hp_rate", BattleConfig.RestartHPRate);
        readValue("restart_sp_rate", BattleConfig.RestartSPRate);
        readValue("max_hp_lv99", BattleConfig.MaxHPLevel99);
        readValue("max_hp_lv150", BattleConfig.MaxHPLevel150);
        readValue("max_hp", BattleConfig.MaxHP);
        readValue("max_sp", BattleConfig.MaxSP);
        readValue("max_aspd", maxAspd);
        readValue("max_parameter", BattleConfig.MaxParameter);
        readValue("max_trans_parameter", BattleConfig.MaxTransParameter);
        readValue("max_baby_parameter", BattleConfig.MaxBabyParameter);
        readValue("transcendent_status_points", BattleConfig.TranscendentStatusPoints);
        readValue("dancing_weaponswitch_fix", BattleConfig.DancingWeaponSwitchFix);
        readValue("max_def", BattleConfig.MaxDef);
        readValue("over_def_bonus", BattleConfig.OverDefBonus);
        readValue("weapon_defense_type", BattleConfig.WeaponDefenseType);
        readValue("magic_defense_type", BattleConfig.MagicDefenseType);
        readValue("pc_damage_delay_rate", BattleConfig.PCDamageDelayRate);
        readValue("natural_healhp_interval", BattleConfig.NaturalHealHPInterval);
        readValue("natural_healsp_interval", BattleConfig.NaturalHealSPInterval);
        readValue("enable_baseatk", BattleConfig.EnableBaseAtk);
        readValue("enable_critical", BattleConfig.EnableCritical);
        readValue("enable_perfect_flee", BattleConfig.EnablePerfectFlee);
        readValue("mob_critical_rate", BattleConfig.MobCriticalRate);
        readValue("min_hitrate", BattleConfig.MinHitRate);
        readValue("max_hitrate", BattleConfig.MaxHitRate);
        readValue("agi_penalty_type", BattleConfig.AgiPenaltyType);
        readValue("agi_penalty_target", BattleConfig.AgiPenaltyTarget);
        readValue("agi_penalty_count", BattleConfig.AgiPenaltyCount);
        readValue("agi_penalty_num", BattleConfig.AgiPenaltyNum);
        readValue("vit_penalty_type", BattleConfig.VitPenaltyType);
        readValue("vit_penalty_target", BattleConfig.VitPenaltyTarget);
        readValue("vit_penalty_count", BattleConfig.VitPenaltyCount);
        readValue("vit_penalty_num", BattleConfig.VitPenaltyNum);
        readValue("attack_attr_none", BattleConfig.AttackAttrNone);
        readValue("attribute_recover", BattleConfig.AttrRecover);
        readValue("skill_min_damage", BattleConfig.SkillMinDamage);
        readValue("arrow_decrement", BattleConfig.ArrowDecrement);
        readValue("ammo_check_weapon", BattleConfig.AmmoCheckWeapon);
        readValue("monster_hp_rate", BattleConfig.MonsterHPRate);
        readValue("mvp_hp_rate", BattleConfig.MvpHPRate);
        readValue("monster_damage_delay_rate", BattleConfig.MonsterDamageDelayRate);
        readValue("area_size", BattleConfig.AreaSize);
        readValue("monster_ai", BattleConfig.MobAI);
        readValue("monster_chase_refresh", BattleConfig.MobChaseRefresh);
        readValue("assist_range", BattleConfig.AssistRange);
        readValue("loot_range", BattleConfig.LootRange);
        readValue("view_range_rate", BattleConfig.ViewRangeRate);
        readValue("chase_range_rate", BattleConfig.ChaseRangeRate);
        readValue("monster_active_enable", BattleConfig.MonsterActiveEnable);
        readValue("mob_unlock_time", BattleConfig.MobUnlockTime);
        readValue("mob_respawn_time", BattleConfig.MobRespawnTime);
        readValue("mob_skill_rate", BattleConfig.MobSkillRate);
        readValue("mob_skill_delay", BattleConfig.MobSkillDelayRate);
        readValue("mob_max_skilllvl", BattleConfig.MobMaxSkillLevel);
        readValue("mob_changetarget_byskill", BattleConfig.MobChangeTargetBySkill);
        readValue("slaves_inherit_mode", BattleConfig.SlavesInheritMode);
        readValue("slaves_inherit_speed", BattleConfig.SlavesInheritSpeed);
        readValue("slave_stick_with_master", BattleConfig.SlaveStickWithMaster);
        readValue("monster_class_change_full_recover", BattleConfig.MonsterClassChangeRecover);
        readValue("npc_emotion_behavior", BattleConfig.NpcEmotionBehavior);
        readValue("mvp_exp_rate", BattleConfig.MvpExpRate);
        readValue("mvp_to_loot_priority", BattleConfig.MvpToLootPriority);
        readValue("item_rate_mvp", BattleConfig.ItemRateMvp);
        readValue("item_drop_mvp_min", BattleConfig.ItemDropMvpMin);
        readValue("item_drop_mvp_max", BattleConfig.ItemDropMvpMax);
        readValue("item_drop_mvp_mode", BattleConfig.ItemDropMvpMode);
        readValue("mob_spawn_variance", BattleConfig.MobSpawnVariance);
        readValue("mob_spawn_delay", BattleConfig.MobSpawnDelay);
        readValue("plant_spawn_delay", BattleConfig.PlantSpawnDelay);
        readValue("boss_spawn_delay", BattleConfig.BossSpawnDelay);
        readValue("mob_nopc_move_rate", BattleConfig.MobNoPCMoveRate);
        readValue("boss_nopc_move_rate", BattleConfig.BossNoPCMoveRate);
        readValue("mob_nopc_idleskill_rate", BattleConfig.MobNoPCIdleSkillRate);
        readValue("boss_nopc_idleskill_rate", BattleConfig.BossNoPCIdleSkillRate);
        readValue("retaliate_to_master", BattleConfig.RetaliateToMaster);
        readValue("damage_walk_delay_rate", BattleConfig.WalkDelayRate);
        readValue("pc_damage_walk_delay_rate", BattleConfig.PCWalkDelayRate);
        readValue("multihit_delay", BattleConfig.MultihitDelay);
        readValue("max_walk_path", BattleConfig.MaxWalkPath);
        readValue("map_edge_size", BattleConfig.MapEdgeSize);
        readValue("skill_wall_check", BattleConfig.SkillWallCheck);
        readValue("damage_walk_delay", BattleConfig.DamageWalkDelay);
        readValue("sdelay_attack_enable", BattleConfig.SDelayAttackEnable);
        readValue("item_use_interval", BattleConfig.ItemUseInterval);
        readValue("ammo_unequip", BattleConfig.AmmoUnequip);
        readValue("skillup_limit", BattleConfig.SkillUpLimit);
        readValue("skillfree", BattleConfig.SkillFree);
        readValue("quest_skill_learn", BattleConfig.QuestSkillLearn);
        readValue("casting_rate", BattleConfig.CastRate);
        readValue("castrate_dex_scale", BattleConfig.CastrateDexScale);
        readValue("delay_rate", BattleConfig.DelayRate);
        readValue("delay_dependon_dex", BattleConfig.DelayDependOnDex);
        readValue("delay_dependon_agi", BattleConfig.DelayDependOnAgi);
        readValue("min_skill_delay_limit", BattleConfig.MinSkillDelayLimit);
        readValue("no_skill_delay", BattleConfig.NoSkillDelay);
        readValue("max_heal", BattleConfig.MaxHeal);
        readValue("max_heal_lv", BattleConfig.MaxHealLv);
        readValue("delay_battle_damage", BattleConfig.DelayBattleDamage);
        readValue("knockback_left", BattleConfig.KnockbackLeft);
        readValue("skillrange_by_distance", BattleConfig.SkillrangeByDistance);
        readValue("skillrange_from_weapon", BattleConfig.SkillrangeFromWeapon);
        readValue("skill_caster_check", BattleConfig.SkillCasterCheck);
        readValue("cardfix_monster_physical", BattleConfig.CardfixMonsterPhysical);
        readValue("path_blown_halt", BattleConfig.PathBlownHalt);
        readValue("exp_bonus_nodamage_attacker", BattleConfig.ExpBonusNodamageAttacker);
        readValue("heal_exp", BattleConfig.HealExp);
        readValue("skill_add_range", BattleConfig.SkillAddRange);
        readValue("pc_status_def_rate", BattleConfig.PCScDefRate);
        readValue("mob_status_def_rate", BattleConfig.MobScDefRate);
        readValue("pc_max_status_def", BattleConfig.PCMaxScDef);
        readValue("mob_max_status_def", BattleConfig.MobMaxScDef);
        readValue("status_cast_cancel", BattleConfig.ScCastCancel);
        readValue("max_walk_speed", BattleConfig.MaxWalkSpeed);
        readValue("gtb_sc_immunity", BattleConfig.GtbScImmunity);
        readValue("player_cloak_check_type", BattleConfig.PCCloakCheckType);
        readValue("monster_cloak_check_type", BattleConfig.MonsterCloakCheckType);
        readValue("infinite_endure", BattleConfig.InfiniteEndure);
        readValue("undead_detect_type", BattleConfig.UndeadDetectType);
        readValue("debuff_on_logout", BattleConfig.DebuffOnLogout);
        readValue("auto_counter_type", BattleConfig.AutoCounterType);
        readValue("equip_natural_break_rate", BattleConfig.EquipNaturalBreakRate);
        readValue("equip_self_break_rate", BattleConfig.EquipSelfBreakRate);
        readValue("equip_skill_break_rate", BattleConfig.EquipSkillBreakRate);
        readValue("break_mob_equip", BattleConfig.BreakMobEquip);
        readValue("stormgust_knockback", BattleConfig.StormgustKnockback);
        readValue("cart_revo_knockback", BattleConfig.CartRevoKnockback);
        readValue("arrow_shower_knockback", BattleConfig.ArrowShowerKnockback);
        readValue("player_invincible_time", BattleConfig.PCInvincibleTime);
        readValue("party_even_share_bonus", BattleConfig.PartyEvenShareBonus);
        readValue("party_item_share_type", BattleConfig.PartyShareType);
        readValue("show_party_share_picker", BattleConfig.PartyShowSharePicker);
        readValue("show_picker.item_type", BattleConfig.ShowPickerItemType);
        readValue("pet_legacy_formula", BattleConfig.PetLegacyFormula);
        readValue("pet_catch_rate", BattleConfig.PetCatchRate);
        readValue("pet_distance_check", BattleConfig.PetDistanceCheck);
        readValue("pet_hide_check", BattleConfig.PetHideCheck);
        readValue("pet_rename", BattleConfig.PetRename);
        readValue("hom_setting", BattleConfig.HomSetting);
        readValue("hom_rename", BattleConfig.HomRename);
        readValue("homunculus_friendly_rate", BattleConfig.HomunculusFriendlyRate);
        readValue("homunculus_evo_intimacy_need", BattleConfig.HomunculusEvoIntimacyNeed);
        readValue("homunculus_evo_intimacy_reset", BattleConfig.HomunculusEvoIntimacyReset);
        readValue("hvan_explosion_intimate", BattleConfig.HvanExplosionIntimate);
        readValue("homunculus_show_growth", BattleConfig.HomunculusShowGrowth);
        readValue("homunculus_auto_vapor", BattleConfig.HomunculusAutoVapor);
        readValue("homunculus_max_level", BattleConfig.HomMaxLevel);
        readValue("homunculus_starving_rate", BattleConfig.HomunculusStarvingRate);
        readValue("homunculus_starving_delay", BattleConfig.HomunculusStarvingDelay);
        readValue("feature.homunculus_autofeed", BattleConfig.FeatureHomunculusAutofeed);
        readValue("feature.homunculus_autofeed_rate", BattleConfig.FeatureHomunculusAutofeedRate);
        readValue("amotion_min_skill_delay", BattleConfig.AmotionMinSkillDelay);
        readValue("critical_rate", BattleConfig.CriticalRate);
        readValue("pet_friendly_rate", BattleConfig.PetFriendlyRate);
        readValue("pet_hungry_delay_rate", BattleConfig.PetHungryDelayRate);
        readValue("pet_unequip_destroy", BattleConfig.PetUnequipDestroy);
        readValue("pet_walk_speed", BattleConfig.PetWalkSpeed);
        int32 maxCartWeight = BattleConfig.MaxCartWeight / 10; // The setting is in whole weight, the cart counts tenths (battle_adjust_conf)
        readValue("max_cart_weight", maxCartWeight);
        BattleConfig.MaxCartWeight = maxCartWeight * 10;
        readValue("vending_max_value", BattleConfig.VendingMaxValue);
        readValue("vending_over_max", BattleConfig.VendingOverMax);
        readValue("vending_tax", BattleConfig.VendingTax);
        readValue("vending_tax_min", BattleConfig.VendingTaxMin);
        readValue("min_npc_vendchat_distance", BattleConfig.MinNpcVendChatDistance);
        readValue("chat_warpportal", BattleConfig.ChatWarpPortal);
        readValue("sg_angel_skill_ratio", BattleConfig.SGAngelSkillRatio);
        readValue("taekwon_ranker_min_lv", BattleConfig.TaekwonRankerMinLv);
        readValue("fame_taekwon_mission", BattleConfig.FameTaekwonMission);
        readValue("fame_refine_lv1", BattleConfig.FameRefineLv1);
        readValue("fame_refine_lv2", BattleConfig.FameRefineLv2);
        readValue("fame_refine_lv3", BattleConfig.FameRefineLv3);
        readValue("fame_forge", BattleConfig.FameForge);
        readValue("blacksmith_fame_refine_threshold", BattleConfig.BlacksmithFameRefineThreshold);
        readValue("fame_pharmacy_3", BattleConfig.FamePharmacy3);
        readValue("fame_pharmacy_5", BattleConfig.FamePharmacy5);
        readValue("fame_pharmacy_7", BattleConfig.FamePharmacy7);
        readValue("fame_pharmacy_10", BattleConfig.FamePharmacy10);
        readValue("produce_item_name_input", BattleConfig.ProduceItemNameInput);
        readValue("skill_steal_max_tries", BattleConfig.SkillStealMaxTries);
        readValue("autospell_check_range", BattleConfig.AutospellCheckRange);
        readValue("autospell_stacking", BattleConfig.AutospellStacking);
        readValue("skip_teleport_lv1_menu", BattleConfig.SkipTeleportLv1Menu);
        readValue("basic_skill_check", BattleConfig.BasicSkillCheck);
        readValue("natural_heal_skill_interval", BattleConfig.NaturalHealSkillInterval);
        readValue("devotion_level_difference", BattleConfig.DevotionLevelDifference);
        readValue("devotion_rdamage", BattleConfig.DevotionRDamage);
        readValue("devotion_rdamage_skill_only", BattleConfig.DevotionRDamageSkillOnly);
        readValue("devotion_standup_fix", BattleConfig.DevotionStandupFix);
        readValue("bowling_bash_area", BattleConfig.BowlingBashArea);
        readValue("gx_allhit", BattleConfig.GxAllHit);
        readValue("skill_eightpath_same_cell", BattleConfig.SkillEightpathSameCell);
        readValue("eq_single_target_reflectable", BattleConfig.EqSingleTargetReflectable);
        readValue("mob_slave_keep_target", BattleConfig.MobSlaveKeepTarget);
        readValue("land_protector_behavior", BattleConfig.LandProtectorBehavior);
        readValue("dispel_song", BattleConfig.DispelSong);
        readValue("combo_delay_rate", BattleConfig.ComboDelayRate);
        readValue("finger_offensive_type", BattleConfig.FingerOffensiveType);
        readValue("player_skill_partner_check", BattleConfig.PlayerSkillPartnerCheck);
        readValue("resurrection_exp", BattleConfig.ResurrectionExp);
        readValue("exp_cost_redemptio", BattleConfig.ExpCostRedemptio);
        readValue("exp_cost_redemptio_limit", BattleConfig.ExpCostRedemptioLimit);
        readValue("traps_setting", BattleConfig.TrapsSetting);
        readValue("multi_trigger_trap", BattleConfig.MultiTriggerTrap);
        readValue("skill_removetrap_type", BattleConfig.SkillRemoveTrapType);
        readValue("refresh_song", BattleConfig.RefreshSong);
        readValue("weapon_produce_rate", BattleConfig.WeaponProduceRate);
        readValue("potion_produce_rate", BattleConfig.PotionProduceRate);
        readValue("oridecon_research_fix", BattleConfig.OrideconResearchFix);
        readValue("land_skill_limit", BattleConfig.LandSkillLimit);
        readValue("alchemist_summon_setting", BattleConfig.AlchemistSummonSetting);
        readValue("alchemist_summon_reward", BattleConfig.AlchemistSummonReward);
        readValue("skill_add_heal_rate", BattleConfig.SkillAddHealRate);
        readValue("first_attack_loot_bonus", BattleConfig.FirstAttackLootBonus);
        readValue("flooritem_lifetime", BattleConfig.FlooritemLifetime);
        readValue("item_first_get_time", BattleConfig.ItemFirstGetTime);
        readValue("item_second_get_time", BattleConfig.ItemSecondGetTime);
        readValue("item_third_get_time", BattleConfig.ItemThirdGetTime);
        readValue("mvp_item_first_get_time", BattleConfig.MvpItemFirstGetTime);
        readValue("mvp_item_second_get_time", BattleConfig.MvpItemSecondGetTime);
        readValue("mvp_item_third_get_time", BattleConfig.MvpItemThirdGetTime);
        readValue("item_flooritem_check", BattleConfig.ItemOnFloor);
        readValue("monster_loot_type", BattleConfig.MonsterLootType);
        readValue("monster_loot_search_type", BattleConfig.MonsterLootSearchType);
        auto readDropRates = [&readValue](char const* name, ROWOWDropRateConfig& rates)
        {
            readValue((std::string("item_rate_") + name).c_str(), rates.Rate);
            readValue((std::string("item_rate_") + name + "_boss").c_str(), rates.RateBoss);
            readValue((std::string("item_rate_") + name + "_mvp").c_str(), rates.RateMvp);
            readValue((std::string("item_drop_") + name + "_min").c_str(), rates.Min);
            readValue((std::string("item_drop_") + name + "_max").c_str(), rates.Max);
        };
        readDropRates("common", BattleConfig.ItemDropCommon);
        readDropRates("heal", BattleConfig.ItemDropHeal);
        readDropRates("use", BattleConfig.ItemDropUse);
        readDropRates("equip", BattleConfig.ItemDropEquip);
        readDropRates("card", BattleConfig.ItemDropCard);
        readDropRates("treasure", BattleConfig.ItemDropTreasure);
        readValue("item_rate_adddrop", BattleConfig.ItemRateAddDrop);
        readValue("item_drop_add_min", BattleConfig.ItemDropAddDropMin);
        readValue("item_drop_add_max", BattleConfig.ItemDropAddDropMax);
        readValue("item_group_rate", BattleConfig.ItemGroupRate);
        readValue("item_group_drop_min", BattleConfig.ItemGroupDropMin);
        readValue("item_group_drop_max", BattleConfig.ItemGroupDropMax);
        readValue("item_logarithmic_drops", BattleConfig.LogarithmicDrops);
        readValue("drop_rate0item", BattleConfig.DropRate0Item);
        readValue("drop_rateincrease", BattleConfig.DropRateIncrease);
        readValue("drops_by_luk", BattleConfig.DropsByLuk);
        readValue("drops_by_luk2", BattleConfig.DropsByLuk2);
        readValue("drop_rate_cap", BattleConfig.DropRateCap);
        readValue("backstab_bow_penalty", BattleConfig.BackstabBowPenalty);
        readValue("switch_remove_edp", BattleConfig.SwitchRemoveEdp);
        BattleConfig.MultiLevelUp = multiLevelUp != 0;
        BattleConfig.MaxAspdAmotion = (2000 - maxAspd * 10) * 2; // As battle.cpp converts it after loading
    }
    else
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadStatusData found no {} table, so the RO rules use rAthena's default battle config", ROWOW_DATA_TABLE_BATTLE_CONFIG);

    // The jobs' stat bonuses by job level and their attack motion with each weapon type.  rAthena lists the bonus each job level adds,
    // and keeps the totals
    std::unordered_map<std::string, ROWOWJob*> jobsByName;
    for (auto& jobByID : JobsByID)
        jobsByName[GetJobNameKey(jobByID.second.Name)] = &jobByID.second;
    if (ROWOWDataTable const* jobTable = GetDataTable(ROWOW_DATA_TABLE_JOB))
    {
        int32 jobsColumnIndex = jobTable->GetColumnIndex("Jobs");
        int32 bonusStatsColumnIndex = jobTable->GetColumnIndex("BonusStats");
        int32 baseASPDColumnIndex = jobTable->GetColumnIndex("BaseASPD");
        for (size_t rowIndex = 0; rowIndex < jobTable->Rows.size(); rowIndex++)
        {
            std::string const& bonusStatsCell = jobTable->GetCell(rowIndex, bonusStatsColumnIndex);
            std::string const& baseASPDCell = jobTable->GetCell(rowIndex, baseASPDColumnIndex);
            if (bonusStatsCell.empty() == true && baseASPDCell.empty() == true)
                continue;
            for (auto const& jobField : ROWOWDataTable::ParseMap(jobTable->GetCell(rowIndex, jobsColumnIndex)))
            {
                auto jobIter = jobsByName.find(GetJobNameKey(jobField.first));
                if (jobIter == jobsByName.end() || jobField.second != "true")
                    continue;
                ROWOWJob& job = *jobIter->second;
                if (bonusStatsCell.empty() == false)
                {
                    job.BonusStatsByJobLevel.assign(job.MaxJobLevel, std::array<int32, ROWOW_STAT_COUNT>{});
                    for (auto const& bonusEntry : ROWOWDataTable::ParseMapList(bonusStatsCell))
                    {
                        uint32 level = 0;
                        for (auto const& bonusField : bonusEntry)
                            if (bonusField.first == "Level")
                                level = uint32(strtoul(bonusField.second.c_str(), nullptr, 10));
                        if (level == 0 || level > job.MaxJobLevel)
                            continue;
                        for (auto const& bonusField : bonusEntry)
                            for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
                                if (bonusField.first == StatNames[statIndex])
                                    job.BonusStatsByJobLevel[level - 1][statIndex] += int32(strtol(bonusField.second.c_str(), nullptr, 10));
                    }
                    for (size_t levelIndex = 1; levelIndex < job.BonusStatsByJobLevel.size(); levelIndex++)
                        for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
                            job.BonusStatsByJobLevel[levelIndex][statIndex] += job.BonusStatsByJobLevel[levelIndex - 1][statIndex];
                }
                for (auto const& aspdField : ROWOWDataTable::ParseMap(baseASPDCell))
                {
                    int32 weaponType = GetWeaponTypeByName(aspdField.first);
                    if (weaponType >= 0)
                        job.BaseASPDByWeaponType[weaponType] = int32(strtol(aspdField.second.c_str(), nullptr, 10));
                }
            }
        }
    }
    for (auto const& jobByID : JobsByID)
        if (jobByID.second.BaseHPByLevel.size() < jobByID.second.MaxBaseLevel || jobByID.second.BaseSPByLevel.size() < jobByID.second.MaxBaseLevel)
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadStatusData found the RO job {} without base HP or SP for every level", jobByID.second.Name);

    // Each weapon type's damage against each size (size_fix.yml; a size left out is 100%)
    for (uint8 weaponType = 0; weaponType < ROWOW_WEAPON_TYPE_MAX; weaponType++)
        for (uint8 size = 0; size < ROWOW_SIZE_COUNT; size++)
            SizeFixByWeaponType[weaponType][size] = 100;
    if (ROWOWDataTable const* sizeFixTable = GetDataTable(ROWOW_DATA_TABLE_SIZE_FIX))
    {
        int32 weaponColumnIndex = sizeFixTable->GetColumnIndex("Weapon");
        for (size_t rowIndex = 0; rowIndex < sizeFixTable->Rows.size(); rowIndex++)
        {
            int32 weaponType = GetWeaponTypeByName(sizeFixTable->GetCell(rowIndex, weaponColumnIndex));
            if (weaponType < 0)
                continue;
            for (uint8 size = 0; size < ROWOW_SIZE_COUNT; size++)
                SizeFixByWeaponType[weaponType][size] = int32(sizeFixTable->GetInt(rowIndex, sizeFixTable->GetColumnIndex(SizeFixNames[size]), 100));
        }
    }

    // What the RO rules read of the converted RO items (the post-renewal looks only when they were converted)
    for (char const* itemTableName : { ROWOW_DATA_TABLE_ITEM, ROWOW_DATA_TABLE_ITEM_RENEWAL })
    {
        ROWOWDataTable const* itemTable = GetDataTable(itemTableName);
        if (itemTable == nullptr)
            continue;
        bool isRenewal = strcmp(itemTableName, ROWOW_DATA_TABLE_ITEM_RENEWAL) == 0;
        int32 idColumnIndex = itemTable->GetColumnIndex("Id");
        int32 typeColumnIndex = itemTable->GetColumnIndex("Type");
        int32 subTypeColumnIndex = itemTable->GetColumnIndex("SubType");
        int32 attackColumnIndex = itemTable->GetColumnIndex("Attack");
        int32 defenseColumnIndex = itemTable->GetColumnIndex("Defense");
        int32 rangeColumnIndex = itemTable->GetColumnIndex("Range");
        int32 weaponLevelColumnIndex = itemTable->GetColumnIndex("WeaponLevel");
        int32 locationsColumnIndex = itemTable->GetColumnIndex("Locations");
        int32 jobsColumnIndex = itemTable->GetColumnIndex("Jobs");
        int32 classesColumnIndex = itemTable->GetColumnIndex("Classes");
        int32 genderColumnIndex = itemTable->GetColumnIndex("Gender");
        int32 equipLevelMinColumnIndex = itemTable->GetColumnIndex("EquipLevelMin");
        int32 equipLevelMaxColumnIndex = itemTable->GetColumnIndex("EquipLevelMax");
        int32 refineableColumnIndex = itemTable->GetColumnIndex("Refineable");
        int32 flagsColumnIndex = itemTable->GetColumnIndex("Flags");
        int32 delayColumnIndex = itemTable->GetColumnIndex("Delay");
        int32 noUseColumnIndex = itemTable->GetColumnIndex("NoUse");
        int32 weightColumnIndex = itemTable->GetColumnIndex("Weight");
        int32 tradeColumnIndex = itemTable->GetColumnIndex("Trade");
        int32 buyColumnIndex = itemTable->GetColumnIndex("Buy");
        int32 hatEffectsColumnIndex = itemTable->GetColumnIndex("HatEffects"); // The post-renewal looks' (RoItemsRenewal.csv)
        for (size_t rowIndex = 0; rowIndex < itemTable->Rows.size(); rowIndex++)
        {
            uint32 itemID = uint32(itemTable->GetInt(rowIndex, idColumnIndex, 0));
            auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
            if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
                continue;
            ROWOWItem& item = ItemsByItemTemplateID[itemTemplateIDIter->second];
            item.ItemID = itemID;
            item.ItemTemplateID = itemTemplateIDIter->second;
            item.IsRenewal = isRenewal;
            std::string type = itemTable->GetCell(rowIndex, typeColumnIndex);
            if (type.empty() == true)
                type = "Etc";
            for (auto const& itemTypeName : ItemTypeNames)
                if (type == itemTypeName.first)
                    item.Type = itemTypeName.second;
            item.IsWeapon = type == "Weapon";
            item.IsArmor = type == "Armor";
            item.IsAmmo = type == "Ammo";
            item.IsDelayConsume = item.Type == ROWOW_ITEM_TYPE_DELAYCONSUME;

            // Jobs and Classes default to all.  A Jobs map lists the jobs that may (or with All, may not) use it
            std::vector<std::pair<std::string, std::string>> jobFields = ROWOWDataTable::ParseMap(itemTable->GetCell(rowIndex, jobsColumnIndex));
            if (jobFields.empty() == false)
            {
                item.JobMask = 0;
                for (auto const& jobField : jobFields)
                {
                    if (jobField.first == "All")
                    {
                        item.JobMask = jobField.second == "true" ? ROWOW_ITEM_JOB_ALL : 0;
                        continue;
                    }
                    for (int32 itemJobIndex = 0; itemJobIndex < ROWOW_ITEM_JOB_COUNT; itemJobIndex++)
                    {
                        if (jobField.first != ROWOWItemJobNames[itemJobIndex])
                            continue;
                        if (jobField.second == "true")
                            item.JobMask |= uint64(1) << itemJobIndex;
                        else
                            item.JobMask &= ~(uint64(1) << itemJobIndex);
                    }
                }
            }
            std::vector<std::pair<std::string, std::string>> classFields = ROWOWDataTable::ParseMap(itemTable->GetCell(rowIndex, classesColumnIndex));
            if (classFields.empty() == false)
            {
                item.ClassMask = 0;
                for (auto const& classField : classFields)
                {
                    if (classField.first == "All")
                    {
                        item.ClassMask = classField.second == "true" ? ROWOW_ITEM_CLASS_ALL : 0;
                        continue;
                    }
                    for (auto const& itemClassName : ItemClassNames)
                    {
                        if (classField.first != itemClassName.first)
                            continue;
                        if (classField.second == "true")
                            item.ClassMask |= itemClassName.second;
                        else
                            item.ClassMask &= ~itemClassName.second;
                    }
                }
            }
            std::string const& gender = itemTable->GetCell(rowIndex, genderColumnIndex);
            if (gender == "Female")
                item.Gender = ROWOW_GENDER_FEMALE;
            else if (gender == "Male")
                item.Gender = ROWOW_GENDER_MALE;
            item.EquipLevelMin = uint32(std::max<int64>(0, itemTable->GetInt(rowIndex, equipLevelMinColumnIndex, 0)));
            item.EquipLevelMax = uint32(std::max<int64>(0, itemTable->GetInt(rowIndex, equipLevelMaxColumnIndex, 0)));
            item.IsRefineable = itemTable->GetCell(rowIndex, refineableColumnIndex) == "true";
            for (auto const& flagField : ROWOWDataTable::ParseMap(itemTable->GetCell(rowIndex, flagsColumnIndex)))
            {
                if (flagField.first == "Container")
                    item.IsContainer = flagField.second == "true";
                else if (flagField.first == "DeadBranch")
                    item.IsDeadBranch = flagField.second == "true";
                else if (flagField.first == "BuyingStore")
                    item.IsBuyingStore = flagField.second == "true";
            }
            for (auto const& delayField : ROWOWDataTable::ParseMap(itemTable->GetCell(rowIndex, delayColumnIndex)))
                if (delayField.first == "Duration")
                    item.DelayDuration = uint32(std::max(0, atoi(delayField.second.c_str())));
            for (auto const& noUseField : ROWOWDataTable::ParseMap(itemTable->GetCell(rowIndex, noUseColumnIndex)))
                if (noUseField.first == "Sitting")
                    item.IsNoUseSitting = noUseField.second == "true";
            item.Weight = int32(std::max<int64>(0, itemTable->GetInt(rowIndex, weightColumnIndex, 0)));
            item.Buy = int32(std::max<int64>(0, itemTable->GetInt(rowIndex, buyColumnIndex, 0)));
            for (auto const& tradeField : ROWOWDataTable::ParseMap(itemTable->GetCell(rowIndex, tradeColumnIndex)))
            {
                if (tradeField.first == "NoTrade")
                    item.IsNoTrade = tradeField.second == "true";
                else if (tradeField.first == "NoSell")
                    item.IsNoSell = tradeField.second == "true";
                else if (tradeField.first == "NoCart")
                    item.IsNoCart = tradeField.second == "true";
                else if (tradeField.first == "NoStorage")
                    item.IsNoStorage = tradeField.second == "true";
                else if (tradeField.first == "NoGuildStorage")
                    item.IsNoGuildStorage = tradeField.second == "true";
                else if (tradeField.first == "NoMail")
                    item.IsNoMail = tradeField.second == "true";
            }
            item.HatEffectNames = ROWOWDataTable::ParseTextList(itemTable->GetCell(rowIndex, hatEffectsColumnIndex));
            for (std::string& hatEffectName : item.HatEffectNames) // The converter's mod_rowow_hat_effect goes by the upper case name
                std::transform(hatEffectName.begin(), hatEffectName.end(), hatEffectName.begin(), [](unsigned char c) { return char(std::toupper(c)); });
            if (item.IsWeapon == true)
                item.WeaponType = uint8(std::max(0, GetWeaponTypeByName(itemTable->GetCell(rowIndex, subTypeColumnIndex))));
            if (item.IsAmmo == true)
            {
                std::string const& subType = itemTable->GetCell(rowIndex, subTypeColumnIndex);
                for (uint8 ammoType = 1; ammoType < std::size(AmmoTypeNames); ammoType++)
                    if (subType == AmmoTypeNames[ammoType])
                        item.AmmoType = ammoType;
            }
            for (auto const& locationField : ROWOWDataTable::ParseMap(itemTable->GetCell(rowIndex, locationsColumnIndex)))
                for (auto const& locationName : LocationNames)
                    if (locationField.first == locationName.first && locationField.second == "true")
                        item.Locations |= locationName.second;
            item.Attack = int32(itemTable->GetInt(rowIndex, attackColumnIndex, 0));
            item.Defense = int32(itemTable->GetInt(rowIndex, defenseColumnIndex, 0));
            item.Range = int32(itemTable->GetInt(rowIndex, rangeColumnIndex, 0));
            item.WeaponLevel = int32(itemTable->GetInt(rowIndex, weaponLevelColumnIndex, 0));
        }
    }

    // The maps with no exp penalty on death (nopenalty), by any of their RO names
    if (ROWOWDataTable const* mapFlagTable = GetDataTable(ROWOW_DATA_TABLE_MAP_FLAG))
    {
        int32 mapColumnIndex = mapFlagTable->GetColumnIndex("Map");
        int32 flagColumnIndex = mapFlagTable->GetColumnIndex("Flag");
        int32 valueColumnIndex = mapFlagTable->GetColumnIndex("Value");
        for (size_t rowIndex = 0; rowIndex < mapFlagTable->Rows.size(); rowIndex++)
        {
            std::string const& flag = mapFlagTable->GetCell(rowIndex, flagColumnIndex);
            if (flag != "nopenalty" && flag != "noexppenalty")
                continue;
            uint32 mapID = GetMapIDByMapName(mapFlagTable->GetCell(rowIndex, mapColumnIndex));
            if (mapID == 0)
                continue;
            if (mapFlagTable->GetCell(rowIndex, valueColumnIndex) == "off")
                NoPenaltyMapIDs.erase(mapID);
            else
                NoPenaltyMapIDs.insert(mapID);
        }
    }

    LOG_INFO("module.ROWOW", "ROWOWMod::LoadStatusData loaded {} RO items for the status calculation and {} maps without a death penalty", ItemsByItemTemplateID.size(),
        NoPenaltyMapIDs.size());
}

ROWOWItem const* ROWOWMod::GetROItem(Item const* item) const
{
    if (item == nullptr)
        return nullptr;
    auto itemIter = ItemsByItemTemplateID.find(item->GetEntry());
    if (itemIter == ItemsByItemTemplateID.end())
        return nullptr;
    return &itemIter->second;
}

void ROWOWMod::CalculateIdentityStatus(Player* player, ROWOWIdentity& identity) const
{
    // status_calc_pc_sub (the base status), then status_calc_bl_main with every flag (the battle status: the statuses' changes)
    ROWOWStatus status;
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player));
    if (sc != nullptr && sc->IsEmpty() == true)
        sc = nullptr;
    status.IsPlayer = true;
    status.Level = identity.BaseLevel;
    status.Mode = ROWOW_MODE_PC;
    ROWOWJob const* job = GetJob(identity.JobID);
    status.Size = (job != nullptr && job->IsBaby == true) ? ROWOW_SIZE_SMALL : ROWOW_SIZE_MEDIUM; // (character_size is 0: riding changes nothing)

    // The worn items, their refine, scripts and cards, the ammo and the item combos, then the statuses' scripts (StatusCalcPCItems)
    StatusCalcPCItems(player, identity, status);
    status.WeaponType = GetCombinedWeaponType(status.RightWeaponType, status.LeftWeaponType);
    status.Range = std::max(1, status.Range);
    status.Bonus.DoubleRate += status.Bonus.DoubleAddRate;
    status.Bonus.PerfectHit += status.Bonus.PerfectHitAdd;
    status.Bonus.SplashRange += status.Bonus.SplashAddRange;

    // Damage modifiers from weapon type
    for (uint8 size = 0; size < ROWOW_SIZE_COUNT; size++)
    {
        status.WeaponSizeMods[size] = SizeFixByWeaponType[status.RightWeaponType][size];
        status.LeftWeaponSizeMods[size] = SizeFixByWeaponType[status.LeftWeaponType][size];
    }
    if (PCIsRiding(player) == true && (status.WeaponType == ROWOW_WEAPON_TYPE_1HSPEAR || status.WeaponType == ROWOW_WEAPON_TYPE_2HSPEAR))
    {
        // When Riding with spear, damage modifier to mid-class becomes same as versus large size.
        status.WeaponSizeMods[ROWOW_SIZE_MEDIUM] = status.WeaponSizeMods[ROWOW_SIZE_BIG];
        status.LeftWeaponSizeMods[ROWOW_SIZE_MEDIUM] = status.LeftWeaponSizeMods[ROWOW_SIZE_BIG];
    }

    // ----- STATS CALCULATION -----
    // Job bonuses, the passive skills, then the base stats.  If a Super Novice has never died and is at least joblv 70, he gets all stats +10
    if (job != nullptr && identity.JobLevel >= 1 && identity.JobLevel <= job->BonusStatsByJobLevel.size())
        for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
            status.JobBonusStats[statIndex] = job->BonusStatsByJobLevel[identity.JobLevel - 1][statIndex];
    int32 skillStats[ROWOW_STAT_COUNT] = { };
    int32 skill = 0;
    if (PCCheckSkill(identity, ROWOW_SKILL_BS_HILTBINDING) > 0)
        skillStats[ROWOW_STAT_STR]++;
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_SA_DRAGONOLOGY)) > 0)
        skillStats[ROWOW_STAT_INT] += (skill + 1) / 2; // +1 INT / 2 lv
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_AC_OWL)) > 0)
        skillStats[ROWOW_STAT_DEX] += skill;
    if (GetNonBabyJobID(identity.JobID) == ROWOW_JOB_SUPER_NOVICE && identity.JobLevel >= 70 && GetIdentityVariable(identity, ROWOW_PCDIECOUNTER_VAR) == 0)
        for (int32& skillStat : skillStats)
            skillStat += 10;
    for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
        status.Stats[statIndex] = std::clamp<int32>(status.JobBonusStats[statIndex] + skillStats[statIndex] + int32(identity.Stats[statIndex]) +
            status.Bonus.ParamBonus[statIndex] + status.Bonus.ParamEquip[statIndex], 0, USHRT_MAX);

    // The base ATK bonus of the weapon type held (bWeaponAtk)
    if (status.WeaponType < ROWOW_WEAPON_TYPE_MAX && status.Bonus.WeaponAtk[status.WeaponType] != 0)
        status.BaseAttack += status.Bonus.WeaponAtk[status.WeaponType];
    // Absolute modifiers from passive skills
    if (PCCheckSkill(identity, ROWOW_SKILL_BS_HILTBINDING) > 0)
        status.BaseAttack += 4;

    // ----- HP MAX CALCULATION -----
    status.MaxHP = StatusCalcMaxHPPC(identity, status, sc);
    if (BattleConfig.HPRate != 100)
        status.MaxHP = uint32(BattleConfig.HPRate * (status.MaxHP / 100.));
    if (identity.BaseLevel < 100)
        status.MaxHP = std::clamp<uint32>(status.MaxHP, 1, uint32(BattleConfig.MaxHPLevel99));
    else if (identity.BaseLevel < 151)
        status.MaxHP = std::clamp<uint32>(status.MaxHP, 1, uint32(BattleConfig.MaxHPLevel150));
    else
        status.MaxHP = std::clamp<uint32>(status.MaxHP, 1, uint32(BattleConfig.MaxHP));

    // ----- SP MAX CALCULATION -----
    status.MaxSP = StatusCalcMaxSPPC(identity, status, sc);
    if (BattleConfig.SPRate != 100)
        status.MaxSP = uint32(BattleConfig.SPRate * (status.MaxSP / 100.));
    status.MaxSP = std::clamp<uint32>(status.MaxSP, 1, uint32(BattleConfig.MaxSP));

    // ----- MISC CALCULATION -----
    StatusCalcMisc(status, identity.BaseLevel);

    // The rate bonuses of the gear (and of the statuses that raise the matk rate, which status_calc_pc_sub gives as pc_bonus SP_MATK_RATE)
    ROWOWBonus& bonus = status.Bonus;
    if (sc != nullptr)
    {
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_INCMATKRATE))
            bonus.MatkRate += sce->Val1;
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_MINDBREAKER))
            bonus.MatkRate += sce->Val2;
    }
    auto applyRate = [](int32& value, int32& rate)
    {
        if (rate < 0)
            rate = 0;
        if (rate != 100)
            value = value * rate / 100;
    };
    applyRate(status.MagicAttackMax, bonus.MatkRate);
    status.MagicAttackMin = bonus.MatkRate != 100 ? status.MagicAttackMin * bonus.MatkRate / 100 : status.MagicAttackMin;
    applyRate(status.Hit, bonus.HitRate);
    applyRate(status.Flee, bonus.FleeRate);
    applyRate(status.Def2, bonus.Def2Rate);
    applyRate(status.MDef2, bonus.Mdef2Rate);
    applyRate(status.Critical, bonus.CriticalRate);
    status.Critical = std::clamp<int32>(status.Critical, SHRT_MIN, SHRT_MAX);
    applyRate(status.Flee2, bonus.Flee2Rate);

    // ----- HIT CALCULATION -----
    // Absolute modifiers from passive skills
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_BS_WEAPONRESEARCH)) > 0)
        status.Hit += skill * 2;
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_AC_VULTURE)) > 0)
    {
        status.Hit += skill;
        if (status.WeaponType == ROWOW_WEAPON_TYPE_BOW)
            status.Range += skill;
    }
    if (status.WeaponType >= ROWOW_WEAPON_TYPE_REVOLVER && status.WeaponType <= ROWOW_WEAPON_TYPE_GRENADE)
    {
        if ((skill = PCCheckSkill(identity, ROWOW_SKILL_GS_SINGLEACTION)) > 0)
            status.Hit += 2 * skill;
        if ((skill = PCCheckSkill(identity, ROWOW_SKILL_GS_SNAKEEYE)) > 0)
        {
            status.Hit += skill;
            status.Range += skill;
        }
    }

    // ----- FLEE CALCULATION -----
    // Absolute modifiers from passive skills (a second job of the Thief line gets more of Improve Dodge)
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_TF_MISS)) > 0)
        status.Flee += skill * ((job != nullptr && job->FirstJobID == ROWOW_JOB_THIEF) ? 4 : 3);
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_MO_DODGE)) > 0)
        status.Flee += (skill * 3) / 2;

    // ----- EQUIPMENT-DEF CALCULATION -----
    if (bonus.DefRate < 0)
        bonus.DefRate = 0;
    if (bonus.DefRate != 100)
        status.Def = std::clamp<int32>(status.Def * bonus.DefRate / 100, CHAR_MIN, CHAR_MAX);
    if (BattleConfig.WeaponDefenseType == 0 && status.Def > BattleConfig.MaxDef)
    {
        status.Def2 += BattleConfig.OverDefBonus * (status.Def - BattleConfig.MaxDef);
        status.Def = BattleConfig.MaxDef;
    }

    // ----- EQUIPMENT-MDEF CALCULATION -----
    if (bonus.MdefRate < 0)
        bonus.MdefRate = 0;
    if (bonus.MdefRate != 100)
        status.MDef = std::clamp<int32>(status.MDef * bonus.MdefRate / 100, CHAR_MIN, CHAR_MAX);
    if (BattleConfig.MagicDefenseType == 0 && status.MDef > BattleConfig.MaxDef)
    {
        status.MDef2 += BattleConfig.OverDefBonus * (status.MDef - BattleConfig.MaxDef);
        status.MDef = BattleConfig.MaxDef;
    }

    // ----- ASPD CALCULATION -----
    // pc_maxaspd / AMOTION_DIVIDER_PC and MIN_ASPD / AMOTION_DIVIDER_PC
    // (status_calc_bl_main then applies the ASPD rate, which the gear's bAspdRate lowered, and caps it again)
    status.AttackMotion = std::clamp(StatusBaseAmotionPC(identity, status), BattleConfig.MaxAspdAmotion / 2, ROWOW_MIN_ASPD / 2);
    // Relative modifiers from passive skills, and riding a Peco (slower but for Cavalier Mastery)
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_SA_ADVANCEDBOOK)) > 0 && status.WeaponType == ROWOW_WEAPON_TYPE_BOOK)
        status.AspdRate -= 5 * skill;
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_SG_DEVIL)) > 0 && PCIsMaxJobLevel(identity) == true)
        status.AspdRate -= 30 * skill;
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_GS_SINGLEACTION)) > 0 && status.WeaponType >= ROWOW_WEAPON_TYPE_REVOLVER &&
        status.WeaponType <= ROWOW_WEAPON_TYPE_GRENADE)
        status.AspdRate -= ((skill + 1) / 2) * 10;
    if (PCIsRiding(player) == true)
        status.AspdRate += 500 - 100 * PCCheckSkill(identity, ROWOW_SKILL_KN_CAVALIERMASTERY);
    status.AttackDelay = 2 * status.AttackMotion;

    // ----- DMOTION -----
    status.DamageMotion = std::clamp(800 - status.Stats[ROWOW_STAT_AGI] * 4, 400, 800);
    if (BattleConfig.PCDamageDelayRate != 100)
        status.DamageMotion = status.DamageMotion * BattleConfig.PCDamageDelayRate / 100;

    // ----- MISC CALCULATIONS -----
    // Skill SP cost
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_HP_MANARECHARGE)) > 0)
        bonus.UseSPRate -= 4 * skill;
    if (sc != nullptr && sc->HasSCE(ROWOW_SC_SERVICE4U) == true)
        bonus.UseSPRate -= sc->GetSCE(ROWOW_SC_SERVICE4U)->Val3;
    bonus.UseSPRate = std::max(0, bonus.UseSPRate);
    bonus.CastRate = std::max(0, bonus.CastRate);
    bonus.HPRecovRate = std::max(0, bonus.HPRecovRate);
    bonus.SPRecovRate = std::max(0, bonus.SPRecovRate);

    // Anti-element and anti-race
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_CR_TRUST)) > 0)
        bonus.SubEle[ROWOW_ELEMENT_HOLY] += skill * 5;
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_BS_SKINTEMPER)) > 0)
    {
        bonus.SubEle[ROWOW_ELEMENT_NEUTRAL] += skill;
        bonus.SubEle[ROWOW_ELEMENT_FIRE] += skill * 5;
    }
    if ((skill = PCCheckSkill(identity, ROWOW_SKILL_SA_DRAGONOLOGY)) > 0)
    {
        bonus.RightWeapon.AddRace[ROWOW_RACE_DRAGON] += skill * 4;
        bonus.LeftWeapon.AddRace[ROWOW_RACE_DRAGON] += skill * 4; // left_cardfix_to_right is off
        bonus.SubRace[ROWOW_RACE_DRAGON] += skill * 4;
        bonus.Deferred.push_back({ "bMagicAddRace", { ROWOW_RACE_DRAGON, skill * 2 } }); // magic_addrace, for magic (R6)
    }
    if (sc != nullptr)
    {
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_SIEGFRIED))
        {
            for (int32 element : { ROWOW_ELEMENT_WATER, ROWOW_ELEMENT_EARTH, ROWOW_ELEMENT_FIRE, ROWOW_ELEMENT_WIND, ROWOW_ELEMENT_POISON, ROWOW_ELEMENT_HOLY,
                ROWOW_ELEMENT_DARK, ROWOW_ELEMENT_GHOST, ROWOW_ELEMENT_UNDEAD })
                bonus.SubEle[element] += sce->Val2;
        }
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_PROVIDENCE))
        {
            bonus.SubEle[ROWOW_ELEMENT_HOLY] += sce->Val2;
            bonus.SubRace[ROWOW_RACE_DEMON] += sce->Val2;
        }
    }

    identity.BaseStatus = status;
    StatusCalcBLMain(player, identity.BaseStatus, sc, identity.Status);
}

int32 ROWOWMod::StatusBaseAmotionPC(ROWOWIdentity const& identity, ROWOWStatus const& status) const
{
    // status_base_amotion_pc
    ROWOWJob const* job = GetJob(identity.JobID);
    if (job == nullptr)
        return 2000; // AMOTION_ZERO_ASPD

    // Base weapon delay
    int32 amotion = (status.WeaponType < ROWOW_WEAPON_TYPE_MAX)
        ? job->BaseASPDByWeaponType[status.WeaponType] // Single weapon
        : (job->BaseASPDByWeaponType[status.RightWeaponType] + job->BaseASPDByWeaponType[status.LeftWeaponType]) * 7 / 10; // Dual-wield

    // Percentual delay reduction from stats
    amotion -= amotion * (4 * status.Stats[ROWOW_STAT_AGI] + status.Stats[ROWOW_STAT_DEX]) / 1000;
    // Raw delay adjustment from bAspd bonus
    amotion += status.Bonus.AspdAdd;
    return amotion;
}

int32 ROWOWMod::StatusBaseAtk(ROWOWStatus const& status) const
{
    // status_base_atk
    if ((GetBattleObjectType(status) & BattleConfig.EnableBaseAtk) == 0)
        return 0;
    int32 str = status.Stats[ROWOW_STAT_STR];
    int32 dex = status.Stats[ROWOW_STAT_DEX];
    if (status.IsPlayer == true && IsRangedWeaponType(status.WeaponType) == true)
        std::swap(str, dex);
    int32 dstr = str / 10;
    str += dstr * dstr;
    if (status.FollowerType != ROWOW_BL_HOM) // Normally only players have base-atk, but homunc have a different batk equation
        str += dex / 5 + status.Stats[ROWOW_STAT_LUK] / 5;
    return std::clamp(str, 0, int32(USHRT_MAX));
}

void ROWOWMod::StatusCalcMisc(ROWOWStatus& status, uint32 level) const
{
    // status_calc_misc.  A player's values stack with the earlier bonuses, and a monster's are set (all of them start at 0 here)
    int32 int_ = status.Stats[ROWOW_STAT_INT];
    int32 objectType = GetBattleObjectType(status);

    // Matk
    status.MagicAttackMin = int_ + (int_ / 7) * (int_ / 7); // status_base_matk_min
    status.MagicAttackMax = int_ + (int_ / 5) * (int_ / 5); // status_base_matk_max
    // Hit
    status.Hit = std::clamp<int32>(status.Hit + int32(level) + status.Stats[ROWOW_STAT_DEX], 1, SHRT_MAX);
    // Flee
    status.Flee = std::clamp<int32>(status.Flee + int32(level) + status.Stats[ROWOW_STAT_AGI], 1, SHRT_MAX);
    // Def2
    status.Def2 = std::clamp<int32>(status.Def2 + status.Stats[ROWOW_STAT_VIT], 0, SHRT_MAX);
    // Mdef2
    status.MDef2 = std::clamp<int32>(status.MDef2 + int_ + (status.Stats[ROWOW_STAT_VIT] / 2), 0, SHRT_MAX);
    // Critical: every 1 LUK is +0.3 critical
    if ((objectType & BattleConfig.EnableCritical) != 0)
        status.Critical = std::clamp<int32>(status.Critical + 10 + (status.Stats[ROWOW_STAT_LUK] * 10 / 3), 1, SHRT_MAX);
    else
        status.Critical = 0;
    // Perfect dodge: every 10 LUK is +1
    if ((objectType & BattleConfig.EnablePerfectFlee) != 0)
        status.Flee2 = std::clamp<int32>(status.Flee2 + status.Stats[ROWOW_STAT_LUK] + 10, 0, SHRT_MAX);
    else
        status.Flee2 = 0;

    status.BaseAttack += StatusBaseAtk(status);

    if (status.Critical != 0 && status.IsPlayer == false)
    {
        int32 criticalRate = status.FollowerType != 0 ? BattleConfig.CriticalRate : BattleConfig.MobCriticalRate; // Players don't have a critical adjustment setting as of yet.
        if (criticalRate != 100)
            status.Critical = std::clamp<int32>(status.Critical * criticalRate / 100, 1, SHRT_MAX);
        if (status.Critical == 0 && criticalRate != 0)
            status.Critical = 10;
    }

    // Monsters don't regenerate (BL_REGEN has no monsters), and a homunculus's or mercenary's own regen is in StatusCalcBLMain, on its battle status
    if (status.IsPlayer == true)
        StatusCalcRegen(nullptr, status);
}

uint32 ROWOWMod::StatusCalcMaxHPPC(ROWOWIdentity const& identity, ROWOWStatus const& status, ROWOWStatusChange const* sc) const
{
    // status_calc_maxhp_pc, with the bonuses of status_get_hpbonus (the gear's, Faith's and the statuses')
    int32 vit = status.Stats[ROWOW_STAT_VIT];
    ROWOWJob const* job = GetJob(identity.JobID);
    if (job == nullptr || job->BaseHPByLevel.empty() == true)
        return 1;
    uint32 level = std::min<uint32>(std::max<uint32>(identity.BaseLevel, 1) - 1, uint32(job->BaseHPByLevel.size()) - 1);
    double dmax = job->BaseHPByLevel[level];
    if (vit > 0)
        dmax *= (1.0 + vit * 0.01);
    if (job->IsUpper == true)
        dmax *= 1.25;
    else if (PCIsTaekwonRanker(identity) == true)
        dmax *= 3;
    // Vit from equip gives +1 additional HP
    dmax += status.Bonus.ParamEquip[ROWOW_STAT_VIT];
    int32 skill = PCCheckSkill(identity, ROWOW_SKILL_CR_TRUST);
    dmax += status.Bonus.HP + (skill > 0 ? skill * 200 : 0) + StatusGetHPBonus(sc, ROWOW_STATUS_BONUS_FIX);
    double equipBonus = dmax * (status.Bonus.HPRate - 100) / 100;
    dmax += equipBonus;
    // Aegis accuracy
    dmax += double(int64(dmax * StatusGetHPBonus(sc, ROWOW_STATUS_BONUS_RATE) / 100));
    if (dmax < 1.0)
        dmax = 1.0;
    return std::max<uint32>(1, uint32(dmax));
}

uint32 ROWOWMod::StatusCalcMaxSPPC(ROWOWIdentity const& identity, ROWOWStatus const& status, ROWOWStatusChange const* sc) const
{
    // status_calc_maxsp_pc, with the bonuses of status_get_spbonus (the gear's, Kaina's and the statuses')
    int32 int_ = status.Stats[ROWOW_STAT_INT];
    ROWOWJob const* job = GetJob(identity.JobID);
    if (job == nullptr || job->BaseSPByLevel.empty() == true)
        return 1;
    uint32 level = std::min<uint32>(std::max<uint32>(identity.BaseLevel, 1) - 1, uint32(job->BaseSPByLevel.size()) - 1);
    double dmax = job->BaseSPByLevel[level];
    if (int_ > 0)
        dmax *= (1.0 + int_ * 0.01);
    if (job->IsUpper == true)
        dmax *= 1.25;
    else if (PCIsTaekwonRanker(identity) == true)
        dmax *= 3;
    // Int from equip gives +1 additional SP
    dmax += status.Bonus.ParamEquip[ROWOW_STAT_INT];
    int32 skill = PCCheckSkill(identity, ROWOW_SKILL_SL_KAINA);
    dmax += status.Bonus.SP + (skill > 0 ? 30 * skill : 0) + StatusGetSPBonus(sc, ROWOW_STATUS_BONUS_FIX);
    double equipBonus = dmax * (status.Bonus.SPRate - 100) / 100;
    double itemBonus = dmax * std::max(-100, StatusGetSPBonus(sc, ROWOW_STATUS_BONUS_ITEM)) / 100;
    dmax += equipBonus + itemBonus;
    // The rate of the passive skills and the statuses (Aegis accuracy)
    int32 rate = PCCheckSkill(identity, ROWOW_SKILL_HP_MEDITATIO) + 2 * PCCheckSkill(identity, ROWOW_SKILL_HW_SOULDRAIN) + StatusGetSPBonus(sc, ROWOW_STATUS_BONUS_RATE);
    dmax += double(int64(dmax * std::max(-100, rate) / 100));
    if (dmax < 1.0)
        dmax = 1.0;
    return std::max<uint32>(1, uint32(dmax));
}

void ROWOWMod::StatusCalcRegen(Unit const* unit, ROWOWStatus& status) const
{
    // status_calc_regen, for a player: the natural regen with the recovery rate bonuses, then (with the unit) the regen skills: Meditatio's
    // SP, Increase HP Recovery and Increase SP Recovery (and the Ninja's Ninpou) every 10 s standing still, Spiritual Cadence sitting, and
    // Moving HP-Recovery's walking regen
    int32 value = (status.Stats[ROWOW_STAT_VIT] / 5) + std::max<int32>(1, int32(status.MaxHP / 200));
    if (status.Bonus.HPRecovRate != 100)
        value = value * status.Bonus.HPRecovRate / 100;
    status.RegenHP = std::clamp<int32>(value, 0, SHRT_MAX);

    value = 1 + (status.Stats[ROWOW_STAT_INT] / 6) + int32(status.MaxSP / 100);
    if (status.Stats[ROWOW_STAT_INT] >= 120)
        value += ((status.Stats[ROWOW_STAT_INT] - 120) / 2) + 4;
    if (status.Bonus.SPRecovRate != 100)
        value = value * status.Bonus.SPRecovRate / 100;
    status.RegenSP = std::clamp<int32>(value, 0, SHRT_MAX);
    if (unit == nullptr)
        return;

    int32 skill = 0;
    if ((skill = PCCheckSkill(unit, ROWOW_SKILL_HP_MEDITATIO)) > 0)
        status.RegenSP = std::clamp<int32>(status.RegenSP * (100 + 3 * skill) / 100, 1, SHRT_MAX);
    // Only players have skill/sitting skill regen for now.
    value = 0;
    if ((skill = PCCheckSkill(unit, ROWOW_SKILL_SM_RECOVERY)) > 0)
        value += skill * 5 + skill * int32(status.MaxHP) / 500;
    status.SkillRegenHP = std::clamp<int32>(value, 0, SHRT_MAX);
    value = 0;
    if ((skill = PCCheckSkill(unit, ROWOW_SKILL_MG_SRECOVERY)) > 0)
        value += skill * 3 + skill * int32(status.MaxSP) / 500;
    if ((skill = PCCheckSkill(unit, ROWOW_SKILL_NJ_NINPOU)) > 0)
        value += skill * 3 + skill * int32(status.MaxSP) / 500;
    status.SkillRegenSP = std::clamp<int32>(value, 0, SHRT_MAX);
    // Skill-related recovery (only when sit): Spiritual Cadence, and a Taekwon's Peaceful Break and Happy Break while resting with another
    // (Kaina powers up the Enjoyable Rest)
    ROWOWIdentity const* restIdentity = unit != nullptr && unit->IsPlayer() == true ? unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
    bool isRest = restIdentity != nullptr && restIdentity->IsTaekwonRest == true;
    value = 0;
    if ((skill = PCCheckSkill(unit, ROWOW_SKILL_MO_SPIRITSRECOVERY)) > 0)
        value += skill * 4 + skill * int32(status.MaxHP) / 500;
    if ((skill = PCCheckSkill(unit, ROWOW_SKILL_TK_HPTIME)) > 0 && isRest == true)
        value += skill * 30 + skill * int32(status.MaxHP) / 500;
    status.SitSkillRegenHP = std::clamp<int32>(value, 0, SHRT_MAX);
    value = 0;
    if ((skill = PCCheckSkill(unit, ROWOW_SKILL_TK_SPTIME)) > 0 && isRest == true)
    {
        value += skill * 3 + skill * int32(status.MaxSP) / 500;
        if ((skill = PCCheckSkill(unit, ROWOW_SKILL_SL_KAINA)) > 0) // Power up Enjoyable Rest
            value += (30 + 10 * skill) * value / 100;
    }
    if ((skill = PCCheckSkill(unit, ROWOW_SKILL_MO_SPIRITSRECOVERY)) > 0)
        value += skill * 2 + skill * int32(status.MaxSP) / 500;
    status.SitSkillRegenSP = std::clamp<int32>(value, 0, SHRT_MAX);
    status.IsRegenWalk = PCCheckSkill(unit, ROWOW_SKILL_SM_MOVINGRECOVERY) > 0;
}

int32 ROWOWMod::PCMaxParameter(ROWOWIdentity const& identity) const
{
    // pc_maxparameter, for the pre-renewal jobs (Babies first; no third jobs here)
    ROWOWJob const* job = GetJob(identity.JobID);
    if (job != nullptr && job->IsBaby == true)
        return BattleConfig.MaxBabyParameter;
    return (job != nullptr && job->IsUpper == true) ? BattleConfig.MaxTransParameter : BattleConfig.MaxParameter;
}

int32 ROWOWMod::PCNeedStatusPoint(ROWOWIdentity const& identity, uint8 statIndex, int32 value) const
{
    // pc_need_status_point
    if (value == 0 || statIndex >= ROWOW_STAT_COUNT)
        return 0;
    int32 low = int32(identity.Stats[statIndex]);
    int32 max = PCMaxParameter(identity);
    if (low >= max && value > 0)
        return 0; // Official servers show '0' when max is reached
    int32 high = low + value;
    if (value < 0)
        std::swap(low, high);
    int32 statusPoints = 0;
    for (; low < high; low++)
        statusPoints += GetStatusPointCost(low);
    return statusPoints;
}

bool ROWOWMod::PCStatusUp(Player* player, uint8 statIndex, int32 increase)
{
    // pc_statusup
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->Exists == false || statIndex >= ROWOW_STAT_COUNT || increase <= 0)
        return false;

    // Check limits (pc_maxparameterincrease)
    int32 current = int32(identity->Stats[statIndex]);
    int32 maxParameter = PCMaxParameter(*identity);
    int32 finalValue = current;
    int32 statusPoints = int32(identity->StatusPoints);
    while (finalValue <= maxParameter && statusPoints >= 0)
    {
        statusPoints -= GetStatusPointCost(finalValue);
        finalValue++;
    }
    finalValue--;
    int32 maxIncrease = finalValue > current ? finalValue - current : 0;
    increase = std::clamp(increase, 0, maxIncrease);
    if (increase <= 0 || current + increase > maxParameter)
        return false;

    // Check status points
    int32 neededPoints = PCNeedStatusPoint(*identity, statIndex, increase);
    if (neededPoints < 0 || neededPoints > int32(identity->StatusPoints))
        return false;

    identity->Stats[statIndex] = uint32(current + increase);
    identity->StatusPoints -= uint32(neededPoints);
    RefreshROForm(player);
    return true;
}

bool ROWOWMod::PCIsMaxBaseLevel(ROWOWIdentity const& identity) const
{
    ROWOWJob const* job = GetJob(identity.JobID);
    return job == nullptr || identity.BaseLevel >= job->MaxBaseLevel;
}

bool ROWOWMod::PCIsMaxJobLevel(ROWOWIdentity const& identity) const
{
    ROWOWJob const* job = GetJob(identity.JobID);
    return job == nullptr || identity.JobLevel >= job->MaxJobLevel;
}

uint64 ROWOWMod::PCNextBaseExp(ROWOWIdentity const& identity) const
{
    // pc_nextbaseexp
    if (identity.BaseLevel == 0)
        return 0;
    if (PCIsMaxBaseLevel(identity) == true)
        return ROWOW_MAX_LEVEL_BASE_EXP;
    ROWOWJob const* job = GetJob(identity.JobID);
    if (job == nullptr || identity.BaseLevel > job->BaseExpByLevel.size())
        return 0;
    return job->BaseExpByLevel[identity.BaseLevel - 1];
}

uint64 ROWOWMod::PCNextJobExp(ROWOWIdentity const& identity) const
{
    // pc_nextjobexp
    if (identity.JobLevel == 0)
        return 0;
    if (PCIsMaxJobLevel(identity) == true)
        return ROWOW_MAX_LEVEL_JOB_EXP;
    ROWOWJob const* job = GetJob(identity.JobID);
    if (job == nullptr || identity.JobLevel > job->JobExpByLevel.size())
        return 0;
    return job->JobExpByLevel[identity.JobLevel - 1];
}

void ROWOWMod::PCCalcExp(Player* player, uint64& baseExp, uint64& jobExp, Unit const* src) const
{
    // pc_calcexp: the receiver's own exp bonuses against the source, and the boosts, which a quest's exp (no source) gets too
    int32 bonus = 0;
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (ROWOWStatus const* status = src != nullptr ? GetROStatus(src) : nullptr)
    {
        ROWOWBonus const& playerBonus = identity->Status.Bonus;
        bonus += playerBonus.ExpAddRace[status->Race];
        bonus += playerBonus.ExpAddRace[ROWOW_RACE_ALL];
        bonus += playerBonus.ExpAddClass[status->Class];
        bonus += playerBonus.ExpAddClass[ROWOW_CLASS_ALL];
    }
    // Give EXPBOOST for quests even if src is nullptr.
    if (ROWOWStatusChangeEntry const* sce = GetSCE(player, ROWOW_SC_EXPBOOST))
        bonus += sce->Val1;
    if (baseExp != 0)
        baseExp = std::clamp<uint64>(uint64(double(baseExp) + double(baseExp) * (bonus / 100.)), 1, std::numeric_limits<uint32>::max());

    // Give JEXPBOOST for quests even if src is nullptr.
    if (ROWOWStatusChangeEntry const* sce = GetSCE(player, ROWOW_SC_JEXPBOOST))
        bonus += sce->Val1;
    if (jobExp != 0)
        jobExp = std::clamp<uint64>(uint64(double(jobExp) + double(jobExp) * (bonus / 100.)), 1, std::numeric_limits<uint32>::max());
}

void ROWOWMod::PCGainExp(Player* player, uint64 baseExp, uint64 jobExp, Unit* src)
{
    // pc_gainexp
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsInForm == false || player->IsAlive() == false)
        return;

    bool isMaxBaseLevel = PCIsMaxBaseLevel(*identity);
    bool isMaxJobLevel = PCIsMaxJobLevel(*identity);
    PCCalcExp(player, baseExp, jobExp, src);
    uint64 nextBaseExp = PCNextBaseExp(*identity);
    uint64 nextJobExp = PCNextJobExp(*identity);
    if (isMaxBaseLevel == true)
    {
        if (identity->BaseExp >= ROWOW_MAX_LEVEL_BASE_EXP)
            baseExp = 0;
        else if (identity->BaseExp + baseExp >= ROWOW_MAX_LEVEL_BASE_EXP)
            baseExp = ROWOW_MAX_LEVEL_BASE_EXP - identity->BaseExp;
    }
    if (isMaxJobLevel == true)
    {
        if (identity->JobExp >= ROWOW_MAX_LEVEL_JOB_EXP)
            jobExp = 0;
        else if (identity->JobExp + jobExp >= ROWOW_MAX_LEVEL_JOB_EXP)
            jobExp = ROWOW_MAX_LEVEL_JOB_EXP - identity->JobExp;
    }
    if (BattleConfig.MaxExpGainRate != 0 && (baseExp != 0 || jobExp != 0))
    {
        if (nextBaseExp > 0 && float(baseExp) / float(nextBaseExp) > BattleConfig.MaxExpGainRate / 1000.)
            baseExp = uint64(BattleConfig.MaxExpGainRate / 1000. * nextBaseExp);
        if (nextJobExp > 0 && float(jobExp) / float(nextJobExp) > BattleConfig.MaxExpGainRate / 1000.)
            jobExp = uint64(BattleConfig.MaxExpGainRate / 1000. * nextJobExp);
    }

    // Give EXP for Base Level, then Job Level
    if (baseExp != 0)
    {
        identity->BaseExp = uint32(std::min<uint64>(uint64(identity->BaseExp) + baseExp, std::numeric_limits<uint32>::max()));
        if (PCCheckBaseLevelUp(player) == false)
            player->SetUInt32Value(PLAYER_XP, identity->BaseExp);
    }
    if (jobExp != 0)
    {
        identity->JobExp = uint32(std::min<uint64>(uint64(identity->JobExp) + jobExp, std::numeric_limits<uint32>::max()));
        PCCheckJobLevelUp(player);
    }
    // (No message: rAthena's disp_experience is off, and the Basic Info window shows the bars)
    if (baseExp != 0 || jobExp != 0)
        SendROStatusToClient(player);
}

bool ROWOWMod::PCCheckBaseLevelUp(Player* player)
{
    // pc_checkbaselevelup
    ROWOWIdentity* identity = GetIdentity(player);
    uint64 next = PCNextBaseExp(*identity);
    if (next == 0 || identity->BaseExp < next || PCIsMaxBaseLevel(*identity) == true)
        return false;

    do
    {
        identity->BaseExp -= uint32(next);
        // Kyoki pointed out that the max overcarry exp is the exp needed for the previous level -1. [Skotlex]
        if ((BattleConfig.MultiLevelUp == false || (BattleConfig.MultiLevelUpBase > 0 && identity->BaseLevel >= uint32(BattleConfig.MultiLevelUpBase))) &&
            identity->BaseExp > next - 1)
            identity->BaseExp = uint32(next - 1);

        // pc_gets_status_point: the table's points at the next level less those at this one
        auto nextPointsIter = StatusPointsByBaseLevel.find(identity->BaseLevel + 1);
        auto currentPointsIter = StatusPointsByBaseLevel.find(identity->BaseLevel);
        uint32 nextPoints = nextPointsIter != StatusPointsByBaseLevel.end() ? nextPointsIter->second : 0;
        uint32 currentPoints = currentPointsIter != StatusPointsByBaseLevel.end() ? currentPointsIter->second : 0;
        if (nextPoints > currentPoints)
            identity->StatusPoints += nextPoints - currentPoints;
        identity->BaseLevel++;

        if (PCIsMaxBaseLevel(*identity) == true)
        {
            identity->BaseExp = std::min<uint32>(identity->BaseExp, ROWOW_MAX_LEVEL_BASE_EXP);
            break;
        }
    } while ((next = PCNextBaseExp(*identity)) > 0 && identity->BaseExp >= next);

    // status_calc_pc and status_percent_heal(sd, 100, 100), then the level up effect
    RefreshROForm(player);
    if (player->IsAlive() == true)
    {
        player->SetHealth(player->GetMaxHealth());
        player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    }
    PCLevelUpAngel(player);
    CastLevelUpEffect(player, ROWOW_SPELL_BASE_LEVEL_UP);
    PetOwnerLevelUp(player);
    ChatHandler(player->GetSession()).PSendSysMessage("Base level up! You are now base level {}.", identity->BaseLevel);
    return true;
}

bool ROWOWMod::PCCheckJobLevelUp(Player* player)
{
    // pc_checkjoblevelup
    ROWOWIdentity* identity = GetIdentity(player);
    uint64 next = PCNextJobExp(*identity);
    if (next == 0 || identity->JobExp < next || PCIsMaxJobLevel(*identity) == true)
        return false;

    do
    {
        identity->JobExp -= uint32(next);
        // Kyoki pointed out that the max overcarry exp is the exp needed for the previous level -1. [Skotlex]
        if ((BattleConfig.MultiLevelUp == false || (BattleConfig.MultiLevelUpJob > 0 && identity->JobLevel >= uint32(BattleConfig.MultiLevelUpJob))) &&
            identity->JobExp > next - 1)
            identity->JobExp = uint32(next - 1);

        identity->JobLevel++;
        identity->SkillPoints++;

        if (PCIsMaxJobLevel(*identity) == true)
        {
            identity->JobExp = std::min<uint32>(identity->JobExp, ROWOW_MAX_LEVEL_JOB_EXP);
            break;
        }
    } while ((next = PCNextJobExp(*identity)) > 0 && identity->JobExp >= next);

    RefreshROForm(player);
    CastLevelUpEffect(player, ROWOW_SPELL_JOB_LEVEL_UP);
    ChatHandler(player->GetSession()).PSendSysMessage("Job level up! You are now job level {}.", identity->JobLevel);
    return true;
}

void ROWOWMod::PCDeadPenalty(Player* player)
{
    // The exp penalty of pc_dead: a share of the exp to the next level (death_penalty_type 1) or of the exp held (2), in hundredths of a
    // percent.  Novices (Novice Highs and Babies too) never lose exp, nor does anyone on a nopenalty map
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsInForm == false || BattleConfig.DeathPenaltyType == 0)
        return;
    if (GetNonBabyJobID(identity->JobID) == ROWOW_JOB_NOVICE || identity->JobID == ROWOW_JOB_NOVICE_HIGH)
        return;
    if (NoPenaltyMapIDs.find(player->GetMapId()) != NoPenaltyMapIDs.end())
        return;

    uint64 basePenalty = 0;
    uint64 jobPenalty = 0;
    if (((BattleConfig.DeathPenaltyMaxLevel & 1) != 0 || PCIsMaxBaseLevel(*identity) == false) && BattleConfig.DeathPenaltyBase > 0)
    {
        switch (BattleConfig.DeathPenaltyType)
        {
            case 1: basePenalty = uint64(PCNextBaseExp(*identity) * (BattleConfig.DeathPenaltyBase / 10000.)); break;
            case 2: basePenalty = uint64(identity->BaseExp * (BattleConfig.DeathPenaltyBase / 10000.)); break;
        }
        basePenalty = std::min<uint64>(identity->BaseExp, basePenalty);
    }
    if (((BattleConfig.DeathPenaltyMaxLevel & 2) != 0 || PCIsMaxJobLevel(*identity) == false) && BattleConfig.DeathPenaltyJob > 0)
    {
        switch (BattleConfig.DeathPenaltyType)
        {
            case 1: jobPenalty = uint64(PCNextJobExp(*identity) * (BattleConfig.DeathPenaltyJob / 10000.)); break;
            case 2: jobPenalty = uint64(identity->JobExp * (BattleConfig.DeathPenaltyJob / 10000.)); break;
        }
        jobPenalty = std::min<uint64>(identity->JobExp, jobPenalty);
    }
    if (basePenalty == 0 && jobPenalty == 0)
        return;
    PCLostExp(player, basePenalty, jobPenalty);
}

void ROWOWMod::PCLostExp(Player* player, uint64 baseExp, uint64 jobExp)
{
    // pc_lostexp: never more than the exp held
    ROWOWIdentity* identity = GetIdentity(player);
    baseExp = std::min<uint64>(identity->BaseExp, baseExp);
    jobExp = std::min<uint64>(identity->JobExp, jobExp);
    if (baseExp == 0 && jobExp == 0)
        return;
    identity->BaseExp -= uint32(baseExp);
    identity->JobExp -= uint32(jobExp);
    player->SetUInt32Value(PLAYER_XP, identity->BaseExp);
    ChatHandler(player->GetSession()).PSendSysMessage("You lost {} base experience and {} job experience.", baseExp, jobExp);
    SendROStatusToClient(player);
}

void ROWOWMod::MobLogDamage(Creature* creature, Unit* attacker, uint32 damage, uint32 damageTanked)
{
    // mob_log_damage: players in the form only (their pets are put away there), a summoned monster's damage counting as its master's.  The
    // damage counted is what the monster had left to lose.  damageTanked is what the monster dealt to the player (status_damage's call), which
    // counts for the MVP
    if (creature == nullptr || attacker == nullptr || (damage == 0 && damageTanked == 0) || GetROMobID(creature) == 0)
        return;
    Player const* player = BattleGetMaster(attacker)->ToPlayer();
    if (player == nullptr || IsInROForm(player) == false)
        return;
    damage = std::min<uint32>(damage, creature->GetHealth());
    if (damage == 0 && damageTanked == 0)
        return;

    int32 flag = GetFollowerDamageLogFlag(attacker);
    ROWOWDamageLog* damageLog = creature->CustomData.GetDefault<ROWOWDamageLog>("ROWOWDamageLog");
    for (ROWOWDamageLogEntry& entry : damageLog->Entries)
    {
        if (entry.PlayerGUID == player->GetGUID() && entry.Flag == flag)
        {
            entry.Damage += damage;
            entry.DamageTanked += damageTanked;
            return;
        }
    }
    if (damageLog->Entries.size() == ROWOW_DAMAGE_LOG_SIZE)
        damageLog->Entries.pop_front(); // The oldest goes
    ROWOWDamageLogEntry entry;
    entry.PlayerGUID = player->GetGUID();
    entry.Flag = flag;
    entry.Damage = damage;
    entry.DamageTanked = damageTanked;
    damageLog->Entries.push_back(entry);
}

void ROWOWMod::MobDeadExp(Creature* creature, Unit* killer)
{
    // The exp part of mob_dead: each attacker's share of the damage (exp_calc_type 0, Aegis's) of the monster's base and job exp, more
    // for each extra attacker (exp_bonus_attacker).  Attackers off the map don't count, and dead ones get nothing.  An attacker's share goes to its party
    // when that's set to Even Share (party_exp_share), all its members' shares in one sum, the members' own bonuses applying to what each gets (pc_calcexp)
    ROWOWDamageLog* damageLog = creature->CustomData.Get<ROWOWDamageLog>("ROWOWDamageLog");
    if (damageLog == nullptr || damageLog->Entries.empty() == true)
        return;
    // Self damage counts as an exp tap (mob_revive's lost HP), never as an attacker's share, which MobDeadAttackerCount and this only skip: it isn't logged
    // Only player-summoned mobs do not give exp. [Skotlex]
    if (ROWOWMobSummon const* summon = creature->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon"); summon != nullptr && summon->AI != ROWOW_AI_NONE &&
        summon->MasterGUID.IsEmpty() == false)
    {
        damageLog->Entries.clear();
        return;
    }
    std::deque<ROWOWDamageLogEntry> entries;
    entries.swap(damageLog->Entries);

    uint32 mobID = GetROMobID(creature);
    int64 mobRowIndex = GetMobRowIndex(mobID);
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    if (mobRowIndex < 0 || mobTable == nullptr)
        return;
    // mob_db's exp, at the configured rates (as rAthena applies them when loading mob_db)
    uint64 mobBaseExp = uint64(std::max<int64>(0, mobTable->GetInt(size_t(mobRowIndex), mobTable->GetColumnIndex("BaseExp"), 0))) * BattleConfig.BaseExpRate / 100;
    uint64 mobJobExp = uint64(std::max<int64>(0, mobTable->GetInt(size_t(mobRowIndex), mobTable->GetColumnIndex("JobExp"), 0))) * BattleConfig.JobExpRate / 100;
    if (mobBaseExp == 0 && mobJobExp == 0)
        return;

    std::vector<Player*> players(entries.size(), nullptr);
    int32 count = 0;
    uint64 totalDamage = 0;
    for (size_t i = 0; i < entries.size(); i++)
    {
        totalDamage += entries[i].Damage;
        Player* player = ObjectAccessor::GetPlayer(*creature, entries[i].PlayerGUID);
        if (player == nullptr)
            continue; // Not on this map
        count++;
        if (player->IsAlive() == false || IsInROForm(player) == false)
            continue;
        if (entries[i].Flag == ROWOW_MDLF_HOMUN && HomIsActive(GetHomunculus(player)) == false)
            continue; // Skip homunculus' share if inactive
        players[i] = player;
    }

    // Bonus on top of your share (common to all attackers): Rich Man Kim on the monster, and the killer's Star Gladiator blessing and Taekwon
    // mission (mob_dead's sd)
    int32 bonus = 100;
    if (ROWOWStatusChangeEntry const* richManKim = GetSCE(creature, ROWOW_SC_RICHMANKIM))
        bonus += richManKim->Val2;
    if (Player* killerPlayer = killer != nullptr ? killer->ToPlayer() : nullptr; killerPlayer != nullptr && IsInROForm(killerPlayer) == true)
        MobDeadSGTaekwon(creature, killerPlayer, bonus);

    // Apply first-attacker 200% exp share bonus
    if (BattleConfig.ExpCalcType == 2 && count > 1)
    {
        totalDamage += entries[0].Damage;
        entries[0].Damage += entries[0].Damage;
    }

    struct PartyShare
    {
        Group* PartyGroup = nullptr;
        uint64 BaseExp = 0;
        uint64 JobExp = 0;
    };
    std::vector<PartyShare> partyShares;
    for (size_t i = 0; i < entries.size(); i++)
    {
        if (players[i] == nullptr)
            continue;
        double per; // Your share of the mob's exp
        if (BattleConfig.ExpCalcType == 1 || totalDamage == 0)
            per = double(entries[i].Damage) / double(creature->GetMaxHealth()); // eAthena's exp formula based on max hp
        else
            per = double(entries[i].Damage) / double(totalDamage); // Aegis's exp formula based on total damage
        if (per > 1)
            per = 1; // To prevent exploits
        if (count > 1 && BattleConfig.ExpBonusAttacker != 0)
        {
            // Exp bonus per additional attacker
            if (count > BattleConfig.ExpBonusMaxAttacker)
                count = BattleConfig.ExpBonusMaxAttacker;
            per += per * ((count - 1) * BattleConfig.ExpBonusAttacker) / 100.;
        }

        uint64 baseExp = mobBaseExp != 0 ? uint64(std::clamp(double(mobBaseExp) * per * bonus / 100., 1.0, double(std::numeric_limits<uint32>::max()))) : 0;
        uint64 jobExp = mobJobExp != 0 ? uint64(std::clamp(double(mobJobExp) * per * bonus / 100., 1.0, double(std::numeric_limits<uint32>::max()))) : 0;
        if (entries[i].Flag == ROWOW_MDLF_HOMUN)
            jobExp = 0; // Homun earned job-exp is always lost.

        // Locate the party, possibly add it; else its own
        bool isOwnExp = true;
        if (Group* group = players[i]->GetGroup())
        {
            auto shareIter = std::find_if(partyShares.begin(), partyShares.end(), [group](PartyShare const& share) { return share.PartyGroup == group; });
            if (shareIter != partyShares.end())
            {
                shareIter->BaseExp += baseExp;
                shareIter->JobExp += jobExp;
                isOwnExp = false;
            }
            else if (GetPartyOption(group).IsExpShare == true)
            {
                PartyShare share;
                share.PartyGroup = group;
                share.BaseExp = baseExp;
                share.JobExp = jobExp;
                partyShares.push_back(share);
                isOwnExp = false;
            }
        }
        if (baseExp != 0 && entries[i].Flag == ROWOW_MDLF_HOMUN)
            if (ROWOWHomunData* hd = GetHomunculus(players[i]))
                HomGainExp(players[i], *hd, baseExp);
        if (isOwnExp == true)
            PCGainExp(players[i], baseExp, jobExp, creature);
    }
    for (PartyShare const& share : partyShares)
        PartyExpShare(share.PartyGroup, creature, share.BaseExp, share.JobExp);
}

void ROWOWMod::UpdateNaturalRegen(Player* player, uint32 diff)
{
    // status_natural_heal for a player with no regen skills: HP every natural_healhp_interval and SP every natural_healsp_interval, twice as
    // often sitting and as fast as the statuses' regen rates.  Walking stops HP regen (and starts its interval over), not SP.  Statuses stop
    // either (status_calc_regen_rate's flag), and nothing regenerates while hidden or cloaked
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->IsInForm == false || player->IsAlive() == false)
        return;
    ROWOWStatus const& status = identity->Status;
    uint8 flag = status.RegenFlag;
    // Only skill-based regen is disabled at max HP/SP
    if ((flag & ROWOW_RGN_SHP) != 0 && player->GetHealth() >= player->GetMaxHealth())
        flag &= ~ROWOW_RGN_SHP;
    if ((flag & ROWOW_RGN_SSP) != 0 && player->GetPower(POWER_MANA) >= player->GetMaxPower(POWER_MANA))
        flag &= ~ROWOW_RGN_SSP;
    if (ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player)); sc != nullptr &&
        (sc->Option & (ROWOW_OPTION_HIDE | ROWOW_OPTION_CLOAK | ROWOW_OPTION_CHASEWALK)) != 0)
        flag = 0;
    int32 multi = player->IsSitState() == true ? 2 : 1;

    // Apply sitting regen bonus (ssregen), every natural_heal_skill_interval of rate-scaled time
    auto skillRegen = [this, player](uint32& timer, uint32 diffTime, int32 rate, int32 hp, int32 sp)
    {
        timer += uint32(diffTime * (rate / 100.));
        while (timer >= uint32(BattleConfig.NaturalHealSkillInterval))
        {
            timer -= uint32(BattleConfig.NaturalHealSkillInterval);
            if (StatusHeal(player, hp, sp, 3, player, 0) < hp + sp)
                break; // Full
        }
    };
    if (player->IsSitState() == true && player->isMoving() == false)
    {
        if ((flag & ROWOW_RGN_SHP) != 0 && status.SitSkillRegenHP != 0)
            skillRegen(identity->SitSkillRegenHPTimer, diff, 100, status.SitSkillRegenHP, 0);
        if ((flag & ROWOW_RGN_SSP) != 0 && status.SitSkillRegenSP != 0)
            skillRegen(identity->SitSkillRegenSPTimer, diff, 100, 0, status.SitSkillRegenSP);
    }
    // Walking stops the skill regen, and HP regen but with Moving HP-Recovery
    if (player->isMoving() == true)
    {
        flag &= ~(ROWOW_RGN_SHP | ROWOW_RGN_SSP);
        if (status.IsRegenWalk == false)
            flag &= ~ROWOW_RGN_HP;
    }

    if ((flag & ROWOW_RGN_HP) == 0)
        identity->RegenHPTimer = 0;
    else
    {
        identity->RegenHPTimer += diff;
        // Half recovery while moving only applies to players with certain traits
        uint32 interval = uint32(BattleConfig.NaturalHealHPInterval / (status.RegenRateHP / 100. * multi)) * (player->isMoving() == true ? 2 : 1);
        if (identity->RegenHPTimer >= interval)
        {
            identity->RegenHPTimer = 0;
            if (player->GetHealth() < player->GetMaxHealth())
                player->ModifyHealth(status.RegenHP);
        }
    }

    // Skill regen (sregen)
    if ((flag & ROWOW_RGN_SHP) != 0 && status.SkillRegenHP != 0)
        skillRegen(identity->SkillRegenHPTimer, diff, 100, status.SkillRegenHP, 0);
    if ((flag & ROWOW_RGN_SSP) != 0 && status.SkillRegenSP != 0)
    {
        identity->SkillRegenSPTimer += diff;
        while (identity->SkillRegenSPTimer >= uint32(BattleConfig.NaturalHealSkillInterval))
        {
            identity->SkillRegenSPTimer -= uint32(BattleConfig.NaturalHealSkillInterval);
            int32 val = status.SkillRegenSP;
            if (identity->IsDoriDori == true)
            {
                val *= 2;
                identity->IsDoriDori = false;
                if (int32 rate = PCCheckSkill(player, ROWOW_SKILL_TK_SPTIME))
                    SCStart(player, player, ROWOW_SC_EARTHSCROLL, 100, rate, SkillGetTime(ROWOW_SKILL_TK_SPTIME, uint16(rate)));
                if ((PCJobIDToMapID(identity->JobID) & ROWOW_MAPID_UPPERMASK) == ROWOW_MAPID_STAR_GLADIATOR && int32(urand(0, 9999)) < BattleConfig.SGAngelSkillRatio)
                {
                    // Angel of the Sun/Moon/Star
                    PCResetHate(player);
                    PCResetFeel(player);
                }
            }
            if (StatusHeal(player, 0, val, 3, player, 0) < val)
                break; // Full
        }
    }

    if ((flag & ROWOW_RGN_SP) == 0)
        identity->RegenSPTimer = 0;
    else
    {
        identity->RegenSPTimer += diff;
        if (identity->RegenSPTimer >= uint32(BattleConfig.NaturalHealSPInterval / (status.RegenRateSP / 100. * multi)))
        {
            identity->RegenSPTimer = 0;
            if (player->GetPower(POWER_MANA) < player->GetMaxPower(POWER_MANA))
                player->ModifyPower(POWER_MANA, status.RegenSP);
        }
    }

    // Kept in step, for the SP a respawn keeps (death empties the power)
    identity->HP = player->GetHealth();
    identity->SP = player->GetPower(POWER_MANA);
}

void ROWOWMod::SendROStatusToClient(Player* player)
{
    // "ROWOWSTATUS\t<job>\t<base level>,<base exp>,<next base exp>,<job level>,<job exp>,<next job exp>,<status points>,<skill points>\t
    // <STR>,..,<LUK>\t<bonus STR>,..\t<raise cost STR>,..\t<the status window's right side>", the window's values as rAthena's
    // pre-renewal clif_couplestatus (the bonus is the battle status's stat over the base stat: job bonus, gear and statuses) and
    // pc_leftside_* / pc_rightside_* show them
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsInForm == false)
        return;
    ROWOWJob const* job = GetJob(identity->JobID);
    ROWOWStatus const& status = identity->Status;
    std::string stats;
    std::string bonusStats;
    std::string raiseCosts;
    for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
    {
        char const* separator = statIndex == 0 ? "" : ",";
        stats += fmt::format("{}{}", separator, identity->Stats[statIndex]);
        bonusStats += fmt::format("{}{}", separator, status.Stats[statIndex] - int32(identity->Stats[statIndex]));
        raiseCosts += fmt::format("{}{}", separator, PCNeedStatusPoint(*identity, statIndex, 1));
    }
    // R9.5: then "\t<1 when the Star Gladiator's Devil blinds it>" (pc_setnewpc / pc_jobchange's EFST_DEVIL1 at the max job level with SG_DEVIL)
    bool isDevilBlind = PCCheckSkill(*identity, ROWOW_SKILL_SG_DEVIL) > 0 && PCIsMaxJobLevel(*identity) == true;
    std::string addonMessage = fmt::format("ROWOWSTATUS\t{}\t{},{},{},{},{},{},{},{}\t{}\t{}\t{}\t{},{},{},{},{},{},{},{},{},{},{},{},{}\t{}",
        job != nullptr ? job->Name : "?", identity->BaseLevel, identity->BaseExp, PCNextBaseExp(*identity), identity->JobLevel, identity->JobExp, PCNextJobExp(*identity),
        identity->StatusPoints, identity->SkillPoints, stats, bonusStats, raiseCosts,
        status.BaseAttack + status.WeaponAttack + status.LeftWeaponAttack, status.WeaponRefineAttack + status.LeftWeaponRefineAttack, status.MagicAttackMin,
        status.MagicAttackMax, status.Def, status.Def2, status.MDef, status.MDef2 - (status.Stats[ROWOW_STAT_VIT] / 2), status.Hit, status.Flee, status.Flee2 / 10,
        status.Critical / 10, (2000 - status.AttackMotion) / 10, isDevilBlind == true ? 1 : 0);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
    player->SendDirectMessage(&data);
}
