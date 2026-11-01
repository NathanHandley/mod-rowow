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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/taekwon, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Creature.h"
#include "Map.h"
#include "Player.h"
#include "Unit.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// The Taekwon line's skills (rAthena's skills/taekwon, skill_factory_taekwon): the Taekwon Kid, the Star Gladiator and the Soul Linker, one
// class per skill.  They run in the caster's map thread

namespace
{
    // dirx and diry: each RO direction's step in cells (0 north, then counter-clockwise)
    int32 const DirX[ROWOW_RO_DIRECTION_COUNT] = { 0, -1, -1, -1, 0, 1, 1, 1 };
    int32 const DirY[ROWOW_RO_DIRECTION_COUNT] = { 1, 1, 0, -1, -1, -1, 0, 1 };

    // The Soul Linker's own buffs (Kaizel, Kaahi, Kaupe, Kaite) go only on a Soul Linker, on itself, its partner or child, or on anyone while
    // it has the Soul Linker spirit; another target stuns the caster for half a second
    bool CheckSoulLinkerBuffTarget(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
    {
        Player* sd = src->ToPlayer();
        if (sd == nullptr)
            return true;
        Player* dstsd = target->ToPlayer();
        ROWOWStatusChangeEntry const* spirit = ROWOW->GetSCE(sd, ROWOW_SC_SPIRIT);
        ROWOWJob const* dstJob = dstsd != nullptr ? ROWOW->GetJob(ROWOW->GetIdentity(dstsd)->JobID) : nullptr;
        // (No marriage or adoption in the RO world: the partner and child are never there)
        if (dstsd == nullptr || ((spirit == nullptr || spirit->Val2 != ROWOW_SKILL_SL_SOULLINKER) && (dstJob == nullptr || dstJob->JobID != ROWOW_JOB_SOUL_LINKER) &&
            dstsd != sd))
        {
            ROWOW->StatusChangeStart(src, src, ROWOW_SC_STUN, 10000, skillLevel, 0, 0, 0, 500, ROWOW_SCSTART_NORATEDEF);
            ROWOW->ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
            return false;
        }
        return true;
    }

    // The Soul Linker's Es spells on a player stun the caster for half a second, unless allow_es_magic_player
    bool CheckEsMagicTarget(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
    {
        Player* sd = src->ToPlayer();
        if (sd != nullptr && ROWOW->BattleConfig.AllowEsMagicPC == 0 && target->IsCreature() == false)
        {
            ROWOW->ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
            ROWOW->StatusChangeStart(src, src, ROWOW_SC_STUN, 10000, skillLevel, 0, 0, 0, 500, ROWOW_SCSTART_NOTICKDEF | ROWOW_SCSTART_NORATEDEF);
            return false;
        }
        return true;
    }

    // SkillRun (Sprint): runs the way the Taekwon faces until something stops it (SC_RUN's start does the running, unit_run), or stops
    class ROWOWSkillRun : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRun() : ROWOWSkillImpl(ROWOW_SKILL_TK_RUN) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if (ROWOW->GetSCE(target, type) != nullptr)
            {
                ROWOW->StatusChangeEnd(target, type);
                return;
            }
            ROWOW->SCStart4(src, target, type, 100, skillLevel, ROWOWMod::UnitGetDir(target), 0, 0, 0);
        }
    };

