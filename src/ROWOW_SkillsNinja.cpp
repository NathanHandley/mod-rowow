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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/ninja, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Creature.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// The Ninja's skills (rAthena's skills/ninja, skill_factory_ninja), one class per skill.  The Ninpou need their stones (skill_db's ItemCost),
// the throws their shuriken, kunai or Huuma shuriken.  (The Kagerou/Oboro charms, spiritcharm, are a third job's.)  They run in the
// caster's map thread

namespace
{
    // SkillThrowShuriken and SkillThrowKunai: the ammo thrown (a weapon hit at 100%: the shuriken's ATK and Shuriken Training are battle's; a
    // kunai hits three times)
    class ROWOWSkillThrow : public ROWOWWeaponSkillImpl
    {
    public:
        explicit ROWOWSkillThrow(uint16 skillID) : ROWOWWeaponSkillImpl(skillID) { }
    };

    // SkillThrowHuumaShuriken: the Huuma shuriken thrown at 200% + 150% a level on the target and around it, the damage split among them
    class ROWOWSkillThrowHuumaShuriken : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillThrowHuumaShuriken() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_NJ_HUUMA) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50 + 150 * skillLevel;
        }
    };

    // SkillThrowZeny: zeny thrown (misc: battle's damage, paid on the hit)
    class ROWOWSkillThrowZeny : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillThrowZeny() : ROWOWSkillImpl(ROWOW_SKILL_NJ_ZENYNAGE) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW->GetSkill(SkillID)->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillImprovisedDefense (Flip Tatami): tatami raised around the Ninja (a unit group that knocks back and hits at 100% + 10% a level what's
    // in it), which block ranged attacks for a while (SC_TATAMIGAESHI)
    class ROWOWSkillImprovisedDefense : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillImprovisedDefense() : ROWOWSkillImpl(ROWOW_SKILL_NJ_TATAMIGAESHI) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 10 * skillLevel;
        }

        void CastendPos2(Unit* src, int32 /*x*/, int32 /*y*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 srcX = 0, srcY = 0;
            ROWOW->GetUnitCell(src, srcX, srcY);
            if (ROWOW->SkillUnitSetting(src, SkillID, skillLevel, srcX, srcY, 0) != nullptr)
                ROWOW->SCStart(src, src, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillVanishingSlash (Haze Slasher): a weapon hit at 100% + 10% a level, and the Ninja hides
    class ROWOWSkillVanishingSlash : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillVanishingSlash() : ROWOWWeaponSkillImpl(ROWOW_SKILL_NJ_KASUMIKIRI) { }

        void ApplyAdditionalEffects(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, src, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 10 * skillLevel;
        }
    };

    // SkillShadowLeap (Shadow Leap): the Ninja moves at once onto a cell it can reach, and comes out of hiding
    class ROWOWSkillShadowLeap : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillShadowLeap() : ROWOWSkillImpl(ROWOW_SKILL_NJ_SHADOWJUMP) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWMapGrid const* mapGrid = ROWOW->GetMapGrid(src->GetMapId());
            if (mapGrid != nullptr && ROWOW->IsCellNoPass(src, *mapGrid, x, y) == false) // You don't move on GVG grounds.
                ROWOW->UnitMovePos(src, x, y);
            ROWOW->StatusChangeEnd(src, ROWOW_SC_HIDING);
        }
    };

    // SkillShadowSlash (Shadow Slash): from hiding, the Ninja appears beside the target and hits it at 100% a level, a critical one
    class ROWOWSkillShadowSlash : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillShadowSlash() : ROWOWWeaponSkillImpl(ROWOW_SKILL_NJ_KIRIKAGE) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * (skillLevel - 1);
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // You don't move on GVG grounds.  (map_search_freecell: a random free cell of the 3x3 around the target)
            int32 x = 0, y = 0;
            if (ROWOW->MapSearchFreeCell(target, 1, x, y) == true)
                ROWOW->UnitMovePos(src, x, y);
            ROWOW->StatusChangeEnd(src, ROWOW_SC_HIDING);
            ROWOWWeaponSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
        }
    };

    // SkillMirrorImage (Mirror Image): shadows that take weapon hits for the Ninja (SC_BUNSINJYUTSU), out of Soul (SC_NEN, which it ends); cast
    // again it starts over
    class ROWOWSkillMirrorImage : public ROWOWStatusSkillImpl
    {
    public:
        ROWOWSkillMirrorImage() : ROWOWStatusSkillImpl(ROWOW_SKILL_NJ_BUNSINJYUTSU) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->StatusChangeEnd(target, ROWOW_SC_BUNSINJYUTSU); // on official recasting cancels existing mirror image [helvetica]
            ROWOWStatusSkillImpl::CastendNoDamageID(src, target, skillLevel, tick, flag);
            ROWOW->StatusChangeEnd(target, ROWOW_SC_NEN);
        }
    };

    // SkillCrimsonFirePetal (Fire Petal), SkillSpearOfIce (Ice Spear), SkillWindBlade (Wind Blade): fire, water and wind bolts (magic, 90% a
    // hit for the fire, 100% the others; the hits by level)
    class ROWOWSkillNinjaBolt : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillNinjaBolt(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            if (SkillID == ROWOW_SKILL_NJ_KOUENKA)
                baseSkillRatio -= 10;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillCrimsonFireFormation (Blaze Shield): a ring of fire around a cell (a Fire Wall-like unit at 50% a hit, a few hits a cell); a new one
    // takes the Ninja's old Blaze Shield and Watery Evasion away
    class ROWOWSkillCrimsonFireFormation : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCrimsonFireFormation() : ROWOWSkillImpl(ROWOW_SKILL_NJ_KAENSIN) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio -= 50;
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            flag |= 1; // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillRagingFireDragon (Exploding Dragon), SkillIceMeteor (Snow Flake Draft), SkillLightningStrikeOfDestruction (Lightning Jolt): an area of
    // the element at the target or ground spot (a unit whose cells hit what's on them once): 200% + 150% a level fire, 100% + 50% a level water
    // that may freeze, 160% + 40% a level wind
    class ROWOWSkillNinpouArea : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillNinpouArea(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            switch (SkillID)
            {
                case ROWOW_SKILL_NJ_BAKUENRYU: baseSkillRatio += 50 + 150 * skillLevel; break;
                case ROWOW_SKILL_NJ_HYOUSYOURAKU: baseSkillRatio += 50 * skillLevel; break;
                case ROWOW_SKILL_NJ_RAIGEKISAI: baseSkillRatio += 60 + 40 * skillLevel; break;
            }
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            if (SkillID == ROWOW_SKILL_NJ_HYOUSYOURAKU)
                ROWOW->SCStart(src, target, ROWOW_SC_FREEZE, 10 + 10 * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            // Exploding Dragon is cast on a target: its unit goes on the target's cell
            int32 x = 0, y = 0;
            ROWOW->GetUnitCell(target, x, y);
            ROWOW->GetSkillUnitData(src)->SkillZ = target->GetPositionZ();
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            flag |= 1; // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillHiddenWater (Watery Evasion): a water area (SC_SUITON on the enemies in it: slower, less AGI; the Ninja itself walks freely); a new
    // one takes the Ninja's old one away
    class ROWOWSkillHiddenWater : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHiddenWater() : ROWOWSkillImpl(ROWOW_SKILL_NJ_SUITON) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillKamaitachi (Wind Blade... Kamaitachi): a wind blade at 200% + 100% a level along the line to the target (skill_attack_area on each
    // unit of the path, the target at least)
    class ROWOWSkillKamaitachi : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillKamaitachi() : ROWOWSkillImpl(ROWOW_SKILL_NJ_KAMAITACHI) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
            ud->AreaTemp.TargetGUID = target->GetGUID();
            int32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            ROWOW->GetUnitCell(src, x0, y0);
            ROWOW->GetUnitCell(target, x1, y1);
            int32 levelIndex = ROWOWSkill::GetLevelIndex(skillLevel);
            int32 attackFlag = flag;
            uint16 skillID = SkillID;
            // skill_eightpath_algorithm: map_foreachindir, then the target alone when it hit nothing
            int32 hit = ROWOW->MapForEachInDir(src, x0, y0, x1, y1, ROWOW->SkillGetSplash(SkillID, skillLevel), skill->MaxCount[levelIndex], 0,
                [src, target, skillID, skillLevel, tick, attackFlag](Unit* bl)
            {
                if (bl->IsAlive() == false)
                    return 0;
                if (ROWOW->BattleCheckTarget(src, bl, ROWOW_BCT_ENEMY) <= 0 || ROWOW->StatusCheckSkillUse(nullptr, bl, skillID, 2) == false)
                    return 0;
                return int32(ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, bl, skillID, skillLevel, tick, attackFlag | (bl == target ? 0 : ROWOW_SD_ANIMATION)));
            });
            if (hit == 0)
                ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
            ud->AreaTemp = outerAreaTemp;
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillFinalStrike: a hit of the Ninja's HP (battle's skill base damage), after which it's left with 1 HP, out of Soul and hiding, and
    // moves 2 cells past the target
    class ROWOWSkillFinalStrike : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillFinalStrike() : ROWOWWeaponSkillImpl(ROWOW_SKILL_NJ_ISSEN) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            int32 srcX = 0, srcY = 0, targetX = 0, targetY = 0;
            ROWOW->GetUnitCell(src, srcX, srcY);
            ROWOW->GetUnitCell(target, targetX, targetY);
            int32 dir = ROWOWMod::GetRODirection(float(targetX - srcX), float(targetY - srcY)); // map_calc_dir
            int32 i = 2; // Move 2 cells (From target)
            int32 x = 0, y = 0;
            if (dir > 0 && dir < 4)
                x = -i;
            else if (dir > 4)
                x = i;
            if (dir > 2 && dir < 6)
                y = -i;
            else if (dir == 7 || dir < 2)
                y = i;
            ROWOWWeaponSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
            ROWOW->StatusSetHP(src, 1);
            ROWOW->StatusChangeEnd(src, ROWOW_SC_NEN);
            ROWOW->StatusChangeEnd(src, ROWOW_SC_HIDING);
            ROWOW->UnitMovePos(src, targetX + x, targetY + y);
        }
    };
}

void AddROWOWSkillsNinja()
{
    // Shuriken Training (Throwing Mastery) and Ninja Mastery (Ninpou) are passives: battle's and the regen's.  Cicada Skin Shed (SC_UTSUSEMI)
    // and Soul (SC_NEN) are their statuses
    ROWOW->RegisterSkillImpl(new ROWOWSkillThrow(ROWOW_SKILL_NJ_SYURIKEN));
    ROWOW->RegisterSkillImpl(new ROWOWSkillThrow(ROWOW_SKILL_NJ_KUNAI));
    ROWOW->RegisterSkillImpl(new ROWOWSkillThrowHuumaShuriken());
    ROWOW->RegisterSkillImpl(new ROWOWSkillThrowZeny());
    ROWOW->RegisterSkillImpl(new ROWOWSkillImprovisedDefense());
    ROWOW->RegisterSkillImpl(new ROWOWSkillVanishingSlash());
    ROWOW->RegisterSkillImpl(new ROWOWSkillShadowLeap());
    ROWOW->RegisterSkillImpl(new ROWOWSkillShadowSlash());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_NJ_UTSUSEMI));
    ROWOW->RegisterSkillImpl(new ROWOWSkillMirrorImage());
    ROWOW->RegisterSkillImpl(new ROWOWSkillNinjaBolt(ROWOW_SKILL_NJ_KOUENKA));
    ROWOW->RegisterSkillImpl(new ROWOWSkillNinjaBolt(ROWOW_SKILL_NJ_HYOUSENSOU));
    ROWOW->RegisterSkillImpl(new ROWOWSkillNinjaBolt(ROWOW_SKILL_NJ_HUUJIN));
    ROWOW->RegisterSkillImpl(new ROWOWSkillCrimsonFireFormation());
    ROWOW->RegisterSkillImpl(new ROWOWSkillNinpouArea(ROWOW_SKILL_NJ_BAKUENRYU));
    ROWOW->RegisterSkillImpl(new ROWOWSkillNinpouArea(ROWOW_SKILL_NJ_HYOUSYOURAKU));
    ROWOW->RegisterSkillImpl(new ROWOWSkillNinpouArea(ROWOW_SKILL_NJ_RAIGEKISAI));
    ROWOW->RegisterSkillImpl(new ROWOWSkillHiddenWater());
    ROWOW->RegisterSkillImpl(new ROWOWSkillKamaitachi());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_NJ_NEN));
    ROWOW->RegisterSkillImpl(new ROWOWSkillFinalStrike());
}
