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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/acolyte, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Player.h"
#include "Unit.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// The Acolyte line's skills (rAthena's skills/acolyte, skill_factory_acolyte): Acolyte, Priest, Monk, High Priest and Champion, one class per
// skill.  They run in the caster's map thread

namespace
{
    // SkillHeal: heals the target, or (cast by a player on an undead enemy) hits it as magic for half the heal, which
    // skill_castend_nodamage_id turns into skill_castend_damage_id
    class ROWOWSkillHeal : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHeal() : ROWOWSkillImpl(ROWOW_SKILL_AL_HEAL) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            // (Kaite's bounce, Berserk's and Saturday Night Fever's block, Bite Scar's end and Akaitsuki's reversal are status; heal_exp is
            // 0, so healing others gives no job exp)
            int32 heal = ROWOW->SkillCalcHeal(src, bl, SkillID, skillLevel, true);
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(bl);
            if (tstatus != nullptr && tstatus->Class == ROWOW_CLASS_BATTLEFIELD)
                heal = 0;
            ROWOW->StatusHeal(bl, heal, 0, 0, src, ROWOW->GetSkillSpellID(SkillID, skillLevel));
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillTeleport: level 1 to a random spot on the map, level 2 to the save point too, picked from the warp point menu
    // (clif_skill_warppoint, then skill_castend_map); an autospell, a Fly Wing or skip_teleport_lv1_menu skip the menu at level 1, and level
    // 3 (a Butterfly Wing) goes to the save point.  (A monster's teleport comes with the monster skills, R10)
    class ROWOWSkillTeleport : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillTeleport() : ROWOWSkillImpl(ROWOW_SKILL_AL_TELEPORT) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            auto mapIDsIter = ROWOW->MapIDsByMapFlag.find("noteleport");
            if (skillLevel <= 2 && mapIDsIter != ROWOW->MapIDsByMapFlag.end() && mapIDsIter->second.find(target->GetMapId()) != mapIDsIter->second.end())
            {
                ChatHandler(sd->GetSession()).SendNotification("You can't teleport in this area."); // clif_skill_teleportmessage
                return;
            }
            if ((ROWOW->BattleConfig.SkipTeleportLv1Menu != 0 && skillLevel == 1) || skillLevel == 3)
            {
                if (skillLevel == 1)
                    ROWOW->TeleportToRandomSpot(sd);
                else
                    ROWOW->TeleportToSavePoint(sd);
                return;
            }
            std::vector<uint32> mapIDs = { 0 }; // "Random"
            if (skillLevel > 1)
            {
                ROWOWIdentity const* identity = ROWOW->GetIdentity(sd);
                ROWOWPosition savePoint = identity->SavePoint;
                if (savePoint.IsSet == false)
                    ROWOW->GetCellPosition(ROWOW->StartMapID, ROWOW->StartCellX, ROWOW->StartCellY, savePoint);
                mapIDs.push_back(savePoint.MapID);
            }
            ROWOW->ClifSkillWarpPoint(sd, SkillID, skillLevel, mapIDs);
        }
    };

    // SkillWarpPortal: the warp point menu of the save point and the memo points the level allows; the portal opens where it was cast once
    // one is picked (skill_castend_map).  The Blue Gemstone is used up then
    class ROWOWSkillWarpPortal : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillWarpPortal() : ROWOWSkillImpl(ROWOW_SKILL_AL_WARP) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            if (Player* sd = src->ToPlayer())
            {
                ROWOWIdentity* identity = ROWOW->GetIdentity(sd);
                ROWOWPosition savePoint = identity->SavePoint;
                if (savePoint.IsSet == false)
                    ROWOW->GetCellPosition(ROWOW->StartMapID, ROWOW->StartCellX, ROWOW->StartCellY, savePoint);
                std::vector<uint32> mapIDs = { savePoint.MapID };
                for (int32 memo = 0; memo < ROWOW_MAX_MEMOPOINTS && memo < skillLevel - 1; memo++)
                    mapIDs.push_back(identity->MemoPoints[memo].MapID);
                identity->MenuSkillValue = (x << 16) | (y & 0xFFFF);
                identity->MenuSkillZ = ROWOW->GetSkillUnitData(src)->SkillZ;
                ROWOW->ClifSkillWarpPoint(sd, SkillID, skillLevel, mapIDs);
            }
            // not to consume item.
            flag |= ROWOW_SKILL_NOCONSUME_REQ;
        }
    };

    // SkillPneuma: its 3x3 of units on the spot, which keep ranged attacks off what stands in them
    class ROWOWSkillPneuma : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillPneuma() : ROWOWSkillImpl(ROWOW_SKILL_AL_PNEUMA) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillHolyLight: a holy magic attack (125%)
    class ROWOWSkillHolyLight : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHolyLight() : ROWOWSkillImpl(ROWOW_SKILL_AL_HOLYLIGHT) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->StatusChangeEnd(target, ROWOW_SC_P_ALTER);
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* src, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 25;
            ROWOWStatusChangeEntry const* spirit = src->IsPlayer() == true ? ROWOW->GetSCE(src, ROWOW_SC_SPIRIT) : nullptr;
            if (spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_PRIEST)
                baseSkillRatio *= 5; // Does 5x damage include bonuses from other skills?
        }
    };

    // SkillCure: ends silence, blindness and confusion
    class ROWOWSkillCure : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCure() : ROWOWSkillImpl(ROWOW_SKILL_AL_CURE) { }

        void CastendNoDamageID(Unit* /*src*/, Unit* bl, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (ROWOW->StatusIsImmune(bl) != 0)
                return;
            ROWOW->StatusChangeEnd(bl, ROWOW_SC_SILENCE);
            ROWOW->StatusChangeEnd(bl, ROWOW_SC_BLIND);
            ROWOW->StatusChangeEnd(bl, ROWOW_SC_CONFUSION);
            ROWOW->StatusChangeEnd(bl, ROWOW_SC_BITESCAR);
        }
    };

    // SkillHolyWater: an Empty Bottle made into Holy Water (skill_produce_mix)
    class ROWOWSkillHolyWater : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillHolyWater() : ROWOWSkillImpl(ROWOW_SKILL_AL_HOLYWATER) { }

        void CastendNoDamageID(Unit* src, Unit* /*bl*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            // (Turning a Ninja's Suiton cell back comes with the Ninja)
            if (ROWOW->SkillProduceMix(sd, SkillID, ROWOW_ITEMID_HOLY_WATER, 1) == false)
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
        }
    };

    // SkillBlessing and SkillIncreaseAgi: the status on the target, or a player turned undead (Change Undead) is hit as misc instead (for 1,
    // which skill_attack sets).  (Blessing's halving of an undead or demon monster's stats is the status's own, with val2 0)
    class ROWOWSkillBlessingIncreaseAgi : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillBlessingIncreaseAgi(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOWStatusChange const* tsc = ROWOW->GetStatusChange(static_cast<Unit const*>(bl));
            if (bl->IsPlayer() == true && tsc != nullptr && tsc->HasSCE(ROWOW_SC_CHANGEUNDEAD) == true)
            {
                if (bl->GetHealth() > 1)
                    ROWOW->SkillAttack(ROWOW_BF_MISC, src, src, bl, SkillID, skillLevel, tick, flag);
                return;
            }
            ROWOW->SCStart(src, bl, skill->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillDecreaseAgi: its chance is 50% + 3% a level + (the caster's base level + INT) / 5
    class ROWOWSkillDecreaseAgi : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillDecreaseAgi() : ROWOWSkillImpl(ROWOW_SKILL_AL_DECAGI) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOWStatus const* sstatus = ROWOW->GetROStatus(src);
            int32 rate = 50 + skillLevel * 3 + (int32(sstatus->Level) + sstatus->Stats[ROWOW_STAT_INT]) / 5;
            ROWOW->SCStart(src, bl, skill->StatusID, rate, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }
    };

    // SkillAngelus, SkillMagnificat and SkillGloria: the status on the caster, or in a party on each member on the map within the skill's
    // splash (party_foreachsamemap through skill_area_sub, which comes back here with flag & 1)
    class ROWOWSkillPartyStatus : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillPartyStatus(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            Player* sd = src->ToPlayer();
            if (sd == nullptr || sd->GetGroup() == nullptr || (flag & 1) != 0)
            {
                ROWOW->SCStart(src, bl, skill->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
                return;
            }
            int32 areaFlag = flag | ROWOW_BCT_PARTY | 1;
            ROWOW->PartyForEachSameMap(sd, skill->SplashArea[ROWOWSkill::GetLevelIndex(skillLevel)], [this, src, skillLevel, tick, areaFlag](Unit* member)
            {
                return ROWOW->SkillAreaSub(member, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
            });
        }
    };

    // SkillCrucis: Signum Crucis on every enemy around the caster (the status takes only undead and demon monsters), each at 25% + 4% a level
    // + the caster's base level - the target's
    class ROWOWSkillCrucis : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillCrucis() : ROWOWSkillImpl(ROWOW_SKILL_AL_CRUCIS) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            if ((flag & 1) != 0)
            {
                ROWOWStatus const* sstatus = ROWOW->GetROStatus(src);
                ROWOWStatus const* tstatus = ROWOW->GetROStatus(bl);
                int32 rate = 25 + skillLevel * 4 + int32(sstatus->Level) - int32(tstatus->Level);
                ROWOW->SCStart(src, bl, skill->StatusID, rate, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
                return;
            }
            int32 areaFlag = flag | ROWOW_BCT_ENEMY | 1;
            ROWOW->MapForEachInRange(src, skill->SplashArea[ROWOWSkill::GetLevelIndex(skillLevel)], [this, src, skillLevel, tick, areaFlag](Unit* target)
            {
                return ROWOW->SkillAreaSub(target, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
            });
        }
    };

    // SkillAspersio: the holy weapon status, which a player can't give a monster (an undead one is hit instead, skill_castend_nodamage_id's
    // undead check), and its magic attack
    class ROWOWSkillAspersio : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillAspersio() : ROWOWSkillImpl(ROWOW_SKILL_PR_ASPERSIO) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (src->IsPlayer() == true && target->IsCreature() == true)
                return;
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOW->SCStart(src, target, skill->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillRuwach: the status (val2 the skill), whose timer reveals the hidden around and hits the enemies among them as magic
    class ROWOWSkillRuwach : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRuwach() : ROWOWSkillImpl(ROWOW_SKILL_AL_RUWACH) { }

        void CastendNoDamageID(Unit* src, Unit* bl, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            ROWOW->SCStart2(src, bl, skill->StatusID, 100, skillLevel, SkillID, ROWOW->SkillGetTime(SkillID, skillLevel));
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 45;
        }
    };

    // SkillSanctuary: its 21 cells, which heal what stands in them each second and hit the undead and demons (skill_unit_onplace_timer)
    class ROWOWSkillSanctuary : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSanctuary() : ROWOWSkillImpl(ROWOW_SKILL_PR_SANCTUARY) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }
    };

    // SkillBenedictioSanctissimiSacramenti: with its two partners, the holy armor status on the players around the spot, and a magic hit on
    // the undead and demons there
    class ROWOWSkillBSSacramenti : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBSSacramenti() : ROWOWSkillImpl(ROWOW_SKILL_PR_BENEDICTIO) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == false && tstatus->Race != ROWOW_RACE_DEMON)
                ROWOW->SCStart(src, target, ROWOW->GetSkill(SkillID)->StatusID, 100, skillLevel, ROWOW->SkillGetTime(SkillID, skillLevel));
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Should attack undead and demons. [Skotlex]
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == true || tstatus->Race == ROWOW_RACE_DEMON)
                ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
            ud->AreaTemp.TargetGUID = src->GetGUID();
            int32 i = ROWOW->GetSkill(SkillID)->SplashArea[ROWOWSkill::GetLevelIndex(skillLevel)];
            int32 playerFlag = flag | ROWOW_BCT_ALL | 1;
            ROWOW->MapForEachInArea(src, x - i, y - i, x + i, y + i, [this, src, skillLevel, tick, playerFlag](Unit* bl)
            {
                return bl->IsPlayer() == true ? ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, playerFlag, ROWOWSkillAreaFunc::CastendNoDamageID) : 0;
            });
            int32 enemyFlag = flag | ROWOW_BCT_ENEMY | 1;
            ROWOW->MapForEachInArea(src, x - i, y - i, x + i, y + i, [this, src, skillLevel, tick, enemyFlag](Unit* bl)
            {
                return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, enemyFlag, ROWOWSkillAreaFunc::CastendDamageID);
            });
            ud->AreaTemp = outerAreaTemp;
        }
    };

    // SkillLexDivina: silence a second later (a timer skill), or a silenced target's silence ended
    class ROWOWSkillLexDivina : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillLexDivina() : ROWOWSkillImpl(ROWOW_SKILL_PR_LEXDIVINA) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            int32 type = ROWOW->GetSkill(SkillID)->StatusID;
            if (ROWOW->GetSCE(target, type) != nullptr)
                ROWOW->StatusChangeEnd(target, type);
            else
                ROWOW->SkillAddTimerSkill(src, tick + 1000, target->GetGUID(), 0, 0, SkillID, skillLevel, 100, flag);
        }
    };

    // SkillTurnUndead: on the undead only, a chance to kill outright, else a little holy damage (battle_calc_magic_attack's case)
    class ROWOWSkillTurnUndead : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillTurnUndead() : ROWOWSkillImpl(ROWOW_SKILL_PR_TURNUNDEAD) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == false)
                return;
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }
    };

    // SkillMagnusExorcismus: its 33 cells, which hit the undead and demons in them every 3 seconds (pre-renewal's), 30% harder
    class ROWOWSkillMagnusExorcismus : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMagnusExorcismus() : ROWOWSkillImpl(ROWOW_SKILL_PR_MAGNUS) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            // Set flag to 1 to prevent deleting ammo (it will be deleted on group-delete).
            flag |= 1;
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* target, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == true || tstatus->Race == ROWOW_RACE_DEMON)
                baseSkillRatio += 30;
        }
    };

    // SkillStatusRecovery: ends the stopped states (stone, freeze, stun, sleep...) and resets a monster's target; on the undead, blindness a
    // second later instead
    class ROWOWSkillStatusRecovery : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillStatusRecovery() : ROWOWSkillImpl(ROWOW_SKILL_PR_STRECOVERY) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            if (ROWOW->StatusIsImmune(target) != 0)
                return;
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == true)
                ROWOW->SkillAddTimerSkill(src, tick + 1000, target->GetGUID(), 0, 0, SkillID, skillLevel, 100, flag);
            else
            {
                // Bodystate is reset to "normal" for non-undead: the following are bodystate status changes
                ROWOW->StatusChangeEnd(target, ROWOW_SC_STONE);
                ROWOW->StatusChangeEnd(target, ROWOW_SC_FREEZE);
                ROWOW->StatusChangeEnd(target, ROWOW_SC_STUN);
                ROWOW->StatusChangeEnd(target, ROWOW_SC_SLEEP);
                ROWOW->StatusChangeEnd(target, ROWOW_SC_STONEWAIT);
                ROWOW->StatusChangeEnd(target, ROWOW_SC_BURNING);
                ROWOW->StatusChangeEnd(target, ROWOW_SC_WHITEIMPRISON);
                // Resetting bodystate to normal always also resets the monster AI to idle (mob_unlocktarget)
                if (Creature* dstmd = target->ToCreature(); dstmd != nullptr && dstmd->IsAIEnabled == true && dstmd->IsInCombat() == true)
                    dstmd->AI()->EnterEvadeMode(CreatureAI::EVADE_REASON_OTHER);
            }
            // Ends SC_NETHERWORLD and SC_NORECOVER_STATE (even on undead)
            ROWOW->StatusChangeEnd(target, ROWOW_SC_NETHERWORLD);
            ROWOW->StatusChangeEnd(target, ROWOW_SC_NORECOVER_STATE);
        }
    };

    // Resurrection's revive (skill_castend_nodamage_id's case, which Redemptio falls into at level 3): the dead target stands with 10/30/50/80%
    // of its HP, and with resurrection_exp the reviver gains exp from a higher level player.  Not on War of Emperium or battleground maps (none here)
    void ResurrectionRevive(Unit* src, Unit* target, uint16 skillLevel)
    {
        Player* sd = src->ToPlayer();
        Player* dstsd = target->ToPlayer();
        if (target->IsAlive() == true)
            return;
        if (ROWOW->GetSCE(target, ROWOW_SC_HELLPOWER) != nullptr)
            return;
        uint8 per = 0, sper = 0;
        switch (skillLevel)
        {
            case 1: per = 10; break;
            case 2: per = 30; break;
            case 3: per = 50; break;
            case 4: per = 80; break;
        }
        if (dstsd != nullptr && ROWOW->GetDeferredBonus(dstsd, "bRestartFullRecover") != 0)
            per = sper = 100;
        if (ROWOW->StatusRevive(target, per, sper) == false)
            return;
        if (sd != nullptr && dstsd != nullptr && ROWOW->BattleConfig.ResurrectionExp > 0)
        {
            ROWOWIdentity const* identity = ROWOW->GetIdentity(sd);
            ROWOWIdentity const* dstIdentity = ROWOW->GetIdentity(dstsd);
            uint64 exp = 0, jexp = 0;
            int32 lv = int32(dstIdentity->BaseLevel) - int32(identity->BaseLevel);
            int32 jlv = int32(dstIdentity->JobLevel) - int32(identity->JobLevel);
            if (lv > 0 && ROWOW->PCNextBaseExp(*dstIdentity) != 0)
                exp = std::max<uint64>(1, uint64(double(dstIdentity->BaseExp) * lv * ROWOW->BattleConfig.ResurrectionExp / 1000000.));
            if (jlv > 0 && ROWOW->PCNextJobExp(*dstIdentity) != 0)
                jexp = std::max<uint64>(1, uint64(double(dstIdentity->JobExp) * lv * ROWOW->BattleConfig.ResurrectionExp / 1000000.)); // (rAthena scales it by the base levels)
            if (exp > 0 || jexp > 0)
                ROWOW->PCGainExp(sd, exp, jexp);
        }
    }

    // SkillResurrection: revives a dead player, or (cast on the undead) hits it as holy magic, Turn Undead's way
    class ROWOWSkillResurrection : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillResurrection() : ROWOWSkillImpl(ROWOW_SKILL_ALL_RESURRECTION) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if (ROWOWMod::BattleCheckUndead(tstatus->Race, tstatus->DefElement) == false)
                return;
            ROWOW->SkillAttack(ROWOW_BF_MAGIC, src, src, target, SkillID, skillLevel, tick, flag);
        }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ResurrectionRevive(src, target, skillLevel);
        }
    };

    // SkillRedemptio: revives every dead party member around (as Resurrection 3), at the caster's cost: its SP, all but 1 HP, and (pre-renewal)
    // exp unless it revived exp_cost_redemptio_limit players.  It fails with no party, or with no one to revive
    class ROWOWSkillRedemptio : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillRedemptio() : ROWOWSkillImpl(ROWOW_SKILL_PR_REDEMPTIO) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            if (sd != nullptr && (flag & 1) == 0)
            {
                if (sd->GetGroup() == nullptr)
                {
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                    return;
                }
                // party_foreachsamemap through skill_area_sub: the members (the dead too, whom battle_check_target doesn't turn away in rAthena)
                ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
                ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
                ud->AreaTemp.Values[0] = 0;
                int32 areaFlag = flag | ROWOW_BCT_PARTY | 1;
                ROWOW->PartyForEachSameMap(sd, ROWOW->GetSkill(SkillID)->SplashArea[ROWOWSkill::GetLevelIndex(skillLevel)], [this, src, skillLevel, tick, areaFlag](Unit* member)
                {
                    return ROWOW->SkillCastendNoDamageID(src, member, SkillID, skillLevel, tick, areaFlag);
                });
                int32 revived = ud->AreaTemp.Values[0];
                ud->AreaTemp = outerAreaTemp;
                if (revived == 0)
                {
                    ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                    return;
                }
                int32 penalty = ROWOW->BattleConfig.ExpCostRedemptioLimit - revived; // The actual penalty...
                if (penalty > 0 && ROWOW->BattleConfig.ExpCostRedemptio != 0 && ROWOW->BattleConfig.ExpCostRedemptioLimit != 0)
                {
                    // Apply penalty: if total penalty is 1% => reduced 0.2% penalty per each revived player
                    ROWOWIdentity const* identity = ROWOW->GetIdentity(sd);
                    uint64 lost = std::min<uint64>(identity->BaseExp, (ROWOW->PCNextBaseExp(*identity) * uint64(penalty) * uint64(ROWOW->BattleConfig.ExpCostRedemptio) /
                        uint64(ROWOW->BattleConfig.ExpCostRedemptioLimit)) / 100);
                    ROWOW->PCLostExp(sd, lost, 0);
                }
                ROWOW->StatusSetSP(src, 0);
                src->SetHealth(1); // status_set_hp
                return;
            }
            if (target->IsAlive() == true || (flag & 1) == 0)
                return; // Invalid target, skip resurrection.
            // Revive: count it in, then fall-through to the Resurrection code (level 3 is used)
            ROWOW->GetSkillUnitData(src)->AreaTemp.Values[0]++;
            ResurrectionRevive(src, target, 3);
        }
    };

    // SkillAssumptio: the status (damage taken halved), which a player can't give a monster
    class ROWOWSkillAssumptio : public ROWOWStatusSkillImpl
    {
    public:
        ROWOWSkillAssumptio() : ROWOWStatusSkillImpl(ROWOW_SKILL_HP_ASSUMPTIO) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            if (src->IsPlayer() == true && target->IsCreature() == true)
                ROWOW->ClifSkillFail(src->ToPlayer(), SkillID, ROWOW_USESKILL_FAIL_LEVEL);
            else
                ROWOWStatusSkillImpl::CastendNoDamageID(src, target, skillLevel, tick, flag);
        }
    };

    // SkillBasilica: pre-renewal's 5x5 sanctuary around the caster (who can't move while it stands): no attacks or skills in it, monsters
    // pushed out, and no hits from outside on those in it.  Cast again it ends (without paying again), and it fails on Land Protector.  The
    // caster's other ground skills go first (skill_clear_unitgroup)
    class ROWOWSkillBasilica : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillBasilica() : ROWOWSkillImpl(ROWOW_SKILL_HP_BASILICA) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            if (ROWOW->GetSCE(src, ROWOW_SC_BASILICA) != nullptr)
            {
                ROWOW->StatusChangeEnd(src, ROWOW_SC_BASILICA); // Cancel Basilica and return so requirement isn't consumed again
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            if (ROWOW->MapGetCellLandProtector(src->GetMap(), x, y) == true)
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            // Create Basilica
            ROWOW->SkillClearUnitGroup(src);
            ROWOW->SkillUnitSetting(src, SkillID, skillLevel, x, y, 0);
            flag |= 1;
        }
    };

    // SkillSummoningSpiritSphere: a Spirit Sphere, up to the skill's level
    class ROWOWSkillSummonSpiritSphere : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSummonSpiritSphere() : ROWOWSkillImpl(ROWOW_SKILL_MO_CALLSPIRITS) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                ROWOW->PCAddSpiritBall(sd, ROWOW->SkillGetTime(SkillID, skillLevel), skillLevel);
        }
    };

    // SkillZen: five Spirit Spheres at once
    class ROWOWSkillZen : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillZen() : ROWOWSkillImpl(ROWOW_SKILL_CH_SOULCOLLECT) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
            {
                int32 limit = 5;
                for (int32 i = 0; i < limit; i++)
                    ROWOW->PCAddSpiritBall(sd, ROWOW->SkillGetTime(SkillID, skillLevel), limit);
            }
        }
    };

    // SkillAbsorbSpiritSphere: a player's spheres (its own, or an enemy's) taken for 7 SP each, or a 20% chance at a monster's 2 SP a level,
    // which turns it on the Monk
    class ROWOWSkillAbsorbSpiritSphere : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillAbsorbSpiritSphere() : ROWOWSkillImpl(ROWOW_SKILL_MO_ABSORBSPIRITS) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            Player* dstsd = target->ToPlayer();
            Creature* dstmd = target->ToCreature();
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            int32 i = 0;
            if (dstsd != nullptr && (ROWOW->BattleCheckTarget(src, target, ROWOW_BCT_SELF) > 0 || ROWOW->BattleCheckTarget(src, target, ROWOW_BCT_ENEMY) > 0) && // Only works on self and enemies
                ROWOW->GetIdentity(dstsd)->JobID != ROWOW_JOB_GUNSLINGER) // so that gunslingers' coins cannot be removed
            {
                if (ROWOW->PCGetSpiritBall(dstsd) > 0)
                {
                    i = ROWOW->PCGetSpiritBall(dstsd) * 7;
                    ROWOW->PCDelSpiritBall(dstsd, ROWOW->PCGetSpiritBall(dstsd), 0);
                }
            }
            else if (dstmd != nullptr && (tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) == 0 && urand(0, 99) < 20)
            {
                // the 20% chance to absorb 2 SP per monster's level
                i = 2 * int32(tstatus->Level);
                if (dstmd->IsAIEnabled == true)
                    dstmd->AI()->AttackStart(src); // mob_target
            }
            else
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                return;
            }
            if (i != 0)
                ROWOW->StatusHeal(src, 0, i, 3, src, 0);
        }
    };

    // SkillRagingTrifectaBlow (Triple Attack): three hits (20% more a level), which battle_weapon_attack swings in place of a normal attack
    class ROWOWSkillTripleAttack : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillTripleAttack() : ROWOWWeaponSkillImpl(ROWOW_SKILL_MO_TRIPLEATTACK) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            int32 sflag = flag | ROWOW_SD_ANIMATION;
            ROWOWWeaponSkillImpl::CastendDamageID(src, target, skillLevel, tick, sflag);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 20 * skillLevel;
        }
    };

    // SkillRagingQuadrupleBlow (Chain Combo): four hits after Triple Attack (or held by Blade Stop), which ends Blade Stop
    class ROWOWSkillChainCombo : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillChainCombo() : ROWOWWeaponSkillImpl(ROWOW_SKILL_MO_CHAINCOMBO) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWWeaponSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
            ROWOW->StatusChangeEnd(src, ROWOW_SC_BLADESTOP);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50 + 50 * skillLevel;
        }
    };

    // SkillRagingThrust (Combo Finish): one hit after Chain Combo; with the Monk spirit it hits everything around the target.  (Gentle Touch's
    // energy gain bonus is a third job's)
    class ROWOWSkillComboFinish : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillComboFinish() : ROWOWWeaponSkillImpl(ROWOW_SKILL_MO_COMBOFINISH) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWStatusChangeEntry const* spirit = ROWOW->GetSCE(src, ROWOW_SC_SPIRIT);
            if ((flag & 1) == 0 && spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_MONK)
            {
                // Becomes a splash attack when Soul Linked.
                int32 areaFlag = flag | ROWOW_BCT_ENEMY | 1;
                ROWOW->MapForEachInRange(target, ROWOW->GetSkill(SkillID)->SplashArea[ROWOWSkill::GetLevelIndex(skillLevel)], [this, src, skillLevel, tick, areaFlag](Unit* bl)
                {
                    return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendDamageID);
                });
            }
            else
                ROWOWWeaponSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 140 + 60 * skillLevel;
        }
    };

    // SkillAsuraStrike: one hit of 8 times the attack plus 10% a point of SP, then all SP gone and the Monk 3 cells on (SkillExtremityFistEnd)
    class ROWOWSkillAsuraStrike : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillAsuraStrike() : ROWOWWeaponSkillImpl(ROWOW_SKILL_MO_EXTREMITYFIST) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWWeaponSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
            ROWOW->SkillExtremityFistEnd(src, target, skillLevel);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* src, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 700 + int32(src->GetPower(POWER_MANA)) * 10;
            baseSkillRatio = std::min(500000, baseSkillRatio); // We stop at roughly 50k SP for overflow protection
        }
    };

    // SkillOccultImpaction: one hit that DEF makes harder (is_attack_piercing), which ends Blade Stop
    class ROWOWSkillOccultImpaction : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillOccultImpaction() : ROWOWWeaponSkillImpl(ROWOW_SKILL_MO_INVESTIGATE) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWWeaponSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
            ROWOW->StatusChangeEnd(src, ROWOW_SC_BLADESTOP);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 75 * skillLevel;
        }
    };

    // SkillThrowSpiritSphere: a hit for each sphere thrown (no more than the Monk had: spiritball_old, castbegin's), which ends Blade Stop.
    // (finger_offensive_type 1 throws them one after another as timer skills)
    class ROWOWSkillThrowSpiritSphere : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillThrowSpiritSphere() : ROWOWWeaponSkillImpl(ROWOW_SKILL_MO_FINGEROFFENSIVE) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& src, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            if (src.IsPlayer() == false)
                return;
            ROWOWSkillUnitData const* ud = src.CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
            int32 spheres = ROWOW->PCGetSpiritBall(&src) + (ud != nullptr ? ud->SpiritBallOld : 0);
            if (ROWOW->BattleConfig.FingerOffensiveType != 0)
                dmg.Div = 1;
            else if (spheres < dmg.Div)
                dmg.Div = int16(spheres);
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWWeaponSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
            if (ROWOW->BattleConfig.FingerOffensiveType != 0 && src->IsPlayer() == true)
            {
                for (int32 i = 1; i < ROWOW->GetSkillUnitData(src)->SpiritBallOld; i++)
                    ROWOW->SkillAddTimerSkill(src, tick + uint64(i) * 200, target->GetGUID(), 0, 0, SkillID, skillLevel, ROWOW_BF_WEAPON, flag);
            }
            ROWOW->StatusChangeEnd(src, ROWOW_SC_BLADESTOP);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50 * skillLevel;
        }
    };

    // SkillSnap: the Monk moves to the spot at once; Asura Strike can't be used for 2 seconds after
    class ROWOWSkillSnap : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillSnap() : ROWOWSkillImpl(ROWOW_SKILL_MO_BODYRELOCATION) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 /*skillLevel*/, uint64 tick, int32& /*flag*/) const override
        {
            if (ROWOW->UnitMovePos(src, x, y) == false)
                return;
            if (Player* sd = src->ToPlayer())
            {
                // skill_blockpc_start
                ROWOW->GetSkillUnitData(sd)->CooldownEndTimesBySkillID[ROWOW_SKILL_MO_EXTREMITYFIST] = tick + 2000;
                ROWOW->SendSkillDelayToClient(sd, ROWOW_SKILL_MO_EXTREMITYFIST);
            }
        }
    };

    // SkillKiTranslation: the Monk's spheres the skill needs given to a party member with fewer than five (not a Gunslinger)
    class ROWOWSkillKiTranslation : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillKiTranslation() : ROWOWSkillImpl(ROWOW_SKILL_MO_KITRANSLATION) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            Player* dstsd = target->ToPlayer();
            if (sd != nullptr && dstsd != nullptr && ROWOW->GetIdentity(dstsd)->JobID != ROWOW_JOB_GUNSLINGER && ROWOW->PCGetSpiritBall(dstsd) < 5)
            {
                // Require will define how many spiritballs will be transferred
                ROWOWSkillCondition require = ROWOW->SkillGetRequirement(sd, SkillID, skillLevel);
                ROWOW->PCDelSpiritBall(sd, require.SpiritBall, 0);
                for (int32 i = 0; i < require.SpiritBall; i++)
                    ROWOW->PCAddSpiritBall(dstsd, ROWOW->SkillGetTime(SkillID, skillLevel), 5);
            }
            else
            {
                ROWOW->ClifSkillFail(sd, SkillID, ROWOW_USESKILL_FAIL_LEVEL);
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
            }
        }
    };

    // SkillKiExplosion: a hit on the target, and everything around it pushed back and (70%) stunned
    class ROWOWSkillKiExplosion : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillKiExplosion() : ROWOWSkillImpl(ROWOW_SKILL_MO_BALKYOUNG) { }

        void ModifyDamageData(ROWOWDamage& dmg, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/) const override
        {
            dmg.BlewCount = 0;
        }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& /*flag*/) const override
        {
            // Passive part of the attack. Splash knock-back+stun. [Skotlex]
            if (ROWOW->GetSkillUnitData(src)->AreaTemp.TargetGUID != target->GetGUID())
            {
                ROWOW->SkillBlown(src, target, ROWOW->GetSkill(SkillID)->Knockback[ROWOWSkill::GetLevelIndex(skillLevel)], -1, ROWOW_BLOWN_NONE);
                ROWOW->SkillAdditionalEffect(src, target, SkillID, skillLevel, ROWOW_BF_MISC, ROWOW_ATK_DEF, tick); // Use Misc rather than weapon to signal passive pushback
            }
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Active part of the attack. Skill-attack [Skotlex].  The target is kept to keep the splash off it (skill_area_temp[1])
            ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
            ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
            ud->AreaTemp.TargetGUID = target->GetGUID();
            if (ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, flag) != 0)
            {
                int32 areaFlag = flag | ROWOW_BCT_ENEMY | 1;
                ROWOW->MapForEachInRange(target, ROWOW->GetSkill(SkillID)->SplashArea[ROWOWSkill::GetLevelIndex(skillLevel)], [this, src, skillLevel, tick, areaFlag](Unit* bl)
                {
                    return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
                });
            }
            ud->AreaTemp = outerAreaTemp;
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 200;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 attackType, uint8 /*dmgLv*/) const override
        {
            // Note: attack_type is passed as BF_WEAPON for the actual target, BF_MISC for the splash-affected mobs.
            if ((attackType & ROWOW_BF_MISC) != 0) // 70% base stun chance...
                ROWOW->SCStart(src, target, ROWOW_SC_STUN, 70, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillRagingPalmStrike: under Fury, a hit a second (plus the attack motion) after the cast (a timer skill), which knocks back 3 cells
    class ROWOWSkillPalmStrike : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillPalmStrike() : ROWOWSkillImpl(ROWOW_SKILL_CH_PALMSTRIKE) { }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            // Palm Strike takes effect 1sec after casting. [Skotlex]
            ROWOWStatus const* sstatus = ROWOW->GetROStatus(src);
            ROWOW->SkillAddTimerSkill(src, tick + uint64(1000 + sstatus->AttackMotion), target->GetGUID(), 0, 0, SkillID, skillLevel, ROWOW_BF_WEAPON, flag);
        }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 100 + 100 * skillLevel;
        }
    };

    // SkillGlacierFist (Tiger Knuckle Fist): a hit after Combo Finish or Chain Crush Combo with a chance to hold the target (ankle snare),
    // shorter on agile targets and a fifth as long on bosses
    class ROWOWSkillGlacierFist : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillGlacierFist() : ROWOWWeaponSkillImpl(ROWOW_SKILL_CH_TIGERFIST) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += -60 + 100 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            int64 basetime = ROWOW->SkillGetTime(SkillID, skillLevel);
            int64 mintime = 15 * (int64(ROWOW->GetROStatus(src)->Level) + 100);
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            if ((tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0)
                basetime /= 5;
            basetime = std::max((basetime * tstatus->Stats[ROWOW_STAT_AGI]) / -200 + basetime, mintime);
            ROWOW->SCStart(src, target, ROWOW_SC_ANKLE, (1 + skillLevel) * 10, 0, basetime);
        }
    };

    // SkillChainCrushCombo: hits after Combo Finish or Glacier Fist, 400% and 100% a level
    class ROWOWSkillChainCrushCombo : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillChainCrushCombo() : ROWOWWeaponSkillImpl(ROWOW_SKILL_CH_CHAINCRUSH) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 300 + 100 * skillLevel;
        }
    };
}