    // SkillStormKick (Tornado Kick): a weapon hit at 160% + 20% a level on every enemy around the Taekwon (skill_attack_area in its shoot range)
    class ROWOWSkillStormKick : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillStormKick() : ROWOWSkillImpl(ROWOW_SKILL_TK_STORMKICK) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 60 + 20 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // skill_area_temp[1] = 0: no unit is the skill's own target
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
            ud->AreaTemp = ROWOWSkillAreaTemp();
            int32 attackFlag = flag;
            uint16 skillID = SkillID;
            ROWOW->MapForEachInRange(src, ROWOW->SkillGetSplash(SkillID, skillLevel), [src, skillID, skillLevel, tick, attackFlag](Unit* bl)
            {
                if (bl->IsAlive() == false || (bl != src && src->IsWithinLOSInMap(bl) == false))
                    return 0;
                if (ROWOW->BattleCheckTarget(src, bl, ROWOW_BCT_ENEMY) <= 0 || ROWOW->StatusCheckSkillUse(nullptr, bl, skillID, 2) == false)
                    return 0;
                return int32(ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, bl, skillID, skillLevel, tick, attackFlag | ROWOW_SD_ANIMATION));
            });
            ud->AreaTemp = outerAreaTemp;
        }
    };

    // SkillDownKick (Heel Drop): a weapon hit at 160% + 20% a level that stuns
    class ROWOWSkillDownKick : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillDownKick() : ROWOWWeaponSkillImpl(ROWOW_SKILL_TK_DOWNKICK) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 60 + 20 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_STUN, 3333, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillTurnKick (Roundhouse Kick): a weapon hit at 190% + 30% a level on the target (no knockback), and the monsters around it pushed back,
    // stunned and kept from acting (the passive part, as a misc additional effect)
    class ROWOWSkillTurnKick : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillTurnKick() : ROWOWSkillImpl(ROWOW_SKILL_TK_TURNKICK) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            dmg.BlewCount = 0;
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 90 + 30 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 attackType, uint8 /*dmgLv*/) const override
        {
            // Note: attack_type is passed as BF_WEAPON for the actual target, BF_MISC for the splash-affected mobs.  (EF_SPINEDBODY has no look)
            if ((attackType & ROWOW_BF_MISC) != 0)
            {
                ROWOW->SCStart(src, target, ROWOW_SC_STUN, 200, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
                ROWOW->SCStart(src, target, ROWOW_SC_NOACTION, 100, 1, ROWOW->SkillGetTime2(SkillID, skillLevel));
            }
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Active part of the attack.  Note: skill_area_temp[1] is used in CastendNoDamageID to avoid affecting the target.
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
            ud->AreaTemp.TargetGUID = target->GetGUID();
            if (ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, flag) != 0)
            {
                int32 areaFlag = flag | ROWOW_BCT_ENEMY | 1;
                uint16 skillID = SkillID;
                ROWOW->MapForEachInRange(target, ROWOW->SkillGetSplash(SkillID, skillLevel), [src, skillID, skillLevel, tick, areaFlag](Unit* bl)
                {
                    if (bl->IsCreature() == false) // BL_MOB
                        return 0;
                    return ROWOW->SkillAreaSub(bl, src, skillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
                });
            }
            ud->AreaTemp = outerAreaTemp;
        }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& /*flag*/) const override
        {
            // Passive part of the attack. Splash knock-back+stun.
            if (ROWOW->GetSkillUnitData(src)->AreaTemp.TargetGUID != target->GetGUID())
            {
                ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
                ROWOW->SkillBlown(src, target, ROWOWSkill::GetLevelValue(skill->Knockback, skillLevel), -1, ROWOW_BLOWN_NONE);
                ROWOW->SkillAdditionalEffect(src, target, SkillID, skillLevel, ROWOW_BF_MISC, ROWOW_ATK_DEF, tick); // Use Misc rather than weapon to signal passive pushback
            }
        }
    };

    // SkillCounter (Counter Kick): a weapon hit at 190% + 30% a level that never misses
    class ROWOWSkillCounter : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillCounter() : ROWOWWeaponSkillImpl(ROWOW_SKILL_TK_COUNTER) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 90 + 30 * skillLevel;
        }
    };

    // SkillJumpKick (Flying Side Kick): the Taekwon flies at the enemy and kicks it: 30% + 10% a level, or 4% a base level from Tumbling (the
    // combo Dodge opens) or from running (8% with Spurt).  A player hit loses its spirit link and the buffs it brings, unless it's a Soul Linker
    // or keeps them with Preserve
    class ROWOWSkillJumpKick : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillJumpKick() : ROWOWSkillImpl(ROWOW_SKILL_TK_JUMPKICK) { }

        void CalculateSkillRatio(ROWOWDamage const* wd, Unit const* src, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            // Different damage formulas depending on damage trigger
            ROWOWStatusChangeEntry const* combo = ROWOW->GetSCE(src, ROWOW_SC_COMBO);
            int32 level = int32(ROWOW->GetROStatus(src)->Level);
            if (combo != nullptr && combo->Val1 == SkillID)
                baseSkillRatio += -100 + 4 * level; // Tumble formula [4%*baselevel]
            else if (wd->MiscFlag != 0)
            {
                baseSkillRatio += -100 + 4 * level; // Running formula [4%*baselevel]
                if (ROWOW->GetSCE(src, ROWOW_SC_SPURT) != nullptr) // Spurt formula [8%*baselevel]
                    baseSkillRatio *= 2;
            }
            else
                baseSkillRatio += -70 + 10 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* /*src*/, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            // debuff the following statuses
            Player* dstsd = target->ToPlayer();
            if (dstsd == nullptr || ROWOW->GetIdentity(dstsd)->JobID == ROWOW_JOB_SOUL_LINKER || ROWOW->GetSCE(target, ROWOW_SC_PRESERVE) != nullptr)
                return;
            ROWOW->StatusChangeEnd(target, ROWOW_SC_SPIRIT);
            ROWOW->StatusChangeEnd(target, ROWOW_SC_ADRENALINE2);
            ROWOW->StatusChangeEnd(target, ROWOW_SC_KAITE);
            ROWOW->StatusChangeEnd(target, ROWOW_SC_KAAHI);
            ROWOW->StatusChangeEnd(target, ROWOW_SC_ONEHAND);
            ROWOW->StatusChangeEnd(target, ROWOW_SC_ASPDPOTION2);
        }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Check if the target is an enemy; if not, skill should fail so the character doesn't unit_movepos (exploitable)
            Player* sd = src->ToPlayer();
            if (ROWOW->BattleCheckTarget(src, target, ROWOW_BCT_ENEMY) > 0)
            {
                int32 x = 0, y = 0;
                ROWOW->GetUnitCell(target, x, y);
                if (ROWOW->UnitMovePos(src, x, y) == true)
                    ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, flag);
            }
            else if (sd != nullptr)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL);
        }
    };

    // SkillHighJump (Leap): the Taekwon jumps 2 cells a level the way it faces (4/3 a level diagonally) onto a free cell it can reach; not
    // on a noteleport map
    class ROWOWSkillHighJump : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHighJump() : ROWOWSkillImpl(ROWOW_SKILL_TK_HIGHJUMP) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 dir = ROWOWMod::UnitGetDir(src);
            // Fails on noteleport maps, except for GvG and BG maps [Skotlex]
            if (ROWOW->IsUnitWarpForbidden(src) == true)
                return;
            int32 srcX = 0, srcY = 0;
            ROWOW->GetUnitCell(src, srcX, srcY);
            int32 x = 0, y = 0;
            if (dir % 2 != 0)
            {
                // Diagonal
                x = srcX + DirX[dir] * (skillLevel * 4) / 3;
                y = srcY + DirY[dir] * (skillLevel * 4) / 3;
            }
            else
            {
                x = srcX + DirX[dir] * skillLevel * 2;
                y = srcY + DirY[dir] * skillLevel * 2;
            }
            int32 x1 = x + DirX[dir];
            int32 y1 = y + DirY[dir];
            ROWOWMapGrid const* mapGrid = ROWOW->GetMapGrid(src->GetMapId());
            if (mapGrid == nullptr)
                return;
            if (ROWOW->MapCountOnCell(src, x, y) == 0 && ROWOW->IsCellNoPass(src, *mapGrid, x, y) == false &&
                ROWOW->MapCountOnCell(src, x1, y1) == 0 && ROWOW->IsCellNoPass(src, *mapGrid, x1, y1) == false)
                ROWOW->UnitJumpPos(src, x, y);
        }
    };

    // SkillSevenWind (Mild Wind): the weapon takes the level's element (the endowment statuses and Aspersio), with Mild Wind's own status for
    // the element on the status ATK
    class ROWOWSkillSevenWind : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSevenWind() : ROWOWSkillImpl(ROWOW_SKILL_TK_SEVENWIND) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 type = ROWOW_SC_NONE;
            switch (ROWOWSkill::GetLevelValue(ROWOW->GetSkill(SkillID)->Element, skillLevel))
            {
                case ROWOW_ELEMENT_EARTH: type = ROWOW_SC_EARTHWEAPON; break;
                case ROWOW_ELEMENT_WIND: type = ROWOW_SC_WINDWEAPON; break;
                case ROWOW_ELEMENT_WATER: type = ROWOW_SC_WATERWEAPON; break;
                case ROWOW_ELEMENT_FIRE: type = ROWOW_SC_FIREWEAPON; break;
                case ROWOW_ELEMENT_GHOST: type = ROWOW_SC_GHOSTWEAPON; break;
                case ROWOW_ELEMENT_DARK: type = ROWOW_SC_SHADOWWEAPON; break;
                case ROWOW_ELEMENT_HOLY: type = ROWOW_SC_ASPERSIO; break;
            }
            ROWOW->SCStart(src, target, type, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
            ROWOW->SCStart(src, target, ROWOW_SC_SEVENWIND, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillMission (Taekwon Mission): a monster to hunt (MOBG_TAEKWON_MISSION), which can't be changed while it has kills, and only 1 time in
    // 100 without.  The count is mob_dead's
    class ROWOWSkillMission : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMission() : ROWOWSkillImpl(ROWOW_SKILL_TK_MISSION) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            ROWOWIdentity& identity = *ROWOW->GetIdentity(sd);
            uint32 missionMobID = uint32(ROWOW->GetIdentityVariable(identity, ROWOW_TKMISSIONID_VAR));
            int32 missionCount = int32(ROWOW->GetIdentityVariable(identity, ROWOW_TKMISSIONCOUNT_VAR));
            if (missionMobID != 0 && (missionCount != 0 || urand(0, 99) != 0))
            {
                // Cannot change target when already have one
                ROWOW->ClifMissionInfo(sd, missionMobID, missionCount);
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            uint32 id = ROWOW->MobGetRandomID("TAEKWON_MISSION", false);
            if (id == 0)
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            ROWOW->SetIdentityVariable(identity, ROWOW_TKMISSIONID_VAR, id);
            ROWOW->SetIdentityVariable(identity, ROWOW_TKMISSIONCOUNT_VAR, 0);
            ROWOW->ClifMissionInfo(sd, id, 0);
        }
    };

    // SkillFeelingtheSunMoonandStars (Feeling the Sun, Moon and Stars): the map the Star Gladiator stands on becomes its place of the sun (level
    // 1), moon (2) or star (3), after it confirms (clif_feel_req); a place already chosen is only shown.  The same map may be all three
    class ROWOWSkillFeel : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFeel() : ROWOWSkillImpl(ROWOW_SKILL_SG_FEEL) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr || skillLevel < 1 || skillLevel > ROWOW_MAX_PC_FEELHATE)
                return;
            if (ROWOW->PCGetFeelMap(*ROWOW->GetIdentity(sd), skillLevel - 1) == 0)
                ROWOW->ClifFeelReq(sd, skillLevel);
            else
                ROWOW->ClifFeelInfo(sd, skillLevel - 1, 1);
        }
    };

    // SkillHatredoftheSunMoonandStars (Hatred of the Sun, Moon and Stars): the target becomes the hated monster (or job) of the sun (level 1,
    // a small monster), moon (2, a medium one with 6000 HP) or star (3, a large one with 20000 HP); a hatred already chosen is only shown
    class ROWOWSkillHate : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHate() : ROWOWSkillImpl(ROWOW_SKILL_SG_HATE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                if (ROWOW->PCSetHateMob(sd, skillLevel - 1, target) == false)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillWarmthoftheSun, Moon and Stars: an area around the Star Gladiator that follows it (SC_WARM's group), knocking back the monsters in
    // it and draining players' SP, at 2 SP a hit
    class ROWOWSkillWarmth : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillWarmth(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            // A random 0~3 knockback bonus is added to the base knockback
            dmg.BlewCount += int32(urand(0, 3));
        }

        void CastendPos2(Unit* src, int32 /*x*/, int32 /*y*/, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            ROWOW->SkillClearUnitGroup(src);
            int32 srcX = 0, srcY = 0;
            ROWOW->GetUnitCell(src, srcX, srcY);
            ROWOW->GetSkillUnitData(src)->SkillZ = src->GetPositionZ();
            if (std::shared_ptr<ROWOWSkillUnitGroup> group = ROWOW->SkillUnitSetting(src, SkillID, skillLevel, srcX, srcY, 0))
                ROWOW->SCStart4(src, src, type, 100, skillLevel, 0, 0, group->GroupID, ROWOW->SkillGetTime(SkillID, skillLevel));
            flag |= 1;
        }
    };

    // SkillSpiritofthe<Job> (the Soul Linker's spirits): links the target, who must be of the spirit's job (SC_SPIRIT's start checks it),
    // which unlocks the job's spirit skills and effects; the Soul Linker gets Esma's opening (SC_SMA).  The Super Novice's spirit also forgets
    // its deaths 1 time in 100
    class ROWOWSkillSpirit : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillSpirit(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if (ROWOW->SCStart2(src, target, type, 100, skillLevel, SkillID, ROWOW->SkillGetTime(SkillID, skillLevel)) == false)
            {
                if (sd != nullptr)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            if (SkillID == ROWOW_SKILL_SL_SUPERNOVICE)
            {
                // 1% chance to erase death count on successful cast
                Player* dstsd = target->ToPlayer();
                if (dstsd != nullptr && ROWOW->GetIdentityVariable(*ROWOW->GetIdentity(dstsd), ROWOW_PCDIECOUNTER_VAR) != 0 && urand(1, 100) == 1)
                    ROWOW->PCSetDieCounter(dstsd, 0);
            }
            ROWOW->SCStart(src, src, ROWOW_SC_SMA, 100, skillLevel, ROWOW->SkillGetTime(ROWOW_SKILL_SL_SMA, skillLevel));
        }
    };

    // SkillKaizel, SkillKaahi, SkillKaupe, SkillKaite: the Soul Linker's buffs, on the targets CheckSoulLinkerBuffTarget allows
    class ROWOWSkillSoulLinkerBuff : public ROWOWStatusSkillImpl
    {
    public:
        explicit ROWOWSkillSoulLinkerBuff(uint16 skillID) : ROWOWStatusSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            if (CheckSoulLinkerBuffTarget(src, target, SkillID, skillLevel) == true)
                ROWOWStatusSkillImpl::CastendNoDamageID(src, target, skillLevel, tick, flag);
        }
    };

    // SkillEstin: magic at 100% + 10% a level on a small monster (1% on the others), with a knockback
    class ROWOWSkillEstin : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillEstin() : ROWOWSkillImpl(ROWOW_SKILL_SL_STIN) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* target, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            // Target size must be small (0) for full damage
            baseSkillRatio += ROWOW->GetROStatus(target)->Size != ROWOW_SIZE_SMALL ? -99 : 10 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            if (CheckEsMagicTarget(src, target, SkillID, skillLevel) == true)
                ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillEstun: magic at 100% + 5% a level that stuns a medium monster
    class ROWOWSkillEstun : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillEstun() : ROWOWSkillImpl(ROWOW_SKILL_SL_STUN) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 5 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            if (CheckEsMagicTarget(src, target, SkillID, skillLevel) == true)
                ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            if (ROWOW->GetROStatus(target)->Size == ROWOW_SIZE_MEDIUM) // Only stuns mid-sized mobs.
                ROWOW->SCStart(src, target, ROWOW_SC_STUN, 30 + 10 * skillLevel, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillEsma: magic at 40% + 1% a base level a hit (a hit a level), only in the opening another Es spell or a spirit gave (SC_SMA, which it
    // ends)
    class ROWOWSkillEsma : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillEsma() : ROWOWSkillImpl(ROWOW_SKILL_SL_SMA) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* src, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            // Base damage is 40% + lv%
            baseSkillRatio += -60 + int32(ROWOW->GetROStatus(src)->Level);
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->StatusChangeEnd(src, ROWOW_SC_SMA);
            if (CheckEsMagicTarget(src, target, SkillID, skillLevel) == true)
                ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillEswoo, SkillEska, SkillEske: the Soul Linker's curses on a monster (slow, MDEF/DEF down, ATK up and DEF down).  Eswoo and Eska on a
    // target that already has theirs stun the caster for 10 seconds and end its Eswoo; Eske opens Esma
    class ROWOWSkillEsCurse : public ROWOWStatusSkillImpl
    {
    public:
        explicit ROWOWSkillEsCurse(uint16 skillID) : ROWOWStatusSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if (SkillID != ROWOW_SKILL_SL_SKE && ROWOW->GetSCE(target, type) != nullptr)
            {
                if (sd != nullptr)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                ROWOW->StatusChangeStart(src, src, ROWOW_SC_STUN, 10000, skillLevel, 0, 0, 0, 10000, ROWOW_SCSTART_NORATEDEF);
                ROWOW->StatusChangeEnd(target, ROWOW_SC_SWOO);
                return;
            }
            if (CheckEsMagicTarget(src, target, SkillID, skillLevel) == false)
                return;
            ROWOWStatusSkillImpl::CastendNoDamageID(src, target, skillLevel, tick, flag);
            if (SkillID == ROWOW_SKILL_SL_SKE)
                ROWOW->SCStart(src, src, ROWOW_SC_SMA, 100, skillLevel, ROWOW->SkillGetTime(ROWOW_SKILL_SL_SMA, skillLevel));
        }
    };
}

