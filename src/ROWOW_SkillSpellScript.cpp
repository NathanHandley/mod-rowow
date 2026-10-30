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

#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// An RO skill level is a WoW spell that only shows the cast (its targeting, range, cast bar, visuals and sounds, and a dummy effect on the
// target).  This script hands the cast to the RO skill engine (ROWOW_SkillEngine.cpp): the checks when it starts (unit_skilluse_id2), the
// checks when it ends (skill_castend_id), then the costs, the delays and the skill's own rules.  The cast time comes from the engine
// through the core's cast time hook (ROWOW_SkillAllSpellScript).  Runs in the caster's map thread
class ROWOW_SkillSpellScript : public SpellScript
{
    PrepareSpellScript(ROWOW_SkillSpellScript);

    // The skill and level this spell is, cast by an RO combatant, and the unit it's used on (the caster for self and ground skills: rAthena
    // never trusts the client's target for a self skill)
    bool GetSkillCast(Unit*& casterOut, Unit*& targetOut, uint16& skillIDOut, uint16& skillLevelOut)
    {
        casterOut = GetCaster();
        if (ROWOW->IsEnabled == false || casterOut == nullptr || ROWOW->IsROCombatant(casterOut) == false ||
            ROWOW->GetSkillOfSpell(GetSpellInfo()->Id, skillIDOut, skillLevelOut) == false)
            return false;
        ROWOWSkill const* skill = ROWOW->GetSkill(skillIDOut);
        targetOut = GetExplTargetUnit();
        if (targetOut == nullptr || skill == nullptr || (skill->Inf & (ROWOW_INF_SELF_SKILL | ROWOW_INF_GROUND_SKILL)) != 0)
            targetOut = casterOut;
        return true;
    }

    SpellCastResult CheckSkillCast()
    {
        // The first check is the cast starting; once the cast bar ran, the spell checks again as its cast ends
        Unit* caster = nullptr;
        Unit* target = nullptr;
        uint16 skillID = 0;
        uint16 skillLevel = 0;
        if (GetSkillCast(caster, target, skillID, skillLevel) == false)
            return SPELL_CAST_OK;
        if (caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) == GetSpell())
            return ROWOW->SkillCastEndCheck(caster, target, GetExplTargetDest(), skillID, skillLevel);
        return ROWOW->UnitSkillUse(caster, target, GetExplTargetDest(), skillID, skillLevel);
    }

    void HandleSkillCast()
    {
        Unit* caster = nullptr;
        Unit* target = nullptr;
        uint16 skillID = 0;
        uint16 skillLevel = 0;
        if (GetSkillCast(caster, target, skillID, skillLevel) == false)
            return;
        ROWOW->SkillCastEnd(caster, target, GetExplTargetDest(), skillID, skillLevel);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(ROWOW_SkillSpellScript::CheckSkillCast);
        OnCast += SpellCastFn(ROWOW_SkillSpellScript::HandleSkillCast);
    }
};

// The cast time of an RO skill spell is RO's (skill_castfix, through the core's cast time hook), and a cast that stops before it ends
// (moving, damage, the player) lets the caster act again at once (unit_skillcastcancel).  Runs in the caster's map thread
class ROWOW_SkillAllSpellScript : public AllSpellScript
{
public:
    ROWOW_SkillAllSpellScript() : AllSpellScript("ROWOW_SkillAllSpellScript", { ALLSPELLHOOK_ON_CALC_CAST_TIME, ALLSPELLHOOK_ON_CAST_CANCEL }) { }

    void OnSpellCalcCastTime(Spell* spell, int32& castTime) override
    {
        Unit* caster = spell->GetCaster() != nullptr ? spell->GetCaster()->ToUnit() : nullptr;
        uint16 skillID = 0;
        uint16 skillLevel = 0;
        if (ROWOW->IsEnabled == false || caster == nullptr || ROWOW->IsROCombatant(caster) == false || ROWOW->GetSkillOfSpell(spell->GetSpellInfo()->Id, skillID, skillLevel) == false)
            return;
        castTime = ROWOW->UnitSkillCastTime(caster, spell->m_targets.GetUnitTarget(), skillID, skillLevel);
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*bySelf*/) override
    {
        uint16 skillID = 0;
        uint16 skillLevel = 0;
        if (ROWOW->IsEnabled == false || caster == nullptr || spell->getState() != SPELL_STATE_PREPARING || ROWOW->GetSkillOfSpell(spell->GetSpellInfo()->Id, skillID, skillLevel) == false)
            return;
        ROWOW->UnitSkillCastCancel(caster, 0);
    }
};

void AddROWOWSkillSpellScripts()
{
    RegisterSpellScript(ROWOW_SkillSpellScript);
    new ROWOW_SkillAllSpellScript();
}
