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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/npc, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Creature.h"
#include "ObjectAccessor.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// The monsters' own skills (rAthena's skills/npc, skill_factory_npc), one class per skill rAthena has, over a few bases for the shapes many share.
// A skill's look is its spell's, which the cast shows; only a look on another unit than the cast's is asked for here.  (rAthena's other NPC_ skills
// aren't in the pre-renewal skill table.)  Everything runs in the caster's map thread: a monster's row of its skill list, the row in use, is read
// from the loaded tables and its AI state from its own data
namespace
{
    uint64 constexpr MOB_SKILL_INTERVAL = 1000; // MOB_SKILL_INTERVAL
    int32 constexpr ET_ANGER = 7; // e_emotion_type's ET_ANGER

    Creature* GetMob(Unit* src)
    {
        // BL_CAST(BL_MOB, src), with a monster's data: an RO monster or nothing
        Creature* md = src->ToCreature();
        return md != nullptr && ROWOW->GetROMobID(md) != 0 ? md : nullptr;
    }

    // The attacks that are a weapon attack of an element at 100% a level, 20% more likely to hit (SkillFireAttributeAttack and the like)
    class ROWOWSkillAttributeAttack : public ROWOWWeaponSkillImpl
    {
    public:
        explicit ROWOWSkillAttributeAttack(uint16 skillID) : ROWOWWeaponSkillImpl(skillID) { }

        void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * (skillLevel - 1);
        }

