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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/swordman, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Creature.h"
#include "Map.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// The Swordman line's skills (rAthena's skills/swordman, skill_factory_swordman), one class per skill: the Swordman's, the Knight's and Lord
// Knight's, the Crusader's and Paladin's.  They run in the caster's map thread

namespace
{
    // dirx and diry: each RO direction's step in cells (0 north, then counter-clockwise)
    int32 const DirX[ROWOW_RO_DIRECTION_COUNT] = { 0, -1, -1, -1, 0, 1, 1, 1 };
    int32 const DirY[ROWOW_RO_DIRECTION_COUNT] = { 1, 1, 0, -1, -1, -1, 0, 1 };

    // (class_ & MAPID_SECONDMASK) == MAPID_CRUSADER: the Crusader and Paladin, on a Peco or not, whom Devotion and Providence don't take
    bool IsCrusaderClass(Unit const* unit)
    {
        Player const* player = unit->ToPlayer();
        ROWOWIdentity const* identity = player != nullptr ? player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
        if (identity == nullptr)
            return false;
        switch (identity->JobID)
        {
            case 14: case 21: case 4015: case 4022:
                return true;
            default:
                return false;
        }
    }

    // SkillBash
    class ROWOWSkillBash : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillBash() : ROWOWWeaponSkillImpl(ROWOW_SKILL_SM_BASH) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            // Base 100% + 30% per level
            baseSkillRatio += 30 * skillLevel;
        }

        void ModifyHitRate(int16& hitRate, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel) const override
        {
            // It is proven that bonus is applied on final hitrate, not hit.
            // +5% hit per level
            hitRate += int16(hitRate * 5 * skillLevel / 100);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            // Fatal Blow: levels 6-10 can stun.  BaseChance gets multiplied with BaseLevel/50.0; 500/50 simplifies to 10 [Playtester]
            Player* sd = src->ToPlayer();
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            if (sd == nullptr || skill == nullptr || skillLevel <= 5 || ROWOW->PCCheckSkill(sd, ROWOW_SKILL_SM_FATALBLOW) <= 0)
                return;
            ROWOWStatus const* sstatus = ROWOW->GetROStatus(src);
            int32 stunChance = (skillLevel - 5) * int32(sstatus->Level) * 10;
            ROWOW->StatusChangeStart(src, target, ROWOW_SC_STUN, stunChance, skillLevel, 0, 0, 0, ROWOWSkill::GetLevelValue(skill->Duration2, skillLevel), 0);
        }
    };

    // SkillMagnumBreak
    class ROWOWSkillMagnumBreak : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMagnumBreak() : ROWOWSkillImpl(ROWOW_SKILL_SM_MAGNUM) { }

        void CalculateSkillRatio(ROWOWDamage const* wd, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            if (wd->MiscFlag == 1)
                baseSkillRatio += 20 * skillLevel; // Inner 3x3 circle takes 100%+20%*level damage [Playtester]
            else
                baseSkillRatio += 10 * skillLevel; // Outer 5x5 circle takes 100%+10%*level damage [Playtester]
        }

        void ModifyHitRate(int16& hitRate, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel) const override
        {
            hitRate += int16(hitRate * 10 * skillLevel / 100);
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // For players, damage depends on distance, so add it to flag if it is > 1.  Cannot hit hidden targets
            if ((flag & 1) == 0)
                return;
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOW->SkillAttack(skill->AttackType, src, src, target, SkillID, skillLevel, tick,
                flag | ROWOW_SD_ANIMATION | (src->IsPlayer() == true ? ROWOW->DistanceBL(src, target) : 0));
        }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Passive Magnum, should had been casted on yourself: every enemy within its splash (map_foreachinshootrange).  Then 20% of your
            // damage becomes fire element (SC_WATK_ELEMENT)
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
            ud->AreaTemp.TargetGUID.Clear();
            int32 areaFlag = flag | ROWOW_BCT_ENEMY | 1;
            ROWOW->MapForEachInRange(src, ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel), [this, src, skillLevel, tick, areaFlag](Unit* bl)
            {
                return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendDamageID);
            });
            ud->AreaTemp = outerAreaTemp;
            ROWOW->SCStart4(src, src, ROWOW_SC_WATK_ELEMENT, 100, ROWOW_ELEMENT_FIRE, 20, 0, 0, ROWOWSkill::GetLevelValue(skill->Duration2, skillLevel));
        }
    };

    // SkillAutoBerserk: switches its status on and off (Provoke at a quarter of the HP or less, which the status's timer does)
    class ROWOWSkillAutoBerserk : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillAutoBerserk() : ROWOWSkillImpl(ROWOW_SKILL_SM_AUTOBERSERK) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if (ROWOW->GetSCE(bl, type) != nullptr)
                ROWOW->StatusChangeEnd(bl, type);
            else
                ROWOW->SCStart(src, bl, type, 100, skillLevel, 60000);
        }
    };

    // SkillProvoke: fails on status immune monsters and the undead; otherwise its chance is 70% + 3% a level + the caster's base level -
    // the target's.  A provoked target's cast stops, and a provoked monster turns on the caster (mob_target, with provoke_flag keeping it
    // there: WoW's fixated target)
    class ROWOWSkillProvoke : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillProvoke() : ROWOWSkillImpl(ROWOW_SKILL_SM_PROVOKE) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOWStatus const* sstatus = ROWOW->GetROStatus(src);
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(bl);
            if ((tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0 || ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == true)
                return;
            // Official chance is 70% + 3%*skill_lv + srcBaseLevel% - tarBaseLevel%
            int32 rate = 70 + 3 * skillLevel + int32(sstatus->Level) - int32(tstatus->Level);
            if (ROWOW->SCStart(src, bl, skill->StatusID, rate, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel)) == false)
            {
                if (Player* sd = src->ToPlayer())
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            ROWOW->UnitSkillCastCancel(bl, 2);
            if (Creature* dstmd = bl->ToCreature())
            {
                src->AtTargetAttacked(dstmd, true);
                ROWOW->MobSetProvoked(dstmd, src);
            }
        }
    };

    // SkillPierce: one hit per size step of the target (small 1, medium 2, large 3).  (Charging Pierce's double is a fourth job's)
    class ROWOWSkillPierce : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillPierce() : ROWOWWeaponSkillImpl(ROWOW_SKILL_KN_PIERCE) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& /*src*/, Unit const& target, uint16 /*skillLevel*/) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(&target);
            dmg.Div = int16(dmg.Div > 0 ? tstatus->Size + 1 : -(tstatus->Size + 1));
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 10 * skillLevel;
        }

        void ModifyHitRate(int16& hitRate, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel) const override
        {
            hitRate += int16(hitRate * 5 * skillLevel / 100);
        }
    };

    // SkillSpearStab: the target and every enemy on the cells between it and the caster (four cells from the target on), each knocked back
    class ROWOWSkillSpearStab : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSpearStab() : ROWOWSkillImpl(ROWOW_SKILL_KN_SPEARSTAB) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            dmg.BlewCount = 0; // The knockback is the skill's own, after each hit
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            if ((flag & 1) != 0)
            {
                if (target->GetGUID() == ud->AreaTemp.TargetGUID)
                    return;
                if (ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, ROWOW_SD_ANIMATION) != 0)
                    ROWOW->SkillBlown(src, target, ud->AreaTemp.Values[2], -1, ROWOW_BLOWN_NONE);
                return;
            }
            int32 x = 0, y = 0, srcX = 0, srcY = 0;
            ROWOW->GetUnitCell(target, x, y);
            ROWOW->GetUnitCell(src, srcX, srcY);
            uint8 dir = ROWOWMod::GetRODirection(float(srcX - x), float(srcY - y)); // map_calc_dir(target, src->x, src->y)
            ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
            ud->AreaTemp.TargetGUID = target->GetGUID();
            ud->AreaTemp.Values[2] = ROWOWSkill::GetLevelValue(ROWOW->GetSkill(SkillID)->Knockback, skillLevel);
            // all the enemies between the caster and the target are hit, as well as the target
            if (ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, 0) != 0)
                ROWOW->SkillBlown(src, target, ud->AreaTemp.Values[2], -1, ROWOW_BLOWN_NONE);
            int32 areaFlag = flag | ROWOW_BCT_ENEMY | 1;
            for (int32 i = 0; i < 4; i++)
            {
                ROWOW->MapForEachInCellArea(src->GetMap(), src->GetPhaseMask(), x, y, x, y, [this, src, skillLevel, tick, areaFlag](Unit* bl)
                {
                    return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendDamageID);
                });
                x += DirX[dir];
                y += DirY[dir];
            }
            ud->AreaTemp = outerAreaTemp;
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 20 * skillLevel;
        }
    };

    // SkillBrandishSpear: on a Peco, the enemies on a path five cells long and five wide toward the target, hit harder the nearer they stand
    // (the farther rows join at levels 4, 7 and 10: the damage calculation reads which row, skill_attack's flag)
    class ROWOWSkillBrandishSpear : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBrandishSpear() : ROWOWSkillImpl(ROWOW_SKILL_KN_BRANDISHSPEAR) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            int32 splash = ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel);
            int32 maxCount = ROWOWSkill::GetLevelValue(skill->MaxCount, skillLevel);
            bool isPlayer = src->IsPlayer();
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
            ud->AreaTemp.TargetGUID = target->GetGUID();
            int32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            ROWOW->GetUnitCell(src, x0, y0);
            ROWOW->GetUnitCell(target, x1, y1);
            auto forEachInDir = [this, src, skillLevel, tick, x0, y0, x1, y1, splash](int32 length, int32 offset, int32 areaFlag)
            {
                ROWOW->MapForEachInDir(src, x0, y0, x1, y1, splash, length, offset, [this, src, skillLevel, tick, areaFlag](Unit* bl)
                {
                    return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendDamageID);
                });
            };
            if (skillLevel >= 10)
                forEachInDir(1, maxCount - 1, flag | ROWOW_BCT_ENEMY | (isPlayer == true ? 3 : 0));
            if (skillLevel >= 7)
                forEachInDir(1, maxCount - 2, flag | ROWOW_BCT_ENEMY | (isPlayer == true ? 2 : 0));
            if (skillLevel >= 4)
                forEachInDir(1, maxCount - 3, flag | ROWOW_BCT_ENEMY | (isPlayer == true ? 1 : 0));
            forEachInDir(maxCount - 3, 0, flag | ROWOW_BCT_ENEMY | 0);
            ud->AreaTemp = outerAreaTemp;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Coded apart for it needs the flag passed to the damage calculation.
            ROWOWSkillUnitData const* ud = ROWOW->GetSkillUnitData(src);
            int32 type = ROWOW->GetSkill(SkillID)->AttackType;
            if (ud->AreaTemp.TargetGUID != target->GetGUID())
                ROWOW->SkillAttack(type, src, src, target, SkillID, skillLevel, tick, flag | ROWOW_SD_ANIMATION);
            else
                ROWOW->SkillAttack(type, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void CalculateSkillRatio(ROWOWDamage const* wd, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            int32 ratio = 100 + 20 * skillLevel;
            baseSkillRatio += -100 + ratio;
            if (skillLevel > 3 && wd->MiscFlag == 0)
                baseSkillRatio += ratio / 2;
            if (skillLevel > 6 && wd->MiscFlag == 0)
                baseSkillRatio += ratio / 4;
            if (skillLevel > 9 && wd->MiscFlag == 0)
                baseSkillRatio += ratio / 8;
            if (skillLevel > 6 && wd->MiscFlag == 1)
                baseSkillRatio += ratio / 2;
            if (skillLevel > 9 && wd->MiscFlag == 1)
                baseSkillRatio += ratio / 4;
            if (skillLevel > 9 && wd->MiscFlag == 2)
                baseSkillRatio += ratio / 2;
        }
    };

    // SkillSpearBoomerang
    class ROWOWSkillSpearBoomerang : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillSpearBoomerang() : ROWOWWeaponSkillImpl(ROWOW_SKILL_KN_SPEARBOOMERANG) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50 * skillLevel;
        }
    };

    // SkillCounterAttack: Auto Counter's status, whose counter (battle_weapon_attack) is a critical normal attack.  (The skill timer that
    // only shows the skill a moment later is left out)
    class ROWOWSkillCounterAttack : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCounterAttack() : ROWOWSkillImpl(ROWOW_SKILL_KN_AUTOCOUNTER) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            dmg.Flag = (dmg.Flag & ~ROWOW_BF_SKILLMASK) | ROWOW_BF_NORMAL;
        }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }

        void ModifyHitRate(int16& hitRate, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/) const override
        {
            hitRate += int16(hitRate * 20 / 100);
        }
    };

    // SkillBowlingBash, pre-renewal: the target bowled one cell along the way the Knight faces; when it lands next to other enemies (inside
    // the 40x40 gutter square of bowling_bash_area 0) those are bowled on in random ways, each chain shorter, and the target is hit again.
    // Each unit is bowled once per cast (bowling_db, kept on the caster)
    class ROWOWSkillBowlingBash : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBowlingBash() : ROWOWSkillImpl(ROWOW_SKILL_KN_BOWLINGBASH) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            dmg.BlewCount = 0;
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 40 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            int32 depth = flag & 0xFFF;
            // Chain effect and check range gets reduction by recursive depth, as this can reach 0, we don't use blowcount
            int32 c = (skillLevel - depth + 1) / 2;
            int32 srcX = 0, srcY = 0, targetX = 0, targetY = 0;
            ROWOW->GetUnitCell(src, srcX, srcY);
            ROWOW->GetUnitCell(target, targetX, targetY);
            // Determine the Bowling Bash area depending on configuration
            int32 minX = 0, maxX = 0, minY = 0, maxY = 0;
            if (ROWOW->BattleConfig.BowlingBashArea == 0)
            {
                // Gutter line system
                minX = std::max(0, (srcX - c) - (srcX - c) % 40);
                maxX = minX + 39;
                minY = std::max(0, (srcY - c) - (srcY - c) % 40);
                maxY = minY + 39;
            }
            else if (ROWOW->BattleConfig.BowlingBashArea == 1)
            {
                // Gutter line system without demi gutter bug
                minX = srcX - srcX % 40;
                maxX = minX + 39;
                minY = srcY - srcY % 40;
                maxY = minY + 39;
            }
            else
            {
                // Area around caster
                minX = srcX - ROWOW->BattleConfig.BowlingBashArea;
                maxX = srcX + ROWOW->BattleConfig.BowlingBashArea;
                minY = srcY - ROWOW->BattleConfig.BowlingBashArea;
                maxY = srcY + ROWOW->BattleConfig.BowlingBashArea;
            }
            // Initialization, break checks, direction
            uint8 dir = 0;
            if (depth > 0)
            {
                // Ignore monsters outside area, and monsters already in list
                if (targetX < minX || targetX > maxX || targetY < minY || targetY > maxY)
                    return;
                if (std::find(ud->BowlingTargetGUIDs.begin(), ud->BowlingTargetGUIDs.end(), target->GetGUID()) != ud->BowlingTargetGUIDs.end())
                    return;
                // Random direction
                dir = uint8(urand(0, ROWOW_RO_DIRECTION_COUNT - 1));
            }
            else
            {
                // Create an empty list of already hit targets.  Direction is walkpath
                ud->BowlingTargetGUIDs.clear();
                dir = uint8((ROWOWMod::UnitGetDir(src) + 4) % ROWOW_RO_DIRECTION_COUNT);
            }
            // Add current target to the list of already hit targets
            ud->BowlingTargetGUIDs.push_back(target->GetGUID());

            // Keep moving target in direction square by square.  The knockback is one slide over the cells it gets through (one cell at a
            // time would each start from where the target stood, a WoW slide taking a moment), made when it collides or runs out
            ROWOWMapGrid const* mapGrid = ROWOW->GetMapGrid(target->GetMapId());
            Map* map = target->GetMap();
            int32 tx = targetX, ty = targetY;
            int32 steps = 0;
            bool isBlown = false;
            for (int32 i = 0; i < c; i++)
            {
                // Target coordinates (get changed even if knockback fails)
                tx -= DirX[dir];
                ty -= DirY[dir];
                // If target cell is a wall then break
                if (mapGrid != nullptr && ROWOW->IsCellNoPass(target, *mapGrid, tx, ty) == true)
                    break;
                steps++;
                // Splash around target cell, but only cells inside area; we first have to check the area is not negative
                int32 areaX0 = std::max(minX, tx - 1), areaY0 = std::max(minY, ty - 1), areaX1 = std::min(maxX, tx + 1), areaY1 = std::min(maxY, ty + 1);
                if (areaX0 > areaX1 || areaY0 > areaY1)
                    continue;
                // The target counts among them: it's knocked onto that cell (a knockback-immune one stays beside it)
                int32 countFlag = flag | ROWOW_BCT_ENEMY;
                int32 count = 1 + ROWOW->MapForEachInCellArea(map, target->GetPhaseMask(), areaX0, areaY0, areaX1, areaY1, [this, src, target, skillLevel, tick, countFlag](Unit* bl)
                {
                    return bl == target ? 0 : ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, countFlag, ROWOWSkillAreaFunc::Count);
                });
                int32 moved = ROWOW->SkillBlown(src, target, steps, int8(dir), ROWOW_BLOWN_NONE);
                isBlown = true;
                // Recursive call
                int32 chainFlag = (flag | ROWOW_BCT_ENEMY) + 1;
                ROWOW->MapForEachInCellArea(map, target->GetPhaseMask(), areaX0, areaY0, areaX1, areaY1, [this, src, skillLevel, tick, chainFlag](Unit* bl)
                {
                    return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, chainFlag, ROWOWSkillAreaFunc::CastendDamageID);
                });
                // Self-collision
                int32 landedX = targetX - DirX[dir] * moved, landedY = targetY - DirY[dir] * moved;
                if (landedX >= minX && landedX <= maxX && landedY >= minY && landedY <= maxY)
                    ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, depth > 0 ? ROWOW_SD_ANIMATION | count : count);
                break;
            }
            if (isBlown == false && steps > 0)
                ROWOW->SkillBlown(src, target, steps, int8(dir), ROWOW_BLOWN_NONE);
            // Original hit or chain hit depending on flag
            ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, depth > 0 ? ROWOW_SD_ANIMATION : 0);
        }
    };

    // SkillChargeAttack, pre-renewal: the Knight dashes next to the target, and when nothing stood between them the target is hit harder the
    // farther it was (the distance in cells, distance_math_bl) and knocked back as far
    class ROWOWSkillChargeAttack : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillChargeAttack() : ROWOWSkillImpl(ROWOW_SKILL_KN_CHARGEATK) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& /*flag*/) const override
        {
            bool path = src->IsWithinLOSInMap(target); // path_search_long with CELL_CHKWALL
            // Charge attack in pre-renewal calculates the distance mathetically
            int32 dist = ROWOW->DistanceMathBL(src, target);
            int32 srcX = 0, srcY = 0, targetX = 0, targetY = 0;
            ROWOW->GetUnitCell(src, srcX, srcY);
            ROWOW->GetUnitCell(target, targetX, targetY);
            uint8 dir = ROWOWMod::GetRODirection(float(srcX - targetX), float(srcY - targetY)); // map_calc_dir(target, src->x, src->y)
            // teleport to target (if not on WoE grounds)
            ROWOW->UnitMovePos(src, targetX + DirX[dir], targetY + DirY[dir]);
            // cause damage and knockback if the path to target was a straight one
            if (path == true && ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, dist) != 0)
                ROWOW->SkillBlown(src, target, dist, int8(dir), ROWOW_BLOWN_NONE);
        }

        void CalculateSkillRatio(ROWOWDamage const* wd, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            // +100% every 3 cells of distance but hard-limited to 500%
            int32 k = std::clamp((wd->MiscFlag - 1) / 3, 0, 4);
            baseSkillRatio += 100 * k;
        }
    };

    // SkillSacrifice: Devotion.  The Crusader takes the hits of a party member (a player, not a Crusader nor one devoted by another, within
    // devotion_level_difference base levels) while it stays in reach, in one of as many slots as the level (up to five)
    class ROWOWSkillSacrifice : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSacrifice() : ROWOWSkillImpl(ROWOW_SKILL_CR_DEVOTION) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            Player* dstsd = target->ToPlayer();
            if (sd == nullptr)
                return;
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            int32 type = skill->StatusID;
            if (dstsd == nullptr)
            {
                // Only players can be devoted
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            int32 levelDifference = std::abs(int32(ROWOW->GetROStatus(src)->Level) - int32(ROWOW->GetROStatus(target)->Level));
            ROWOWStatusChangeEntry const* sce = ROWOW->GetSCE(dstsd, type);
            if (levelDifference > ROWOW->BattleConfig.DevotionLevelDifference || // Level difference requeriments
                (sce != nullptr && uint32(sce->Val1) != sd->GetGUID().GetCounter()) || // Cannot Devote a player devoted from another source
                IsCrusaderClass(dstsd) == true || // Crusader Cannot be devoted
                ROWOW->GetSCE(dstsd, ROWOW_SC_HELLPOWER) != nullptr) // Players affected by SC_HELLPOWER cannot be devoted.
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            int32 i = ROWOW->PCDevotionSlot(sd, target->GetGUID(), std::min<int32>(skillLevel, ROWOW_MAX_DEVOTION));
            if (i < 0)
            {
                // No free slots, skill Fail
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            ROWOW->GetIdentity(sd)->DevotionGUIDs[i] = target->GetGUID();
            ROWOW->SCStart4(src, target, type, 10000, int32(sd->GetGUID().GetCounter()), i, ROWOW->SkillGetRange2(src, SkillID, skillLevel, true), 0,
                ROWOW->SkillGetTime2(SkillID, skillLevel));
            // (clif_devotion: the client draws no link between them)
        }
    };

    // SkillResistantSouls: Providence, on anyone but another Crusader
    class ROWOWSkillResistantSouls : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillResistantSouls() : ROWOWSkillImpl(ROWOW_SKILL_CR_PROVIDENCE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd != nullptr && target->IsPlayer() == true && IsCrusaderClass(target) == true)
            {
                // Check they are not another crusader [Skotlex]
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillShieldReflect: Reflect Shield's status, which Dark Crow keeps off (a third job's)
    class ROWOWSkillShieldReflect : public ROWOWStatusSkillImpl
    {
    public:
        ROWOWSkillShieldReflect() : ROWOWStatusSkillImpl(ROWOW_SKILL_CR_REFLECTSHIELD) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            if (ROWOW->GetSCE(target, ROWOW_SC_DARKCROW) != nullptr)
            {
                // SC_DARKCROW prevents using reflecting skills
                if (Player* sd = src->ToPlayer())
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL);
                return;
            }
            ROWOWStatusSkillImpl::CastendNoDamageID(src, target, skillLevel, tick, flag);
        }
    };

    // SkillShieldBoomerang: the shield thrown (its base damage is the shield's, battle_calc_skill_base_damage)
    class ROWOWSkillShieldBoomerang : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillShieldBoomerang() : ROWOWWeaponSkillImpl(ROWOW_SKILL_CR_SHIELDBOOMERANG) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 30 * skillLevel;
        }
    };

    // SkillSmite: Shield Charge, which can stun
    class ROWOWSkillSmite : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillSmite() : ROWOWWeaponSkillImpl(ROWOW_SKILL_CR_SHIELDCHARGE) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 20 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_STUN, 15 + skillLevel * 5, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillHolyCross: a holy weapon attack that can blind
    class ROWOWSkillHolyCross : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillHolyCross() : ROWOWWeaponSkillImpl(ROWOW_SKILL_CR_HOLYCROSS) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 35 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_BLIND, 3 * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillGrandCross: the holy cross of cells around the Crusader (its Dummyskill units, skill_unitsetting), each hitting what stands on it
    // three times with the Crusader's ATK and MATK, the Crusader too for half (battle_calc_magic_attack)
    class ROWOWSkillGrandCross : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGrandCross() : ROWOWSkillImpl(ROWOW_SKILL_CR_GRANDCROSS) { }

        void ApplyCounterAdditionalEffects(Unit* src, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& attackType) const override
        {
            if (src == target)
            {
                // Grand Cross on self specifically only triggers "When hit by physical attack" autospells and ignores everything else
                attackType |= ROWOW_BF_WEAPON;
                attackType &= ~ROWOW_BF_MAGIC;
            }
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            // Chance to cause blind status vs demon and undead element, but not against players
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (target->IsPlayer() == false && (ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == true || tstatus->Race == ROWOW_RACE_DEMON))
                ROWOW->SCStart(src, target, ROWOW_SC_BLIND, 100, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillRelax: Tension Relax, the Lord Knight sitting down to regain HP faster
    class ROWOWSkillRelax : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRelax() : ROWOWSkillImpl(ROWOW_SKILL_LK_TENSIONRELAX) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart4(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, 0, 0, int32(ROWOW->SkillGetTime2(SkillID, skillLevel)),
                ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillSpiralPierce: a player's weight-based thrust (its base damage, battle_calc_skill_base_damage) that holds the target in place.  A
    // monster's is a long ranged misc attack of its ATK, neutral
    class ROWOWSkillSpiralPierce : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillSpiralPierce() : ROWOWWeaponSkillImpl(ROWOW_SKILL_LK_SPIRALPIERCE) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& src, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            if (src.IsPlayer() == false)
                dmg.Flag = (dmg.Flag & ~(ROWOW_BF_RANGEMASK | ROWOW_BF_WEAPONMASK)) | ROWOW_BF_LONG | ROWOW_BF_MISC;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            // Does not work on status immune
            if (target->IsPlayer() == true || (ROWOW->GetROStatus(target)->Mode & ROWOW_MODE_STATUS_IMMUNE) == 0)
                ROWOW->SCStart(src, target, ROWOW_SC_ANKLE, 100, 0, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }

        void ModifyElement(ROWOWDamage const& /*dmg*/, Unit const& src, Unit const& /*target*/, uint16 /*skillLevel*/, int32& element, int32 /*flag*/) const override
        {
            if (src.IsPlayer() == false)
                element = ROWOW_ELEMENT_NEUTRAL; // forced neutral for monsters
        }
    };

    // SkillTraumaticBlow: Head Crush, which can make the target bleed (not the undead or demons); bosses can't be hit by it
    class ROWOWSkillTraumaticBlow : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillTraumaticBlow() : ROWOWWeaponSkillImpl(ROWOW_SKILL_LK_HEADCRUSH) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 40 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            if (ROWOW->GetROStatus(target)->Class == ROWOW_CLASS_BOSS)
            {
                if (Player* sd = src->ToPlayer())
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            ROWOWWeaponSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            // Headcrush has chance of causing Bleeding status, except on demon and undead element
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == false && tstatus->Race != ROWOW_RACE_DEMON)
                ROWOW->SCStart2(src, target, ROWOW_SC_BLEEDING, 50, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillVitalStrike: Joint Beat, breaking one of six joints at random (a broken neck doubles the damage, and stays the joint hit while it
    // lasts)
    class ROWOWSkillVitalStrike : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillVitalStrike() : ROWOWSkillImpl(ROWOW_SKILL_LK_JOINTBEAT) { }

        void CalculateSkillRatio(ROWOWDamage const* wd, Unit const* /*src*/, Unit const* target, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            ROWOWStatusChangeEntry const* tsce = ROWOW->GetSCE(target, ROWOW_SC_JOINTBEAT);
            baseSkillRatio += 10 * skillLevel - 50;
            // The 2x damage is only for the BREAK_NECK ailment.
            if ((wd->MiscFlag & ROWOW_BREAK_NECK) != 0 || (tsce != nullptr && (tsce->Val2 & ROWOW_BREAK_NECK) != 0))
                baseSkillRatio *= 2;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            ROWOWStatusChangeEntry const* tsce = ROWOW->GetSCE(target, ROWOW_SC_JOINTBEAT);
            flag = 1 << urand(0, 5);
            if (flag != ROWOW_BREAK_NECK && tsce != nullptr && (tsce->Val2 & ROWOW_BREAK_NECK) != 0)
                flag = ROWOW_BREAK_NECK; // Target should always receive double damage if neck is already broken
            if (ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, flag) != 0)
                ROWOW->StatusChangeStart(src, target, ROWOW_SC_JOINTBEAT, (50 * (skillLevel + 1) - (270 * tstatus->Stats[ROWOW_STAT_STR]) / 100) * 10, skillLevel,
                    flag & ROWOW_BREAK_FLAGS, int32(src->GetGUID().GetCounter()), 0, ROWOW->SkillGetTime2(SkillID, skillLevel), ROWOW_SCSTART_NONE);
        }
    };

    // SkillBattleChant: Gospel, the Paladin's chant: its units around the Paladin bless the party and curse enemies standing in them while it
    // chants (and can use no other skill); cast again, it stops
    class ROWOWSkillBattleChant : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBattleChant() : ROWOWSkillImpl(ROWOW_SKILL_PA_GOSPEL) { }

        void CastendPos2(Unit* src, int32 /*x*/, int32 /*y*/, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            ROWOWStatusChangeEntry const* sce = type != ROWOW_SC_NONE ? ROWOW->GetSCE(src, type) : nullptr;
            if (sce != nullptr && sce->Val4 == ROWOW_BCT_SELF)
            {
                ROWOW->StatusChangeEnd(src, ROWOW_SC_GOSPEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            int32 x = 0, y = 0;
            ROWOW->GetUnitCell(src, x, y);
            std::shared_ptr<ROWOWSkillUnitGroup> group = ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
            if (group == nullptr)
                return;
            if (sce != nullptr)
                ROWOW->StatusChangeEnd(src, type); // Was under someone else's Gospel. [Skotlex]
            ROWOW->SCStart4(src, src, type, 100, skillLevel, 0, group->GroupID, ROWOW_BCT_SELF, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillGloriaDomini: Pressure, pre-renewal: fixed misc damage (battle_calc_misc_attack) that also takes some of the target's SP
    class ROWOWSkillGloriaDomini : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGloriaDomini() : ROWOWSkillImpl(ROWOW_SKILL_PA_PRESSURE) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW->GetSkill(SkillID)->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void ApplyAdditionalEffects(Unit* /*src*/, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            // status_percent_damage: 15% + 5% a level of the SP it has.  (rAthena also marks the attack physical so physical autospells
            // trigger, on its own copy of the attack type, which changes nothing)
            int64 sp = int64(target->GetPower(POWER_MANA)) * (15 + 5 * skillLevel) / 100;
            if (sp > 0)
                ROWOW->StatusZap(target, 0, uint32(sp));
        }
    };

    // SkillMartyrsReckoning: Sacrifice, whose next normal attacks are its hits (battle_weapon_attack), each costing 9% of the Paladin's max HP
    // and dealing that much, raised by the level
    class ROWOWSkillMartyrsReckoning : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillMartyrsReckoning() : ROWOWWeaponSkillImpl(ROWOW_SKILL_PA_SACRIFICE) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -10 + 10 * skillLevel;
        }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillShieldChain: Rapid Smiting, five hits of the shield (its base damage and a random bonus of its weight and refine,
    // battle_calc_skill_base_damage and battle_calc_skill_constant_addition; +20 hit after the cap)
    class ROWOWSkillShieldChain : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillShieldChain() : ROWOWWeaponSkillImpl(ROWOW_SKILL_PA_SHIELDCHAIN) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 30 * skillLevel; // (Shield Power's +50% is a fourth job's status)
        }
    };
}

void AddROWOWSkillsSwordman()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillBash());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMagnumBreak());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_SM_ENDURE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillProvoke());
    ROWOW->RegisterSkillImpl(new ROWOWSkillAutoBerserk());

    // Knight
    ROWOW->RegisterSkillImpl(new ROWOWSkillPierce());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSpearStab());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBrandishSpear());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSpearBoomerang());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_KN_TWOHANDQUICKEN));
    ROWOW->RegisterSkillImpl(new ROWOWSkillCounterAttack());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBowlingBash());
    ROWOW->RegisterSkillImpl(new ROWOWSkillChargeAttack());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_KN_ONEHAND));

    // Crusader
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_CR_AUTOGUARD, true));
    ROWOW->RegisterSkillImpl(new ROWOWSkillSmite());
    ROWOW->RegisterSkillImpl(new ROWOWSkillShieldBoomerang());
    ROWOW->RegisterSkillImpl(new ROWOWSkillShieldReflect());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHolyCross());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGrandCross());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSacrifice());
    ROWOW->RegisterSkillImpl(new ROWOWSkillResistantSouls());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_CR_DEFENDER, true));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_CR_SPEARQUICKEN));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_CR_SHRINK, true));

    // Lord Knight
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_LK_AURABLADE));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_LK_PARRYING));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_LK_CONCENTRATION));
    ROWOW->RegisterSkillImpl(new ROWOWSkillRelax());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_LK_BERSERK));
    ROWOW->RegisterSkillImpl(new ROWOWSkillSpiralPierce());
    ROWOW->RegisterSkillImpl(new ROWOWSkillTraumaticBlow());
    ROWOW->RegisterSkillImpl(new ROWOWSkillVitalStrike());

    // Paladin
    ROWOW->RegisterSkillImpl(new ROWOWSkillGloriaDomini());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMartyrsReckoning());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBattleChant());
    ROWOW->RegisterSkillImpl(new ROWOWSkillShieldChain());
}