void AddROWOWSkillsAcolyte()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillHeal());
    ROWOW->RegisterSkillImpl(new ROWOWSkillTeleport());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBlessingIncreaseAgi(ROWOW_SKILL_AL_BLESSING));
    ROWOW->RegisterSkillImpl(new ROWOWSkillBlessingIncreaseAgi(ROWOW_SKILL_AL_INCAGI));
    ROWOW->RegisterSkillImpl(new ROWOWSkillDecreaseAgi());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPartyStatus(ROWOW_SKILL_AL_ANGELUS));
    ROWOW->RegisterSkillImpl(new ROWOWSkillCrucis());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_PR_KYRIE)); // SkillKyrieEleison: the status alone
    ROWOW->RegisterSkillImpl(new ROWOWSkillPartyStatus(ROWOW_SKILL_PR_MAGNIFICAT));
    ROWOW->RegisterSkillImpl(new ROWOWSkillPartyStatus(ROWOW_SKILL_PR_GLORIA));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_PR_IMPOSITIO)); // SkillImpositioManus: pre-renewal's is a StatusSkillImpl
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_PR_SUFFRAGIUM)); // SkillSuffragium: pre-renewal's is a StatusSkillImpl
    ROWOW->RegisterSkillImpl(new ROWOWSkillAspersio());
    ROWOW->RegisterSkillImpl(new ROWOWSkillRuwach());
    ROWOW->RegisterSkillImpl(new ROWOWSkillWarpPortal());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPneuma());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHolyLight());
    ROWOW->RegisterSkillImpl(new ROWOWSkillCure());
    ROWOW->RegisterSkillImpl(new ROWOWSkillHolyWater());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSanctuary());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBSSacramenti());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_PR_SLOWPOISON)); // SkillSlowPoison: the status alone
    ROWOW->RegisterSkillImpl(new ROWOWSkillStatusRecovery());
    ROWOW->RegisterSkillImpl(new ROWOWSkillLexDivina());
    ROWOW->RegisterSkillImpl(new ROWOWSkillTurnUndead());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_PR_LEXAETERNA)); // SkillLexAeterna: the status alone
    ROWOW->RegisterSkillImpl(new ROWOWSkillMagnusExorcismus());
    ROWOW->RegisterSkillImpl(new ROWOWSkillResurrection());
    ROWOW->RegisterSkillImpl(new ROWOWSkillRedemptio());
    ROWOW->RegisterSkillImpl(new ROWOWSkillAssumptio());
    ROWOW->RegisterSkillImpl(new ROWOWSkillBasilica());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSummonSpiritSphere());
    ROWOW->RegisterSkillImpl(new ROWOWSkillAbsorbSpiritSphere());
    ROWOW->RegisterSkillImpl(new ROWOWSkillTripleAttack());
    ROWOW->RegisterSkillImpl(new ROWOWSkillChainCombo());
    ROWOW->RegisterSkillImpl(new ROWOWSkillComboFinish());
    ROWOW->RegisterSkillImpl(new ROWOWSkillAsuraStrike());
    ROWOW->RegisterSkillImpl(new ROWOWSkillOccultImpaction());
    ROWOW->RegisterSkillImpl(new ROWOWSkillThrowSpiritSphere());
    ROWOW->RegisterSkillImpl(new ROWOWSkillSnap());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_MO_STEELBODY)); // SkillMentalStrength: the status alone
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_MO_BLADESTOP)); // SkillRoot (Blade Stop): its waiting status alone
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_MO_EXPLOSIONSPIRITS)); // SkillFury: the status alone
    ROWOW->RegisterSkillImpl(new ROWOWSkillKiTranslation());
    ROWOW->RegisterSkillImpl(new ROWOWSkillKiExplosion());
    ROWOW->RegisterSkillImpl(new ROWOWSkillZen());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPalmStrike());
    ROWOW->RegisterSkillImpl(new ROWOWSkillGlacierFist());
    ROWOW->RegisterSkillImpl(new ROWOWSkillChainCrushCombo());
}
