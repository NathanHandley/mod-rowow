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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/gunslinger, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Creature.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// The Gunslinger's skills (rAthena's skills/gunslinger, skill_factory_gunslinger), one class per skill.  A Gunslinger's coins are its Spirit
// Spheres (sd->spiritball, Flip the Coin's).  They run in the caster's map thread

namespace
{
    // SkillGlittering (Flip the Coin): a coin (20% + 10% a level, up to 10, each lasting the skill's time), else one is lost
    class ROWOWSkillGlittering : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGlittering() : ROWOWSkillImpl(ROWOW_SKILL_GS_GLITTERING) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            if (int32(urand(0, 99)) < 20 + 10 * skillLevel)
                ROWOW->PCAddSpiritBall(sd, ROWOW->SkillGetTime(SkillID, skillLevel), 10);
            else if (ROWOW->PCGetSpiritBall(sd) > 0)
                ROWOW->PCDelSpiritBall(sd, 1, 0);
        }
    };

    // SkillFling: the coins thrown (misc, 1 a coin, as many as it had up to the level) lower the target's DEF by 5% a coin (SC_FLING)
    class ROWOWSkillFling : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFling() : ROWOWSkillImpl(ROWOW_SKILL_GS_FLING) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW->GetSkill(SkillID)->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            int32 coins = src->IsPlayer() == true ? ROWOW->GetSkillUnitData(src)->SpiritBallOld : 5;
            ROWOW->SCStart(src, target, ROWOW_SC_FLING, 100, coins, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillTripleAction: three gun hits at 150% (one coin)
    class ROWOWSkillTripleAction : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillTripleAction() : ROWOWWeaponSkillImpl(ROWOW_SKILL_GS_TRIPLEACTION) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50 * skillLevel;
        }
    };

    // SkillBullseye: a revolver or rifle shot at 500% on brutes and demi-humans (100% on the others, and on status immune ones), which may put
    // them in a coma (0.1%)
    class ROWOWSkillBullseye : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillBullseye() : ROWOWWeaponSkillImpl(ROWOW_SKILL_GS_BULLSEYE) { }

        static bool IsBullseyeRace(uint8 race)
        {
            return race == ROWOW_RACE_BRUTE || race == ROWOW_RACE_DEMIHUMAN || race == ROWOW_RACE_PLAYER_HUMAN || race == ROWOW_RACE_PLAYER_DORAM;
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* target, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (IsBullseyeRace(tstatus->Race) == true && (tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) == 0)
                baseSkillRatio += 400;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            if (IsBullseyeRace(ROWOW->GetROStatus(target)->Race) == true)
                ROWOW->StatusChangeStart(src, target, ROWOW_SC_COMA, 10, skillLevel, 0, int32(src->GetGUID().GetCounter()), 0, 0, ROWOW_SCSTART_NONE);
        }
    };

    // SkillCracker: a coin that stuns (65% less 5% a cell away, at least 30%) a player or monster
    class ROWOWSkillCracker : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCracker() : ROWOWSkillImpl(ROWOW_SKILL_GS_CRACKER) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            // per official standards, this skill works on players and mobs.
            if (src->IsPlayer() == false || (target->IsPlayer() == false && target->IsCreature() == false))
                return;
            int32 i = std::max(30, 65 - 5 * ROWOW->DistanceBL(src, target)); // Base rate
            ROWOW->SCStart(src, target, ROWOW_SC_STUN, i, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillTracking: an aimed revolver or rifle shot at 200% + 100% a level
    class ROWOWSkillTracking : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillTracking() : ROWOWWeaponSkillImpl(ROWOW_SKILL_GS_TRACKING) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * (skillLevel + 1);
        }
    };

    // SkillDisarm: a revolver or rifle shot that may strip the target's weapon (skill_strip_equip)
    class ROWOWSkillDisarm : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillDisarm() : ROWOWWeaponSkillImpl(ROWOW_SKILL_GS_DISARM) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SkillStripEquip(src, target, SkillID, skillLevel);
        }
    };

    // SkillPiercingShot: a rifle shot at 100% + 20% a level through DEF that may make the target bleed (3% a level)
    class ROWOWSkillPiercingShot : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillPiercingShot() : ROWOWWeaponSkillImpl(ROWOW_SKILL_GS_PIERCINGSHOT) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 20 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart2(src, target, ROWOW_SC_BLEEDING, skillLevel * 3, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillRapidShower: five revolver hits at 500% + 50% a level
    class ROWOWSkillRapidShower : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillRapidShower() : ROWOWWeaponSkillImpl(ROWOW_SKILL_GS_RAPIDSHOWER) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 400 + 50 * skillLevel;
        }
    };

    // SkillDesperado: shots all around the Gunslinger for a second (a unit group of the cells around it; each cell's chance to hit what's on
    // it falls off away from the middle, the group's onplace timer), at 50% + 50% a level
    class ROWOWSkillDesperado : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillDesperado() : ROWOWSkillImpl(ROWOW_SKILL_GS_DESPERADO) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            // (Fallen Angel's doubling is a third job's)
            baseSkillRatio += 50 * (skillLevel - 1);
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillGatlingfever (Gatling Fever): more ATK and ASPD with a gatling gun, slower and less FLEE; cast again it ends
    class ROWOWSkillGatlingfever : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGatlingfever() : ROWOWSkillImpl(ROWOW_SKILL_GS_GATLINGFEVER) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if (ROWOW->GetSCE(target, type) != nullptr)
            {
                ROWOW->StatusChangeEnd(target, type);
                return;
            }
            ROWOW->SCStart(src, target, type, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillDust: a shotgun blast at 100% + 50% a level that knocks the target back
    class ROWOWSkillDust : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillDust() : ROWOWWeaponSkillImpl(ROWOW_SKILL_GS_DUST) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50 * skillLevel;
        }
    };

    // SkillFullBuster: a shotgun blast at 300% + 100% a level, which may blind the Gunslinger itself (2% a level)
    class ROWOWSkillFullBuster : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillFullBuster() : ROWOWWeaponSkillImpl(ROWOW_SKILL_GS_FULLBUSTER) { }

        void ApplyCounterAdditionalEffects(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*attackType*/) const override
        {
            ROWOW->SCStart(src, src, ROWOW_SC_BLIND, 2 * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * (skillLevel + 2);
        }
    };

    // SkillSpreadAttack: a shotgun blast at 80% + 20% a level on the target and around it
    class ROWOWSkillSpreadAttack : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillSpreadAttack() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_GS_SPREADATTACK) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 20 * (skillLevel - 1);
        }
    };

    // SkillGroundDrift: a grenade laid on the ground (a trap-like unit of the grenade's element), which goes off on what steps on it: a hit at
    // 100% of the element it was laid with, around it, with no knockback
    class ROWOWSkillGroundDrift : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGroundDrift() : ROWOWSkillImpl(ROWOW_SKILL_GS_GROUNDDRIFT) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& src, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            dmg.AMotion = ROWOW->GetROStatus(&src)->AttackMotion;
            dmg.BlewCount = 0;
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }

        void ModifyElement(ROWOWDamage const& dmg, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/, int32& element, int32 /*flag*/) const override
        {
            element = dmg.MiscFlag; // element comes in flag.
        }
    };

    // SkillChainAction: as a monster's skill, a weapon attack shown as a multi hit (a player's Chain Action is battle's)
    class ROWOWSkillChainAction : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillChainAction() : ROWOWWeaponSkillImpl(ROWOW_SKILL_GS_CHAINACTION) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            dmg.Type = ROWOW_DMG_MULTI_HIT;
        }
    };
}

