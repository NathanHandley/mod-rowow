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

#include "Chat.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// The Archer line's skills (rAthena's skills/archer, skill_factory_archer), one class per skill: the Archer's, the Hunter's traps and falcon,
// the Bard's and Dancer's songs, dances and ensembles, the Sniper's, the Clown's and the Gypsy's.  The traps, songs and ensembles are ground
// units (ROWOW_SkillUnit.cpp); their effects are its unit cases.  They run in the caster's map thread

namespace
{
    // SkillDoubleStrafe
    class ROWOWSkillDoubleStrafe : public ROWOWWeaponSkillImpl
    {
    public:
        explicit ROWOWSkillDoubleStrafe(uint16 skillID = ROWOW_SKILL_AC_DOUBLE) : ROWOWWeaponSkillImpl(skillID) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 10 * (skillLevel - 1);
        }
    };

    // SkillArrowShower: an arrow attack on everything around the ground spot (-25% + 5% a level), knocking it away from the spot
    class ROWOWSkillArrowShower : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        explicit ROWOWSkillArrowShower(uint16 skillID = ROWOW_SKILL_AC_SHOWER) : ROWOWRecursiveDamageSplashSkillImpl(skillID) { }

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
        explicit ROWOWSkillChargeArrow(uint16 skillID = ROWOW_SKILL_AC_CHARGEARROW) : ROWOWWeaponSkillImpl(skillID) { }

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

    // SkillConcentration: the status on the caster, which also reveals the hidden within its splash (status_change_timer_sub) and the hidden
    // traps (skill_reveal_trap_inarea)
    class ROWOWSkillConcentration : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillConcentration() : ROWOWSkillImpl(ROWOW_SKILL_AC_CONCENTRATION) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& /*flag*/) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            int32 type = skill->StatusID;
            int32 splash = ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel);
            ROWOW->SCStart(src, target, type, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
            int32 srcX = 0, srcY = 0;
            ROWOW->GetUnitCell(src, srcX, srcY);
            ROWOW->SkillRevealTrapInArea(src, splash, srcX, srcY);
            ROWOW->MapForEachInRange(src, splash, [src, type, tick](Unit* bl)
            {
                ROWOW->StatusChangeTimerSub(bl, src, nullptr, type, tick);
                return 0;
            });
        }
    };

    // A Hunter's trap (SkillSkidTrap, SkillLandMine, SkillAnkleSnare, SkillShockwaveTrap, SkillSandman, SkillFlasher, SkillFreezingTrap,
    // SkillBlastMine, SkillClaymoreTrap, SkillTalkieBox): set on its cell (skill_unitsetting); what it does when it goes off is its unit's case,
    // and the status its hit adds is the class's.  flag 1 keeps the ammo (it goes with the group)
    class ROWOWTrapSkillImpl : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWTrapSkillImpl(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillLandMine: stuns (10%) what it hits
    class ROWOWSkillLandMine : public ROWOWTrapSkillImpl
    {
    public:
        explicit ROWOWSkillLandMine(uint16 skillID = ROWOW_SKILL_HT_LANDMINE) : ROWOWTrapSkillImpl(skillID) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_STUN, 10, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel), 1000);
        }
    };

    // SkillShockwaveTrap: takes 20% + 15% a level of the target's SP
    class ROWOWSkillShockwaveTrap : public ROWOWTrapSkillImpl
    {
    public:
        ROWOWSkillShockwaveTrap() : ROWOWTrapSkillImpl(ROWOW_SKILL_HT_SHOCKWAVE) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->StatusPercentChange(src, target, 0, int8(-(15 * skillLevel + 5)), 2); // status_percent_damage(src, target, 0, -(15*lv+5), false)
        }
    };

    // SkillSandman: sleep (40% + 10% a level) on those around the one that set it off
    class ROWOWSkillSandman : public ROWOWTrapSkillImpl
    {
    public:
        explicit ROWOWSkillSandman(uint16 skillID = ROWOW_SKILL_HT_SANDMAN) : ROWOWTrapSkillImpl(skillID) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_SLEEP, 10 * skillLevel + 40, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel), 1000);
        }
    };

    // SkillFlasher: blinds what it hits
    class ROWOWSkillFlasher : public ROWOWTrapSkillImpl
    {
    public:
        ROWOWSkillFlasher() : ROWOWTrapSkillImpl(ROWOW_SKILL_HT_FLASHER) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_BLIND, 100, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel), 1000);
        }
    };

    // SkillFreezingTrap: a weapon hit (the trap is always short range) that freezes, after the caster's attack motion
    class ROWOWSkillFreezingTrap : public ROWOWTrapSkillImpl
    {
    public:
        explicit ROWOWSkillFreezingTrap(uint16 skillID = ROWOW_SKILL_HT_FREEZINGTRAP) : ROWOWTrapSkillImpl(skillID) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOWStatus const* sstatus = ROWOW->GetROStatus(src);
            ROWOW->SCStart(src, target, ROWOW_SC_FREEZE, 100, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel), sstatus->AttackMotion + 100);
        }
    };

    // SkillRemoveTrap: the Hunter's own trap on the cell it's cast at (traps can't be picked as WoW targets) goes, and gives back its trap item
    // (skill_removetrap_type 0) or every item it was made of; one that went off gives nothing.  Anyone else's trap, or none, fails
    class ROWOWSkillRemoveTrap : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRemoveTrap() : ROWOWSkillImpl(ROWOW_SKILL_HT_REMOVETRAP) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            // Players can only remove their own traps or traps on Vs maps (no RO map here is one)
            Map* map = src->GetMap();
            std::shared_ptr<ROWOWSkillUnitGroup> trapGroup;
            int32 trapIndex = -1;
            ROWOW->MapForEachSkillUnitInArea(map, x, y, x, y, [map, src, &trapGroup, &trapIndex](ROWOWSkillUnitGroup& group, int32 index)
            {
                ROWOWSkill const* skill = ROWOW->GetSkill(group.SkillID);
                if (trapGroup != nullptr || group.SrcGUID != src->GetGUID() || skill == nullptr || skill->Inf2[ROWOW_INF2_ISTRAP] == false)
                    return 0;
                trapGroup = ROWOW->SkillID2Group(map, group.GroupID);
                trapIndex = index;
                return 1;
            });
            if (trapGroup == nullptr)
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            if (trapGroup->UnitID != ROWOW_UNT_USED_TRAPS && (trapGroup->UnitID != ROWOW_UNT_ANKLESNARE || trapGroup->Val2 == 0))
            {
                // prevent picking up expired traps
                ROWOWSkill const* trapSkill = ROWOW->GetSkill(trapGroup->SkillID);
                if (ROWOW->BattleConfig.SkillRemoveTrapType != 0)
                {
                    // get back all items used to deploy the trap
                    for (ROWOWSkillItemRequirement const& item : trapSkill->RequiredItems)
                        if (item.ItemID != 0 && (item.Level == 0 || item.Level == trapGroup->SkillLevel))
                            ROWOW->GiveROItem(sd, item.ItemID, uint32(std::max(1, item.Amount)), true);
                }
                else
                    ROWOW->SkillTrapGiveBack(sd, *trapGroup); // get back 1 trap
            }
            ROWOW->SkillDelUnit(map, *trapGroup, trapIndex);
        }
    };

    // SkillSpringTrap: the falcon springs a trap on the cell it's cast at (anyone's, as long as it hasn't gone off; an Ankle Snare holding
    // something is left to Remove Trap): it turns spent for 1.5 seconds, its effect not set off
    class ROWOWSkillSpringTrap : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSpringTrap() : ROWOWSkillImpl(ROWOW_SKILL_HT_SPRINGTRAP) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 /*skillLevel*/, uint64 tick, int32& /*flag*/) const override
        {
            Map* map = src->GetMap();
            ROWOW->MapForEachSkillUnitInArea(map, x, y, x, y, [map, tick](ROWOWSkillUnitGroup& group, int32 index)
            {
                switch (group.UnitID)
                {
                    case ROWOW_UNT_ANKLESNARE: // ankle snare
                        if (group.Val2 != 0) // if it is already trapping something don't spring it, remove trap should be used instead
                            break;
                        [[fallthrough]];
                    case ROWOW_UNT_BLASTMINE:
                    case ROWOW_UNT_SKIDTRAP:
                    case ROWOW_UNT_LANDMINE:
                    case ROWOW_UNT_SHOCKWAVE:
                    case ROWOW_UNT_SANDMAN:
                    case ROWOW_UNT_FLASHER:
                    case ROWOW_UNT_FREEZINGTRAP:
                    case ROWOW_UNT_CLAYMORETRAP:
                    case ROWOW_UNT_TALKIEBOX:
                        group.UnitID = ROWOW_UNT_USED_TRAPS;
                        ROWOW->ClifChangeTrapLook(map, group, index, nullptr);
                        group.Limit = int64(tick) + ROWOW_TRAP_USED_LIMIT - int64(group.Tick);
                        group.Units[index].Limit = group.Limit;
                        return 1;
                }
                return 0;
            });
        }
    };

    // SkillBlitzBeat: the falcon hits everything around the target (misc damage by DEX, INT and Steel Crow), split among them when autocast
    class ROWOWSkillBlitzBeat : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillBlitzBeat() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_HT_BLITZBEAT) { }
    };

    // SkillDetect: reveals the hidden (SC_SIGHT's status_change_timer_sub) and the hidden traps around the ground spot
    class ROWOWSkillDetect : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillDetect() : ROWOWSkillImpl(ROWOW_SKILL_HT_DETECTING) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 tick, int32& /*flag*/) const override
        {
            int32 i = ROWOWSkill::GetLevelValue(ROWOW->GetSkill(SkillID)->SplashArea, skillLevel);
            ROWOW->MapForEachInArea(src, x - i, y - i, x + i, y + i, [src, tick](Unit* bl)
            {
                ROWOW->StatusChangeTimerSub(bl, src, nullptr, ROWOW_SC_SIGHT, tick);
                return 0;
            });
            ROWOW->SkillRevealTrapInArea(src, i, x, y);
        }
    };

    // SkillPhantasmicArrow: an arrow at 150% that needs no arrow, knocking the target back (but not one it kills)
    class ROWOWSkillPhantasmicArrow : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillPhantasmicArrow() : ROWOWWeaponSkillImpl(ROWOW_SKILL_HT_PHANTASMIC) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50;
        }
    };

    // SkillBeastStrafing: Double Strafe's combo, only on brutes and insects: an arrow at 50% + 8% a point of STR
    class ROWOWSkillBeastStrafing : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBeastStrafing() : ROWOWSkillImpl(ROWOW_SKILL_HT_POWER) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (tstatus->Race == ROWOW_RACE_BRUTE || tstatus->Race == ROWOW_RACE_PLAYER_DORAM || tstatus->Race == ROWOW_RACE_INSECT)
                ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* src, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -50 + 8 * ROWOW->GetROStatus(src)->Stats[ROWOW_STAT_STR];
        }
    };

    // SkillMelodyStrike (Musical Strike) and SkillSlingingArrow (Throw Arrow): an arrow at 60% + 40% a level, with an instrument or a whip
    class ROWOWSkillMelodyStrike : public ROWOWWeaponSkillImpl
    {
    public:
        explicit ROWOWSkillMelodyStrike(uint16 skillID) : ROWOWWeaponSkillImpl(skillID) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -40 + 40 * skillLevel;
        }
    };

    // A song, a dance or an ensemble (SkillPerfectTablature, SkillImpressiveRiff, SkillMagicStrings, SkillSongofLutie, SkillUnchainedSerenade,
    // SkillFocusBallet, SkillSlowGrace, SkillLadyLuck, SkillGypsysKiss, SkillHipShaker, SkillLullaby, SkillMentalSensing, SkillDownTempo,
    // SkillBattleTheme, SkillHarmonicLick, SkillClassicalPluck, SkillPowerChord, SkillAcousticRhythm, SkillShelteringBliss): pre-renewal's are
    // ground units around the performer (cast at its own cell), which start its dance (skill_unitsetting); their effects are their units'
    // cases.  flag 1 keeps the ammo (it goes with the group)
    class ROWOWSongSkillImpl : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSongSkillImpl(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            flag |= 1; // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillHipShaker (Ugly Dance): each interval those around lose SP (5 + 5 a level, more with Dancing Lesson)
    class ROWOWSkillHipShaker : public ROWOWSongSkillImpl
    {
    public:
        ROWOWSkillHipShaker() : ROWOWSongSkillImpl(ROWOW_SKILL_DC_UGLYDANCE) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            int32 rate = 5 + 5 * skillLevel;
            rate += skillLevel * ROWOW->PCCheckSkill(src, ROWOW_SKILL_DC_DANCINGLESSON);
            ROWOW->StatusZap(target, 0, uint32(rate));
        }
    };

    // SkillLullaby: each interval those around may fall asleep, by both performers' INT
    class ROWOWSkillLullaby : public ROWOWSongSkillImpl
    {
    public:
        ROWOWSkillLullaby() : ROWOWSongSkillImpl(ROWOW_SKILL_BD_LULLABY) { }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOWStatus const* sstatus = ROWOW->GetROStatus(src);
            ROWOWStatusChangeEntry const* dancing = ROWOW->GetSCE(src, ROWOW_SC_DANCING);
            if (dancing == nullptr)
                return;
            Unit* partner = ROWOW->MapID2BL(src, dancing->Val4);
            ROWOWStatus const* pstatus = partner != nullptr ? ROWOW->GetROStatus(partner) : nullptr;
            if (pstatus == nullptr)
                return;
            ROWOW->StatusChangeStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, (sstatus->Stats[ROWOW_STAT_INT] + pstatus->Stats[ROWOW_STAT_INT] + int32(urand(100, 300))) * 10,
                skillLevel, 0, 0, 0, ROWOW->SkillGetTime2(SkillID, skillLevel), ROWOW_SCSTART_NONE);
        }
    };

    // SkillUnbarringOctave (Unbarring Octave, the Bard's Frost Joker) and SkillDazzler (the Dancer's Scream): 3 seconds later (a timer skill)
    // everyone around may freeze (or be stunned), the caster's party too (a quarter of the chance, for Duration1)
    class ROWOWSkillFrostJokeScream : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFrostJokeScream(uint16 skillID, int32 extraRate) : ROWOWSkillImpl(skillID), ExtraRate(extraRate) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            int32 srcX = 0, srcY = 0;
            ROWOW->GetUnitCell(src, srcX, srcY);
            ROWOW->SkillAddTimerSkill(src, tick + 3000, target->GetGUID(), srcX, srcY, SkillID, skillLevel, 0, flag);
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            int32 rate = 150 + 50 * skillLevel + ExtraRate; // Aegis accuracy (1000 = 100%).  DC_SCREAM has a 10% higher base chance than BA_FROSTJOKER
            int64 duration = ROWOW->SkillGetTime2(SkillID, skillLevel);
            if (ROWOW->BattleCheckTarget(src, target, ROWOW_BCT_PARTY) > 0)
            {
                // On party members: Chance is divided by 4 and duration is fixed to 15000ms
                rate /= 4;
                duration = ROWOW->SkillGetTime(SkillID, skillLevel);
            }
            ROWOW->StatusChangeStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, rate * 10, skillLevel, 0, 0, 0, duration, ROWOW_SCSTART_NONE);
        }

    private:
        int32 ExtraRate;
    };

    // SkillAmp (Adaptation): pre-renewal's ends the performer's song or dance
    class ROWOWSkillAmp : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillAmp() : ROWOWSkillImpl(ROWOW_SKILL_BD_ADAPTATION) { }

        void CastendNoDamageID(Unit* /*src*/, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (ROWOW->GetSCE(target, ROWOW_SC_DANCING) != nullptr)
                ROWOW->StatusChangeEnd(target, ROWOW_SC_DANCING);
        }
    };

    // SkillEncore: the performer's last song or dance again at half its SP (unit_skilluse_id), started once Encore's cast is over, in the
    // caster's next update, as its own spell
    class ROWOWSkillEncore : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillEncore() : ROWOWSkillImpl(ROWOW_SKILL_BD_ENCORE) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            ROWOWSkillUnitData const* ud = ROWOW->GetSkillUnitData(sd);
            uint16 danceSkillID = ud->SkillIDDance;
            uint16 danceSkillLevel = ud->SkillLevelDance;
            src->m_Events.AddEventAtOffset([src, danceSkillID, danceSkillLevel]()
            {
                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(ROWOW->GetSkillSpellID(danceSkillID, danceSkillLevel));
                if (spellInfo == nullptr || src->IsInWorld() == false || src->IsAlive() == false)
                    return;
                ROWOWSkillUnitData* encoreUD = ROWOW->GetSkillUnitData(src);
                encoreUD->EncoreSkillID = danceSkillID;
                src->CastSpell(src, spellInfo, TRIGGERED_NONE);
                encoreUD->EncoreSkillID = 0;
            }, Milliseconds(1));
        }
    };

    // SkillPangVoice: pre-renewal's confuses (70%)
    class ROWOWSkillPangVoice : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillPangVoice() : ROWOWSkillImpl(ROWOW_SKILL_BA_PANGVOICE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            // In Pre-renewal it causes Confusion to 70% base chance
            ROWOW->SCStart(src, target, ROWOW_SC_CONFUSION, 70, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillWinkofCharm: a player is charmed only if confused (10%); a monster is charmed by the level difference (+40%), and turns to the
    // Dancer (a 0 damage hit)
    class ROWOWSkillWinkofCharm : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillWinkofCharm() : ROWOWSkillImpl(ROWOW_SKILL_DC_WINKCHARM) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& /*flag*/) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if (target->IsPlayer() == true)
            {
                // In Pre-Renewal it only causes Wink Charm, if Confusion was successfully started
                if (ROWOW->SCStart(src, target, ROWOW_SC_CONFUSION, 10, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel)) == true)
                    ROWOW->SCStart(src, target, type, 100, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
            }
            else if (target->IsCreature() == true)
            {
                // For monsters it causes Wink Charm with a chance depending on the level difference
                int32 rate = int32(ROWOW->GetROStatus(src)->Level) - int32(ROWOW->GetROStatus(target)->Level) + 40;
                if (ROWOW->SCStart2(src, target, type, rate, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(SkillID, skillLevel)) == true)
                {
                    // This triggers a 0 damage event and might make the monster switch target to caster
                    ROWOW->BattleDamage(src, target, 0, 1, skillLevel, 0, ROWOW_ATK_DEF, ROWOW_BF_WEAPON | ROWOW_BF_LONG | ROWOW_BF_NORMAL, true, tick, false);
                }
            }
        }
    };

    // SkillVulcanArrow (Arrow Vulcan): an arrow at 200% + 100% a level (9 hits shown as one), with an instrument or a whip
    class ROWOWSkillVulcanArrow : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillVulcanArrow() : ROWOWWeaponSkillImpl(ROWOW_SKILL_CG_ARROWVULCAN) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 + 100 * skillLevel;
        }
    };

    // SkillMarionetteControl: the caster gives half its stats to a player (not a Bard or Dancer of its own sex, nor one cursed or in a
    // Quagmire) while both stay near, and loses them meanwhile; cast again on the same one it ends.  A failure costs nothing
    class ROWOWSkillMarionetteControl : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMarionetteControl() : ROWOWSkillImpl(ROWOW_SKILL_CG_MARIONETTE) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            Player* dstsd = target->ToPlayer();
            if ((sd != nullptr && dstsd != nullptr && ROWOW->PCIsBardDancer(dstsd) == true && dstsd->getGender() == sd->getGender()) ||
                ROWOW->GetSCE(target, ROWOW_SC_CURSE) != nullptr || ROWOW->GetSCE(target, ROWOW_SC_QUAGMIRE) != nullptr)
            {
                // Cannot cast on another bard/dancer-type class of the same gender as caster, or targets under Curse/Quagmire
                if (sd != nullptr)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            ROWOWStatusChangeEntry const* marionette = ROWOW->GetSCE(src, ROWOW_SC_MARIONETTE);
            ROWOWStatusChangeEntry const* marionette2 = ROWOW->GetSCE(target, ROWOW_SC_MARIONETTE2);
            int64 time = ROWOW->SkillGetTime(SkillID, skillLevel);
            if (marionette == nullptr && marionette2 == nullptr)
            {
                ROWOW->SCStart(src, src, ROWOW_SC_MARIONETTE, 100, int32(target->GetGUID().GetCounter()), time);
                ROWOW->SCStart(src, target, ROWOW_SC_MARIONETTE2, 100, int32(src->GetGUID().GetCounter()), time);
            }
            else if (marionette != nullptr && marionette->Val1 == int32(target->GetGUID().GetCounter()) && marionette2 != nullptr &&
                marionette2->Val1 == int32(src->GetGUID().GetCounter()))
            {
                ROWOW->StatusChangeEnd(src, ROWOW_SC_MARIONETTE);
                ROWOW->StatusChangeEnd(target, ROWOW_SC_MARIONETTE2);
            }
            else
            {
                if (sd != nullptr)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
            }
        }
    };

    // SkillLongingForFreedom: while dancing an ensemble (not Moonlight Petals), the performer may move, attack and use skills again, slower
    class ROWOWSkillLongingForFreedom : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillLongingForFreedom() : ROWOWSkillImpl(ROWOW_SKILL_CG_LONGINGFREEDOM) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            ROWOWStatusChangeEntry const* dancing = ROWOW->GetSCE(target, ROWOW_SC_DANCING);
            if (ROWOW->GetSCE(target, type) == nullptr && dancing != nullptr && dancing->Val4 != 0 && (dancing->Val1 & 0xFFFF) != ROWOW_SKILL_CG_MOONLIT) // Can't use Longing for Freedom while under Moonlight Petals. [Skotlex]
                ROWOW->SCStart(src, target, type, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillWandOfHermode: pre-renewal's clears the caster's other ground skills and sets its unit (an ensemble's, danced alone: its dance holds
    // the group in val4, not val2)
    class ROWOWSkillWandOfHermode : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillWandOfHermode() : ROWOWSkillImpl(ROWOW_SKILL_CG_HERMODE) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            ROWOW->SkillClearUnitGroup(src);
            if (std::shared_ptr<ROWOWSkillUnitGroup> sg = ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0))
                ROWOW->SCStart4(src, src, ROWOW_SC_DANCING, 100, SkillID, 0, skillLevel, sg->GroupID, ROWOW->SkillGetTime(SkillID, skillLevel));
            flag |= 1;
        }
    };

    // SkillTarotCardOfFate: at 8% a level (not on a target that drew the Sun, nor in Basilica), one of the 14 cards (by their official
    // chances), the SP paid only then
    class ROWOWSkillTarotCardOfFate : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillTarotCardOfFate() : ROWOWSkillImpl(ROWOW_SKILL_CG_TAROTCARD) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            if (ROWOW->GetSCE(target, ROWOW_SC_TAROTCARD) != nullptr)
            {
                // Target currently has the SUN tarot card effect and is immune to any other effect.
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (int32(urand(0, 99)) > skillLevel * 8 || ROWOW->GetSCE(target, ROWOW_SC_BASILICA) != nullptr ||
                (target->IsCreature() == true && tstatus->Class == ROWOW_CLASS_BATTLEFIELD))
            {
                if (sd != nullptr)
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            ROWOW->StatusZap(src, 0, uint32(ROWOW->GetSkill(SkillID)->SPCost[ROWOWSkill::GetLevelIndex(skillLevel)])); // Consume SP only on success.
            int32 card = SkillTarotCard(src, target, skillLevel, tick); // Actual effect is executed here.
            if (sd != nullptr)
                ChatHandler(sd->GetSession()).PSendSysMessage("Tarot Card of Fate: {}", CardNames[std::clamp(card, 1, 14) - 1]); // clif_specialeffect's card
        }

    private:
        static constexpr char const* CardNames[14] = { "The Fool", "The Magician", "The High Priestess", "The Chariot", "Strength", "The Lovers",
            "Wheel of Fortune", "The Hanged Man", "Death", "Temperance", "The Devil", "The Tower", "The Star", "The Sun" };

        // skill_tarotcard: the card drawn and its effect.  [Playtester]
        int32 SkillTarotCard(Unit* src, Unit* target, uint16 skillLevel, uint64 tick) const
        {
            int32 card = 0;
            // Official chances (tarotcard_equal_chance off)
            int32 rate = int32(urand(0, 99));
            if (rate < 10) card = 1; // THE FOOL
            else if (rate < 20) card = 2; // THE MAGICIAN
            else if (rate < 30) card = 3; // THE HIGH PRIESTESS
            else if (rate < 37) card = 4; // THE CHARIOT
            else if (rate < 47) card = 5; // STRENGTH
            else if (rate < 62) card = 6; // THE LOVERS
            else if (rate < 63) card = 7; // WHEEL OF FORTUNE
            else if (rate < 69) card = 8; // THE HANGED MAN
            else if (rate < 74) card = 9; // DEATH
            else if (rate < 82) card = 10; // TEMPERANCE
            else if (rate < 83) card = 11; // THE DEVIL
            else if (rate < 85) card = 12; // THE TOWER
            else if (rate < 90) card = 13; // THE STAR
            else card = 14; // THE SUN
            int64 time2 = ROWOW->SkillGetTime2(SkillID, skillLevel);
            switch (card)
            {
                case 1: // THE FOOL - heals SP to 0
                    ROWOW->StatusPercentChange(src, target, 0, 100, 2);
                    break;
                case 2: // THE MAGICIAN - matk halved
                    ROWOW->SCStart(src, target, ROWOW_SC_INCMATKRATE, 100, -50, time2);
                    break;
                case 3: // THE HIGH PRIESTESS - all buffs removed
                    ROWOW->StatusChangeClearBuffs(target, ROWOW_SCCB_BUFFS | ROWOW_SCCB_CHEM_PROTECT);
                    break;
                case 4: // THE CHARIOT - 1000 damage, random armor destroyed
                {
                    ROWOW->BattleFixDamage(src, target, 1000, SkillID);
                    if (target->IsAlive() == true)
                    {
                        uint32 const where[] = { ROWOW_EQUIP_POSITION_ARMOR, ROWOW_EQUIP_POSITION_HAND_L,
                            ROWOW_EQUIP_POSITION_HEAD_LOW | ROWOW_EQUIP_POSITION_HEAD_MID | ROWOW_EQUIP_POSITION_HEAD_TOP };
                        ROWOW->SkillBreakEquip(src, target, where[urand(0, 2)], 10000, ROWOW_BCT_ENEMY);
                    }
                    break;
                }
                case 5: // STRENGTH - atk halved
                    ROWOW->SCStart(src, target, ROWOW_SC_INCATKRATE, 100, -50, time2);
                    break;
                case 6: // THE LOVERS - 2000HP heal, random teleported (a monster's teleport comes with the monster skills, R10)
                    ROWOW->StatusHeal(target, 2000, 0, 0, src, 0);
                    if (Player* tsd = target->ToPlayer())
                        ROWOW->TeleportToRandomSpot(tsd);
                    break;
                case 7: // WHEEL OF FORTUNE - random 2 other effects
                    // Recursive call
                    SkillTarotCard(src, target, skillLevel, tick);
                    SkillTarotCard(src, target, skillLevel, tick);
                    break;
                case 8: // THE HANGED MAN - ankle, freeze or stoned
                {
                    int32 const sc[] = { ROWOW_SC_ANKLE, ROWOW_SC_FREEZE, ROWOW_SC_STONEWAIT };
                    uint32 randEff = urand(0, 2);
                    int64 time = randEff == 0 ? time2 : ROWOW->SkillGetTime2(ROWOW->StatusDBGetSkill(sc[randEff]), 1);
                    if (sc[randEff] == ROWOW_SC_STONEWAIT)
                        ROWOW->SCStart2(src, target, ROWOW_SC_STONEWAIT, 100, skillLevel, int32(src->GetGUID().GetCounter()), time,
                            int32(ROWOW->SkillGetTime(ROWOW->StatusDBGetSkill(ROWOW_SC_STONEWAIT), 1)));
                    else
                        ROWOW->SCStart(src, target, sc[randEff], 100, skillLevel, time);
                    break;
                }
                case 9: // DEATH - curse, coma and poison
                    ROWOW->StatusChangeStart(src, target, ROWOW_SC_COMA, 10000, skillLevel, 0, int32(src->GetGUID().GetCounter()), 0, 0, ROWOW_SCSTART_NONE);
                    ROWOW->SCStart(src, target, ROWOW_SC_CURSE, 100, skillLevel, ROWOW->SkillGetTime2(ROWOW->StatusDBGetSkill(ROWOW_SC_CURSE), 1));
                    ROWOW->SCStart2(src, target, ROWOW_SC_POISON, 100, skillLevel, int32(src->GetGUID().GetCounter()), ROWOW->SkillGetTime2(ROWOW->StatusDBGetSkill(ROWOW_SC_POISON), 1));
                    break;
                case 10: // TEMPERANCE - confusion
                    ROWOW->SCStart(src, target, ROWOW_SC_CONFUSION, 100, skillLevel, time2);
                    break;
                case 11: // THE DEVIL - 6666 damage, atk and matk halved, cursed
                    ROWOW->BattleFixDamage(src, target, 6666, SkillID);
                    ROWOW->SCStart(src, target, ROWOW_SC_INCATKRATE, 100, -50, time2);
                    ROWOW->SCStart(src, target, ROWOW_SC_INCMATKRATE, 100, -50, time2);
                    ROWOW->SCStart(src, target, ROWOW_SC_CURSE, skillLevel, 100, ROWOW->SkillGetTime2(ROWOW->StatusDBGetSkill(ROWOW_SC_CURSE), 1));
                    break;
                case 12: // THE TOWER - 4444 damage
                    ROWOW->BattleFixDamage(src, target, 4444, SkillID);
                    break;
                case 13: // THE STAR - stun
                    ROWOW->SCStart(src, target, ROWOW_SC_STUN, 100, skillLevel, ROWOW->SkillGetTime2(ROWOW->StatusDBGetSkill(ROWOW_SC_STUN), 1));
                    break;
                default: // THE SUN - atk, matk, hit, flee and def reduced, immune to more tarot card effects
                    ROWOW->SCStart(src, target, ROWOW_SC_INCATKRATE, 100, -20, time2);
                    ROWOW->SCStart(src, target, ROWOW_SC_INCMATKRATE, 100, -20, time2);
                    ROWOW->SCStart(src, target, ROWOW_SC_INCHITRATE, 100, -20, time2);
                    ROWOW->SCStart(src, target, ROWOW_SC_INCFLEERATE, 100, -20, time2);
                    ROWOW->SCStart(src, target, ROWOW_SC_INCDEFRATE, 100, -20, time2);
                    return 14; // To make sure a valid number is returned
            }
            return card;
        }
    };

    // SkillFalconAssault: the falcon's misc attack (Blitz Beat's damage over its 5 hits, x150% + 70% a level)
    class ROWOWSkillFalconAssault : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillFalconAssault() : ROWOWSkillImpl(ROWOW_SKILL_SN_FALCONASSAULT) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW->GetSkill(SkillID)->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillFocusedArrowStrike (Sharp Shooting): pre-renewal's arrow at 200% + 50% a level hits everything on its path toward the target
    // (map_foreachindir, skill_eightpath_algorithm on), the target itself if the path hits nothing
    class ROWOWSkillFocusedArrowStrike : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        explicit ROWOWSkillFocusedArrowStrike(uint16 skillID = ROWOW_SKILL_SN_SHARPSHOOTING) : ROWOWRecursiveDamageSplashSkillImpl(skillID) { }

        void CalculateSkillRatio(ROWOWDamage const* wd, Unit const* src, Unit const* /*target*/, uint16 skillLevel, int32& skillratio, int32 /*mflag*/) const override
        {
            if (src->IsCreature() == true && SkillID == ROWOW_SKILL_SN_SHARPSHOOTING)
            {
                if ((wd->MiscFlag & 2) != 0) // Splash damage bonus
                    skillratio += -100 + 140 * skillLevel;
                else
                    skillratio += 100 + 50 * skillLevel;
                return;
            }
            skillratio += 100 + 50 * skillLevel;
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
            flag |= 2; // Flag for specific mob damage formula
            ud->AreaTemp.TargetGUID = target->GetGUID();
            int32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            ROWOW->GetUnitCell(src, x0, y0);
            ROWOW->GetUnitCell(target, x1, y1);
            int32 attackFlag = flag;
            uint16 skillID = SkillID;
            // Use official AoE algorithm (skill_attack_area on each unit of the path)
            int32 hit = ROWOW->MapForEachInDir(src, x0, y0, x1, y1, ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel), ROWOWSkill::GetLevelValue(skill->MaxCount, skillLevel), 0,
                [src, target, skill, skillID, skillLevel, tick, attackFlag](Unit* bl)
            {
                if (bl->IsAlive() == false)
                    return 0;
                if (bl == target) // This is the target of the skill, do a full attack and skip target checks.
                    return int32(ROWOW->SkillAttack(skill->AttackType, src, src, bl, skillID, skillLevel, tick, attackFlag));
                if (ROWOW->BattleCheckTarget(src, bl, ROWOW_BCT_ENEMY) <= 0 || ROWOW->StatusCheckSkillUse(nullptr, bl, skillID, 2) == false)
                    return 0;
                // Area-splash, disable skill animation.
                return int32(ROWOW->SkillAttack(skill->AttackType, src, src, bl, skillID, skillLevel, tick, attackFlag | ROWOW_SD_ANIMATION));
            });
            if (hit == 0)
            {
                flag &= ~2; // Only targets in the splash area are affected
                // These skills hit at least the target if the AoE doesn't hit
                ROWOW->SkillAttack(skill->AttackType, src, src, target, SkillID, skillLevel, tick, flag);
            }
            ud->AreaTemp = outerAreaTemp;
        }
    };

    // SkillWindWalker: Flee and walk speed on the caster, or in a party on each member on the map within its splash
    class ROWOWSkillWindWalker : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillWindWalker() : ROWOWSkillImpl(ROWOW_SKILL_SN_WINDWALK) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            Player* sd = src->ToPlayer();
            if (sd == nullptr || sd->GetGroup() == nullptr || (flag & 1) != 0)
            {
                ROWOW->SCStart(src, target, skill->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
                return;
            }
            int32 areaFlag = flag | ROWOW_BCT_PARTY | 1;
            ROWOW->PartyForEachSameMap(sd, ROWOW->SkillGetSplash(SkillID, skillLevel), [this, src, skillLevel, tick, areaFlag](Unit* member)
            {
                return ROWOW->SkillAreaSub(member, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
            });
        }
    };
}

