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

#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "TemporarySummon.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// The Mage line's skills (rAthena's skills/mage, skill_factory_mage), one class per skill: the Mage's, the Wizard's and High Wizard's, the
// Sage's and Professor's, and Abracadabra's special picks.  They run in the caster's map thread.  (The bolts' damage changes from Spell Fist
// and the Sorcerer's elemental options are third-job statuses, left out)

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

    // The Wizard's and High Wizard's skills (rAthena's skills/mage: SkillJupitelThunder ... SkillGanbantein)

    // SkillJupitelThunder: its hits come a moment later (a timer skill), so another spell can be cast before the knockback
    class ROWOWSkillJupitelThunder : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillJupitelThunder() : ROWOWSkillImpl(ROWOW_SKILL_WZ_JUPITEL) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Jupitel Thunder is delayed by 150ms, you can cast another spell before the knockback
            ROWOW->SkillAddTimerSkill(src, tick + ROWOW_TIMERSKILL_INTERVAL, target->GetGUID(), 0, 0, SkillID, skillLevel, 1, flag);
        }
    };

    // A ground skill whose units do its work: the units on the spot (flag 1 keeps the ammo, which the group's end uses up).  Quagmire, Spider
    // Web and Land Protector are this alone
    class ROWOWSkillGroundUnits : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillGroundUnits(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillLordOfVermilion: its units hit everything in the area every interval (as Monsters use the old formula, the pre-renewal one), and may
    // blind (4% a level, at most 40%)
    class ROWOWSkillLordOfVermilion : public ROWOWSkillGroundUnits
    {
    public:
        ROWOWSkillLordOfVermilion() : ROWOWSkillGroundUnits(ROWOW_SKILL_WZ_VERMILION) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 20 * skillLevel - 20;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_BLIND, std::min(4 * skillLevel, 40), skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillWaterBall: the caster's cells of water (or Deluge) around it become water balls, which a timer skill throws at the target one by one
    class ROWOWSkillWaterBall : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillWaterBall() : ROWOWSkillImpl(ROWOW_SKILL_WZ_WATERBALL) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Deploy waterball cells, these are used and turned into waterballs via the timerskill
            int32 x = 0, y = 0;
            ROWOW->GetUnitCell(src, x, y);
            ROWOW->GetSkillUnitData(src)->SkillZ = src->GetPositionZ();
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
            ROWOW->SkillAddTimerSkill(src, tick, target->GetGUID(), x, y, SkillID, skillLevel, 0, flag);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 30 * skillLevel;
        }
    };

    // SkillIceWall: its wall of cells across the line from the caster, which nothing can walk through and which lose HP until they melt
    class ROWOWSkillIceWall : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillIceWall() : ROWOWSkillImpl(ROWOW_SKILL_WZ_ICEWALL) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // (The cast's own look is its spell's at the spot, clif_skill_poseffect's here)
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillFrostNova: a magic attack on everything around the caster in its sight but its own cell (skill_attack_area), which may freeze
    class ROWOWSkillFrostNova : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFrostNova() : ROWOWSkillImpl(ROWOW_SKILL_WZ_FROSTNOVA) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            int32 srcX = 0, srcY = 0;
            ROWOW->GetUnitCell(src, srcX, srcY);
            int32 skillFlag = flag;
            uint16 skillID = SkillID;
            // map_foreachinshootrange(skill_attack_area, ...): skill_area_temp[1] is 0, so no unit is the skill's own target
            ROWOW->MapForEachInRange(src, ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel), [src, srcX, srcY, skillID, skillLevel, tick, skillFlag](Unit* bl)
            {
                if (bl->IsAlive() == false || (bl != src && src->IsWithinLOSInMap(bl) == false))
                    return 0;
                if (ROWOW->BattleCheckTarget(src, bl, ROWOW_BCT_ENEMY) <= 0 || ROWOW->StatusCheckSkillUse(nullptr, bl, skillID, 2) == false)
                    return 0;
                // Skills that don't require the animation to be removed: does not hit the current cell, nor a target on Land Protector
                int32 x = 0, y = 0;
                ROWOW->GetUnitCell(bl, x, y);
                if ((x == srcX && y == srcY) || ROWOW->MapGetCellLandProtector(bl->GetMap(), x, y) == true)
                    return 0;
                return int32(ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, bl, skillID, skillLevel, tick, skillFlag));
            });
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -100 + (100 + skillLevel * 10) * 2 / 3;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_FREEZE, src->IsPlayer() == true ? skillLevel * 5 + 33 : skillLevel * 3 + 35, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillStormGust: its units hit everything in the area every interval; the third hit (of any Storm Gust) freezes at 150%
    class ROWOWSkillStormGust : public ROWOWSkillGroundUnits
    {
    public:
        ROWOWSkillStormGust() : ROWOWSkillGroundUnits(ROWOW_SKILL_WZ_STORMGUST) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 40 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOWStatusChange* tsc = ROWOW->GetStatusChange(target);
            if (tsc == nullptr)
                return;
            // On third hit, there is a 150% to freeze the target
            if (tsc->SGCounter >= 3 && ROWOW->SCStart(src, target, ROWOW_SC_FREEZE, 150, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel)) == true)
                tsc->SGCounter = 0;
            // Being it only resets on success it'd keep stacking and eventually overflowing on mvps, so we reset at a high value
            else if (tsc->SGCounter > 250)
                tsc->SGCounter = 0;
        }
    };

    // SkillMeteorStorm: a meteor a second (its unit interval) for the skill's time, each on a random cell of the area, falling as its unit runs
    // out; a meteor may stun (3% a level)
    class ROWOWSkillMeteorStorm : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMeteorStorm() : ROWOWSkillImpl(ROWOW_SKILL_WZ_METEOR) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            int32 area = ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel);
            int32 interval = std::max(1, skill->UnitInterval);
            for (int32 i = 1; i <= ROWOW->SkillGetTime(SkillID, skillLevel) / interval; i++)
            {
                // Creates a random Cell in the Splash Area
                int32 tmpx = x - area + int32(urand(0, uint32(area * 2)));
                int32 tmpy = y - area + int32(urand(0, uint32(area * 2)));
                ROWOW->SkillUnitSetting(src, SkillID, skillLevel, tmpx, tmpy, flag + i * interval);
            }
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_STUN, 3 * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillEarthSpike: a magic attack of a hit per level (the Wizard's and the Sage's)
    class ROWOWSkillEarthSpike : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillEarthSpike() : ROWOWSkillImpl(ROWOW_SKILL_WZ_EARTHSPIKE) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillHeavensDrive: its unit hits everything in the area once, hidden ones too
    class ROWOWSkillHeavensDrive : public ROWOWSkillGroundUnits
    {
    public:
        ROWOWSkillHeavensDrive() : ROWOWSkillGroundUnits(ROWOW_SKILL_WZ_HEAVENDRIVE) { }

        void ApplyAdditionalEffects(Unit* /*src*/, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->StatusChangeEnd(target, ROWOW_SC_SV_ROOTTWIST);
        }
    };

    // SkillFirePillar: a trap that rises when an enemy comes near and hits everything around it (20% MATK a hit, plus 100 + 50 a level a hit),
    // shown to players as one hit of the total
    class ROWOWSkillFirePillar : public ROWOWSkillGroundUnits
    {
    public:
        ROWOWSkillFirePillar() : ROWOWSkillGroundUnits(ROWOW_SKILL_WZ_FIREPILLAR) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -60 + 20 * skillLevel; // 20% MATK each hit
        }

        // (Its hit also holds the target's walking for the skill's Duration2, unit_set_walkdelay: WoW's movement isn't held)

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& src, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            if (src.IsPlayer() == true && dmg.Div > 0)
                dmg.Div *= -1; // For players, damage is divided by number of hits
        }
    };

    // SkillSightRasher: Sight bursts into a magic attack on everything around the caster in its sight (traps too), knocking it back
    class ROWOWSkillSightRasher : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSightRasher() : ROWOWSkillImpl(ROWOW_SKILL_WZ_SIGHTRASHER) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Passive side of the attack.
            ROWOW->StatusChangeEnd(src, ROWOW_SC_SIGHT);
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            int32 areaFlag = flag | ROWOW_BCT_ENEMY | ROWOW_SD_ANIMATION | 1;
            uint16 skillID = SkillID;
            ROWOW->MapForEachInRange(src, ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel), [src, skillID, skillLevel, tick, areaFlag](Unit* bl)
            {
                if (bl != src && src->IsWithinLOSInMap(bl) == false)
                    return 0; // map_foreachinshootrange
                return ROWOW->SkillAreaSub(bl, src, skillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendDamageID);
            });
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 20 * skillLevel;
        }
    };

    // SkillSense: the monster's window to the caster (not on players)
    class ROWOWSkillSense : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSense() : ROWOWSkillImpl(ROWOW_SKILL_WZ_ESTIMATION) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            if (target->IsPlayer() == true)
            {
                // Fail on Players
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            if (Creature* dstmd = target->ToCreature())
                ROWOW->ClifSkillEstimation(sd, dstmd);
        }
    };

    // SkillSightBlaster: the status (val2 the skill), whose timer bursts it on the first enemy that comes near
    class ROWOWSkillSightBlaster : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSightBlaster() : ROWOWSkillImpl(ROWOW_SKILL_WZ_SIGHTBLASTER) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart2(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, SkillID, ROWOW->SkillGetTime(SkillID, skillLevel));
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillNapalmVulcan: a magic splash (125%) that may curse (5% a level)
    class ROWOWSkillNapalmVulcan : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillNapalmVulcan() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_HW_NAPALMVULCAN) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 25;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_CURSE, 5 * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillGravitationField: the field (a fixed misc hit each second on enemies in it, who are slowed), and the caster's own Gravitation status,
    // which holds it still, keeps its other damage off, and takes the field with it when it ends.  (Its "physical autospell" attack type
    // change is on rAthena's own copy of the type, so it changes nothing)
    class ROWOWSkillGravitationField : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGravitationField() : ROWOWSkillImpl(ROWOW_SKILL_HW_GRAVITATION) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            if (std::shared_ptr<ROWOWSkillUnitGroup> group = ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0))
                ROWOW->SCStart4(src, src, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, 0, ROWOW_BCT_SELF, group->GroupID, ROWOW->SkillGetTime(SkillID, skillLevel));
            flag |= 1;
        }
    };

    // SkillGanbantein: 80% of the time it removes the ground skills' units on the spot (and around it with a splash), else it fails and keeps
    // its gem
    class ROWOWSkillGanbantein : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGanbantein() : ROWOWSkillImpl(ROWOW_SKILL_HW_GANBANTEIN) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            if (urand(0, 99) < 80)
            {
                int32 dummy = 1;
                int32 i = ROWOWSkill::GetLevelValue(ROWOW->GetSkill(SkillID)->SplashArea, skillLevel) != 0 ? 1 : 0; // (a bool in rAthena)
                Map* map = src->GetMap();
                uint16 skillID = SkillID;
                ROWOW->MapForEachSkillUnitInArea(map, x - i, y - i, x + i, y + i, [map, skillID, &dummy](ROWOWSkillUnitGroup& group, int32 index)
                {
                    return ROWOW->SkillCellOverlap(map, group, index, skillID, dummy);
                });
                return;
            }
            if (Player* sd = src->ToPlayer())
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
            flag |= ROWOW_SKILL_NOCONSUME_REQ;
        }
    };

    // The Sage's and Professor's skills (rAthena's skills/mage: SkillCastCancel ... SkillFiberLock)

    // SkillCastCancel: stops the caster's own cast, which still costs part of its SP (90% at level 1, 20% less a level)
    class ROWOWSkillCastCancel : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCastCancel() : ROWOWSkillImpl(ROWOW_SKILL_SA_CASTCANCEL) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->UnitSkillCastCancel(src, 1);
            if (src->IsPlayer() == true)
            {
                ROWOWSkillUnitData const* ud = ROWOW->GetSkillUnitData(src);
                ROWOWSkill const* oldSkill = ROWOW->GetSkill(ud->SkillIDOld);
                int32 sp = oldSkill != nullptr ? oldSkill->SPCost[ROWOWSkill::GetLevelIndex(ud->SkillLevelOld)] : 0;
                sp = std::max(0, sp * (90 - (skillLevel - 1) * 20) / 100);
                ROWOW->StatusZap(src, 0, uint32(sp));
            }
        }
    };

    // SkillMagicRod: the status, which takes in the next spell cast at the caster for SP
    class ROWOWSkillMagicRod : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMagicRod() : ROWOWSkillImpl(ROWOW_SKILL_SA_MAGICROD) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillSpellBreaker: stops the target's cast, which costs it the skill's SP (part back to the caster), and at level 5 2% of its HP (half
    // back); against a player's Magic Rod 20% of the caster's SP goes to that player instead.  Status immune monsters resist 90% of the time
    class ROWOWSkillSpellBreaker : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSpellBreaker() : ROWOWSkillImpl(ROWOW_SKILL_SA_SPELLBREAKER) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            Player* sd = src->ToPlayer();
            if (target->IsPlayer() == true && ROWOW->GetSCE(target, ROWOW_SC_MAGICROD) != nullptr)
            {
                // If target enemy player has Magic Rod, then 20% of your SP is transferred to that player
                int64 sp = ROWOW->StatusPercentChange(target, src, 0, -20, 2);
                ROWOW->StatusHeal(target, 0, sp, 2, target, 0);
                return;
            }
            ROWOWSkillUnitData const* ud = target->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
            if (ud == nullptr || ud->SkillID == 0)
                return; // Nothing to cancel.
            int32 hp = 0;
            if ((tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0)
            {
                // Only 10% success chance against status immune. [Skotlex]
                if (urand(0, 99) < 90)
                {
                    if (sd != nullptr)
                        ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                    return;
                }
            }
            if (skillLevel >= 5 && target->IsPlayer() == false) // HP damage only on pvp-maps when against players (no PvP map here).
                hp = int32(target->GetMaxHealth() / 50); // Siphon 2% HP at level 5
            ROWOWSkill const* castSkill = ROWOW->GetSkill(ud->SkillID);
            int32 sp = castSkill != nullptr ? castSkill->SPCost[ROWOWSkill::GetLevelIndex(ud->SkillLevel)] : 0;
            ROWOW->UnitSkillCastCancel(target, 0);
            ROWOW->StatusZap(target, 0, uint32(sp));
            // Recover some of the SP used
            ROWOW->StatusHeal(src, 0, sp * (25 * (skillLevel - 1)) / 100, 2, src, 0);
            // If damage would be lethal, it does not deal damage
            if (hp != 0 && uint32(hp) < target->GetHealth())
            {
                ROWOW->BattleFixDamage(src, target, hp, SkillID);
                // Recover 50% of damage dealt
                ROWOW->StatusHeal(src, hp / 2, 0, 2, src, ROWOW->GetSkillSpellID(SkillID, skillLevel));
            }
        }
    };

    // SkillHindsight: a player picks the bolt it casts on its attacks from its menu (clif_autospell); a monster gets one by the level
    class ROWOWSkillHindsight : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHindsight() : ROWOWSkillImpl(ROWOW_SKILL_SA_AUTOSPELL) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
            {
                ROWOW->ClifAutospell(sd, skillLevel);
                return;
            }
            int32 maxlv = 1, spellid = 0;
            static uint16 const spellarray[3] = { ROWOW_SKILL_MG_COLDBOLT, ROWOW_SKILL_MG_FIREBOLT, ROWOW_SKILL_MG_LIGHTNINGBOLT };
            if (skillLevel >= 10)
            {
                spellid = ROWOW_SKILL_MG_FROSTDIVER;
                maxlv = skillLevel - 9;
            }
            else if (skillLevel >= 8)
            {
                spellid = ROWOW_SKILL_MG_FIREBALL;
                maxlv = skillLevel - 7;
            }
            else if (skillLevel >= 5)
            {
                spellid = ROWOW_SKILL_MG_SOULSTRIKE;
                maxlv = skillLevel - 4;
            }
            else if (skillLevel >= 2)
            {
                spellid = spellarray[urand(0, 2)];
                maxlv = skillLevel - 1;
            }
            else if (skillLevel > 0)
            {
                spellid = ROWOW_SKILL_MG_NAPALMBEAT;
                maxlv = 3;
            }
            if (spellid > 0)
                ROWOW->SCStart4(src, src, ROWOW_SC_AUTOSPELL, 100, skillLevel, spellid, maxlv, 0, ROWOW->SkillGetTime(ROWOW_SKILL_SA_AUTOSPELL, skillLevel));
        }
    };

    // SkillEndowBlaze, SkillEndowTsunami, SkillEndowTornado, SkillEndowQuake: the element on a party member's weapon, at 60% + 10% a level (100%
    // at 4 and 5, which lasts longer); a failure takes the player's weapon off.  Not on bare hands
    class ROWOWSkillEndow : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillEndow(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            Player* dstsd = target->ToPlayer();
            if (dstsd != nullptr && ROWOW->GetROStatus(dstsd)->WeaponType == ROWOW_WEAPON_TYPE_FIST)
            {
                if (sd != nullptr)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            // 100% success rate at lv4 & 5, but lasts longer at lv5
            if (ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, 60 + skillLevel * 10, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel)) == true)
                return;
            if (dstsd != nullptr && ROWOW->GetIdentityEquippedItem(dstsd, ROWOW_EQUIP_POSITION_HAND_R) != nullptr)
                ROWOW->PCUnequipItem(dstsd, ROWOW_EQUIP_POSITION_HAND_R, 3); // Must unequip the weapon instead of breaking it [Daegaladh]
            if (sd != nullptr)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillVolcano, SkillDeluge, SkillWhirlwind: the elemental field.  A caster's new field takes the place of its old one (and its time left,
    // skill_unitsetting), and doesn't use up the gem while the old one still ran
    class ROWOWSkillElementalField : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillElementalField(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // Does not consumes if the skill is already active. [Skotlex]
            std::shared_ptr<ROWOWSkillUnitGroup> sg2 = ROWOW->SkillLocateElementField(src);
            if (sg2 != nullptr && (sg2->SkillID == ROWOW_SKILL_SA_VOLCANO || sg2->SkillID == ROWOW_SKILL_SA_DELUGE || sg2->SkillID == ROWOW_SKILL_SA_VIOLENTGALE))
            {
                if (sg2->Limit - int64(ROWOWMod::GetTick() - sg2->Tick) > 0)
                {
                    ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
                    flag |= ROWOW_SKILL_NOCONSUME_REQ; // not to consume items
                    return;
                }
                sg2->Limit = 0; // Disable it.
            }
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillDispell: ends the target's statuses (but those that can't be), at 50% + 10% a level; a monster forgets its target.  Outside PvP it
    // only works on party members.  Soul Linkers and Rogue spirits resist it
    class ROWOWSkillDispell : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillDispell() : ROWOWSkillImpl(ROWOW_SKILL_SA_DISPELL) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            Player* dstsd = target->ToPlayer();
            int32 i = ROWOWSkill::GetLevelValue(ROWOW->GetSkill(SkillID)->SplashArea, skillLevel);
            if ((flag & 1) == 0 && i >= 1)
            {
                // Affect all targets on splash area.
                int32 areaFlag = flag | 1;
                uint16 skillID = SkillID;
                ROWOW->MapForEachInRange(target, i, [src, skillID, skillLevel, tick, areaFlag](Unit* bl)
                {
                    return ROWOW->SkillAreaSub(bl, src, skillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendDamageID);
                });
                return;
            }
            // Outside PvP it should only affect party members and no skill fail message (the WoW group stands for the party)
            if (sd != nullptr && dstsd != nullptr && sd != dstsd && sd->IsValidAttackTarget(dstsd) == false && (sd->GetGroup() == nullptr || sd->GetGroup() != dstsd->GetGroup()))
                return;
            ROWOWIdentity* dstIdentity = dstsd != nullptr ? ROWOW->GetIdentity(dstsd) : nullptr;
            ROWOWStatusChangeEntry const* spirit = ROWOW->GetSCE(target, ROWOW_SC_SPIRIT);
            if ((dstIdentity != nullptr && dstIdentity->JobID == JobSoulLinker) || (spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_ROGUE) || // Rogue's spirit defends against dispel.
                int32(urand(0, 99)) >= 50 + 10 * skillLevel)
            {
                if (sd != nullptr)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            if (ROWOW->StatusIsImmune(target) != 0)
                return;
            // Monsters will unlock their target instead.  (A player's bonus_script, cleared here, comes with the item scripts that set one)
            if (Creature* dstmd = target->ToCreature(); dstmd != nullptr && dstmd->IsAIEnabled == true)
                dstmd->AI()->EnterEvadeMode(CreatureAI::EVADE_REASON_OTHER);
            ROWOWStatusChange* tsc = ROWOW->GetStatusChange(target);
            if (tsc == nullptr || tsc->IsEmpty() == true)
                return;
            // Statuses that can't be Dispelled
            std::vector<int32> types;
            for (auto const& entryByType : tsc->EntriesByType)
                types.push_back(entryByType.first);
            for (int32 status : types)
            {
                ROWOWStatusChangeEntry* sce = tsc->GetSCE(status);
                ROWOWStatusChangeDB const* scdb = ROWOW->GetStatusChangeDB(status);
                if (sce == nullptr || scdb == nullptr || scdb->Flag[ROWOW_SCF_NODISPELL] == true)
                    continue;
                switch (status)
                {
                    // bugreport:4888 these songs may only be dispelled if you're not in their song area anymore
                    case ROWOW_SC_WHISTLE:
                    case ROWOW_SC_ASSNCROS:
                    case ROWOW_SC_POEMBRAGI:
                    case ROWOW_SC_APPLEIDUN:
                    case ROWOW_SC_HUMMING:
                    case ROWOW_SC_DONTFORGETME:
                    case ROWOW_SC_FORTUNE:
                    case ROWOW_SC_SERVICE4U:
                        if (ROWOW->BattleConfig.DispelSong == 0 || sce->Val4 == 0)
                            continue; // If in song area don't end it, even if config enabled
                        break;
                    case ROWOW_SC_ASSUMPTIO:
                        if (target->IsCreature() == true)
                            continue;
                        break;
                }
                if (status == ROWOW_SC_BERSERK || status == ROWOW_SC_SATURDAYNIGHTFEVER)
                    sce->Val2 = 0; // Mark a dispelled berserk to avoid setting hp to 100 by setting hp penalty to 0.
                ROWOW->StatusChangeEnd(target, status);
            }
        }

    private:
        static constexpr uint16 JobSoulLinker = 4049; // MAPID_SOUL_LINKER's job (Job_Soul_Linker)
    };

    // SkillHocusPocus (Abracadabra): a random skill of abra_db, tried until one passes its chance at Abracadabra's level (three times the list
    // at most), at Abracadabra's level or the skill's highest.  A player gets it as its skill item, cast once it can be (SkillCastAbraSkill)
    class ROWOWSkillHocusPocus : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHocusPocus() : ROWOWSkillImpl(ROWOW_SKILL_SA_ABRACADABRA) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            std::vector<ROWOWAbraSkill> const& abraSkills = ROWOW->AbraSkills;
            if (abraSkills.empty() == true)
                return;
            uint16 abraSkillID = 0, abraSkillLevel = 0;
            size_t checked = 0, checkedMax = abraSkills.size() * 3;
            do
            {
                ROWOWAbraSkill const& abraSpell = abraSkills[urand(0, uint32(abraSkills.size() - 1))];
                abraSkillID = abraSpell.SkillID;
                abraSkillLevel = std::min<uint16>(skillLevel, ROWOW->GetSkill(abraSkillID)->MaxLevel);
                if (int32(urand(0, 9999)) < abraSpell.Per[ROWOWSkill::GetLevelIndex(skillLevel)])
                    break;
            } while (checked++ < checkedMax);

            if (Player* sd = src->ToPlayer())
            {
                // player-casted: the skill item, and its name and level shown (clif_item_skill)
                ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(sd);
                ud->IsAbraFlag = true;
                ud->SkillItemID = abraSkillID;
                ud->SkillItemLevel = abraSkillLevel;
                ChatHandler(sd->GetSession()).PSendSysMessage("Abracadabra: {} (Lv {})", ROWOW->GetSkill(abraSkillID)->Description, abraSkillLevel);
            }
            // The pick is cast once this skill's cast is over, in the caster's next update (mob-casted ones too)
            src->m_Events.AddEventAtOffset([src, abraSkillID, abraSkillLevel]()
            {
                ROWOW->SkillCastAbraSkill(src, abraSkillID, abraSkillLevel);
            }, Milliseconds(1));
        }
    };

    // SkillCreateElementalConverter: the converters the player can make, to pick from (clif_elementalconverter_list)
    class ROWOWSkillCreateElementalConverter : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCreateElementalConverter() : ROWOWSkillImpl(ROWOW_SKILL_SA_CREATECON) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                ROWOW->ClifElementalConverterList(sd);
        }
    };

    // SkillElementalChangeFire, SkillElementalChangeEarth, SkillElementalChangeWater, SkillElementalChangeWind: the monster's element becomes the
    // skill's (only works on monsters, except status immune monsters)
    class ROWOWSkillElementalChange : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillElementalChange(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (src->IsPlayer() == true && (target->IsCreature() == false || (tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0))
                return;
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOW->SCStart2(src, target, skill->StatusID, 100, skillLevel, ROWOWSkill::GetLevelValue(skill->Element, skillLevel), ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // Abracadabra's special picks (rAthena's SkillClassChange ... SkillMonsterChant), which only it casts

    // SkillClassChange, SkillMonocell: the monster turns into a random Class Change monster, or a Poring; a status immune one can't be
    class ROWOWSkillClassChange : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillClassChange(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Creature* dstmd = target->ToCreature();
            if (dstmd == nullptr)
                return;
            if ((ROWOW->GetROStatus(dstmd)->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0)
            {
                if (Player* sd = src->ToPlayer())
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            uint32 mobID = SkillID == ROWOW_SKILL_SA_MONOCELL ? MobIDPoring : ROWOW->MobGetRandomID("Classchange", true);
            ROWOW->MobClassChange(dstmd, mobID);
            if ((ROWOW->GetROStatus(dstmd)->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0)
            {
                // A status immune monster now: the common statuses and the debuffs it would be immune to end
                static int32 const scs[] = { ROWOW_SC_QUAGMIRE, ROWOW_SC_PROVOKE, ROWOW_SC_ROKISWEIL, ROWOW_SC_GRAVITATION, ROWOW_SC_SUITON, ROWOW_SC_STRIPWEAPON,
                    ROWOW_SC_STRIPSHIELD, ROWOW_SC_STRIPARMOR, ROWOW_SC_STRIPHELM, ROWOW_SC_BLADESTOP };
                for (int32 i = ROWOW_SC_COMMON_MIN; i <= ROWOW_SC_COMMON_MAX; i++)
                    ROWOW->StatusChangeEnd(dstmd, i);
                for (int32 type : scs)
                    ROWOW->StatusChangeEnd(dstmd, type);
            }
        }

    private:
        static constexpr uint32 MobIDPoring = 1002; // MOBID_PORING
    };

    // SkillComa: HP and SP to 1 (the status)
    class ROWOWSkillComa : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillComa() : ROWOWSkillImpl(ROWOW_SKILL_SA_COMA) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillGrimReaper: the target dies (a status immune monster doesn't)
    class ROWOWSkillGrimReaper : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGrimReaper() : ROWOWSkillImpl(ROWOW_SKILL_SA_DEATH) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd != nullptr && target->IsCreature() == true && (ROWOW->GetROStatus(target)->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0)
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            ROWOW->StatusKill(target);
        }
    };

    // SkillGoldDigger: zeny, 100 a level of the target's
    class ROWOWSkillGoldDigger : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGoldDigger() : ROWOWSkillImpl(ROWOW_SKILL_SA_FORTUNE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                sd->ModifyMoney(int32(ROWOW->GetROStatus(target)->Level * 100)); // pc_getzeny: the form's money is its zeny
        }
    };

    // SkillRejuvenation: the target's HP and SP full (not a magic immune one)
    class ROWOWSkillRejuvenation : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRejuvenation() : ROWOWSkillImpl(ROWOW_SKILL_SA_FULLRECOVERY) { }

        void CastendNoDamageID(Unit* /*src*/, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (ROWOW->StatusIsImmune(target) != 0)
                return;
            ROWOW->StatusPercentChange(nullptr, target, 100, 100, 0);
        }
    };

    // SkillGravity, SkillQuestioning: nothing but their look (Questioning's "?" emotion).  It is only triggered randomly by Hocus Pocus
    class ROWOWSkillNoEffect : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillNoEffect(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (SkillID == ROWOW_SKILL_SA_QUESTION)
                src->HandleEmoteCommand(EMOTE_ONESHOT_QUESTION); // clif_emotion ET_QUESTION
        }
    };

    // SkillBeastlyHypnosis: a monster's pet capture (pet_catch_process_start), which comes with the taming pets (R11); until then the pick
    // does nothing
    class ROWOWSkillBeastlyHypnosis : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBeastlyHypnosis() : ROWOWSkillImpl(ROWOW_SKILL_SA_TAMINGMONSTER) { }
    };

    // SkillSuicide: the caster dies
    class ROWOWSkillSuicide : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSuicide() : ROWOWSkillImpl(ROWOW_SKILL_SA_INSTANTDEATH) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->StatusKill(src);
        }
    };

    // SkillLeveling: a tenth of the base exp to the next level
    class ROWOWSkillLeveling : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillLeveling() : ROWOWSkillImpl(ROWOW_SKILL_SA_LEVELUP) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            uint64 nextBaseExp = ROWOW->PCNextBaseExp(*ROWOW->GetIdentity(sd));
            if (nextBaseExp != 0)
                ROWOW->PCGainExp(sd, nextBaseExp * 10 / 100, 0);
        }
    };

    // SkillMonsterChant: a random monster (as Dead Branch's) at the caster's spot
    class ROWOWSkillMonsterChant : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMonsterChant() : ROWOWSkillImpl(ROWOW_SKILL_SA_SUMMONMONSTER) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            // mob_once_spawn "--ja--" (mob_id -1): a random Branch of Dead Tree monster, AI_NONE (a monster of its own, not the caster's)
            if (src->IsPlayer() == false)
                return;
            uint32 entry = ROWOW->GetMobCreatureEntry(ROWOW->MobGetRandomID("Branch_Of_Dead_Tree", true));
            if (entry != 0)
                src->SummonCreature(entry, src->GetPositionX(), src->GetPositionY(), src->GetPositionZ(), src->GetOrientation(), TEMPSUMMON_CORPSE_TIMED_DESPAWN,
                    MonsterCorpseTime);
        }

    private:
        static constexpr uint32 MonsterCorpseTime = 60000; // How long its corpse stays (ms)
    };

    // SkillIndulge: a tenth of the caster's max HP turned into SP (the level's times), on itself
    class ROWOWSkillIndulge : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillIndulge() : ROWOWSkillImpl(ROWOW_SKILL_PF_HPCONVERSION) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 hp = int32(src->GetMaxHealth() / 10);
            int32 sp = hp * skillLevel;
            if (ROWOW->StatusCharge(src, hp, 0) == false)
            {
                if (Player* sd = src->ToPlayer())
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            ROWOW->StatusHeal(target, 0, sp, 2, src, 0);
        }
    };

    // SkillSoulExhale: the caster and target swap SP (a monster gives 3% of the caster's max SP, once)
    class ROWOWSkillSoulExhale : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSoulExhale() : ROWOWSkillImpl(ROWOW_SKILL_PF_SOULCHANGE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Creature* dstmd = target->ToCreature())
            {
                ROWOWSoulChangeState* soulChangeState = dstmd->CustomData.GetDefault<ROWOWSoulChangeState>("ROWOWSoulChangeState");
                if (soulChangeState->IsSoulChangeFlag == true)
                {
                    if (Player* sd = src->ToPlayer())
                        ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                    return;
                }
                soulChangeState->IsSoulChangeFlag = true;
                ROWOW->StatusHeal(src, 0, src->GetMaxPower(POWER_MANA) * 3 / 100, 2, src, 0);
                return;
            }
            uint32 sp1 = src->GetPower(POWER_MANA);
            uint32 sp2 = target->GetPower(POWER_MANA);
            if (ROWOW->GetSCE(target, ROWOW_SC_NORECOVER_STATE) != nullptr)
                sp1 = target->GetPower(POWER_MANA);
            ROWOW->StatusSetSP(src, sp2);
            ROWOW->StatusSetSP(target, sp1);
        }
    };

    // SkillSoulSiphon: the target's SP gone (30% + 10% a level, 70% at 5; else the caster's own), at level 5 with a magic hit of twice it first
    class ROWOWSkillSoulSiphon : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSoulSiphon() : ROWOWSkillImpl(ROWOW_SKILL_PF_SOULBURN) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            Unit* victim = int32(urand(0, 99)) < (skillLevel < 5 ? 30 + skillLevel * 10 : 70) ? target : src;
            if (victim == src)
                ROWOW->ClifSkillNoDamage(src, src, SkillID, skillLevel);
            if (skillLevel == 5)
                ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, victim, SkillID, skillLevel, tick, flag);
            ROWOW->StatusPercentChange(src, victim, 0, 100, 2);
        }
    };

    // SkillMindBreaker: the status (more MATK, less MDEF) at 55% + 5% a level, silently failing on what has it; the target's cast stops, and a
    // monster turns on the caster.  Status immune and undead targets are left alone, and none of the failures use up the SP
    class ROWOWSkillMindBreaker : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMindBreaker() : ROWOWSkillImpl(ROWOW_SKILL_PF_MINDBREAKER) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if ((tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0 || ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == true)
            {
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            if (ROWOW->GetSCE(target, type) != nullptr)
            {
                // HelloKitty2 (?) explained that this silently fails when target is already inflicted. [Skotlex]
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            // Has a 55% + skill_lv*5% success chance.
            if (ROWOW->SCStart(src, target, type, 55 + 5 * skillLevel, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel)) == false)
            {
                if (Player* sd = src->ToPlayer())
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            ROWOW->UnitSkillCastCancel(target, 0);
            if (Creature* dstmd = target->ToCreature(); dstmd != nullptr && dstmd->IsAIEnabled == true && src->IsAlive() == true)
                dstmd->AI()->AttackStart(src); // mob_target
        }
    };

    // SkillBlindingMist (Fog Wall): its cloud, which blinds the enemies in it (not over Deluge, which doubles its time instead)
    class ROWOWSkillBlindingMist : public ROWOWSkillGroundUnits
    {
    public:
        ROWOWSkillBlindingMist() : ROWOWSkillGroundUnits(ROWOW_SKILL_PF_FOGWALL) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            if (src != target && ROWOW->GetSCE(target, ROWOW_SC_DELUGE) == nullptr)
                ROWOW->SCStart(src, target, ROWOW_SC_BLIND, 100, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
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

    // Wizard and High Wizard
    ROWOW->RegisterSkillImpl(new ROWOWSkillJupitelThunder());
    ROWOW->RegisterSkillImpl(new ROWOWSkillLordOfVermilion());
    ROWOW->RegisterSkillImpl(new ROWOWSkillWaterBall());
    ROWOW->RegisterSkillImpl(new ROWOWSkillIceWall());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFrostNova());
    ROWOW->RegisterSkillImpl(new ROWOWSkillStormGust());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMeteorStorm());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEarthSpike());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHeavensDrive());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGroundUnits(ROWOW_SKILL_WZ_QUAGMIRE)); // SkillQuagmire
    ROWOW->RegisterSkillImpl(new ROWOWSkillFirePillar());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSightRasher());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSense());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSightBlaster());
    ROWOW->RegisterSkillImpl(new ROWOWWeaponSkillImpl(ROWOW_SKILL_HW_MAGICCRASHER)); // WeaponSkillImpl (its MATK part is battle's)
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_HW_MAGICPOWER)); // StatusSkillImpl (skill_toggle_magicpower turns it on)
    ROWOW->RegisterSkillImpl(new ROWOWSkillNapalmVulcan());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGravitationField());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGanbantein());

    // Sage and Professor
    ROWOW->RegisterSkillImpl(new ROWOWSkillCastCancel());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMagicRod());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSpellBreaker());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHindsight());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEndow(ROWOW_SKILL_SA_FLAMELAUNCHER)); // SkillEndowBlaze
    ROWOW->RegisterSkillImpl(new ROWOWSkillEndow(ROWOW_SKILL_SA_FROSTWEAPON)); // SkillEndowTsunami
    ROWOW->RegisterSkillImpl(new ROWOWSkillEndow(ROWOW_SKILL_SA_LIGHTNINGLOADER)); // SkillEndowTornado
    ROWOW->RegisterSkillImpl(new ROWOWSkillEndow(ROWOW_SKILL_SA_SEISMICWEAPON)); // SkillEndowQuake
    ROWOW->RegisterSkillImpl(new ROWOWSkillElementalField(ROWOW_SKILL_SA_VOLCANO)); // SkillVolcano
    ROWOW->RegisterSkillImpl(new ROWOWSkillElementalField(ROWOW_SKILL_SA_DELUGE)); // SkillDeluge
    ROWOW->RegisterSkillImpl(new ROWOWSkillElementalField(ROWOW_SKILL_SA_VIOLENTGALE)); // SkillWhirlwind
    ROWOW->RegisterSkillImpl(new ROWOWSkillGroundUnits(ROWOW_SKILL_SA_LANDPROTECTOR)); // SkillMagneticEarth
    ROWOW->RegisterSkillImpl(new ROWOWSkillDispell());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHocusPocus());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCreateElementalConverter());
    ROWOW->RegisterSkillImpl(new ROWOWSkillElementalChange(ROWOW_SKILL_SA_ELEMENTFIRE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillElementalChange(ROWOW_SKILL_SA_ELEMENTGROUND));
    ROWOW->RegisterSkillImpl(new ROWOWSkillElementalChange(ROWOW_SKILL_SA_ELEMENTWATER));
    ROWOW->RegisterSkillImpl(new ROWOWSkillElementalChange(ROWOW_SKILL_SA_ELEMENTWIND));
    ROWOW->RegisterSkillImpl(new ROWOWSkillClassChange(ROWOW_SKILL_SA_CLASSCHANGE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillClassChange(ROWOW_SKILL_SA_MONOCELL));
    ROWOW->RegisterSkillImpl(new ROWOWSkillComa());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGrimReaper());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGoldDigger());
    ROWOW->RegisterSkillImpl(new ROWOWSkillRejuvenation());
    ROWOW->RegisterSkillImpl(new ROWOWSkillNoEffect(ROWOW_SKILL_SA_GRAVITY)); // SkillGravity
    ROWOW->RegisterSkillImpl(new ROWOWSkillNoEffect(ROWOW_SKILL_SA_QUESTION)); // SkillQuestioning
    ROWOW->RegisterSkillImpl(new ROWOWSkillSuicide());
    ROWOW->RegisterSkillImpl(new ROWOWSkillLeveling());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMonsterChant());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_SA_REVERSEORCISH)); // StatusSkillImpl
    ROWOW->RegisterSkillImpl(new ROWOWSkillBeastlyHypnosis());
    ROWOW->RegisterSkillImpl(new ROWOWSkillIndulge());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSoulExhale());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSoulSiphon());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMindBreaker());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_PF_MEMORIZE)); // StatusSkillImpl (Foresight)
    ROWOW->RegisterSkillImpl(new ROWOWSkillBlindingMist());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGroundUnits(ROWOW_SKILL_PF_SPIDERWEB)); // SkillFiberLock
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_PF_DOUBLECASTING)); // StatusSkillImpl
}