void AddROWOWSkillsGunslinger()
{
    // Single Action and Snake Eye are passives (the status's and the range's); Madness Canceller, Adjustment and Increasing Accuracy are their
    // statuses.  Magical Bullet is a gun shot whose MATK is battle's constant addition: rAthena's skill classes made it a StatusSkillImpl with
    // no status (a cast that does nothing, as an attack skill never reaches castend_nodamage), so it keeps the skill_attack it had before them
    ROWOW->RegisterSkillImpl(new ROWOWSkillGlittering());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFling());
    ROWOW->RegisterSkillImpl(new ROWOWSkillTripleAction());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBullseye());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_GS_MADNESSCANCEL));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_GS_ADJUSTMENT));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_GS_INCREASING));
    ROWOW->RegisterSkillImpl(new ROWOWWeaponSkillImpl(ROWOW_SKILL_GS_MAGICALBULLET));
    ROWOW->RegisterSkillImpl(new ROWOWSkillCracker());
    ROWOW->RegisterSkillImpl(new ROWOWSkillChainAction());
    ROWOW->RegisterSkillImpl(new ROWOWSkillTracking());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDisarm());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPiercingShot());
    ROWOW->RegisterSkillImpl(new ROWOWSkillRapidShower());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDesperado());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGatlingfever());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDust());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFullBuster());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSpreadAttack());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGroundDrift());
}
