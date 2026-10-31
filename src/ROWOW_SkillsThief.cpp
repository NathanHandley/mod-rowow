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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/thief, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Creature.h"
#include "Player.h"
#include "Unit.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// The Thief line's skills (rAthena's skills/thief, skill_factory_thief), one class per skill.  They run in the caster's map thread

namespace
{
    // SkillHiding: hides, or comes out when hidden.  (RO shows neither with the skill's shout, clif_skill_nodamage at level -1)
    class ROWOWSkillHiding : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHiding() : ROWOWSkillImpl(ROWOW_SKILL_TF_HIDING) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (ROWOW->GetSCE(bl, ROWOW_SC_HIDING) != nullptr)
            {
                ROWOW->StatusChangeEnd(bl, ROWOW_SC_HIDING);
                return;
            }
            ROWOW->SCStart(src, bl, ROWOW_SC_HIDING, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillSteal: an item off a monster's drops (pc_steal_item)
    class ROWOWSkillSteal : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSteal() : ROWOWSkillImpl(ROWOW_SKILL_TF_STEAL) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd != nullptr && ROWOW->PCStealItem(sd, bl, skillLevel) == false)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL);
        }
    };

    // SkillMug: a monster's zeny (Steal Coin): 10 a level + DEX/2 + LUK/2 + 2 x the base level difference, in tenths of a percent, once per
    // monster.  The monster takes it as a hit (and turns on the thief)
    class ROWOWSkillMug : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMug() : ROWOWSkillImpl(ROWOW_SKILL_RG_STEALCOIN) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            Creature* dstmd = target->ToCreature();
            if (sd == nullptr || dstmd == nullptr)
                return;
            ROWOWStatus const* sstatus = ROWOW->GetROStatus(src);
            int32 targetLevel = int32(ROWOW->GetROStatus(target)->Level);
            int32 rate = 10 * ROWOW->PCCheckSkill(sd, SkillID);
            rate += sstatus->Stats[ROWOW_STAT_DEX] / 2;
            rate += sstatus->Stats[ROWOW_STAT_LUK] / 2;
            rate += 2 * (int32(sstatus->Level) - targetLevel);
            if (int32(urand(0, 999)) >= rate)
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            dstmd->CustomData.GetDefault<ROWOWStealState>("ROWOWStealState")->IsStealCoinFlag = true;
            // Zeny Steal Amount
            int32 amount = int32(urand(uint32(8 * targetLevel), uint32(10 * targetLevel)));
            amount += (skillLevel * targetLevel) / 10;
            sd->ModifyMoney(amount);
            // This triggers a 0 damage event and might make the monster switch target to caster (battle_damage of 0: the monster takes it as
            // an attack)
            src->AtTargetAttacked(target, true);
            ChatHandler(sd->GetSession()).PSendSysMessage("You stole {} zeny.", amount); // The client shows the zeny stolen with the skill
        }
    };

    // SkillEnvenom: a poison weapon attack (its 15 a level is battle's) that may poison (10% + 4% a level)
    class ROWOWSkillEnvenom : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillEnvenom() : ROWOWWeaponSkillImpl(ROWOW_SKILL_TF_POISON) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            Player* sd = src->ToPlayer();
            if (ROWOW->SCStart2(src, target, ROWOW_SC_POISON, 4 * skillLevel + 10, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel)) == false &&
                sd != nullptr)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillDetoxify: ends the poisons
    class ROWOWSkillDetoxify : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillDetoxify() : ROWOWSkillImpl(ROWOW_SKILL_TF_DETOXIFY) { }

        void CastendNoDamageID(Unit* /*src*/, Unit* bl, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->StatusChangeEnd(bl, ROWOW_SC_POISON);
            ROWOW->StatusChangeEnd(bl, ROWOW_SC_DPOISON);
        }
    };

    // SkillSandAttack: an earth weapon attack at 130% that may blind (20% from a player)
    class ROWOWSkillSandAttack : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillSandAttack() : ROWOWWeaponSkillImpl(ROWOW_SKILL_TF_SPRINKLESAND) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 30;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_BLIND, src->IsPlayer() == true ? 20 : 15, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillBackSlide: the caster slides back 5 cells (away from where it faces), past any no-knockback bonus.  For 200 ms it can't be stopped
    // (endure_tick, which has no WoW counterpart)
    class ROWOWSkillBackSlide : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBackSlide() : ROWOWSkillImpl(ROWOW_SKILL_TF_BACKSLIDING) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOW->SkillBlown(src, bl, skill->Knockback[ROWOWSkill::GetLevelIndex(skillLevel)], int8(ROWOWMod::UnitGetDir(bl)), ROWOW_BLOWN_IGNORE_NO_KNOCKBACK);
        }
    };

    // SkillFindStone: a Stone picked up from the ground
    class ROWOWSkillFindStone : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFindStone() : ROWOWSkillImpl(ROWOW_SKILL_TF_PICKSTONE) { }

        void CastendNoDamageID(Unit* src, Unit* /*bl*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                ROWOW->GiveROItem(sd, ROWOW_ITEMID_STONE, 1, true);
        }
    };

    // SkillStoneFling: a thrown Stone (misc, 50 from a player, 30 otherwise) that may stun (3%), or blind (3%) when the stun failed; a monster's
    // only stuns (5%)
    class ROWOWSkillStoneFling : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillStoneFling() : ROWOWSkillImpl(ROWOW_SKILL_TF_THROWSTONE) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOW->SkillAttack(skill->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            if (src->IsPlayer() == true)
            {
                // Only blind if used by player and stun failed
                if (ROWOW->SCStart(src, target, ROWOW_SC_STUN, 3, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel)) == false)
                    ROWOW->SCStart(src, target, ROWOW_SC_BLIND, 3, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
            }
            else // 5% stun chance and no blind chance when used by monsters
                ROWOW->SCStart(src, target, ROWOW_SC_STUN, 5, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillDoubleAttack: as a monster's skill, a weapon attack shown as a double hit
    class ROWOWSkillDoubleAttack : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillDoubleAttack() : ROWOWWeaponSkillImpl(ROWOW_SKILL_TF_DOUBLE) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            // For NPC used skill.
            dmg.Type = ROWOW_DMG_MULTI_HIT;
        }
    };

    // SkillCloaking: cloaks, or comes out when cloaked.  Below level 3 it needs a wall next to the caster, which the status's start checks
    // (skill_check_cloaking), failing with the skill's message
    class ROWOWSkillCloaking : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCloaking() : ROWOWSkillImpl(ROWOW_SKILL_AS_CLOAKING) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if (type != ROWOW_SC_NONE && ROWOW->GetSCE(target, type) != nullptr)
            {
                if (ROWOW->StatusChangeEnd(target, type) == false && sd != nullptr)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            if (ROWOW->SCStart(src, target, type, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel)) == false && sd != nullptr)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillSonicBlow: eight katar hits at 300% + 50% a level, a tenth more (and half again its hit rate) with Sonic Acceleration, that may stun
    // (twice as likely with the Assassin's spirit link)
    class ROWOWSkillSonicBlow : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillSonicBlow() : ROWOWWeaponSkillImpl(ROWOW_SKILL_AS_SONICBLOW) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* src, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 200 + 50 * skillLevel;
            if (ROWOW->PCCheckSkill(src, ROWOW_SKILL_AS_SONICACCEL) > 0)
                baseSkillRatio += baseSkillRatio / 10;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            // Link gives double stun chance outside GVG/BG (no RO map here is either)
            ROWOWStatusChangeEntry const* spirit = ROWOW->GetSCE(src, ROWOW_SC_SPIRIT);
            if (spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_ASSASIN)
                ROWOW->SCStart(src, target, ROWOW_SC_STUN, 4 * skillLevel + 20, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
            else
                ROWOW->SCStart(src, target, ROWOW_SC_STUN, 2 * skillLevel + 10, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }

        void ModifyHitRate(int16& hitRate, Unit const* src, Unit const* /*target*/, uint16 /*skillLevel*/) const override
        {
            if (ROWOW->PCCheckSkill(src, ROWOW_SKILL_AS_SONICACCEL) > 0)
                hitRate += hitRate * 50 / 100;
        }
    };

    // SkillGrimtooth: from hiding, a katar hit at 100% + 20% a level on the target and around it, which slows a monster (Quagmire's speed)
    class ROWOWSkillGrimtooth : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillGrimtooth() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_AS_GRIMTOOTH) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 20 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            flag |= ROWOW_SD_PREAMBLE; // a fake packet will be sent for the first target to be hit
            ROWOWRecursiveDamageSplashSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (target->IsCreature() == true && tstatus != nullptr && (tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) == 0)
                ROWOW->SCStart(src, target, ROWOW_SC_QUAGMIRE, 100, 0, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillEnchantPoison: the target's weapon poisons what it hits (SC_ENCPOISON)
    class ROWOWSkillEnchantPoison : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillEnchantPoison() : ROWOWSkillImpl(ROWOW_SKILL_AS_ENCHANTPOISON) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel)) == false && sd != nullptr)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillVenomDust: a cross of five cells of poisonous dust (the unit's onplace timer poisons who stands in it)
    class ROWOWSkillVenomDust : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillVenomDust() : ROWOWSkillImpl(ROWOW_SKILL_AS_VENOMDUST) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            flag |= 1; // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillVenomSplasher: on a monster below 3/4 of its HP (not status immune), a countdown (SC_SPLASHER) after which it blows up: a weapon
    // hit at 500% + 50% a level (+20% a level of Poison React) on it and around it, which poisons.  The blast doesn't use up another gemstone
    class ROWOWSkillVenomSplasher : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillVenomSplasher() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_AS_SPLASHER) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* src, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 400 + 50 * skillLevel;
            baseSkillRatio += 20 * ROWOW->PCCheckSkill(src, ROWOW_SKILL_AS_POISONREACT);
        }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            Player* sd = src->ToPlayer();
            // Renewal dropped the 3/4 hp requirement
            if ((tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0 || target->GetHealth() > target->GetMaxHealth() * 3 / 4)
            {
                if (sd != nullptr)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            ROWOW->SCStart4(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, SkillID, int32(src->GetGUID().GetCounter()),
                int32(ROWOW->SkillGetTime(SkillID, skillLevel)), 1000);
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWRecursiveDamageSplashSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
            if ((flag & 1) == 0)
                flag |= ROWOW_SKILL_NOCONSUME_REQ; // Don't consume a second gemstone.
        }

        int16 GetSearchSize(Unit* /*src*/, uint16 /*skillLevel*/) const override
        {
            return 1; // Venom Splasher uses a different range for searching than for splashing
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart2(src, target, ROWOW_SC_POISON, 100, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillThrowVenomKnife: a thrown Venom Knife (the ammo), a weapon hit that poisons
    class ROWOWSkillThrowVenomKnife : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillThrowVenomKnife() : ROWOWWeaponSkillImpl(ROWOW_SKILL_AS_VENOMKNIFE) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart2(src, target, ROWOW_SC_POISON, 100, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillEnchantDeadlyPoison: its status (more damage, a chance to deadly poison), and 25% of the weapon's damage added as poison element
    // (SC_WATK_ELEMENT)
    class ROWOWSkillEnchantDeadlyPoison : public ROWOWStatusSkillImpl
    {
    public:
        ROWOWSkillEnchantDeadlyPoison() : ROWOWStatusSkillImpl(ROWOW_SKILL_ASC_EDP) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // EDP also give +25% WATK poison pseudo element to user.
            ROWOWStatusSkillImpl::CastendNoDamageID(src, target, skillLevel, tick, flag);
            ROWOW->SCStart4(src, src, ROWOW_SC_WATK_ELEMENT, 100, ROWOW_ELEMENT_POISON, 25, 0, 0, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillSoulDestroyer: a weapon hit at 100% a level, to which battle adds its INT based part (Breaker's misc attack)
    class ROWOWSkillSoulDestroyer : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillSoulDestroyer() : ROWOWWeaponSkillImpl(ROWOW_SKILL_ASC_BREAKER) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& skillRatio, int32 /*mflag*/) const override
        {
            // Pre-Renewal: skill ratio for weapon part of damage [helvetica]
            skillRatio += -100 + 100 * skillLevel;
        }
    };

    // SkillMeteorAssault: everything around the caster at 40% a level, each hit stunning, blinding or making bleed (one of the three, 5% + 5% a
    // level)
    class ROWOWSkillMeteorAssault : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillMeteorAssault() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_ASC_METEORASSAULT) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& skillRatio, int32 /*mflag*/) const override
        {
            skillRatio += -60 + 40 * skillLevel;
        }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillCastendDamageID(src, target, SkillID, skillLevel, tick, flag);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            // Any enemies hit by this skill will receive Stun, Darkness, or external bleeding status ailment with a 5%+5*skill_lv% chance.
            // (Each takes the duration of the skill's level 1, 2 or 3)
            switch (urand(0, 2))
            {
                case 0:
                    ROWOW->SCStart(src, target, ROWOW_SC_BLIND, 5 + skillLevel * 5, skillLevel, ROWOW->SkillGetTime2(SkillID, 1));
                    break;
                case 1:
                    ROWOW->SCStart(src, target, ROWOW_SC_STUN, 5 + skillLevel * 5, skillLevel, ROWOW->SkillGetTime2(SkillID, 2));
                    break;
                default:
                    ROWOW->SCStart2(src, target, ROWOW_SC_BLEEDING, 5 + skillLevel * 5, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, 3));
                    break;
            }
        }
    };

    // SkillCreateDeadlyPoison: a Poison Bottle made (skill_produce_mix, which also takes its 25% of HP on a failure)
    class ROWOWSkillCreateDeadlyPoison : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCreateDeadlyPoison() : ROWOWSkillImpl(ROWOW_SKILL_ASC_CDP) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                if (ROWOW->SkillProduceMix(sd, SkillID, ROWOW_ITEMID_POISON_BOTTLE, 0, 0, 0, 1, -1) == false) // Produce a Poison Bottle.
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_STUFF_INSUFFICIENT);
        }
    };

    // SkillBackStab: from behind the target (not on its cell), a weapon hit at 300% + 40% a level (half with a bow) that brings the Rogue out of
    // hiding and turns the target round to face it
    class ROWOWSkillBackStab : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBackStab() : ROWOWSkillImpl(ROWOW_SKILL_RG_BACKSTAP) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* src, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            ROWOWStatus const* sstatus = ROWOW->GetROStatus(src);
            if (src->IsPlayer() == true && sstatus != nullptr && sstatus->WeaponType == ROWOW_WEAPON_TYPE_BOW && ROWOW->BattleConfig.BackstabBowPenalty != 0)
                baseSkillRatio += (200 + 40 * skillLevel) / 2;
            else
                baseSkillRatio += 200 + 40 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            if (ROWOW->DistanceBL(src, target) == 0)
                return;
            int32 srcX = 0, srcY = 0, targetX = 0, targetY = 0;
            ROWOW->GetUnitCell(src, srcX, srcY);
            ROWOW->GetUnitCell(target, targetX, targetY);
            if (ROWOWMod::MapCheckDir(ROWOWMod::GetRODirection(float(targetX - srcX), float(targetY - srcY)), ROWOWMod::UnitGetDir(target)) == 0)
            {
                ROWOW->StatusChangeEnd(src, ROWOW_SC_HIDING);
                // change direction [Celest]: a monster turns to face its attacker (a player's facing is its client's)
                if (Creature* md = target->ToCreature())
                    md->SetFacingToObject(src);
                ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, flag);
            }
            else if (sd != nullptr)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillSightlessMind (Raid): everything around the Rogue at 100% + 40% a level, each hit stunning and blinding, and the Rogue comes out of
    // hiding
    class ROWOWSkillSightlessMind : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillSightlessMind() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_RG_RAID) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 40 * skillLevel;
        }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // skill_area_temp[1] = 0: every unit hit shows the skill
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
            ud->AreaTemp = ROWOWSkillAreaTemp();
            int32 areaFlag = flag | ROWOW_BCT_ENEMY | 1;
            ROWOW->MapForEachInRange(target, ROWOW->SkillGetSplash(SkillID, skillLevel), [this, src, skillLevel, tick, areaFlag](Unit* bl)
            {
                return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendDamageID);
            });
            ud->AreaTemp = outerAreaTemp;
            ROWOW->StatusChangeEnd(src, ROWOW_SC_HIDING);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_STUN, 10 + 3 * skillLevel, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
            ROWOW->SCStart(src, target, ROWOW_SC_BLIND, 10 + 3 * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillSnatch (Intimidate): a weapon hit at 100% + 30% a level; skill_attack's chance then warps the Rogue and the target together
    class ROWOWSkillSnatch : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillSnatch() : ROWOWWeaponSkillImpl(ROWOW_SKILL_RG_INTIMIDATE) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 30 * skillLevel;
        }
    };

    // SkillScribble (Graffiti): a message on the ground (set with .ro message beforehand, rAthena's skill packet text), one on the map at a time
    class ROWOWSkillScribble : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillScribble() : ROWOWSkillImpl(ROWOW_SKILL_RG_GRAFFITI) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
            flag |= 1;
        }
    };

    // SkillRemover (Cleaner): the Graffiti cells within its splash of the spot are wiped (skill_graffitiremover)
    class ROWOWSkillRemover : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRemover() : ROWOWSkillImpl(ROWOW_SKILL_RG_CLEANER) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 i = ROWOW->SkillGetSplash(SkillID, skillLevel);
            Map* map = src->GetMap();
            ROWOW->MapForEachSkillUnitInArea(map, x - i, y - i, x + i, y + i, [map](ROWOWSkillUnitGroup& group, int32 index)
            {
                if (group.UnitID != ROWOW_UNT_GRAFFITI)
                    return 0;
                ROWOW->SkillDelUnit(map, group, index);
                return 1;
            });
        }
    };

    // SkillDivestWeapon, Shield, Armor and Helm: skill_strip_equip of that place (fails without a message when nothing was stripped)
    class ROWOWSkillDivest : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillDivest(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            bool i = ROWOW->SkillStripEquip(src, target, SkillID, skillLevel);
            // Nothing stripped.
            if (sd != nullptr && i == false)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillDivestAll: the four places at once (at least its own rate, whatever the DEX), but none against Full Chemical Protection
    class ROWOWSkillDivestAll : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillDivestAll() : ROWOWSkillImpl(ROWOW_SKILL_ST_FULLSTRIP) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            // Special message when trying to use strip on FCP [Jobbie] (clif_gospel_info 0x28)
            if (sd != nullptr && ROWOW->GetSCE(target, ROWOW_SC_CP_WEAPON) != nullptr && ROWOW->GetSCE(target, ROWOW_SC_CP_HELM) != nullptr &&
                ROWOW->GetSCE(target, ROWOW_SC_CP_ARMOR) != nullptr && ROWOW->GetSCE(target, ROWOW_SC_CP_SHIELD) != nullptr)
            {
                ChatHandler(sd->GetSession()).SendNotification("Full strip failed because of coating.");
                return;
            }
            bool i = ROWOW->SkillStripEquip(src, target, SkillID, skillLevel);
            // Nothing stripped.
            if (sd != nullptr && i == false)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillCloseConfine: the target held in place by the Rogue (SC_CLOSECONFINE2, which holds the Rogue too, with more FLEE)
    class ROWOWSkillCloseConfine : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCloseConfine() : ROWOWSkillImpl(ROWOW_SKILL_RG_CLOSECONFINE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart4(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, int32(src->GetGUID().GetCounter()), 0, 0,
                ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillStealth (Chase Walk): walks hidden, or comes out of it at once and for free.  (RO shows neither with the skill's shout)
    class ROWOWSkillStealth : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillStealth() : ROWOWSkillImpl(ROWOW_SKILL_ST_CHASEWALK) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if (type != ROWOW_SC_NONE && ROWOW->GetSCE(target, type) != nullptr)
            {
                ROWOW->StatusChangeEnd(target, type);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            ROWOW->SCStart(src, target, type, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };
}

void AddROWOWSkillsThief()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillHiding());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCloaking());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSteal());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMug());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEnvenom());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDetoxify());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSandAttack());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBackSlide());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFindStone());
    ROWOW->RegisterSkillImpl(new ROWOWSkillStoneFling());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDoubleAttack());

    // Assassin and Assassin Cross (Katar Mastery, Righthand/Lefthand Mastery, Sonic Acceleration and Advanced Katar Mastery are battle's)
    ROWOW->RegisterSkillImpl(new ROWOWSkillSonicBlow());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGrimtooth());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEnchantPoison());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_AS_POISONREACT)); // SkillPoisonReact: the status alone (battle counters with it)
    ROWOW->RegisterSkillImpl(new ROWOWSkillVenomDust());
    ROWOW->RegisterSkillImpl(new ROWOWSkillVenomSplasher());
    ROWOW->RegisterSkillImpl(new ROWOWSkillThrowVenomKnife());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEnchantDeadlyPoison());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSoulDestroyer());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMeteorAssault());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCreateDeadlyPoison());

    // Rogue and Stalker (Gank and Gangster's Paradise are skill_additional_effect's and skill_sit's, Plagiarism skill_attack's, Tunnel Drive and
    // Compulsion Discount the status's and the shop's; Flag Graffiti has no class in rAthena either, so it does nothing after its cost)
    ROWOW->RegisterSkillImpl(new ROWOWSkillBackStab());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSightlessMind());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSnatch());
    ROWOW->RegisterSkillImpl(new ROWOWSkillScribble());
    ROWOW->RegisterSkillImpl(new ROWOWSkillRemover());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDivest(ROWOW_SKILL_RG_STRIPWEAPON));
    ROWOW->RegisterSkillImpl(new ROWOWSkillDivest(ROWOW_SKILL_RG_STRIPSHIELD));
    ROWOW->RegisterSkillImpl(new ROWOWSkillDivest(ROWOW_SKILL_RG_STRIPARMOR));
    ROWOW->RegisterSkillImpl(new ROWOWSkillDivest(ROWOW_SKILL_RG_STRIPHELM));
    ROWOW->RegisterSkillImpl(new ROWOWSkillDivestAll());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCloseConfine());
    ROWOW->RegisterSkillImpl(new ROWOWSkillStealth());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_ST_REJECTSWORD)); // SkillCounterInstinct: the status (battle reflects with it)
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_ST_PRESERVE)); // SkillPreserve: the status alone (Plagiarism keeps its copy)
}
