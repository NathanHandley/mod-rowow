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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/mage, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "ObjectAccessor.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// The Mage line's skills (rAthena's skills/mage, skill_factory_mage), one class per skill.  They run in the caster's map thread.  (The
// bolts' damage changes from Spell Fist and the Sorcerer's elemental options are third-job statuses, left out)

namespace
{
    // SkillFireBolt, SkillColdBolt, SkillLightningBolt: a magic attack of a hit per level (the skill's HitCount)
    class ROWOWSkillBolt : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillBolt(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillNapalmBeat
    class ROWOWSkillNapalmBeat : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillNapalmBeat() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_MG_NAPALMBEAT) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -30 + 10 * skillLevel;
        }
    };

    // SkillSoulStrike
    class ROWOWSkillSoulStrike : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSoulStrike() : ROWOWSkillImpl(ROWOW_SKILL_MG_SOULSTRIKE) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* target, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == true)
                baseSkillRatio += 5 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillFireBall
    class ROWOWSkillFireBall : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillFireBall() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_MG_FIREBALL) { }

        void CalculateSkillRatio(ROWOWDamage const* wd, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -30 + 10 * skillLevel;
            if (wd->MiscFlag == 2) // Enemies at the edge of the area will take 75% of the damage
                baseSkillRatio = baseSkillRatio * 3 / 4;
        }

        int64 SplashDamage(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32 flag) const override
        {
            // For players, the distance between original target and splash target determines the damage
            if (src->IsPlayer() == true)
            {
                ROWOWSkillUnitData const* ud = ROWOW->GetSkillUnitData(src);
                if (Unit* originalTarget = ObjectAccessor::GetUnit(*src, ud->AreaTemp.TargetGUID))
                    flag |= ROWOW->DistanceBL(originalTarget, target);
            }
            // Call default implementation
            return ROWOWRecursiveDamageSplashSkillImpl::SplashDamage(src, target, skillLevel, tick, flag);
        }
    };

    // SkillFrostDiver: a magic attack that may freeze (35% + 3% a level, at most 60% + the level, resisted as usual)
    class ROWOWSkillFrostDiver : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFrostDiver() : ROWOWSkillImpl(ROWOW_SKILL_MG_FROSTDIVER) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 10 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            Player* sd = src->ToPlayer();
            if (ROWOW->SCStart(src, target, ROWOW_SC_FREEZE, std::min(skillLevel * 3 + 35, skillLevel + 60), skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel)) == false &&
                sd != nullptr)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillStoneCurse: the stone curse (20% + 4% a level) on a monster that isn't status immune.  Levels 6-10 don't use up their Red Gemstone
    // when it fails
    class ROWOWSkillStoneCurse : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillStoneCurse() : ROWOWSkillImpl(ROWOW_SKILL_MG_STONECURSE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if ((tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0)
            {
                if (sd != nullptr)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            if (ROWOW->StatusIsImmune(target) != 0)
                return;
            // Except for players, the skill animation shows even if the status change doesn't start; players get a skill has failed message
            // instead
            if (ROWOW->SCStart2(src, target, type, skillLevel * 4 + 20, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel),
                int32(ROWOW->SkillGetTime(SkillID, skillLevel))) == true || sd == nullptr)
                return;
            ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
            // Level 6-10 doesn't consume a red gem if it fails [celest]
            if (skillLevel > 5)
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
        }
    };

    // SkillSafetyWall: its unit on the spot (a Land Protector there takes the cast but not the gems)
    class ROWOWSkillSafetyWall : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSafetyWall() : ROWOWSkillImpl(ROWOW_SKILL_MG_SAFETYWALL) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            int32 dummy = 1;
            uint16 skillID = SkillID;
            if (ROWOW->MapForEachSkillUnitInArea(src->GetMap(), x, y, x, y, [src, skillID, &dummy](ROWOWSkillUnitGroup& group, int32 index)
            {
                return ROWOW->SkillCellOverlap(src->GetMap(), group, index, skillID, dummy);
            }) != 0)
            {
                ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
                // Don't consume gems if cast on Land Protector
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillFireWall: its wall of units across the line from the caster, which hit (and knock back) what walks into them
    class ROWOWSkillFireWall : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFireWall() : ROWOWSkillImpl(ROWOW_SKILL_MG_FIREWALL) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio -= 50;
        }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& /*src*/, Unit const& target, uint16 /*skillLevel*/) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(&target);
            if (tstatus->DefElement == ROWOW_ELEMENT_FIRE || ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == true)
                dmg.BlewCount = 0; // No knockback
        }
    };

    // SkillThunderStorm: its unit (a Dummyskill) hits everything in its range once as it lands
    class ROWOWSkillThunderStorm : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillThunderStorm() : ROWOWSkillImpl(ROWOW_SKILL_MG_THUNDERSTORM) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            // in Renewal Thunder Storm boost is 100% (in pre-re, 80%)
            baseSkillRatio -= 20;
        }
    };

    // SkillSight: the status (val2 the skill), whose timer reveals the hidden around
    class ROWOWSkillSight : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSight() : ROWOWSkillImpl(ROWOW_SKILL_MG_SIGHT) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOW->SCStart2(src, target, skill->StatusID, 100, skillLevel, SkillID, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };
}

void AddROWOWSkillsMage()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillBolt(ROWOW_SKILL_MG_FIREBOLT));
    ROWOW->RegisterSkillImpl(new ROWOWSkillBolt(ROWOW_SKILL_MG_COLDBOLT));
    ROWOW->RegisterSkillImpl(new ROWOWSkillBolt(ROWOW_SKILL_MG_LIGHTNINGBOLT));
    ROWOW->RegisterSkillImpl(new ROWOWSkillNapalmBeat());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSoulStrike());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFireBall());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSight());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_MG_ENERGYCOAT)); // SkillEnergyCoat: the StatusSkillImpl alone
    ROWOW->RegisterSkillImpl(new ROWOWSkillFrostDiver());
    ROWOW->RegisterSkillImpl(new ROWOWSkillStoneCurse());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSafetyWall());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFireWall());
    ROWOW->RegisterSkillImpl(new ROWOWSkillThunderStorm());
}
