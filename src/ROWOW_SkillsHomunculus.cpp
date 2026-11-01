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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/homunculus, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Creature.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"
#include "ROWOW_Skill.h"

using namespace std;

// The homunculi's skills (rAthena's skills/homunculus, skill_factory_homunculus), one class per skill.  Homunculus S's skills are renewal's and aren't here.  Brain Surgery,
// Adamantium Skin and Instruction Change are passives, in status_calc_homunculus_.  Each runs in the map thread of the homunculus and its master

namespace
{
    // SkillHealingTouch: its heal on its master (unit_skilluse_id2's auto target)
    class ROWOWSkillHealingTouch : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHealingTouch() : ROWOWSkillImpl(ROWOW_SKILL_HLIF_HEAL) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            // (Heal bouncing off Kaite and Saturday Night Fever's block are a target's status; heal_exp is 0)
            int32 heal = ROWOW->SkillCalcHeal(src, target, SkillID, skillLevel, true);
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (tstatus != nullptr && tstatus->Class == ROWOW_CLASS_BATTLEFIELD)
                heal = 0;
            if (ROWOW->GetSCE(target, ROWOW_SC_BERSERK) != nullptr)
                heal = 0; // Needed so that it actually displays 0 when healing.
            ROWOW->StatusChangeEnd(target, ROWOW_SC_BITESCAR);
            ROWOW->StatusHeal(target, heal, 0, 0, src, ROWOW->GetSkillSpellID(SkillID, skillLevel));
        }
    };

    // SkillAvoid and SkillDefense: the skill's status on its master and on itself
    class ROWOWSkillMasterAndSelf : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillMasterAndSelf(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            int64 duration = ROWOW->SkillGetTime(SkillID, skillLevel);
            ROWOW->SCStart(src, target, type, 100, skillLevel, duration); // Master
            ROWOW->SCStart(src, src, type, 100, skillLevel, duration); // Homunculus
        }
    };

    // SkillChange: Mental Change, a full heal in pre-renewal, then the status
    class ROWOWSkillChange : public ROWOWStatusSkillImpl
    {
    public:
        ROWOWSkillChange() : ROWOWStatusSkillImpl(ROWOW_SKILL_HLIF_CHANGE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->StatusPercentChange(nullptr, target, -100, -100, 0); // status_percent_heal
            ROWOWStatusSkillImpl::CastendNoDamageID(src, target, skillLevel, tick, flag);
        }
    };

    // SkillCastling: changes places with its master, 20% a level, one of the monsters on the master turning on it
    class ROWOWSkillCastling : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCastling() : ROWOWSkillImpl(ROWOW_SKILL_HAMI_CASTLE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = ROWOW->GetFollowerMaster(src);
            if (src != target && int32(urand(0, 99)) < 20 * skillLevel)
            {
                // Get one of the monsters targeting the player and set the homunculus as its new target
                if (Unit* tbl = ROWOW->BattleGetTargeted(target); tbl != nullptr && tbl->IsCreature() == true && ROWOW->GetROMobID(tbl->ToCreature()) != 0)
                    ROWOW->MobTarget(tbl->ToCreature(), src);
                Position srcPosition = src->GetPosition();
                Position targetPosition = target->GetPosition();
                src->NearTeleportTo(targetPosition.GetPositionX(), targetPosition.GetPositionY(), targetPosition.GetPositionZ(), src->GetOrientation());
                target->NearTeleportTo(srcPosition.GetPositionX(), srcPosition.GetPositionY(), srcPosition.GetPositionZ(), target->GetOrientation());
            }
            else if (sd != nullptr)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillMoonlight: Moonlight's hits
    class ROWOWSkillMoonlight : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillMoonlight() : ROWOWWeaponSkillImpl(ROWOW_SKILL_HFLI_MOON) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 10 + 110 * skillLevel;
        }
    };

    // SkillSBR44: its intimacy as the hit (battle_calc_skill_base_damage), and it's spent
    class ROWOWSkillSBR44 : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillSBR44() : ROWOWWeaponSkillImpl(ROWOW_SKILL_HFLI_SBR44) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 * (skillLevel - 1);
        }

        void ApplyCounterAdditionalEffects(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*attackType*/) const override
        {
            if (ROWOWFollower* fd = ROWOW->GetFollowerType(src) == ROWOW_BL_HOM ? ROWOW->GetFollower(src) : nullptr)
            {
                ROWOWHomunData* hd = static_cast<ROWOWHomunData*>(fd);
                hd->Intimacy = ROWOW->HomIntimacyGrade2Intimacy(ROWOW_HOMGRADE_HATE_WITH_PASSION);
                if (Player* sd = ROWOW->GetFollowerMaster(src))
                    ROWOW->ClifHomInfo(sd);
            }
        }
    };

    // SkillCaprice: one of four bolts at its level
    class ROWOWSkillCaprice : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCaprice() : ROWOWSkillImpl(ROWOW_SKILL_HVAN_CAPRICE) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            static uint16 const subSkills[] = { ROWOW_SKILL_MG_COLDBOLT, ROWOW_SKILL_MG_FIREBOLT, ROWOW_SKILL_MG_LIGHTNINGBOLT, ROWOW_SKILL_WZ_EARTHSPIKE };
            uint16 subSkillID = subSkills[urand(0, 3)];
            ROWOWSkill const* subSkill = ROWOW->GetSkill(subSkillID);
            if (subSkill != nullptr)
                ROWOW->SkillAttack(subSkill->AttackType, src, src, target, subSkillID, skillLevel, tick, flag);
        }
    };

    // SkillBenedictionOfChaos: a heal at a random level on the homunculus, its master or an enemy, by chance per level
    class ROWOWSkillBenedictionOfChaos : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBenedictionOfChaos() : ROWOWSkillImpl(ROWOW_SKILL_HVAN_CHAOTIC) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            // Chance per skill level
            static int32 const chanceHomunculus[] = { 20, 50, 25, 50, 34 };
            static int32 const chanceMaster[] = { 20 + 30, 50 + 10, 25 + 50, 50 + 4, 34 + 33 };
            int32 chance = irand(1, 100);
            int32 levelIndex = std::clamp<int32>(skillLevel, 1, 5) - 1;
            Unit* master = ROWOW->GetFollowerMaster(src);
            if (chance <= chanceHomunculus[levelIndex])
                target = src; // Homunculus
            else if (chance <= chanceMaster[levelIndex])
                target = master; // Master
            else
                target = master != nullptr ? ROWOW->BattleGetTargeted(master) : nullptr; // Enemy (A random enemy targeting the master)

            // If there's no enemy the chance reverts to the homunculus
            if (target == nullptr)
                target = src;
            int32 heal = ROWOW->SkillCalcHeal(src, target, SkillID, uint16(irand(1, skillLevel)), true);
            ROWOW->StatusHeal(target, heal, 0, 0, src, ROWOW->GetSkillSpellID(SkillID, skillLevel));
        }
    };

    // SkillBioExplosion: every enemy in reach for a share of its max HP, its intimacy spent, and it dies a moment later
    class ROWOWSkillBioExplosion : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBioExplosion() : ROWOWSkillImpl(ROWOW_SKILL_HVAN_EXPLOSION) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWFollower* fd = ROWOW->GetFollowerType(src) == ROWOW_BL_HOM ? ROWOW->GetFollower(src) : nullptr;
            if (fd == nullptr)
                return;
            int32 areaFlag = flag | ROWOW_BCT_ENEMY;
            ROWOW->MapForEachInRange(target, ROWOW->SkillGetSplash(SkillID, skillLevel), [this, src, skillLevel, tick, areaFlag](Unit* bl)
            {
                return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendDamageID);
            });
            ROWOWHomunData* hd = static_cast<ROWOWHomunData*>(fd);
            hd->Intimacy = ROWOW->HomIntimacyGrade2Intimacy(ROWOW_HOMGRADE_HATE_WITH_PASSION);
            if (Player* sd = ROWOW->GetFollowerMaster(src))
                ROWOW->ClifHomInfo(sd);

            // There's a delay between the explosion and the homunculus death
            ROWOW->SkillAddTimerSkill(src, tick + uint64(ROWOW->SkillGetTime(SkillID, skillLevel)), src->GetGUID(), 0, 0, SkillID, skillLevel, 0, flag);
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            if (src != target)
                ROWOW->SkillAttack(ROWOW->GetSkill(SkillID)->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };
}

void AddROWOWSkillsHomunculus()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillHealingTouch());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMasterAndSelf(ROWOW_SKILL_HLIF_AVOID));
    ROWOW->RegisterSkillImpl(new ROWOWSkillChange());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCastling());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMasterAndSelf(ROWOW_SKILL_HAMI_DEFENCE));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_HAMI_BLOODLUST));
    ROWOW->RegisterSkillImpl(new ROWOWSkillMoonlight());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_HFLI_FLEET));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_HFLI_SPEED));
    ROWOW->RegisterSkillImpl(new ROWOWSkillSBR44());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCaprice());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBenedictionOfChaos());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBioExplosion());
}