        void ModifyHitRate(int16& hitRate, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/) const override
        {
            hitRate += hitRate * 20 / 100;
        }
    };

    // SkillRandomAttack: a weapon attack of a random element at 100% a level
    class ROWOWSkillRandomAttack : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillRandomAttack() : ROWOWWeaponSkillImpl(ROWOW_SKILL_NPC_RANDOMATTACK) { }

        void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * (skillLevel - 1);
        }
    };

    // The weapon attacks with a chance of a status, 20% more likely to hit (SkillStunAttack and the like): the chance a level, the skill's Duration2
    class ROWOWSkillStatusAttack : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillStatusAttack(uint16 skillID, int32 type, int32 ratePerLevel = 20) : ROWOWWeaponSkillImpl(skillID), Type(type), RatePerLevel(ratePerLevel) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, Type, RatePerLevel * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }

        void ModifyHitRate(int16& hitRate, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/) const override
        {
            hitRate += hitRate * 20 / 100;
        }

    private:
        int32 Type;
        int32 RatePerLevel;
    };

    // The weapon attacks that may break a piece of equipment, at 1.5% a level (SkillBreakArmor and the like)
    class ROWOWSkillBreakEquipment : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillBreakEquipment(uint16 skillID, uint32 where) : ROWOWWeaponSkillImpl(skillID), Where(where) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SkillBreakEquip(src, target, Where, 150 * skillLevel, ROWOW_BCT_ENEMY);
        }

    private:
        uint32 Where;
    };

    // SkillBreakArmor, SkillBreakHelm, SkillBreakShield: rAthena's EQP_ARMOR, EQP_HELM and EQP_SHIELD
    class ROWOWSkillBreakArmor : public ROWOWSkillBreakEquipment
    {
    public:
        ROWOWSkillBreakArmor() : ROWOWSkillBreakEquipment(ROWOW_SKILL_NPC_ARMORBRAKE, ROWOW_EQUIP_POSITION_ARMOR) { }
    };

    class ROWOWSkillBreakHelm : public ROWOWSkillBreakEquipment
    {
    public:
        ROWOWSkillBreakHelm() : ROWOWSkillBreakEquipment(ROWOW_SKILL_NPC_HELMBRAKE, ROWOW_EQUIP_POSITION_HEAD_LOW | ROWOW_EQUIP_POSITION_HEAD_MID | ROWOW_EQUIP_POSITION_HEAD_TOP) { }
    };

    class ROWOWSkillBreakShield : public ROWOWSkillBreakEquipment
    {
    public:
        ROWOWSkillBreakShield() : ROWOWSkillBreakEquipment(ROWOW_SKILL_NPC_SHIELDBRAKE, ROWOW_EQUIP_POSITION_HAND_L) { }
    };

    // The monster breaths (SkillFireBreath, SkillDarknessBreath, SkillAcidBreath): a line of cells to the target, at 100% a level and twice as likely to hit
    class ROWOWSkillBreath : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillBreath(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * (skillLevel - 1);
        }

        void ModifyHitRate(int16& hitRate, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/) const override
        {
            hitRate *= 2;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Use official AoE algorithm (skill_attack_area, skill_eightpath_algorithm): these skills hit at least the target if the AoE doesn't hit.
            // The breaths keep their animation on every unit
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            int32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            ROWOW->GetUnitCell(src, x0, y0);
            ROWOW->GetUnitCell(target, x1, y1);
            int32 attackFlag = flag;
            int32 hit = ROWOW->MapForEachInDir(src, x0, y0, x1, y1, ROWOW->SkillGetSplash(SkillID, skillLevel), ROWOWSkill::GetLevelValue(skill->MaxCount, skillLevel), 0,
                [this, src, target, skill, skillLevel, tick, attackFlag](Unit* bl)
            {
                if (bl->IsAlive() == false)
                    return 0;
                if (bl != target && (ROWOW->BattleCheckTarget(src, bl, ROWOW_BCT_ENEMY) <= 0 || ROWOW->StatusCheckSkillUse(nullptr, bl, SkillID, 2) == false))
                    return 0;
                return int32(ROWOW->SkillAttack(skill->AttackType, src, src, bl, SkillID, skillLevel, tick, attackFlag));
            });
            if (hit == 0)
                ROWOW->SkillAttack(skill->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    class ROWOWSkillFireBreath : public ROWOWSkillBreath
    {
    public:
        ROWOWSkillFireBreath() : ROWOWSkillBreath(ROWOW_SKILL_NPC_FIREBREATH) { }
    };

    class ROWOWSkillDarknessBreath : public ROWOWSkillBreath
    {
    public:
        ROWOWSkillDarknessBreath() : ROWOWSkillBreath(ROWOW_SKILL_NPC_DARKNESSBREATH) { }
    };

    class ROWOWSkillAcidBreath : public ROWOWSkillBreath
    {
    public:
        ROWOWSkillAcidBreath() : ROWOWSkillBreath(ROWOW_SKILL_NPC_ACIDBREATH) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_POISON, 70, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillAttributeChange and its elements' (SkillFireAttributeChange...): the target's element, at the skill's level
    class ROWOWSkillAttributeChange : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillAttributeChange(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOW->SCStart2(src, target, skill->StatusID, 100, skillLevel, ROWOWSkill::GetLevelValue(skill->Element, skillLevel), ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // The statuses given around the target: the first call (with the target the caster) searches the area, each unit found comes with flag&1
    // (SkillSlowCast, SkillWideBleeding, SkillWideCurse, SkillWideFreeze, SkillHellDignity, SkillWideSilence, SkillWideSleep, SkillWideStun, SkillWidePetrify,
    // SkillWideSoulDrain, SkillDragonFear)
    class ROWOWSkillWideArea : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillWideArea(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            if ((flag & 1) != 0)
            {
                ApplyToUnit(src, target, skillLevel);
                return;
            }
            int32 areaFlag = flag | ROWOW_BCT_ENEMY | ROWOW_SD_PREAMBLE | 1;
            ROWOW->MapForEachInRange(target, ROWOW->SkillGetSplash(SkillID, skillLevel), [this, src, skillLevel, tick, areaFlag](Unit* bl)
            {
                return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
            });
        }

    protected:
        virtual void ApplyToUnit(Unit* src, Unit* target, uint16 skillLevel) const
        {
            ROWOW->SCStart2(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    class ROWOWSkillWidePetrify : public ROWOWSkillWideArea
    {
    public:
        ROWOWSkillWidePetrify() : ROWOWSkillWideArea(ROWOW_SKILL_NPC_WIDESTONE) { }

    protected:
        void ApplyToUnit(Unit* src, Unit* target, uint16 skillLevel) const override
        {
            ROWOW->SCStart2(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel),
                int32(ROWOW->SkillGetTime(SkillID, skillLevel)));
        }
    };

    class ROWOWSkillWideSoulDrain : public ROWOWSkillWideArea
    {
    public:
        ROWOWSkillWideSoulDrain() : ROWOWSkillWideArea(ROWOW_SKILL_NPC_WIDESOULDRAIN) { }

    protected:
        void ApplyToUnit(Unit* src, Unit* target, uint16 skillLevel) const override
        {
            ROWOW->StatusPercentChange(src, target, 0, int8(((skillLevel - 1) % 5 + 1) * 20), 2); // status_percent_damage, no kill
        }
    };

    class ROWOWSkillDragonFear : public ROWOWSkillWideArea
    {
    public:
        ROWOWSkillDragonFear() : ROWOWSkillWideArea(ROWOW_SKILL_NPC_DRAGONFEAR) { }

    protected:
        void ApplyToUnit(Unit* src, Unit* target, uint16 skillLevel) const override
        {
            // One of four statuses, from a random one on until one takes
            int32 const sc[] = { ROWOW_SC_STUN, ROWOW_SC_SILENCE, ROWOW_SC_CONFUSION, ROWOW_SC_BLEEDING };
            int32 count = int32(sizeof(sc) / sizeof(sc[0]));
            int32 j = int32(urand(0, uint32(count - 1)));
            int32 i = j;
            while (ROWOW->SCStart2(src, target, sc[i], 100, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, uint16(i + 1))) == false)
            {
                i++;
                if (i == count)
                    i = 0;
                if (i == j)
                    break;
            }
        }
    };

    // SkillAgilityUp and SkillPowerUp: the status with its two rates
    class ROWOWSkillRateUp : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRateUp(uint16 skillID, int32 val1, int32 val2) : ROWOWSkillImpl(skillID), Val1(val1), Val2(val2) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart2(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, Val1, Val2, ROWOW->SkillGetTime(SkillID, skillLevel));
        }

    private:
        int32 Val1;
        int32 Val2;
    };

    class ROWOWSkillAgilityUp : public ROWOWSkillRateUp
    {
    public:
        ROWOWSkillAgilityUp() : ROWOWSkillRateUp(ROWOW_SKILL_NPC_AGIUP, 50, 100) { }
    };

    class ROWOWSkillPowerUp : public ROWOWSkillRateUp
    {
    public:
        ROWOWSkillPowerUp() : ROWOWSkillRateUp(ROWOW_SKILL_NPC_POWERUP, 200, 100) { }
    };

    // SkillFullHeal (NPC_ALLHEAL): all its HP, and the damage log reset
    class ROWOWSkillFullHeal : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFullHeal() : ROWOWSkillImpl(ROWOW_SKILL_NPC_ALLHEAL) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (ROWOW->StatusIsImmune(target) != 0)
                return;
            ROWOW->StatusHeal(target, target->GetMaxHealth(), 0, 0, src, ROWOW->GetSkillSpellID(ROWOW_SKILL_AL_HEAL, 1));
            if (GetMob(target) != nullptr)
                target->CustomData.Erase("ROWOWDamageLog"); // Reset Damage Logs
        }
    };

    // SkillSuckingBlood (NPC_BLOODDRAIN): a weapon attack whose damage heals the caster
    class ROWOWSkillSuckingBlood : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSuckingBlood() : ROWOWSkillImpl(ROWOW_SKILL_NPC_BLOODDRAIN) { }

        void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * (skillLevel - 1);
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            int32 heal = int32(ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, flag));
            if (heal > 0)
                ROWOW->StatusHeal(src, heal, 0, 0, src, ROWOW->GetSkillSpellID(ROWOW_SKILL_AL_HEAL, 1));
        }
    };

    // SkillEnergyDrain: a spell at 100% more a level whose damage heals the caster
    class ROWOWSkillEnergyDrain : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillEnergyDrain() : ROWOWSkillImpl(ROWOW_SKILL_NPC_ENERGYDRAIN) { }

        void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            int32 heal = int32(ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag));
            if (heal > 0)
                ROWOW->StatusHeal(src, heal, 0, 0, src, ROWOW->GetSkillSpellID(ROWOW_SKILL_AL_HEAL, 1));
        }
    };

    // SkillRecallSlaves (NPC_CALLSLAVE): its slaves to it
    class ROWOWSkillRecallSlaves : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRecallSlaves() : ROWOWSkillImpl(ROWOW_SKILL_NPC_CALLSLAVE) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Creature* md = GetMob(src))
                ROWOW->MobWarpSlave(md, ROWOW_MOB_SLAVEDISTANCE);
        }
    };

    // SkillUndeadAttributeChange (NPC_CHANGEUNDEAD): a weapon attack that may turn the target undead
    class ROWOWSkillUndeadAttributeChange : public ROWOWSkillStatusAttack
    {
    public:
        ROWOWSkillUndeadAttributeChange() : ROWOWSkillStatusAttack(ROWOW_SKILL_NPC_CHANGEUNDEAD, ROWOW_SC_CHANGEUNDEAD, 10) { }
    };

    // SkillMultiStageAttack (NPC_COMBOATTACK): a weapon attack at 25% more a level, its hits by level
    class ROWOWSkillMultiStageAttack : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillMultiStageAttack() : ROWOWWeaponSkillImpl(ROWOW_SKILL_NPC_COMBOATTACK) { }

        void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 25 * skillLevel;
        }
    };

    // SkillCriticalWounds: a weapon attack that always gives its status
    class ROWOWSkillCriticalWounds : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillCriticalWounds() : ROWOWWeaponSkillImpl(ROWOW_SKILL_NPC_CRITICALWOUND) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_CRITICALWOUND, 100, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillDarkBlessing (NPC_DARKBLESSING): Coma at 50% + 5% a level
    class ROWOWSkillDarkBlessing : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillDarkBlessing() : ROWOWSkillImpl(ROWOW_SKILL_NPC_DARKBLESSING) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart2(src, target, ROWOW->GetSkill(SkillID)->StatusID, 50 + skillLevel * 5, skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillDarkBreath: a part of the target's HP as magic, at 50% with the caster's anger either way
    class ROWOWSkillDarkBreath : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillDarkBreath() : ROWOWSkillImpl(ROWOW_SKILL_NPC_DARKBREATH) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->ClifEmotion(src, ET_ANGER);
            if (urand(0, 1) == 0)
                return; // 50% chance
            ROWOW->SkillAttack(ROWOW->GetSkill(SkillID)->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillCrossOfDarkness (NPC_DARKCROSS): a weapon attack at 35% more a level with a chance of Blind
    class ROWOWSkillCrossOfDarkness : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillCrossOfDarkness() : ROWOWWeaponSkillImpl(ROWOW_SKILL_NPC_DARKCROSS) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_BLIND, 3 * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }

        void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 35 * skillLevel;
        }
    };

    // SkillSoulStrikeOfDarkness and SkillDarknessJupitel (NPC_DARKSTRIKE, NPC_DARKTHUNDER): a magic attack
    class ROWOWSkillMagicAttack : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillMagicAttack(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    class ROWOWSkillSoulStrikeOfDarkness : public ROWOWSkillMagicAttack
    {
    public:
        ROWOWSkillSoulStrikeOfDarkness() : ROWOWSkillMagicAttack(ROWOW_SKILL_NPC_DARKSTRIKE) { }
    };

    class ROWOWSkillDarknessJupitel : public ROWOWSkillMagicAttack
    {
    public:
        ROWOWSkillDarknessJupitel() : ROWOWSkillMagicAttack(ROWOW_SKILL_NPC_DARKTHUNDER) { }
    };

    // SkillEarthquake: its unit, at the caster's feet or its spot, hits three times around (its first hit gets through Dodge and Kaupe); its hits
    // are neutral
    class ROWOWSkillEarthquake : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillEarthquake() : ROWOWSkillImpl(ROWOW_SKILL_NPC_EARTHQUAKE) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }

        void ModifyElement(ROWOWDamage const& /*damage*/, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/, int32& element, int32 /*flag*/) const override
        {
            element = ROWOW_ELEMENT_NEUTRAL;
        }
    };

    // SkillEvilLand: a Sanctuary for demons and the undead, which hurts the others, with a chance of Blind
    class ROWOWSkillEvilLand : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillEvilLand() : ROWOWSkillImpl(ROWOW_SKILL_NPC_EVILLAND) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_BLIND, 5 * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            flag |= 1; // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillGrandCrossOfDarkness: Grand Cross's cross of hits at the caster, always blinding
    class ROWOWSkillGrandCrossOfDarkness : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillGrandCrossOfDarkness() : ROWOWSkillImpl(ROWOW_SKILL_NPC_GRANDDARKNESS) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_BLIND, 100, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            flag |= 1; // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillEmotion and SkillEmotionOn: the emotion of the row (val[0]), and a monster's mode changing 'permanently': val[1] 'sets' the mode, val[2]
    // adds to the current mode, val[3] removes from the current mode, val[4] if set asks to delete the previous mode change [Skotlex]
    class ROWOWSkillEmotionBase : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillEmotionBase(uint16 skillID, bool isUnlockingTarget) : ROWOWSkillImpl(skillID), IsUnlockingTarget(isUnlockingTarget) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& /*flag*/) const override
        {
            Creature* md = GetMob(src);
            ROWOWMobSkill const* ms = ROWOW->GetMobSkillInUse(src);
            if (md == nullptr || ms == nullptr)
                return;
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            ROWOW->ClifEmotion(target, ms->Val[0]);
            if (ms->Val[4] != 0 && ROWOW->GetSCE(target, type) != nullptr)
                ROWOW->StatusChangeEnd(target, type);
            ROWOWMobData* mob = ROWOW->GetMobData(md);

            // If mode gets set by NPC_EMOTION then the target should be reset [Playtester]
            if (IsUnlockingTarget == true && ROWOW->BattleConfig.NpcEmotionBehavior == 0 && mob->SkillState != ROWOW_MSS_IDLE && mob->SkillState != ROWOW_MSS_WALK && ms->Val[1] != 0)
                ROWOW->MobUnlockTarget(md, tick);
            if (ms->Val[1] != 0 || ms->Val[2] != 0)
                ROWOW->SCStart4(src, src, type, 100, skillLevel, ms->Val[1], ms->Val[2], ms->Val[3], ROWOW->SkillGetTime(SkillID, skillLevel));

            // Reset aggressive state depending on resulting mode
            if (ROWOW->BattleConfig.NpcEmotionBehavior == 0)
                mob->IsAggressive = (ROWOW->GetROStatus(md)->Mode & ROWOW_MODE_ANGRY) != 0;
        }

    private:
        bool IsUnlockingTarget;
    };

    class ROWOWSkillEmotion : public ROWOWSkillEmotionBase
    {
    public:
        ROWOWSkillEmotion() : ROWOWSkillEmotionBase(ROWOW_SKILL_NPC_EMOTION, true) { }
    };

    class ROWOWSkillEmotionOn : public ROWOWSkillEmotionBase
    {
    public:
        ROWOWSkillEmotionOn() : ROWOWSkillEmotionBase(ROWOW_SKILL_NPC_EMOTION_ON, false) { }
    };

    // SkillExpulsion: the target to a random spot of the map
    class ROWOWSkillExpulsion : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillExpulsion() : ROWOWSkillImpl(ROWOW_SKILL_NPC_EXPULSION) { }

        void CastendNoDamageID(Unit* /*src*/, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Position dest;
            ROWOW->UnitWarpRandom(target, dest);
        }
    };

    // SkillHallucination, SkillHellPower: a status at 20% a level for the skill's Duration2
    class ROWOWSkillLevelRateStatus : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillLevelRateStatus(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, skillLevel * 20, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    class ROWOWSkillHallucination : public ROWOWSkillLevelRateStatus
    {
    public:
        ROWOWSkillHallucination() : ROWOWSkillLevelRateStatus(ROWOW_SKILL_NPC_HALLUCINATION) { }
    };

    class ROWOWSkillHellPower : public ROWOWSkillLevelRateStatus
    {
    public:
        ROWOWSkillHellPower() : ROWOWSkillLevelRateStatus(ROWOW_SKILL_NPC_HELLPOWER) { }
    };

    // The splash attacks around the caster cast as self skills (SkillHellsJudgement, SkillPulseStrike): a weapon attack at 100% a level on everything around
    class ROWOWSkillSelfSplash : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        explicit ROWOWSkillSelfSplash(uint16 skillID) : ROWOWRecursiveDamageSplashSkillImpl(skillID) { }

        void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * (skillLevel - 1);
        }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillCastendDamageID(src, src, SkillID, skillLevel, tick, flag);
        }
    };

    class ROWOWSkillHellsJudgement : public ROWOWSkillSelfSplash
    {
    public:
        ROWOWSkillHellsJudgement() : ROWOWSkillSelfSplash(ROWOW_SKILL_NPC_HELLJUDGEMENT) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_CURSE, 100, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    class ROWOWSkillPulseStrike : public ROWOWSkillSelfSplash
    {
    public:
        ROWOWSkillPulseStrike() : ROWOWSkillSelfSplash(ROWOW_SKILL_NPC_PULSESTRIKE) { }
    };

    // SkillVampireGift: the splash around the caster at 100% more (the level's fifth round) that heals it by each hit
    class ROWOWSkillVampireGift : public ROWOWSkillSelfSplash
    {
    public:
        ROWOWSkillVampireGift() : ROWOWSkillSelfSplash(ROWOW_SKILL_NPC_VAMPIRE_GIFT) { }

        void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += ((skillLevel - 1) % 5 + 1) * 100;
        }

        int64 SplashDamage(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32 flag) const override
        {
            int32 heal = int32(ROWOWSkillSelfSplash::SplashDamage(src, target, skillLevel, tick, flag));
            if (heal > 0)
                ROWOW->StatusHeal(src, heal, 0, 0, src, ROWOW->GetSkillSpellID(ROWOW_SKILL_AL_HEAL, 1));
            return heal;
        }
    };

    // SkillSplashAttack: a splash weapon attack around the target
    class ROWOWSkillSplashAttack : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillSplashAttack() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_NPC_SPLASHATTACK) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            flag |= ROWOW_SD_PREAMBLE; // a fake packet will be sent for the first target to be hit
            ROWOWRecursiveDamageSplashSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
        }
    };

    // SkillInvisible: Cloaking that stays on attacks and skill use (val4 6, "infinite cloak"), with no wall needed
    class ROWOWSkillInvisible : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillInvisible() : ROWOWSkillImpl(ROWOW_SKILL_NPC_INVISIBLE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart4(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, 0, 0, 6, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillLick: 100 SP off the target and a chance of Stun
    class ROWOWSkillLick : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillLick() : ROWOWSkillImpl(ROWOW_SKILL_NPC_LICK) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->StatusZap(target, 0, 100);
            ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, skillLevel * 20, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillDemonShockAttack (NPC_MAGICALATTACK): a magic attack, and its normal attacks turn to magic for a while (SC_MAGICALATTACK)
    class ROWOWSkillDemonShockAttack : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillDemonShockAttack() : ROWOWSkillImpl(ROWOW_SKILL_NPC_MAGICALATTACK) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW->GetSkill(SkillID)->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
            ROWOW->SCStart(src, src, ROWOW_SC_MAGICALATTACK, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillSpiritDestruction (NPC_MENTALBREAKER): a weapon attack that takes a share of the most SP
    class ROWOWSkillSpiritDestruction : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillSpiritDestruction() : ROWOWWeaponSkillImpl(ROWOW_SKILL_NPC_MENTALBREAKER) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            // SP Damage 12%/16%/25%/50%/100% of MaxSP
            int32 rate = 0;
            switch (skillLevel)
            {
                case 1: rate = 12; break;
                case 2: rate = 16; break;
                case 3: rate = 25; break;
                case 4: rate = 50; break;
                case 5: rate = 100; break;
                default: rate = std::min<int32>(skillLevel, 100); break; // For easy customization
            }
            ROWOW->StatusPercentChange(src, target, 0, int8(-rate), 2); // status_percent_damage, no kill
        }

        void ModifyHitRate(int16& hitRate, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/) const override
        {
            hitRate += hitRate * 20 / 100;
        }
    };

    // SkillMetamorphosis: into a random monster of the row, the rest of the level's as new monsters
    class ROWOWSkillMetamorphosis : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMetamorphosis() : ROWOWSkillImpl(ROWOW_SKILL_NPC_METAMORPHOSIS) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Creature* md = GetMob(src);
            ROWOWMobSkill const* ms = ROWOW->GetMobSkillInUse(src);
            if (md == nullptr || ms == nullptr)
                return;
            uint32 classID = ROWOW->MobRandomClass(ms->Val);
            if (skillLevel > 1) // Multiply the rest of mobs. [Skotlex]
                ROWOW->MobSummonSlave(md, ms->Val, skillLevel - 1, SkillID);
            if (classID != 0)
                ROWOW->MobClassChange(md, classID);
        }
    };

    // SkillPetrifyAttack: a weapon attack with a chance of the slow turn to stone
    class ROWOWSkillPetrifyAttack : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillPetrifyAttack() : ROWOWWeaponSkillImpl(ROWOW_SKILL_NPC_PETRIFYATTACK) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart2(src, target, ROWOW_SC_STONEWAIT, 20 * skillLevel, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel),
                int32(ROWOW->SkillGetTime(SkillID, skillLevel)));
        }

        void ModifyHitRate(int16& hitRate, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/) const override
        {
            hitRate += hitRate * 20 / 100;
        }
    };

    // SkillPiercingAttack: a weapon attack through defence at 75% base damage
    class ROWOWSkillPiercingAttack : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillPiercingAttack() : ROWOWWeaponSkillImpl(ROWOW_SKILL_NPC_PIERCINGATT) { }

        void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -25; // 75% base damage
        }
    };

    // SkillProvocation: the monster drops its target
    class ROWOWSkillProvocation : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillProvocation() : ROWOWSkillImpl(ROWOW_SKILL_NPC_PROVOCATION) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 tick, int32& /*flag*/) const override
        {
            if (Creature* md = GetMob(src))
                ROWOW->MobUnlockTarget(md, tick);
        }
    };

    // SkillRandomMove: a fake cast bar while the monster moves up to 8 cells, which NPC_SPEEDUP goes on from.  (The fake bar isn't shown: the client's cast
    // bar comes with a spell cast)
    class ROWOWSkillRandomMove : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRandomMove() : ROWOWSkillImpl(ROWOW_SKILL_NPC_RANDOMMOVE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 /*skillLevel*/, uint64 tick, int32& /*flag*/) const override
        {
            Creature* md = GetMob(src);
            if (md == nullptr)
                return;
            ROWOWMobData* mob = ROWOW->GetMobData(md);
            mob->TrickCasting = tick + MOB_SKILL_INTERVAL * 3; // This skill creates fake casting state where a monster moves while showing a cast bar
            mob->IsCanEscape = true; // Monster cannot be stopped while moving
            ROWOW->UnitEscape(md, target, 8, 3); // Move up to 8 cells
        }
    };

    // SkillRebirth: once, the status that stands the monster up at its death
    class ROWOWSkillRebirth : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRebirth() : ROWOWSkillImpl(ROWOW_SKILL_NPC_REBIRTH) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Creature* md = GetMob(src))
                if (ROWOW->GetMobData(md)->IsRebirth == true)
                    return; // only works once
            ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW_INFINITE_TICK);
        }
    };

    // SkillRevenge: a slave turns on what attacks its master
    class ROWOWSkillRevenge : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRevenge() : ROWOWSkillImpl(ROWOW_SKILL_NPC_REVENGE) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Creature* md = GetMob(src);
            Unit* master = md != nullptr ? ROWOW->MobGetMaster(md) : nullptr;
            Unit* tbl = master != nullptr ? ROWOW->BattleGetTargeted(master) : nullptr;
            if (tbl == nullptr)
                return;
            ROWOW->MobSetProvoked(md, tbl);
        }
    };

    // SkillNpcRun: the monster drops its target and runs from it
    class ROWOWSkillNpcRun : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillNpcRun() : ROWOWSkillImpl(ROWOW_SKILL_NPC_RUN) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 tick, int32& /*flag*/) const override
        {
            Creature* md = GetMob(src);
            if (md == nullptr)
                return;
            ROWOWMobData* mob = ROWOW->GetMobData(md);
            Unit* tbl = mob->TargetID.IsEmpty() == false ? ObjectAccessor::GetUnit(*md, mob->TargetID) : nullptr;
            if (tbl == nullptr)
                return;
            mob->IsCanEscape = true;
            ROWOW->MobUnlockTarget(md, tick);

            // Official distance is 7, if level > 1, distance = level
            uint32 time = ROWOW->UnitEscape(md, tbl, skillLevel > 1 ? skillLevel : 7, 3);
            if (time != 0)
            {
                ROWOW->MobSetState(md, mob, ROWOW_MSS_WALK); // Need to set state here as it's not set otherwise
                mob->NextThinkTime = tick + time; // Set AI to inactive for the duration of this movement
            }
        }
    };

    // SkillSmoking: 3 damage to whoever it's on
    class ROWOWSkillSmoking : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSmoking() : ROWOWSkillImpl(ROWOW_SKILL_NPC_SMOKING) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW->GetSkill(SkillID)->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillSpeedUp: while NPC_RANDOMMOVE's fake cast goes on the monster's base speed rises, then the trick ends
    class ROWOWSkillSpeedUp : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSpeedUp() : ROWOWSkillImpl(ROWOW_SKILL_NPC_SPEEDUP) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 tick, int32& /*flag*/) const override
        {
            Creature* md = GetMob(src);
            if (md == nullptr)
                return;
            ROWOWMobData* mob = ROWOW->GetMobData(md);

            // Officially, trickcasting continues as long as there are more than 700ms left
            int64 trickstop = int64(MOB_SKILL_INTERVAL * 7 / 10);
            int64 left = int64(mob->TrickCasting) - int64(tick);
            if (left >= trickstop)
            {
                // This skill directly modifies a monster's base speed value
                ROWOWStatus status = *ROWOW->GetROBaseStatus(md);
                status.Speed = std::max(status.Speed - 250, ROWOW_MIN_WALK_SPEED);
                md->CustomData.GetDefault<ROWOWMobBaseStatus>("ROWOWMobBaseStatus")->Status = status;

                // Need to recalc speed based on new base value
                ROWOW->StatusCalcBL(md, std::bitset<ROWOW_SCB_MAX>().set(ROWOW_SCB_SPEED));

                // We use skills only on each full cell, to fix the inaccuracy we do this on last move interval
                if (left < trickstop + int64(MOB_SKILL_INTERVAL))
                    mob->LastSkillCheck = tick + 100;
            }
            else
            {
                mob->LastSkillCheck = mob->TrickCasting; // Synchronize skill usage
                mob->TrickCasting = 0; // Causes monster to stop and get ready for next alchemist skill
                mob->IsCanEscape = false;
            }
        }
    };

    // SkillStoneSkin: its armor change (rAthena's SC_ARMORCHANGE), the skill telling which way
    class ROWOWSkillStoneSkin : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillStoneSkin() : ROWOWSkillImpl(ROWOW_SKILL_NPC_STONESKIN) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SCStart2(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, SkillID, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillStop: the target and the caster held together; either's end ends the other's
    class ROWOWSkillStop : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillStop() : ROWOWSkillImpl(ROWOW_SKILL_NPC_STOP) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            int64 time = ROWOW->SkillGetTime(SkillID, skillLevel);
            if (ROWOW->SCStart2(src, target, type, 100, skillLevel, int32(src->GetGUID().GetCounter()), time) == true)
                ROWOW->SCStart2(src, src, type, 100, skillLevel, int32(target->GetGUID().GetCounter()), time);
        }
    };

    // SkillNpcSuicide: when suiciding, neither exp nor drops is given.
    class ROWOWSkillNpcSuicide : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillNpcSuicide() : ROWOWSkillImpl(ROWOW_SKILL_NPC_SUICIDE) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->StatusKill(src);
        }
    };

    // SkillFollowerSummons and SkillMonsterSummons (NPC_SUMMONSLAVE, NPC_SUMMONMONSTER): the row's monsters, level of them
    class ROWOWSkillSummons : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillSummons(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Creature* md = GetMob(src);
            ROWOWMobSkill const* ms = ROWOW->GetMobSkillInUse(src);
            if (md != nullptr && ms != nullptr)
                ROWOW->MobSummonSlave(md, ms->Val, skillLevel, SkillID);
        }
    };

    class ROWOWSkillFollowerSummons : public ROWOWSkillSummons
    {
    public:
        ROWOWSkillFollowerSummons() : ROWOWSkillSummons(ROWOW_SKILL_NPC_SUMMONSLAVE) { }
    };

    class ROWOWSkillMonsterSummons : public ROWOWSkillSummons
    {
    public:
        ROWOWSkillMonsterSummons() : ROWOWSkillSummons(ROWOW_SKILL_NPC_SUMMONMONSTER) { }
    };

    // SkillTalk: only the row's chat line, which mobskill_use says
    class ROWOWSkillTalk : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillTalk() : ROWOWSkillImpl(ROWOW_SKILL_NPC_TALK) { }
    };
}