void AddROWOWSkillsTaekwon()
{
    // Taekwon Kid (Peaceful Break / Happy Break are the HP/SP regen passives with a rest, TK_HPTIME/TK_SPTIME, skill_sit's and the regen's;
    // Taekwon Mastery is TK_POWER's battle bonus in a party)
    ROWOW->RegisterSkillImpl(new ROWOWSkillRun());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_TK_READYSTORM, true));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_TK_READYDOWN, true));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_TK_READYTURN, true));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_TK_READYCOUNTER, true));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_TK_DODGE, true));
    ROWOW->RegisterSkillImpl(new ROWOWSkillStormKick());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDownKick());
    ROWOW->RegisterSkillImpl(new ROWOWSkillTurnKick());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCounter());
    ROWOW->RegisterSkillImpl(new ROWOWSkillJumpKick());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHighJump());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSevenWind());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMission());

    // Star Gladiator (the Angers, Blessings, Devil, Friend and Knowledge are passives: battle's, mob_dead's, party_skill_check's and the statuses')
    ROWOW->RegisterSkillImpl(new ROWOWSkillFeel());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHate());
    ROWOW->RegisterSkillImpl(new ROWOWSkillWarmth(ROWOW_SKILL_SG_SUN_WARM));
    ROWOW->RegisterSkillImpl(new ROWOWSkillWarmth(ROWOW_SKILL_SG_MOON_WARM));
    ROWOW->RegisterSkillImpl(new ROWOWSkillWarmth(ROWOW_SKILL_SG_STAR_WARM));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_SG_SUN_COMFORT));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_SG_MOON_COMFORT));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_SG_STAR_COMFORT));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_SG_FUSION, true));

    // Soul Linker (Kaina is a passive: the SP bonus, regen and Es spells' SP)
    for (uint16 spiritSkillID : { ROWOW_SKILL_SL_ALCHEMIST, ROWOW_SKILL_SL_MONK, ROWOW_SKILL_SL_STAR, ROWOW_SKILL_SL_SAGE, ROWOW_SKILL_SL_CRUSADER,
        ROWOW_SKILL_SL_SUPERNOVICE, ROWOW_SKILL_SL_KNIGHT, ROWOW_SKILL_SL_WIZARD, ROWOW_SKILL_SL_PRIEST, ROWOW_SKILL_SL_BARDDANCER, ROWOW_SKILL_SL_ROGUE,
        ROWOW_SKILL_SL_ASSASIN, ROWOW_SKILL_SL_BLACKSMITH, ROWOW_SKILL_SL_HUNTER, ROWOW_SKILL_SL_SOULLINKER, ROWOW_SKILL_SL_HIGH })
        ROWOW->RegisterSkillImpl(new ROWOWSkillSpirit(spiritSkillID));
    ROWOW->RegisterSkillImpl(new ROWOWSkillSoulLinkerBuff(ROWOW_SKILL_SL_KAIZEL));
    ROWOW->RegisterSkillImpl(new ROWOWSkillSoulLinkerBuff(ROWOW_SKILL_SL_KAAHI));
    ROWOW->RegisterSkillImpl(new ROWOWSkillSoulLinkerBuff(ROWOW_SKILL_SL_KAUPE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillSoulLinkerBuff(ROWOW_SKILL_SL_KAITE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillEstin());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEstun());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEsma());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEsCurse(ROWOW_SKILL_SL_SWOO));
    ROWOW->RegisterSkillImpl(new ROWOWSkillEsCurse(ROWOW_SKILL_SL_SKA));
    ROWOW->RegisterSkillImpl(new ROWOWSkillEsCurse(ROWOW_SKILL_SL_SKE));
}
