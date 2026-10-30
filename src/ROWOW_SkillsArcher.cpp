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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/archer, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// The Archer line's skills (rAthena's skills/archer, skill_factory_archer), one class per skill.  They run in the caster's map thread

namespace
{
    // SkillDoubleStrafe
    class ROWOWSkillDoubleStrafe : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillDoubleStrafe() : ROWOWWeaponSkillImpl(ROWOW_SKILL_AC_DOUBLE) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 10 * (skillLevel - 1);
        }
    };

    // SkillArrowShower: an arrow attack on everything around the ground spot (-25% + 5% a level), knocking it away from the spot
    class ROWOWSkillArrowShower : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillArrowShower() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_AC_SHOWER) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -25 + 5 * skillLevel;
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->StatusChangeEnd(src, ROWOW_SC_CAMOUFLAGE);
            ROWOWRecursiveDamageSplashSkillImpl::CastendPos2(src, x, y, skillLevel, tick, flag);
        }
    };

    // SkillChargeArrow: an arrow at 150% that knocks the target back
    class ROWOWSkillChargeArrow : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillChargeArrow() : ROWOWWeaponSkillImpl(ROWOW_SKILL_AC_CHARGEARROW) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50;
        }
    };

    // SkillMakingArrow: the list of the items in the bags that can be made into arrows (clif_arrow_create_list); the one picked is made
    // (skill_arrow_create)
    class ROWOWSkillMakingArrow : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMakingArrow() : ROWOWSkillImpl(ROWOW_SKILL_AC_MAKINGARROW) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                ROWOW->ClifArrowCreateList(sd);
        }
    };

    // SkillConcentration: the status on the caster, which also reveals the hidden within its splash (status_change_timer_sub).  (Its
    // revealing of traps, skill_reveal_trap_inarea, comes with the Hunter's traps: before transcendent jobs' hidden traps none are hidden)
    class ROWOWSkillConcentration : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillConcentration() : ROWOWSkillImpl(ROWOW_SKILL_AC_CONCENTRATION) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& /*flag*/) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            int32 type = skill->StatusID;
            ROWOW->SCStart(src, target, type, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
            ROWOW->MapForEachInRange(src, skill->SplashArea[ROWOWSkill::GetLevelIndex(skillLevel)], [src, type, tick](Unit* bl)
            {
                ROWOW->StatusChangeTimerSub(bl, src, nullptr, type, tick);
                return 0;
            });
        }
    };
}

void AddROWOWSkillsArcher()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillDoubleStrafe());
    ROWOW->RegisterSkillImpl(new ROWOWSkillConcentration());
    ROWOW->RegisterSkillImpl(new ROWOWSkillArrowShower());
    ROWOW->RegisterSkillImpl(new ROWOWSkillChargeArrow());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMakingArrow());
}
