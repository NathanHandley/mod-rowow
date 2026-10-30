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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/merchant, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Item.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"
#include "ROWOW_Script.h"

using namespace std;

// The Merchant line's skills (rAthena's skills/merchant, skill_factory_merchant), one class per skill: the Merchant's, Blacksmith's,
// Alchemist's, Whitesmith's and Creator's.  Their passives are in the status and battle code, forging and brewing in ROWOW_Produce.cpp.
// They run in the caster's map thread

namespace
{
    // SkillMammonite: a weapon attack paid in zeny (the skill's ZenyCost, used up when the cast ends)
    class ROWOWSkillMammonite : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillMammonite() : ROWOWWeaponSkillImpl(ROWOW_SKILL_MC_MAMMONITE) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50 * skillLevel;
        }
    };

    // SkillCartRevolution: a neutral weapon attack on the target and around it, 150% plus 1% a percent of the cart's weight (monsters at
    // full), knocking west
    class ROWOWSkillCartRevolution : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillCartRevolution() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_MC_CARTREVOLUTION) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* src, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50;
            ROWOWIdentity const* identity = src->IsPlayer() == true ? src->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
            if (identity != nullptr && identity->CartWeight != 0 && identity->CartWeightMax > 0)
                baseSkillRatio += 100 * identity->CartWeight / identity->CartWeightMax; // +1% every 1% weight
            else if (src->IsPlayer() == false)
                baseSkillRatio += 100; // Max damage for non players.
        }

        void ModifyHitRate(int16& hitRate, Unit const* src, Unit const* /*target*/, uint16 /*skillLevel*/) const override
        {
            int32 remodeling = ROWOW->PCCheckSkill(src, ROWOW_SKILL_GN_REMODELING_CART);
            if (remodeling != 0)
                hitRate += int16(remodeling * 4);
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            flag |= ROWOW_SD_PREAMBLE; // a fake packet will be sent for the first target to be hit
            ROWOWRecursiveDamageSplashSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
        }
    };

    // SkillChangeCart: the cart's look is picked after (clif_parse_ChangeCart, the cart window's list); the cast itself only shows.
    // SkillDecorateCart opens that list (clif_SelectCart)
    class ROWOWSkillChangeCart : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillChangeCart(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd != nullptr && SkillID == ROWOW_SKILL_MC_CARTDECORATE)
                ROWOW->SendCartToClient(sd, *ROWOW->GetIdentity(sd), true);
        }
    };

    // SkillItemAppraisal: the list of the unidentified items (clif_item_identify_list); with none, nothing is used up
    class ROWOWSkillItemAppraisal : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillItemAppraisal() : ROWOWSkillImpl(ROWOW_SKILL_MC_IDENTIFY) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            ROWOW->SendItemIdentifyList(sd);
            if (ROWOW->GetIdentity(sd)->IsIdentifyMenuOpen == false)
                flag |= ROWOW_SKILL_NOCONSUME_REQ; // failed, dont consume anything
        }
    };

    // SkillVending: the shop setup of the cart's items (clif_openvendingreq, as many items as 2 + the level)
    class ROWOWSkillVending : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillVending() : ROWOWSkillImpl(ROWOW_SKILL_MC_VENDING) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                ROWOW->VendingOpenVendingReq(sd, uint8(skillLevel));
        }
    };

    // SkillAdrenalineRush, SkillWeaponPerfection and SkillPowerThrust: the status (val2 1 on the caster itself, whose share is bigger) on
    // the caster, or in a party on each member on the map within the skill's splash (party_foreachsamemap through skill_area_sub, which comes
    // back here with flag & 1).  A player gets it only holding one of the skill's weapons
    class ROWOWSkillPartyWeaponStatus : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillPartyWeaponStatus(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            Player* sd = src->ToPlayer();
            if (sd == nullptr || sd->GetGroup() == nullptr || (flag & 1) != 0)
            {
                uint32 weapontype = skill->RequiredWeapons;
                if (weapontype == 0 || target->IsPlayer() == false || ROWOW->PCCheckWeaponType(target, weapontype) == true)
                    ROWOW->SCStart2(src, target, skill->StatusID, 100, skillLevel, src == target ? 1 : 0, ROWOW->SkillGetTime(SkillID, skillLevel));
                return;
            }
            int32 areaFlag = flag | ROWOW_BCT_PARTY | 1;
            ROWOW->PartyForEachSameMap(sd, ROWOW->SkillGetSplash(SkillID, skillLevel), [this, src, skillLevel, tick, areaFlag](Unit* member)
            {
                return ROWOW->SkillAreaSub(member, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
            });
        }
    };

    // SkillHammerFall: everything around the spot may be stunned a second later (a timer skill), at 20% + 10% a level (50% + 5% from level 7)
    class ROWOWSkillHammerFall : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHammerFall() : ROWOWSkillImpl(ROWOW_SKILL_BS_HAMMERFALL) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAddTimerSkill(src, tick + 1000, target->GetGUID(), 0, 0, SkillID, skillLevel, std::min(20 + 10 * skillLevel, 50 + 5 * skillLevel), flag);
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            int32 i = ROWOW->SkillGetSplash(SkillID, skillLevel);
            int32 areaFlag = flag | ROWOW_BCT_ENEMY | 2;
            ROWOW->MapForEachInArea(src, x - i, y - i, x + i, y + i, [this, src, skillLevel, tick, areaFlag](Unit* bl)
            {
                return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
            });
        }
    };

    // SkillWeaponRepair: the target's broken items listed (clif_item_repair_list); the one picked is mended (skill_repairweapon)
    class ROWOWSkillWeaponRepair : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillWeaponRepair() : ROWOWSkillImpl(ROWOW_SKILL_BS_REPAIRWEAPON) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            Player* dstsd = target->ToPlayer();
            if (sd != nullptr && dstsd != nullptr)
                ROWOW->ClifItemRepairList(sd, dstsd, skillLevel);
        }
    };

    // SkillGreed: picks up the floor items around the caster (skill_greed).  RO monsters' drops are still their corpses' loot, not floor
    // items (the ground drops are R10), so there is nothing for it to pick up yet
    class ROWOWSkillGreed : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGreed() : ROWOWSkillImpl(ROWOW_SKILL_BS_GREED) { }
    };

    // SkillAcidTerror: a weapon attack (50% a level; DEF ignored and VIT DEF taken off the damage, in battle_calc_attack) that may make the
    // target bleed (3% a level) and break its armor (Duration1 in 10000 a hit).  A hit Pneuma blocks still does both (skill_additional_effect)
    class ROWOWSkillAcidTerror : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillAcidTerror() : ROWOWWeaponSkillImpl(ROWOW_SKILL_AM_ACIDTERROR) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -50 + 50 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart2(src, target, ROWOW_SC_BLEEDING, skillLevel * 3, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel));
            ROWOW->SkillBreakEquip(src, target, ROWOW_EQUIP_POSITION_ARMOR, int32(100 * ROWOW->SkillGetTime(SkillID, skillLevel)), ROWOW_BCT_ENEMY);
        }
    };

    // SkillBomb (Demonstration): a ground unit that burns what stands in it (a fire weapon attack, 100% + 20% a level, from
    // skill_unit_onplace_timer) and may break its weapon (1% a level)
    class ROWOWSkillBomb : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBomb() : ROWOWSkillImpl(ROWOW_SKILL_AM_DEMONSTRATION) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 20 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SkillBreakEquip(src, target, ROWOW_EQUIP_POSITION_HAND_R, 100 * skillLevel, ROWOW_BCT_ENEMY);
        }
    };

    // SkillAidPotion and SkillAidBerserkPotion: the level's potion (its Requires_ItemCost by level) thrown at the target, its heal (its
    // script's itemheal or percentheal, run with potion_flag 1) raised by Aid Potion and Potion Research, the target's VIT/INT and Increase
    // HP/SP Recovery; its statuses go to the target for half their time.  A Berserk Potion needs the target at its base level.  A monster
    // throws a set heal by level
    class ROWOWSkillAidPotion : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillAidPotion(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            Player* dstsd = target->ToPlayer();
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            ROWOWStatusChange const* tsc = ROWOW->GetStatusChange(static_cast<Unit const*>(target));
            int32 hp = 0, sp = 0;
            if (tstatus == nullptr)
                return;
            if (sd != nullptr)
            {
                int32 bonus = 100;
                ROWOWSkillCondition require = ROWOW->SkillGetRequirement(sd, SkillID, skillLevel);
                int32 x = skillLevel % 11 - 1;
                Item* item = require.ItemIDs[x] != 0 ? ROWOW->PCSearchInventory(sd, require.ItemIDs[x]) : nullptr;
                ROWOWItem const* roItem = ROWOW->GetROItem(item);
                if (item == nullptr || roItem == nullptr || int32(item->GetCount()) < require.ItemAmounts[x])
                {
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                    flag |= ROWOW_SKILL_NOCONSUME_REQ;
                    return;
                }
                if (SkillID == ROWOW_SKILL_AM_BERSERKPITCHER && dstsd != nullptr && ROWOW->GetIdentity(dstsd)->BaseLevel < roItem->EquipLevelMin)
                {
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                    flag |= ROWOW_SKILL_NOCONSUME_REQ;
                    return;
                }
                ROWOWPotionHeal potion;
                potion.Target = target;
                if (roItem->UseScript != nullptr && roItem->UseScript->Error.empty() == true)
                {
                    ROWOWScriptContext context;
                    context.AttachedPlayer = sd;
                    context.Identity = ROWOW->GetIdentity(sd);
                    context.UsedItemTemplateID = item->GetEntry();
                    context.Potion = &potion;
                    ROWOWSCRIPT->Run(*roItem->UseScript, context);
                }
                if (ROWOWStatusChangeEntry const* spirit = ROWOW->GetSCE(sd, ROWOW_SC_SPIRIT); spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_ALCHEMIST)
                    bonus += int32(ROWOW->GetIdentity(sd)->BaseLevel);
                int32 pitcherRate = 100 + ROWOW->PCCheckSkill(sd, ROWOW_SKILL_AM_POTIONPITCHER) * 10 + ROWOW->PCCheckSkill(sd, ROWOW_SKILL_AM_LEARNINGPOTION) * 5;
                if (potion.PerHP > 0 || potion.PerSP > 0)
                {
                    hp = int32(target->GetMaxHealth()) * potion.PerHP / 100;
                    hp = hp * pitcherRate * bonus / 10000;
                    if (dstsd != nullptr)
                    {
                        sp = int32(dstsd->GetMaxPower(POWER_MANA)) * potion.PerSP / 100;
                        sp = sp * pitcherRate * bonus / 10000;
                    }
                }
                else
                {
                    if (potion.HP > 0)
                    {
                        hp = potion.HP * pitcherRate * bonus / 10000;
                        hp = hp * (100 + (tstatus->Stats[ROWOW_STAT_VIT] * 2)) / 100;
                        if (dstsd != nullptr)
                            hp = hp * (100 + ROWOW->PCCheckSkill(dstsd, ROWOW_SKILL_SM_RECOVERY) * 10) / 100;
                    }
                    if (potion.SP > 0)
                    {
                        sp = potion.SP * pitcherRate * bonus / 10000;
                        sp = sp * (100 + (tstatus->Stats[ROWOW_STAT_INT] * 2)) / 100;
                        if (dstsd != nullptr)
                            sp = sp * (100 + ROWOW->PCCheckSkill(dstsd, ROWOW_SKILL_MG_SRECOVERY) * 10) / 100;
                    }
                }
                // (The potion item group heal rates, bAddItemGroupHealRate, come with the item heal rate bonuses)
                if (int32 j = ROWOW->PCSkillHealBonus(sd, SkillID))
                {
                    hp += hp * j / 100;
                    sp += sp * j / 100;
                }
            }
            else
            {
                // Maybe replace with potion_hp, but I'm unsure how that works [Playtester]
                switch (skillLevel)
                {
                    case 1: hp = 45; break;
                    case 2: hp = 105; break;
                    case 3: hp = 175; break;
                    default: hp = 325; break;
                }
                hp = (hp + int32(urand(0, skillLevel * 20))) * (150 + skillLevel * 10) / 100;
                hp = hp * (100 + (tstatus->Stats[ROWOW_STAT_VIT] * 2)) / 100;
                if (dstsd != nullptr)
                    hp = hp * (100 + ROWOW->PCCheckSkill(dstsd, ROWOW_SKILL_SM_RECOVERY) * 10) / 100;
            }
            if (int32 j = dstsd != nullptr ? ROWOW->PCSkillHeal2Bonus(dstsd, SkillID) : 0)
            {
                hp += hp * j / 100;
                sp += sp * j / 100;
            }
            // (Final heal increased by HPlus: bHPlus is a renewal bonus)
            if (tsc != nullptr && tsc->IsEmpty() == false)
            {
                int32 penalty = 0;
                if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_CRITICALWOUND))
                    penalty += sce->Val2;
                if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_DEATHHURT); sce != nullptr && sce->Val3 != 0)
                    penalty += 20;
                if (tsc->HasSCE(ROWOW_SC_NORECOVER_STATE) == true)
                    penalty = 100;
                if (penalty > 0)
                {
                    hp -= hp * penalty / 100;
                    sp -= sp * penalty / 100;
                }
            }
            // The heal shows as one heal of the skill's (rAthena shows Heal's and Increase SP Recovery's numbers)
            ROWOW->StatusHeal(target, hp, sp, 0, src, ROWOW->GetSkillSpellID(SkillID, skillLevel));
        }
    };

    // SkillSummonFlora and SkillSummonMarineSphere: the level's plant (Mandragora, Hydra, Flora, Parasite, Geographer) or a Marine Sphere on the
    // spot as the caster's (master_id, AI_FLORA or AI_SPHERE) for the skill's Duration1: the plants attack the caster's enemies, the sphere
    // blows up when hit
    class ROWOWSkillSummonFlora : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillSummonFlora(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            // Correct info, don't change any of this! [celest]
            if (SkillID == ROWOW_SKILL_AM_CANNIBALIZE)
                ROWOW->MobOnceSpawnSub(src, x, y, ROWOWMod::FloraSummonMobIDs[std::clamp<int32>(skillLevel, 1, 5) - 1], ROWOW_AI_FLORA, ROWOW->SkillGetTime(SkillID, skillLevel));
            else
                ROWOW->MobOnceSpawnSub(src, x, y, ROWOW_MOBID_MARINE_SPHERE, ROWOW_AI_SPHERE, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillAlchemicalWeapon, SkillSynthesizedShield, SkillSyntheticArmor and SkillBiochemicalHelm: a player wearing something there is kept
    // from having it broken or stripped (SC_CP_WEAPON...)
    class ROWOWSkillChemicalProtection : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillChemicalProtection(uint16 skillID, uint32 equipPosition) : ROWOWSkillImpl(skillID), EquipPosition(equipPosition) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            Player* dstsd = target->ToPlayer();
            if (sd != nullptr && (dstsd == nullptr || ROWOW->GetIdentityEquippedItem(dstsd, EquipPosition) == nullptr))
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }

    private:
        uint32 EquipPosition;
    };

    // SkillFullProtection: all four Chemical Protections, on what the player wears
    class ROWOWSkillFullProtection : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFullProtection() : ROWOWSkillImpl(ROWOW_SKILL_CR_FULLPROTECTION) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            Player* dstsd = target->ToPlayer();
            uint32 const equip[] = { ROWOW_EQUIP_POSITION_HAND_R, ROWOW_EQUIP_POSITION_HAND_L, ROWOW_EQUIP_POSITION_ARMOR, ROWOW_EQUIP_POSITION_HEAD_TOP };
            int32 s = 0;
            int64 skilltime = ROWOW->SkillGetTime(SkillID, skillLevel);
            for (int32 i = 0; i < 4; i++)
            {
                if (dstsd == nullptr || ROWOW->GetIdentityEquippedItem(dstsd, equip[i]) == nullptr)
                    continue;
                ROWOW->SCStart(src, target, ROWOW_SC_CP_WEAPON + i, 100, skillLevel, skilltime);
                s++;
            }
            if (sd != nullptr && s == 0)
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ; // Don't consume item requirements
            }
        }
    };

    // SkillPreparePotion: the potions the Alchemist can brew listed (clif_skill_produce_mix_list of item level 22); the one picked is made
    // (skill_produce_mix)
    class ROWOWSkillPreparePotion : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillPreparePotion() : ROWOWSkillImpl(ROWOW_SKILL_AM_PHARMACY) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                ROWOW->ClifSkillProduceMixList(sd, SkillID, 22);
        }
    };

    // SkillCallHomunculus, SkillVaporize and SkillHomunculusResurrection: the homunculus comes with R11; until then there is none to call,
    // rest or bring back, so they fail without using anything up
    class ROWOWSkillHomunculus : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillHomunculus(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& flag) const override
        {
            Fail(src, flag);
        }

        void CastendPos2(Unit* src, int32 /*x*/, int32 /*y*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& flag) const override
        {
            Fail(src, flag);
        }

    private:
        void Fail(Unit* src, int32& flag) const
        {
            if (Player* sd = src->ToPlayer())
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
            flag |= ROWOW_SKILL_NOCONSUME_REQ;
        }
    };

    // SkillTwilightAlchemy1 and 2: 200 White Potions or 200 Condensed White Potions brewed at once, each at the brewing rate
    class ROWOWSkillTwilightAlchemy : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillTwilightAlchemy(uint16 skillID, uint32 nameID) : ROWOWSkillImpl(skillID), NameID(nameID) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd != nullptr && ROWOW->SkillProduceMix(sd, SkillID, NameID, 0, 0, 0, 200, -1) == false)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }

    private:
        uint32 NameID;
    };

    // SkillTwilightAlchemy3: 100 Alcohol, 50 Acid Bottles and 50 Bottle Grenades at once, if all three can be made (and 200 Empty Bottles held)
    class ROWOWSkillTwilightAlchemy3 : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillTwilightAlchemy3() : ROWOWSkillImpl(ROWOW_SKILL_AM_TWILIGHT3) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            auto emptyBottleIter = ROWOW->ItemTemplateIDsByItemID.find(ItemIDEmptyBottle);
            uint32 ebottle = emptyBottleIter != ROWOW->ItemTemplateIDsByItemID.end() ? sd->GetItemCount(emptyBottleIter->second) : 0;
            int32 alcoholIndex = 0, acidIndex = 0, fireIndex = 0;
            // check if you can produce all three, if not, then fail:
            if ((alcoholIndex = ROWOW->SkillCanProduceMix(sd, ItemIDAlcohol, -1, 100)) == 0 // 100 Alcohol
                || (acidIndex = ROWOW->SkillCanProduceMix(sd, ItemIDAcidBottle, -1, 50)) == 0 // 50 Acid Bottle
                || (fireIndex = ROWOW->SkillCanProduceMix(sd, ItemIDFireBottle, -1, 50)) == 0 // 50 Flame Bottle
                || ebottle < 200) // 200 empty bottle are required at total.
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            ROWOW->SkillProduceMix(sd, SkillID, ItemIDAlcohol, 0, 0, 0, 100, alcoholIndex - 1);
            ROWOW->SkillProduceMix(sd, SkillID, ItemIDAcidBottle, 0, 0, 0, 50, acidIndex - 1);
            ROWOW->SkillProduceMix(sd, SkillID, ItemIDFireBottle, 0, 0, 0, 50, fireIndex - 1);
        }

    private:
        static constexpr uint32 ItemIDEmptyBottle = 713;
        static constexpr uint32 ItemIDAlcohol = 970;
        static constexpr uint32 ItemIDFireBottle = 7135;
        static constexpr uint32 ItemIDAcidBottle = 7136;
    };

    // SkillCartTermination: a weapon attack by the cart's weight (its weight / (10 x (16 - level)) percent at max_cart_weight 8000; a monster's
    // as with a full cart), which may stun (5% a level)
    class ROWOWSkillCartTermination : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillCartTermination() : ROWOWWeaponSkillImpl(ROWOW_SKILL_WS_CARTTERMINATION) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* src, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            int32 i = 10 * (16 - skillLevel);
            if (i < 1)
                i = 1;
            // Preserve damage ratio when max cart weight is changed.  (Both in tenths, as max_cart_weight is after battle_adjust_conf)
            ROWOWIdentity const* identity = src->IsPlayer() == true ? src->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
            if (identity != nullptr && identity->CartWeight != 0)
                baseSkillRatio += identity->CartWeight / i * 80000 / ROWOW->BattleConfig.MaxCartWeight - 100;
            else if (src->IsPlayer() == false)
                baseSkillRatio += 80000 / i - 100;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_STUN, 5 * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillUpgradeWeapon: the weapons the Whitesmith can refine listed (clif_item_refine_list); the one picked is refined (skill_weaponrefine)
    class ROWOWSkillUpgradeWeapon : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillUpgradeWeapon() : ROWOWSkillImpl(ROWOW_SKILL_WS_WEAPONREFINE) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                ROWOW->ClifItemRefineList(sd);
        }
    };

    // SkillAcidDemonstration: pre-renewal's is skill_attack of its own type (a misc attack by the target's VIT and the caster's INT, in
    // battle_calc_misc_attack, halved on players), which may break the target's weapon and armor (1% a level)
    class ROWOWSkillAcidDemonstration : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillAcidDemonstration() : ROWOWSkillImpl(ROWOW_SKILL_CR_ACIDDEMONSTRATION) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW->GetSkill(SkillID)->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SkillBreakEquip(src, target, ROWOW_EQUIP_POSITION_HAND_R | ROWOW_EQUIP_POSITION_ARMOR, 100 * skillLevel, ROWOW_BCT_ENEMY);
        }
    };

    // SkillPlantCultivation: on an empty cell, half the time a plant grows for the skill's Duration1 (level 1 a mushroom, level 2 a herb by
    // chance), a monster of its own (AI_NONE)
    class ROWOWSkillPlantCultivation : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillPlantCultivation() : ROWOWSkillImpl(ROWOW_SKILL_CR_CULTIVATION) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            if (ROWOW->MapForEachInArea(src, x, y, x, y, [](Unit*) { return 1; }) > 0) // map_count_oncell
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            if (urand(0, 99) < 50)
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            uint32 mobID = 0;
            if (skillLevel == 1)
                mobID = ROWOW_MOBID_BLACK_MUSHROOM + urand(0, 1);
            else
            {
                uint32 randVal = urand(0, 99);
                if (randVal < 30)
                    mobID = ROWOW_MOBID_GREEN_PLANT;
                else if (randVal < 55)
                    mobID = ROWOW_MOBID_RED_PLANT;
                else if (randVal < 80)
                    mobID = ROWOW_MOBID_YELLOW_PLANT;
                else if (randVal < 90)
                    mobID = ROWOW_MOBID_WHITE_PLANT;
                else if (randVal < 98)
                    mobID = ROWOW_MOBID_BLUE_PLANT;
                else
                    mobID = ROWOW_MOBID_SHINING_PLANT;
            }
            ROWOW->MobOnceSpawnSub(src, x, y, mobID, ROWOW_AI_NONE, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillAidCondensedPotion (Slim Potion Pitcher): the level's condensed potion thrown on the spot, its heal (raised by Slim Potion Pitcher,
    // Aid Potion and Potion Research) on the party around it, each raised by its own VIT/INT and Increase HP/SP Recovery.  rAthena keeps the
    // heal in potion_hp/potion_sp for the area's calls; here the caster keeps it (PotionHP, PotionSP)
    class ROWOWSkillAidCondensedPotion : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillAidCondensedPotion() : ROWOWSkillImpl(ROWOW_SKILL_CR_SLIMPITCHER) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* dstsd = target->ToPlayer();
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            ROWOWStatusChange const* tsc = ROWOW->GetStatusChange(static_cast<Unit const*>(target));
            ROWOWSkillUnitData const* ud = ROWOW->GetSkillUnitData(src);
            if (tstatus == nullptr || (ud->PotionHP == 0 && ud->PotionSP == 0))
                return;
            int32 hp = ud->PotionHP, sp = ud->PotionSP;
            hp = hp * (100 + (tstatus->Stats[ROWOW_STAT_VIT] * 2)) / 100;
            sp = sp * (100 + (tstatus->Stats[ROWOW_STAT_INT] * 2)) / 100;
            if (dstsd != nullptr)
            {
                if (hp != 0)
                    hp = hp * (100 + ROWOW->PCCheckSkill(dstsd, ROWOW_SKILL_SM_RECOVERY) * 10 + ROWOW->PCSkillHeal2Bonus(dstsd, SkillID)) / 100;
                if (sp != 0)
                    sp = sp * (100 + ROWOW->PCCheckSkill(dstsd, ROWOW_SKILL_MG_SRECOVERY) * 10 + ROWOW->PCSkillHeal2Bonus(dstsd, SkillID)) / 100;
            }
            if (tsc != nullptr && tsc->IsEmpty() == false)
            {
                int32 penalty = 0;
                if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_CRITICALWOUND))
                    penalty += sce->Val2;
                if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_DEATHHURT); sce != nullptr && sce->Val3 == 1)
                    penalty += 20;
                if (tsc->HasSCE(ROWOW_SC_NORECOVER_STATE) == true)
                    penalty = 100;
                if (penalty > 0)
                {
                    hp -= hp * penalty / 100;
                    sp -= sp * penalty / 100;
                }
            }
            ROWOW->StatusHeal(target, hp, sp, 0, src, ROWOW->GetSkillSpellID(SkillID, skillLevel));
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            ud->PotionHP = 0;
            ud->PotionSP = 0;
            if (sd != nullptr)
            {
                ROWOWSkillCondition require = ROWOW->SkillGetRequirement(sd, SkillID, skillLevel);
                int32 lv = skillLevel % 11 - 1;
                Item* item = require.ItemIDs[lv] != 0 ? ROWOW->PCSearchInventory(sd, require.ItemIDs[lv]) : nullptr;
                ROWOWItem const* roItem = ROWOW->GetROItem(item);
                if (item == nullptr || roItem == nullptr || int32(item->GetCount()) < require.ItemAmounts[lv])
                {
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                    flag |= ROWOW_SKILL_NOCONSUME_REQ;
                    return;
                }
                ROWOWPotionHeal potion;
                if (roItem->UseScript != nullptr && roItem->UseScript->Error.empty() == true)
                {
                    ROWOWScriptContext context;
                    context.AttachedPlayer = sd;
                    context.Identity = ROWOW->GetIdentity(sd);
                    context.UsedItemTemplateID = item->GetEntry();
                    context.Potion = &potion;
                    ROWOWSCRIPT->Run(*roItem->UseScript, context);
                }
                // Apply skill bonuses
                int32 i = ROWOW->PCCheckSkill(sd, ROWOW_SKILL_CR_SLIMPITCHER) * 10 + ROWOW->PCCheckSkill(sd, ROWOW_SKILL_AM_POTIONPITCHER) * 10 +
                    ROWOW->PCCheckSkill(sd, ROWOW_SKILL_AM_LEARNINGPOTION) * 5 + ROWOW->PCSkillHealBonus(sd, SkillID);
                ud->PotionHP = potion.HP * (100 + i) / 100;
                ud->PotionSP = potion.SP * (100 + i) / 100;
                // (Final heal increased by HPlus: bHPlus is a renewal bonus)
            }
            else
            {
                // A monster's: the level's potion, raised as at Slim Potion Pitcher's highest level
                ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
                uint32 itemID = size_t(skillLevel - 1) < skill->RequiredItems.size() ? skill->RequiredItems[skillLevel - 1].ItemID : 0;
                auto itemTemplateIDIter = ROWOW->ItemTemplateIDsByItemID.find(itemID);
                auto itemIter = itemTemplateIDIter != ROWOW->ItemTemplateIDsByItemID.end() ? ROWOW->ItemsByItemTemplateID.find(itemTemplateIDIter->second) : ROWOW->ItemsByItemTemplateID.end();
                if (itemIter == ROWOW->ItemsByItemTemplateID.end() || itemIter->second.UseScript == nullptr || itemIter->second.UseScript->Error.empty() == false)
                    return;
                int32 id = skill->MaxLevel * 10;
                ROWOWPotionHeal potion;
                ROWOWScriptContext context;
                context.Potion = &potion;
                ROWOWSCRIPT->Run(*itemIter->second.UseScript, context);
                ud->PotionHP = potion.HP * (100 + id) / 100;
                ud->PotionSP = potion.SP * (100 + id) / 100;
            }
            if (ud->PotionHP > 0 || ud->PotionSP > 0)
            {
                int32 i = ROWOW->SkillGetSplash(SkillID, skillLevel);
                int32 areaFlag = flag | ROWOW_BCT_PARTY | ROWOW_BCT_GUILD | 1;
                ROWOW->MapForEachInArea(src, x - i, y - i, x + i, y + i, [this, src, skillLevel, tick, areaFlag](Unit* bl)
                {
                    return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
                });
            }
        }
    };

    // SkillSuicideBombing (NPC_SELFDESTRUCTION): the caster's HP as fire damage on everything around it (a summoned Marine Sphere's: its
    // master's enemies only), then it dies.  Monsters use their skills with R10; until then only a summoned Marine Sphere uses it, when hit
    class ROWOWSkillSuicideBombing : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSuicideBombing() : ROWOWSkillImpl(ROWOW_SKILL_NPC_SELFDESTRUCTION) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWStatusChange const* tsc = ROWOW->GetStatusChange(static_cast<Unit const*>(target));
            if (tsc != nullptr && tsc->HasSCE(ROWOW_SC_HIDING) == true)
                return;
            if (src != target)
                ROWOW->SkillAttack(ROWOW->GetSkill(SkillID)->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Self Destruction hits everyone in range (allies+enemies), except for Summoned Marine spheres on non-versus maps, where it's just
            // enemies and your own slaves.  (No RO map here is a versus map)
            ROWOWMobSummon const* summon = src->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon");
            int32 i = ROWOW_BCT_ALL;
            if (src->IsCreature() == false || (summon != nullptr && summon->AI == ROWOW_AI_SPHERE))
                i = (ROWOW->BattleConfig.AlchemistSummonSetting & 8) != 0 ? (ROWOW_BCT_ENEMY | ROWOW_BCT_SLAVE) : ROWOW_BCT_ENEMY;
            int32 areaFlag = flag | i;
            ROWOW->MapForEachInRange(target, ROWOW->SkillGetSplash(SkillID, skillLevel), [this, src, skillLevel, tick, areaFlag](Unit* bl)
            {
                return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendDamageID);
            });
            // Won't display the damage, but drop items and give exp
            if (src->IsAlive() == true)
                ROWOW->StatusZap(src, src->GetHealth(), 0);
        }
    };
}

void AddROWOWSkillsMerchant()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillMammonite());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_MC_LOUD)); // SkillCrazyUproar: pre-renewal's is the StatusSkillImpl alone
    ROWOW->RegisterSkillImpl(new ROWOWSkillCartRevolution());
    ROWOW->RegisterSkillImpl(new ROWOWSkillChangeCart(ROWOW_SKILL_MC_CHANGECART));
    ROWOW->RegisterSkillImpl(new ROWOWSkillChangeCart(ROWOW_SKILL_MC_CARTDECORATE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillItemAppraisal());
    ROWOW->RegisterSkillImpl(new ROWOWSkillVending());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPartyWeaponStatus(ROWOW_SKILL_BS_ADRENALINE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillPartyWeaponStatus(ROWOW_SKILL_BS_WEAPONPERFECT));
    ROWOW->RegisterSkillImpl(new ROWOWSkillPartyWeaponStatus(ROWOW_SKILL_BS_OVERTHRUST));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_BS_MAXIMIZE, true)); // Ends when cast again
    ROWOW->RegisterSkillImpl(new ROWOWSkillPartyWeaponStatus(ROWOW_SKILL_BS_ADRENALINE2)); // SkillAdvancedAdrenalineRush
    ROWOW->RegisterSkillImpl(new ROWOWSkillHammerFall());
    ROWOW->RegisterSkillImpl(new ROWOWSkillWeaponRepair());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGreed());
    ROWOW->RegisterSkillImpl(new ROWOWSkillAcidTerror());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBomb());
    ROWOW->RegisterSkillImpl(new ROWOWSkillAidPotion(ROWOW_SKILL_AM_POTIONPITCHER));
    ROWOW->RegisterSkillImpl(new ROWOWSkillAidPotion(ROWOW_SKILL_AM_BERSERKPITCHER)); // SkillAidBerserkPotion
    ROWOW->RegisterSkillImpl(new ROWOWSkillSummonFlora(ROWOW_SKILL_AM_CANNIBALIZE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillSummonFlora(ROWOW_SKILL_AM_SPHEREMINE)); // SkillSummonMarineSphere
    ROWOW->RegisterSkillImpl(new ROWOWSkillChemicalProtection(ROWOW_SKILL_AM_CP_WEAPON, ROWOW_EQUIP_POSITION_HAND_R));
    ROWOW->RegisterSkillImpl(new ROWOWSkillChemicalProtection(ROWOW_SKILL_AM_CP_SHIELD, ROWOW_EQUIP_POSITION_HAND_L));
    ROWOW->RegisterSkillImpl(new ROWOWSkillChemicalProtection(ROWOW_SKILL_AM_CP_ARMOR, ROWOW_EQUIP_POSITION_ARMOR));
    ROWOW->RegisterSkillImpl(new ROWOWSkillChemicalProtection(ROWOW_SKILL_AM_CP_HELM, ROWOW_EQUIP_POSITION_HEAD_TOP));
    ROWOW->RegisterSkillImpl(new ROWOWSkillPreparePotion());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHomunculus(ROWOW_SKILL_AM_CALLHOMUN));
    ROWOW->RegisterSkillImpl(new ROWOWSkillHomunculus(ROWOW_SKILL_AM_REST));
    ROWOW->RegisterSkillImpl(new ROWOWSkillHomunculus(ROWOW_SKILL_AM_RESURRECTHOMUN));
    ROWOW->RegisterSkillImpl(new ROWOWSkillTwilightAlchemy(ROWOW_SKILL_AM_TWILIGHT1, 504)); // Prepare 200 White Potions.
    ROWOW->RegisterSkillImpl(new ROWOWSkillTwilightAlchemy(ROWOW_SKILL_AM_TWILIGHT2, 547)); // Prepare 200 Slim White Potions.
    ROWOW->RegisterSkillImpl(new ROWOWSkillTwilightAlchemy3());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_WS_MELTDOWN));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_WS_CARTBOOST));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_WS_OVERTHRUSTMAX));
    ROWOW->RegisterSkillImpl(new ROWOWSkillCartTermination());
    ROWOW->RegisterSkillImpl(new ROWOWSkillUpgradeWeapon());
    ROWOW->RegisterSkillImpl(new ROWOWSkillAcidDemonstration());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPlantCultivation());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFullProtection());
    ROWOW->RegisterSkillImpl(new ROWOWSkillAidCondensedPotion());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSuicideBombing()); // The Marine Sphere's (NPC_SELFDESTRUCTION)
}