void AddROWOWSkillsArcher()
{
    // Archer
    ROWOW->RegisterSkillImpl(new ROWOWSkillDoubleStrafe());

    // A mercenary's own of these (skills/mercenary): the same but for their SkillID
    ROWOW->RegisterSkillImpl(new ROWOWSkillDoubleStrafe(ROWOW_SKILL_MA_DOUBLE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillArrowShower(ROWOW_SKILL_MA_SHOWER));
    ROWOW->RegisterSkillImpl(new ROWOWSkillChargeArrow(ROWOW_SKILL_MA_CHARGEARROW));
    ROWOW->RegisterSkillImpl(new ROWOWTrapSkillImpl(ROWOW_SKILL_MA_SKIDTRAP));
    ROWOW->RegisterSkillImpl(new ROWOWSkillLandMine(ROWOW_SKILL_MA_LANDMINE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillSandman(ROWOW_SKILL_MA_SANDMAN));
    ROWOW->RegisterSkillImpl(new ROWOWSkillFreezingTrap(ROWOW_SKILL_MA_FREEZINGTRAP));
    ROWOW->RegisterSkillImpl(new ROWOWSkillFocusedArrowStrike(ROWOW_SKILL_MA_SHARPSHOOTING));
    ROWOW->RegisterSkillImpl(new ROWOWSkillConcentration());
    ROWOW->RegisterSkillImpl(new ROWOWSkillArrowShower());
    ROWOW->RegisterSkillImpl(new ROWOWSkillChargeArrow());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMakingArrow());

    // Hunter (Beast Bane, Falconry Mastery and Steel Crow are passives in the battle code)
    ROWOW->RegisterSkillImpl(new ROWOWTrapSkillImpl(ROWOW_SKILL_HT_SKIDTRAP)); // SkillSkidTrap
    ROWOW->RegisterSkillImpl(new ROWOWSkillLandMine());
    ROWOW->RegisterSkillImpl(new ROWOWTrapSkillImpl(ROWOW_SKILL_HT_ANKLESNARE)); // SkillAnkleSnare
    ROWOW->RegisterSkillImpl(new ROWOWSkillShockwaveTrap());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSandman());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFlasher());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFreezingTrap());
    ROWOW->RegisterSkillImpl(new ROWOWTrapSkillImpl(ROWOW_SKILL_HT_BLASTMINE)); // SkillBlastMine
    ROWOW->RegisterSkillImpl(new ROWOWTrapSkillImpl(ROWOW_SKILL_HT_CLAYMORETRAP)); // SkillClaymoreTrap
    ROWOW->RegisterSkillImpl(new ROWOWTrapSkillImpl(ROWOW_SKILL_HT_TALKIEBOX)); // SkillTalkieBox
    ROWOW->RegisterSkillImpl(new ROWOWSkillRemoveTrap());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSpringTrap());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBlitzBeat());
    ROWOW->RegisterSkillImpl(new ROWOWSkillDetect());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPhantasmicArrow());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBeastStrafing());

    // Bard and Dancer (Music Lessons and Dance Lessons are passives in the battle code)
    ROWOW->RegisterSkillImpl(new ROWOWSkillMelodyStrike(ROWOW_SKILL_BA_MUSICALSTRIKE)); // SkillMelodyStrike
    ROWOW->RegisterSkillImpl(new ROWOWSkillMelodyStrike(ROWOW_SKILL_DC_THROWARROW)); // SkillSlingingArrow
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BA_DISSONANCE)); // SkillUnchainedSerenade
    ROWOW->RegisterSkillImpl(new ROWOWSkillFrostJokeScream(ROWOW_SKILL_BA_FROSTJOKER, 0)); // SkillUnbarringOctave
    ROWOW->RegisterSkillImpl(new ROWOWSkillFrostJokeScream(ROWOW_SKILL_DC_SCREAM, 100)); // SkillDazzler
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BA_WHISTLE)); // SkillPerfectTablature
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BA_ASSASSINCROSS)); // SkillImpressiveRiff
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BA_POEMBRAGI)); // SkillMagicStrings
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BA_APPLEIDUN)); // SkillSongofLutie
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_DC_HUMMING)); // SkillFocusBallet
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_DC_DONTFORGETME)); // SkillSlowGrace
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_DC_FORTUNEKISS)); // SkillLadyLuck
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_DC_SERVICEFORYOU)); // SkillGypsysKiss
    ROWOW->RegisterSkillImpl(new ROWOWSkillHipShaker());
    ROWOW->RegisterSkillImpl(new ROWOWSkillAmp());
    ROWOW->RegisterSkillImpl(new ROWOWSkillEncore());
    ROWOW->RegisterSkillImpl(new ROWOWSkillLullaby());
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BD_RICHMANKIM)); // SkillMentalSensing
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BD_ETERNALCHAOS)); // SkillDownTempo
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BD_DRUMBATTLEFIELD)); // SkillBattleTheme
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BD_RINGNIBELUNGEN)); // SkillHarmonicLick
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BD_ROKISWEIL)); // SkillClassicalPluck
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BD_INTOABYSS)); // SkillPowerChord
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_BD_SIEGFRIED)); // SkillAcousticRhythm
    ROWOW->RegisterSkillImpl(new ROWOWSkillPangVoice());
    ROWOW->RegisterSkillImpl(new ROWOWSkillWinkofCharm());

    // Sniper (True Sight is the status alone)
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_SN_SIGHT));
    ROWOW->RegisterSkillImpl(new ROWOWSkillFalconAssault());
    ROWOW->RegisterSkillImpl(new ROWOWSkillFocusedArrowStrike());
    ROWOW->RegisterSkillImpl(new ROWOWSkillWindWalker());

    // Clown and Gypsy
    ROWOW->RegisterSkillImpl(new ROWOWSkillVulcanArrow());
    ROWOW->RegisterSkillImpl(new ROWOWSongSkillImpl(ROWOW_SKILL_CG_MOONLIT)); // SkillShelteringBliss
    ROWOW->RegisterSkillImpl(new ROWOWSkillMarionetteControl());
    ROWOW->RegisterSkillImpl(new ROWOWSkillLongingForFreedom());
    ROWOW->RegisterSkillImpl(new ROWOWSkillWandOfHermode());
    ROWOW->RegisterSkillImpl(new ROWOWSkillTarotCardOfFate());
}