void AddROWOWSkillsNpc()
{
    // The elements' attacks
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeAttack(ROWOW_SKILL_NPC_FIREATTACK)); // SkillFireAttributeAttack
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeAttack(ROWOW_SKILL_NPC_WATERATTACK)); // SkillWaterAttributeAttack
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeAttack(ROWOW_SKILL_NPC_GROUNDATTACK)); // SkillEarthAttributeAttack
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeAttack(ROWOW_SKILL_NPC_WINDATTACK)); // SkillWindAttributeAttack
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeAttack(ROWOW_SKILL_NPC_POISONATTACK)); // SkillPoisonAttributeAttack
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeAttack(ROWOW_SKILL_NPC_HOLYATTACK)); // SkillHolyAttributeAttack
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeAttack(ROWOW_SKILL_NPC_DARKNESSATTACK)); // SkillShadowAttributeAttack
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeAttack(ROWOW_SKILL_NPC_TELEKINESISATTACK)); // SkillGhostAttributeAttack
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeAttack(ROWOW_SKILL_NPC_UNDEADATTACK)); // SkillUndeadElementAttack
    ROWOW->RegisterSkillImpl(new ROWOWSkillRandomAttack());

    // The status attacks
    ROWOW->RegisterSkillImpl(new ROWOWSkillStatusAttack(ROWOW_SKILL_NPC_POISON, ROWOW_SC_POISON)); // SkillPoisonAttack
    ROWOW->RegisterSkillImpl(new ROWOWSkillStatusAttack(ROWOW_SKILL_NPC_BLINDATTACK, ROWOW_SC_BLIND));
    ROWOW->RegisterSkillImpl(new ROWOWSkillStatusAttack(ROWOW_SKILL_NPC_SILENCEATTACK, ROWOW_SC_SILENCE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillStatusAttack(ROWOW_SKILL_NPC_STUNATTACK, ROWOW_SC_STUN));
    ROWOW->RegisterSkillImpl(new ROWOWSkillStatusAttack(ROWOW_SKILL_NPC_CURSEATTACK, ROWOW_SC_CURSE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillStatusAttack(ROWOW_SKILL_NPC_SLEEPATTACK, ROWOW_SC_SLEEP));
    ROWOW->RegisterSkillImpl(new ROWOWSkillStatusAttack(ROWOW_SKILL_NPC_BLEEDING, ROWOW_SC_BLEEDING));
    ROWOW->RegisterSkillImpl(new ROWOWSkillPetrifyAttack());
    ROWOW->RegisterSkillImpl(new ROWOWSkillUndeadAttributeChange());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCriticalWounds());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSpiritDestruction());

    // The other weapon attacks
    ROWOW->RegisterSkillImpl(new ROWOWSkillBreakArmor());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBreakHelm());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBreakShield());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMultiStageAttack());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPiercingAttack());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCrossOfDarkness());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSuckingBlood());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSplashAttack());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHellsJudgement());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPulseStrike());
    ROWOW->RegisterSkillImpl(new ROWOWSkillVampireGift());
    ROWOW->RegisterSkillImpl(new ROWOWWeaponSkillImpl(ROWOW_SKILL_NPC_CRITICALSLASH));
    ROWOW->RegisterSkillImpl(new ROWOWWeaponSkillImpl(ROWOW_SKILL_NPC_GUIDEDATTACK));
    ROWOW->RegisterSkillImpl(new ROWOWWeaponSkillImpl(ROWOW_SKILL_NPC_RANGEATTACK));

    // The breaths and the spells
    ROWOW->RegisterSkillImpl(new ROWOWSkillFireBreath());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDarknessBreath());
    ROWOW->RegisterSkillImpl(new ROWOWSkillAcidBreath());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDarkBreath());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSoulStrikeOfDarkness());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDarknessJupitel());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEnergyDrain());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDemonShockAttack());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSmoking());

    // The ground units
    ROWOW->RegisterSkillImpl(new ROWOWSkillEarthquake());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEvilLand());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGrandCrossOfDarkness());

    // The statuses on one unit
    ROWOW->RegisterSkillImpl(new ROWOWSkillAgilityUp());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPowerUp());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHallucination());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHellPower());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDarkBlessing());
    ROWOW->RegisterSkillImpl(new ROWOWSkillInvisible());
    ROWOW->RegisterSkillImpl(new ROWOWSkillLick());
    ROWOW->RegisterSkillImpl(new ROWOWSkillStoneSkin());
    ROWOW->RegisterSkillImpl(new ROWOWSkillStop());
    ROWOW->RegisterSkillImpl(new ROWOWSkillRebirth());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_NPC_BARRIER));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_NPC_DEFENDER));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_NPC_INVINCIBLE));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_NPC_KEEPING));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_NPC_MAGICMIRROR));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_NPC_WEAPONBRAKER));
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeChange(ROWOW_SKILL_NPC_ATTRICHANGE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeChange(ROWOW_SKILL_NPC_CHANGEDARKNESS)); // SkillShadowAttributeChange
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeChange(ROWOW_SKILL_NPC_CHANGEFIRE)); // SkillFireAttributeChange
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeChange(ROWOW_SKILL_NPC_CHANGEGROUND)); // SkillEarthAttributeChange
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeChange(ROWOW_SKILL_NPC_CHANGEHOLY)); // SkillHolyAttributeChange
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeChange(ROWOW_SKILL_NPC_CHANGETELEKINESIS)); // SkillGhostAttributeChange
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeChange(ROWOW_SKILL_NPC_CHANGEWATER)); // SkillWaterAttributeChange
    ROWOW->RegisterSkillImpl(new ROWOWSkillAttributeChange(ROWOW_SKILL_NPC_CHANGEWIND)); // SkillWindAttributeChange

    // The statuses around
    ROWOW->RegisterSkillImpl(new ROWOWSkillWideArea(ROWOW_SKILL_NPC_SLOWCAST)); // SkillSlowCast
    ROWOW->RegisterSkillImpl(new ROWOWSkillWideArea(ROWOW_SKILL_NPC_WIDEBLEEDING));
    ROWOW->RegisterSkillImpl(new ROWOWSkillWideArea(ROWOW_SKILL_NPC_WIDECURSE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillWideArea(ROWOW_SKILL_NPC_WIDEFREEZE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillWideArea(ROWOW_SKILL_NPC_WIDEHELLDIGNITY)); // SkillHellDignity
    ROWOW->RegisterSkillImpl(new ROWOWSkillWideArea(ROWOW_SKILL_NPC_WIDESILENCE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillWideArea(ROWOW_SKILL_NPC_WIDESLEEP));
    ROWOW->RegisterSkillImpl(new ROWOWSkillWideArea(ROWOW_SKILL_NPC_WIDESTUN));
    ROWOW->RegisterSkillImpl(new ROWOWSkillWidePetrify());
    ROWOW->RegisterSkillImpl(new ROWOWSkillWideSoulDrain());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDragonFear());

    // The monster's own state, its slaves and its mode
    ROWOW->RegisterSkillImpl(new ROWOWSkillEmotion());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEmotionOn());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFullHeal());
    ROWOW->RegisterSkillImpl(new ROWOWSkillRecallSlaves());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFollowerSummons());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMonsterSummons());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMetamorphosis());
    ROWOW->RegisterSkillImpl(new ROWOWSkillProvocation());
    ROWOW->RegisterSkillImpl(new ROWOWSkillRandomMove());
    ROWOW->RegisterSkillImpl(new ROWOWSkillRevenge());
    ROWOW->RegisterSkillImpl(new ROWOWSkillNpcRun());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSpeedUp());
    ROWOW->RegisterSkillImpl(new ROWOWSkillNpcSuicide());
    ROWOW->RegisterSkillImpl(new ROWOWSkillExpulsion());
    ROWOW->RegisterSkillImpl(new ROWOWSkillTalk());
}
