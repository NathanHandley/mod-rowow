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
}
