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
#include "Player.h"
#include "Unit.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// The Acolyte line's skills (rAthena's skills/acolyte, skill_factory_acolyte), one class per skill.  They run in the caster's map thread

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
}
