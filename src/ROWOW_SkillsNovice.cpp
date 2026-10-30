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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/novice, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// The Novice's skills (rAthena's skills/novice, skill_factory_novice), one class per skill.  They run in the caster's map thread.  (Basic
// Skill is the tree's and the actions' gate; the Super Novice's skills come with the expanded jobs)

namespace
{
    // SkillFirstAid: heals 5
    class ROWOWSkillFirstAid : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFirstAid() : ROWOWSkillImpl(ROWOW_SKILL_NV_FIRSTAID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->StatusHeal(target, 5, 0, 0, src, ROWOW->GetSkillSpellID(SkillID, skillLevel));
        }
    };
}

void AddROWOWSkillsNovice()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillFirstAid());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_NV_TRICKDEAD, true)); // Play Dead: the StatusSkillImpl, ended when cast again
}
