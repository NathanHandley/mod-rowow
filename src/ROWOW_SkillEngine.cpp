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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skill.cpp, unit.cpp, battle.cpp and status.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "CellImpl.h"
#include "Chat.h"
#include "Creature.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// The RO skill engine: rAthena's skill use with its function names (in the comments beside each and in ROWOW.h).  A cast starts in
// UnitSkillUse (unit_skilluse_id2's checks), takes UnitSkillCastTime (skill_castfix, the delays set at cast begin), and ends in
// SkillCastEndCheck and SkillCastEnd (skill_castend_id / skill_castend_pos: the costs, the after-cast delay and the cooldown, then the
// castend dispatch to the skill's ROWOWSkillImpl).  Everything runs in the map thread the caster and its targets share: a unit's skill
// state is on the unit, and the skill data and impl registry are only read after startup

namespace
{
    // The attack type and range checks of an added status (skill_additional_effect's and skill_counter_additional_effect's ATF_ flags): a
    // trigger with some of the types needs one of them, and with one range needs it
    bool IsAddEffectTriggered(uint8 flag, int32 attackType)
    {
        if ((flag & (ROWOW_ATF_WEAPON | ROWOW_ATF_MAGIC | ROWOW_ATF_MISC)) != (ROWOW_ATF_WEAPON | ROWOW_ATF_MAGIC | ROWOW_ATF_MISC))
        {
            // Trigger has attack type consideration.
            if (((flag & ROWOW_ATF_WEAPON) != 0 && (attackType & ROWOW_BF_WEAPON) != 0) || ((flag & ROWOW_ATF_MAGIC) != 0 && (attackType & ROWOW_BF_MAGIC) != 0) ||
                ((flag & ROWOW_ATF_MISC) != 0 && (attackType & ROWOW_BF_MISC) != 0))
                ;
            else
                return false;
        }
        if ((flag & (ROWOW_ATF_LONG | ROWOW_ATF_SHORT)) != (ROWOW_ATF_LONG | ROWOW_ATF_SHORT))
        {
            // Trigger has range consideration.
            if (((flag & ROWOW_ATF_LONG) != 0 && (attackType & ROWOW_BF_LONG) == 0) || ((flag & ROWOW_ATF_SHORT) != 0 && (attackType & ROWOW_BF_SHORT) == 0))
                return false; // Range Failed.
        }
        return true;
    }

    // dirx and diry: each RO direction's step in cells (0 north, then counter-clockwise)
    int32 const DirX[ROWOW_RO_DIRECTION_COUNT] = { 0, -1, -1, -1, 0, 1, 1, 1 };
    int32 const DirY[ROWOW_RO_DIRECTION_COUNT] = { 1, 1, 0, -1, -1, -1, 0, 1 };

    // Whether two names are the same but for case (the bonus types, as scripts write them)
    bool IsSameName(std::string const& a, char const* b)
    {
        size_t length = strlen(b);
        return a.size() == length && std::equal(a.begin(), a.end(), b, [](char x, char y) { return tolower((unsigned char)x) == tolower((unsigned char)y); });
    }

    // Yards an area search reaches past its cells, so the units of its cells standing above or below the origin (the search is 3D) are found
    constexpr float MapSearchHeightAllowance = 5.0f;

    // How high a knocked back unit may be pushed up or down from one cell to the next before it's a wall or a cliff (yards)
    constexpr float KnockbackMaxStepHeight = 1.5f;
    constexpr float KnockbackSpeed = 25.0f; // Yards a second: RO's knockback is a quick slide

    // The client's RO message for each skill failure (msgstringtable), as clif_skill_fail sends them
    char const* GetSkillFailText(int32 cause)
    {
        switch (cause)
        {
            case ROWOW_USESKILL_FAIL_SP_INSUFFICIENT: return "SP Insufficient";
            case ROWOW_USESKILL_FAIL_HP_INSUFFICIENT: return "HP Insufficient";
            case ROWOW_USESKILL_FAIL_STUFF_INSUFFICIENT: return "Insufficient materials.";
            case ROWOW_USESKILL_FAIL_SKILLINTERVAL: return "You can't use skills right after another (after-cast delay).";
            case ROWOW_USESKILL_FAIL_MONEY: return "Insufficient Zeny";
            case ROWOW_USESKILL_FAIL_THIS_WEAPON: return "This skill can't be used with this weapon.";
            case ROWOW_USESKILL_FAIL_REDJAMSTONE: return "Red Gemstone required.";
            case ROWOW_USESKILL_FAIL_BLUEJAMSTONE: return "Blue Gemstone required.";
            case ROWOW_USESKILL_FAIL_TOTARGET: return "This skill can't be used on that target.";
            case ROWOW_USESKILL_FAIL_HOLYWATER: return "Holy Water required.";
            case ROWOW_USESKILL_FAIL_CART: return "A Pushcart is required.";
            case ROWOW_USESKILL_FAIL_NEED_EQUIPMENT: return "The required equipment isn't worn.";
            case ROWOW_USESKILL_FAIL_SPIRITS: return "Spirit Spheres required.";
            case ROWOW_USESKILL_FAIL_COINS: return "Coins required.";
            case ROWOW_USESKILL_FAIL_NEED_MORE_BULLET: return "More ammunition is required.";
            case ROWOW_USESKILL_FAIL_NO_AMMO: return "Please equip the proper ammunition first.";
            default: return "Skill has failed.";
        }
    }
}

void ROWOWMod::RegisterSkillImpl(ROWOWSkillImpl* impl)
{
    SkillImplsBySkillID[impl->GetSkillID()].reset(impl);
}

ROWOWSkillImpl const* ROWOWMod::GetSkillImpl(uint16 skillID) const
{
    auto implIter = SkillImplsBySkillID.find(skillID);
    if (implIter == SkillImplsBySkillID.end())
        return nullptr;
    return implIter->second.get();
}

bool ROWOWMod::GetSkillOfSpell(uint32 spellID, uint16& skillIDOut, uint16& skillLevelOut) const
{
    auto skillIDIter = SkillIDsBySpellID.find(spellID);
    auto levelIter = SkillLevelsBySpellID.find(spellID);
    if (skillIDIter == SkillIDsBySpellID.end() || levelIter == SkillLevelsBySpellID.end())
        return false;
    skillIDOut = skillIDIter->second;
    skillLevelOut = levelIter->second;
    return true;
}

uint32 ROWOWMod::GetSkillSpellID(uint16 skillID, uint16 skillLevel) const
{
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr || skillLevel == 0 || skillLevel > skill->SpellIDsByLevel.size())
        return 0;
    return skill->SpellIDsByLevel[skillLevel - 1];
}

uint64 ROWOWMod::GetTick()
{
    return uint64(GameTime::GetGameTimeMS().count());
}

ROWOWSkillUnitData* ROWOWMod::GetSkillUnitData(Unit* unit) const
{
    return unit->CustomData.GetDefault<ROWOWSkillUnitData>("ROWOWSkillUnitData");
}

Unit* ROWOWMod::MapID2BL(Unit* origin, int32 id) const
{
    // map_id2bl for a unit's id kept in a status's value (its GUID's counter): a player on the origin's map, else an RO combatant within the
    // area around the origin (the partner of a Blade Stop, the target of a combo), or null
    if (id == 0 || origin->IsInWorld() == false)
        return nullptr;
    if (Player* player = ObjectAccessor::GetPlayer(origin->GetMap(), ObjectGuid::Create<HighGuid::Player>(uint32(id))))
        return player;
    Unit* found = nullptr;
    MapForEachInRange(origin, BattleConfig.AreaSize, [id, &found](Unit* bl)
    {
        if (found == nullptr && bl->IsPlayer() == false && int32(bl->GetGUID().GetCounter()) == id)
            found = bl;
        return 0;
    });
    return found;
}

void ROWOWMod::GetWorldCell(uint32 mapID, float worldX, float worldY, int32& xOut, int32& yOut) const
{
    // RO's x runs against WoW's Y and RO's y along WoW's X; off the RO maps the cells are laid from the map's origin
    if (ROWOWMapGrid const* mapGrid = GetMapGrid(mapID))
    {
        xOut = mapGrid->GetCellX(worldY);
        yOut = mapGrid->GetCellY(worldX);
        return;
    }
    xOut = int32(std::floor(-worldY / CellSize));
    yOut = int32(std::floor(worldX / CellSize));
}

void ROWOWMod::GetUnitCell(Unit const* unit, int32& xOut, int32& yOut) const
{
    GetWorldCell(unit->GetMapId(), unit->GetPositionX(), unit->GetPositionY(), xOut, yOut);
}

int32 ROWOWMod::DistanceBL(Unit const* src, Unit const* target) const
{
    // distance_bl: the larger of the x and y cell distances (no CIRCULAR_AREA)
    int32 srcX = 0, srcY = 0, targetX = 0, targetY = 0;
    GetUnitCell(src, srcX, srcY);
    GetUnitCell(target, targetX, targetY);
    return std::max(std::abs(targetX - srcX), std::abs(targetY - srcY));
}

int32 ROWOWMod::DistanceMathBL(Unit const* src, Unit const* target) const
{
    // distance_math_bl: the straight distance in cells, rounded down
    int32 srcX = 0, srcY = 0, targetX = 0, targetY = 0;
    GetUnitCell(src, srcX, srcY);
    GetUnitCell(target, targetX, targetY);
    return int32(std::sqrt(float((targetX - srcX) * (targetX - srcX) + (targetY - srcY) * (targetY - srcY))));
}

int32 ROWOWMod::SkillGetRange2(Unit const* unit, uint16 skillID, uint16 skillLevel, bool isServer) const
{
    // skill_get_range2: the skill's range with the attack range and Vulture's Eye, Snake Eye and Shadow Jump [Skotlex]
    ROWOWSkill const* skill = GetSkill(skillID);
    ROWOWStatus const* status = GetROStatus(unit);
    if (skill == nullptr || status == nullptr)
        return 0;
    int32 range = ROWOWSkill::GetLevelValue(skill->Range, skillLevel);
    if (range < 0)
    {
        if ((BattleConfig.SkillrangeFromWeapon & GetBattleObjectType(*status)) != 0)
            return status->Range;
        range *= -1;
    }
    if (isServer == true && range > 14)
        range = 14; // Server-sided base range can't be above 14
    if (unit->IsPlayer() == true)
    {
        if (skill->Inf2[ROWOW_INF2_ALTERRANGEVULTURE] == true)
            range += PCCheckSkill(unit, ROWOW_SKILL_AC_VULTURE);
        if (skill->Inf2[ROWOW_INF2_ALTERRANGESNAKEEYE] == true)
            range += PCCheckSkill(unit, ROWOW_SKILL_GS_SNAKEEYE); // added to allow GS skills to be effected by the range of Snake Eyes [Reddozen]
        if (skill->Inf2[ROWOW_INF2_ALTERRANGESHADOWJUMP] == true)
        {
            ROWOWSkill const* shadowJump = GetSkill(ROWOW_SKILL_NJ_SHADOWJUMP);
            range = shadowJump != nullptr ? shadowJump->Range[ROWOWSkill::GetLevelIndex(uint16(PCCheckSkill(unit, ROWOW_SKILL_NJ_SHADOWJUMP)))] : range;
        }
    }
    if (range == 0 && unit->IsPlayer() == false)
        return 9; // Enable non players to use self skills on others. [Skotlex]
    return range;
}

uint8 ROWOWMod::SkillGetCastType(uint16 skillID) const
{
    // skill_get_casttype
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr)
        return ROWOW_CAST_DAMAGE;
    if ((skill->Inf & ROWOW_INF_GROUND_SKILL) != 0)
        return ROWOW_CAST_GROUND;
    if ((skill->Inf & ROWOW_INF_SUPPORT_SKILL) != 0)
        return ROWOW_CAST_NODAMAGE;
    if ((skill->Inf & ROWOW_INF_SELF_SKILL) != 0)
    {
        if (skill->Inf2[ROWOW_INF2_NOTARGETSELF] == true)
            return ROWOW_CAST_DAMAGE; // Combo skill.
        return ROWOW_CAST_NODAMAGE;
    }
    if (skill->NK[ROWOW_NK_NODAMAGE] == true)
        return ROWOW_CAST_NODAMAGE;
    return ROWOW_CAST_DAMAGE;
}

int32 ROWOWMod::BattleCheckTarget(Unit const* src, Unit const* target, int32 flag) const
{
    // battle_check_target: 1 when the target is one of the flag's kinds (BCT_ENEMY, BCT_PARTY...), -1 when it isn't, 0 when it can't be
    // targeted at all.  RO combatants are players in the form and RO monsters: a monster is every player's natural enemy, monsters are
    // each other's friends, and players are enemies only where WoW lets them fight (rAthena's PvP map flags).  A monster a player summoned
    // (an Alchemist's plant) takes its master's side (battle_get_master).  (Slaves of monsters, skill units, the immunity statuses and
    // battlegrounds come with their systems)
    if (src == nullptr || target == nullptr)
        return 0;

    // t_bl/s_bl hold the 'master' of the attack, while src/target are the actual objects involved.
    Unit const* tBL = BattleGetMaster(target);
    Unit const* sBL = BattleGetMaster(src);

    // Can't hit self and master, but can hit other slaves
    if ((flag & ROWOW_BCT_WOS) != 0 && (src == target || sBL == target))
        return -1;
    if (GetROStatus(target) == nullptr || target->IsAlive() == false || target->IsInWorld() == false || src->GetMap() != target->GetMap())
        return 0;
    if (target->IsCreature() == true && target->ToCreature()->IsEvadingAttacks() == true)
        return 0;
    if (target->IsPlayer() == true && (target->ToPlayer()->IsGameMaster() == true || PCIsInvincible(target) == true) && src != target)
        return -1; // Cannot be targeted yet (invincible_timer, pc_isinvisible)

    int32 state = 0;
    bool stripEnemy = true; // Flag which marks whether to remove the BCT_ENEMY status if it's also friend/ally.
    if (ROWOWMobSummon const* summon = target->IsCreature() == true ? target->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon") : nullptr)
    {
        // Targettable by players: Marine Spheres and Floras (alchemist_summon_setting)
        if (((summon->AI == ROWOW_AI_SPHERE && (BattleConfig.AlchemistSummonSetting & 2) != 0) || (summon->AI == ROWOW_AI_FLORA && (BattleConfig.AlchemistSummonSetting & 1) != 0)) &&
            sBL->IsPlayer() == true && src->IsCreature() == false)
        {
            state |= ROWOW_BCT_ENEMY;
            stripEnemy = false;
        }
    }
    if (sBL->IsPlayer() == true)
    {
        if (tBL->IsPlayer() == false)
            state |= ROWOW_BCT_ENEMY; // Natural enemy.
    }
    else if ((target->IsCreature() == true && IsROFollower(target) == false) || tBL->IsCreature() == true)
        state |= ROWOW_BCT_PARTY; // Normal mobs with no ai are friends, and so are the monsters players summoned to them
    else
        state |= ROWOW_BCT_ENEMY; // However, all else are enemies.

    if ((flag & ROWOW_BCT_ALL) == ROWOW_BCT_ALL)
        return 1; // All actually stands for all attackable chars
    if (flag == ROWOW_BCT_NOONE)
        return -1;
    if (tBL == sBL)
    {
        // No need for further testing.
        state |= ROWOW_BCT_SELF | ROWOW_BCT_PARTY | ROWOW_BCT_GUILD;
        if ((state & ROWOW_BCT_ENEMY) != 0 && stripEnemy == true)
            state &= ~ROWOW_BCT_ENEMY;
        return (flag & state) != 0 ? 1 : -1;
    }

    Player const* srcPlayer = sBL->ToPlayer();
    Player const* targetPlayer = tBL->ToPlayer();
    bool isSameParty = srcPlayer != nullptr && targetPlayer != nullptr && srcPlayer->GetGroup() != nullptr && srcPlayer->GetGroup() == targetPlayer->GetGroup();
    bool isSameGuild = srcPlayer != nullptr && targetPlayer != nullptr && srcPlayer->GetGuildId() != 0 && srcPlayer->GetGuildId() == targetPlayer->GetGuildId();
    if (srcPlayer != nullptr && targetPlayer != nullptr && sBL->IsValidAttackTarget(tBL) == true)
    {
        // mapdata_flag_vs: check rivalry settings
        if ((flag & (ROWOW_BCT_PARTY | ROWOW_BCT_ENEMY)) != 0)
            state |= isSameParty == true ? ROWOW_BCT_PARTY : ROWOW_BCT_ENEMY;
        if ((flag & (ROWOW_BCT_GUILD | ROWOW_BCT_ENEMY)) != 0)
            state |= isSameGuild == true ? ROWOW_BCT_GUILD : ROWOW_BCT_ENEMY;
    }
    else
    {
        // Non pvp/gvg, check party/guild settings.
        if (((flag & ROWOW_BCT_PARTY) != 0 || (state & ROWOW_BCT_ENEMY) != 0) && isSameParty == true)
            state |= ROWOW_BCT_PARTY;
        if (((flag & ROWOW_BCT_GUILD) != 0 || (state & ROWOW_BCT_ENEMY) != 0) && isSameGuild == true)
            state |= ROWOW_BCT_GUILD;
    }
    if (state == 0)
        state = ROWOW_BCT_NEUTRAL;
    // Alliance state takes precedence over enemy one.
    else if ((state & ROWOW_BCT_ENEMY) != 0 && stripEnemy == true && (state & (ROWOW_BCT_SELF | ROWOW_BCT_PARTY | ROWOW_BCT_GUILD)) != 0)
        state &= ~ROWOW_BCT_ENEMY;
    return (flag & state) != 0 ? 1 : -1;
}

bool ROWOWMod::StatusCheckSkillUse(Unit const* src, Unit const* target, uint16 skillID, int32 flag) const
{
    // status_check_skilluse: whether the caster can use a skill (0 a normal attack) now, and on the target.  flag 0: the cast starts; 1: it
    // ends; 2: a target of an area.  A target must be in the world, and alive past the cast's start (the castend and area checks) but for the
    // skills that revive (rAthena leaves death to skill.cpp).  (The item skills, the wings, don't come through here)
    ROWOWStatus const* status = src != nullptr ? GetROStatus(src) : nullptr;
    if (src != nullptr)
    {
        if (src->IsInWorld() == false || (src->IsPlayer() == false && src->IsAlive() == false))
            return false;
    }
    // Without a caster (a ground skill's unit with skill_caster_check off) rAthena's dummy status has every mode
    uint32 mode = status != nullptr ? status->Mode : std::numeric_limits<uint32>::max();
    ROWOWStatusChange const* sc = src != nullptr ? GetStatusChange(src) : nullptr;
    ROWOWStatusChange const* tsc = target != nullptr ? GetStatusChange(target) : nullptr;
    if (sc != nullptr && sc->IsEmpty() == true && sc->Option == 0)
        sc = nullptr;
    if (tsc != nullptr && tsc->IsEmpty() == true && tsc->Option == 0)
        tsc = nullptr;

    if (skillID == 0)
    {
        // Normal attack checks.
        if (sc != nullptr && sc->Cant.Attack == true)
            return false;
        // This mode is only needed for melee attacking.
        if ((mode & ROWOW_MODE_CANATTACK) == 0)
            return false;
        // Dead state is not checked for skills as some skills can be used on dead characters, said checks are left to skill.cpp [Skotlex]
        if (target != nullptr && target->IsAlive() == false)
            return false;
    }

    if (skillID == ROWOW_SKILL_PA_PRESSURE && flag != 0 && tsc != nullptr && (tsc->Option & ROWOW_OPTION_HIDE) != 0)
        return false; // Gloria Avoids pretty much everything....
    if (int32 targetFollowerType = GetFollowerType(target); targetFollowerType != 0 && skillID != 0)
    {
        ROWOWSkill const* targetSkill = GetSkill(skillID);
        if (targetFollowerType == ROWOW_BL_HOM && (BattleConfig.HomSetting & ROWOW_HOMSET_NO_SUPPORT_SKILL) != 0 && targetSkill != nullptr && (targetSkill->Inf & ROWOW_INF_SUPPORT_SKILL) != 0 &&
            BattleGetMaster(target) != src)
            return false; // Can't use support skills on Homunculus (only Master/Self)
        if (targetFollowerType == ROWOW_BL_MER && (skillID == ROWOW_SKILL_PR_ASPERSIO || (skillID >= ROWOW_SKILL_SA_FLAMELAUNCHER && skillID <= ROWOW_SKILL_SA_SEISMICWEAPON)) && BattleGetMaster(target) != src)
            return false; // Can't use Weapon endow skills on Mercenary (only Master)
        if (skillID == ROWOW_SKILL_AM_POTIONPITCHER && targetFollowerType == ROWOW_BL_MER)
            return false; // Can't use Potion Pitcher on Mercenaries
    }
    if (skillID == ROWOW_SKILL_AL_TELEPORT && src != nullptr && (mode & ROWOW_MODE_STATUS_IMMUNE) == 0)
    {
        // Should fail when used on top of Land Protector [Skotlex] (but cast as a skill item)
        int32 x = 0, y = 0;
        GetUnitCell(src, x, y);
        ROWOWSkillUnitData const* ud = src->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
        if (MapGetCellLandProtector(src->GetMap(), x, y) == true && (src->IsPlayer() == false || ud == nullptr || ud->SkillItemID != skillID))
            return false;
    }

    if (sc != nullptr)
    {
        if (skillID > 0 && sc->Opt1 != 0 && sc->Opt1 != ROWOW_OPT1_STONEWAIT && sc->Opt1 != ROWOW_OPT1_BURNING)
        {
            // Stuned/Frozen/etc
            if (flag != 1) // Can't cast, casted stuff can't damage.
                return false;
            if (SkillGetCastType(skillID) == ROWOW_CAST_DAMAGE)
                return false; // Damage spells stop casting.
        }

        if ((sc->HasSCE(ROWOW_SC_TRICKDEAD) == true && skillID != ROWOW_SKILL_NV_TRICKDEAD) || (sc->HasSCE(ROWOW_SC_AUTOCOUNTER) == true && flag == 0 && skillID != 0) ||
            (sc->HasSCE(ROWOW_SC_GOSPEL) == true && sc->GetSCE(ROWOW_SC_GOSPEL)->Val4 == ROWOW_BCT_SELF && skillID != ROWOW_SKILL_PA_GOSPEL))
            return false;

        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_BLADESTOP))
        {
            // Blade Stop's level lets a Monk use more of its skills while holding the blade
            int32 level = sce->Val1;
            bool allowed = (level >= 5 && skillID == ROWOW_SKILL_MO_EXTREMITYFIST) || (level >= 4 && skillID == ROWOW_SKILL_MO_CHAINCOMBO) ||
                (level >= 3 && skillID == ROWOW_SKILL_MO_INVESTIGATE) || (level >= 2 && skillID == ROWOW_SKILL_MO_FINGEROFFENSIVE);
            if (allowed == false)
                return false;
        }

        // Only do skill specific checks here, normal attacks should have been checked already (additionally the skill db lookup would fail)
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_DANCING); sce != nullptr && flag != 2 && skillID != 0)
        {
            ROWOWSkill const* skill = GetSkill(skillID);
            if (skill == nullptr)
                return false;
            if (sc->HasSCE(ROWOW_SC_LONGING) == true)
            {
                // Allow everything except dancing/re-dancing. [Skotlex]
                if (skillID == ROWOW_SKILL_BD_ENCORE || skill->Inf2[ROWOW_INF2_ISSONG] == true || skill->Inf2[ROWOW_INF2_ISENSEMBLE] == true)
                    return false;
            }
            else if (skill->Inf2[ROWOW_INF2_ALLOWWHENPERFORMING] == false) // Skills that can be used in dancing state
                return false;
            if ((sce->Val1 & 0xFFFF) == ROWOW_SKILL_CG_HERMODE && skillID == ROWOW_SKILL_BD_ADAPTATION)
                return false; // Can't amp out of Wand of Hermode :/ [Skotlex]
        }

        if (skillID != 0)
        {
            // Skills blocked through status changes...  Blocked only from using the skill (stuff like autospell may still go through
            ROWOWStatusChangeEntry const* basilica = sc->GetSCE(ROWOW_SC_BASILICA);
            if (flag == 0 && (sc->Cant.Cast == true ||
                (basilica != nullptr && (basilica->Val4 != int32(src->GetGUID().GetCounter()) || skillID != ROWOW_SKILL_HP_BASILICA)) || // Only Basilica caster that can cast, and only Basilica to cancel it
                (sc->HasSCE(ROWOW_SC_MARIONETTE) == true && skillID != ROWOW_SKILL_CG_MARIONETTE) || // Only skill you can use is marionette again to cancel it
                (sc->HasSCE(ROWOW_SC_MARIONETTE2) == true && skillID == ROWOW_SKILL_CG_MARIONETTE) || // Cannot use marionette if you are being buffed by another
                (sc->HasSCE(ROWOW_SC_ANKLE) == true && skillID == ROWOW_SKILL_AL_TELEPORT))) // skill_block_check
                return false;

            // Skill blocking.
            ROWOWSkill const* skill = GetSkill(skillID);
            ROWOWStatusChangeEntry const* noChat = sc->GetSCE(ROWOW_SC_NOCHAT);
            if ((sc->HasSCE(ROWOW_SC_VOLCANO) == true && skillID == ROWOW_SKILL_WZ_ICEWALL) || (sc->HasSCE(ROWOW_SC_ROKISWEIL) == true && skillID != ROWOW_SKILL_BD_ADAPTATION) ||
                (sc->HasSCE(ROWOW_SC_HERMODE) == true && skill != nullptr && (skill->Inf & ROWOW_INF_SUPPORT_SKILL) != 0) ||
                (noChat != nullptr && (noChat->Val1 & 0x02) != 0)) // MANNER_NOSKILL
                return false;
        }

        if (sc->Option != 0)
        {
            // We do not check it for non-players here as hide will not stop monsters from scanning for new targets and use skills.  The logic
            // that normal attacks will not actually be executed when hidden needs to be put in the AI code instead
            ROWOWSkill const* skill = GetSkill(skillID);
            if ((sc->Option & ROWOW_OPTION_HIDE) != 0 && src->IsPlayer() == true && (skillID == 0 || skill == nullptr || skill->Inf2[ROWOW_INF2_ALLOWWHENHIDDEN] == false))
                return false;
            if ((sc->Option & ROWOW_OPTION_CHASEWALK) != 0 && skillID != ROWOW_SKILL_ST_CHASEWALK)
                return false;
        }
    }

    if (target == nullptr || target == src) // No further checking needed.
        return true;
    if (target->IsInWorld() == false || (flag != 0 && target->IsAlive() == false && skillID != ROWOW_SKILL_ALL_RESURRECTION && skillID != ROWOW_SKILL_PR_REDEMPTIO))
        return false;

    if (tsc != nullptr)
    {
        if (skillID == 0 && tsc->HasSCE(ROWOW_SC_TRICKDEAD) == true)
            return false;
        if ((skillID == ROWOW_SKILL_WZ_STORMGUST || skillID == ROWOW_SKILL_WZ_FROSTNOVA || skillID == ROWOW_SKILL_NJ_HYOUSYOURAKU || skillID == ROWOW_SKILL_NPC_STORMGUST2) &&
            tsc->HasSCE(ROWOW_SC_FREEZE) == true)
            return false;
        if (skillID == ROWOW_SKILL_PR_LEXAETERNA && (tsc->HasSCE(ROWOW_SC_FREEZE) == true || tsc->HasSCE(ROWOW_SC_STONE) == true))
            return false;
    }

    // If targetting, cloak+hide protect you, otherwise only hiding does.
    uint32 hideFlag = flag != 0 ? ROWOW_OPTION_HIDE : (ROWOW_OPTION_HIDE | ROWOW_OPTION_CLOAK | ROWOW_OPTION_CHASEWALK);
    // Skill that can hit hidden target
    if (ROWOWSkill const* skill = GetSkill(skillID); skill != nullptr && skill->Inf2[ROWOW_INF2_TARGETHIDDEN] == true)
        hideFlag &= ~ROWOW_OPTION_HIDE;
    if (tsc != nullptr && (tsc->Option & hideFlag) != 0)
    {
        // A player hides from all but bosses and detectors (no perfect hiding before the third jobs); anything else from all but detectors
        bool isBoss = status != nullptr && status->Class == ROWOW_CLASS_BOSS;
        bool isDetect = (mode & ROWOW_MODE_DETECTOR) != 0;
        if (target->IsPlayer() == true ? (isBoss == false && isDetect == false) : isDetect == false)
            return false;
    }
    return true;
}

int32 ROWOWMod::MapForEachInArea(Unit* origin, int32 x0, int32 y0, int32 x1, int32 y1, std::function<int32(Unit*)> const& func) const
{
    // map_foreachinarea over the RO combatants (BL_CHAR) of the origin's map whose cell is in the square x0,y0 - x1,y1: the units within
    // reach of it in yards, then those standing in it in RO cells
    int32 originX = 0, originY = 0;
    GetUnitCell(origin, originX, originY);
    int32 reachCells = std::max({ std::abs(x0 - originX), std::abs(x1 - originX), std::abs(y0 - originY), std::abs(y1 - originY) }) + 1;
    ROWOWMapGrid const* mapGrid = GetMapGrid(origin->GetMapId());
    float searchRadius = float(reachCells) * (mapGrid != nullptr ? mapGrid->CellSize : CellSize) * 1.5f + MapSearchHeightAllowance;
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(origin, searchRadius);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(origin, units, check);
    Cell::VisitObjects(origin, searcher, searchRadius);
    int32 returnCount = 0;
    for (Unit* unit : units)
    {
        if (unit->IsInWorld() == false || IsROCombatant(unit) == false) // Not a pet, which has a status to walk by
            continue;
        int32 unitX = 0, unitY = 0;
        GetUnitCell(unit, unitX, unitY);
        if (unitX < std::min(x0, x1) || unitX > std::max(x0, x1) || unitY < std::min(y0, y1) || unitY > std::max(y0, y1))
            continue;
        returnCount += func(unit);
    }
    return returnCount;
}

int32 ROWOWMod::MapForEachInRange(Unit* center, int32 range, std::function<int32(Unit*)> const& func) const
{
    // map_foreachinallrange: the RO combatants within range cells of the center's cell
    int32 centerX = 0, centerY = 0;
    GetUnitCell(center, centerX, centerY);
    return MapForEachInArea(center, centerX - range, centerY - range, centerX + range, centerY + range, func);
}

int32 ROWOWMod::MapForEachInDir(Unit* origin, int32 x0, int32 y0, int32 x1, int32 y1, int32 range, int32 length, int32 offset,
    std::function<int32(Unit*)> const& func) const
{
    // map_foreachindir: the RO combatants on a path going from x0,y0 into the one of the eight directions toward x1,y1: range cells to each
    // side (its width range*2+1), length cells long, moved offset cells along (half-cells for a diagonal path).  The cost of a diagonal step is
    // that of a straight one.  A unit out of sight of the origin (path_search_long's CELL_CHKWALL) isn't on it
    if (range < 0 || length < 1 || offset < 0)
        return 0;
    uint8 dir = GetRODirection(float(x1 - x0), float(y1 - y0)); // map_calc_dir_xy, east on the same cell
    int32 dx = DirX[dir];
    int32 dy = DirY[dir];
    bool isDiagonal = (dir % 2) != 0; // direction_diagonal

    // Special offset handling for diagonal paths
    if (offset != 0 && isDiagonal == true)
    {
        // So that diagonal paths can attach to each other, we have to work with half-tile offsets
        offset = (2 * offset) - 1;
        // To get the half-tiles we need to increase length by one
        length++;
    }

    // Get area that needs to be checked
    int32 mx0 = x0 + dx * (offset / ((dir % 2) + 1));
    int32 my0 = y0 + dy * (offset / ((dir % 2) + 1));
    int32 mx1 = x0 + dx * (offset / ((dir % 2) + 1) + length - 1);
    int32 my1 = y0 + dy * (offset / ((dir % 2) + 1) + length - 1);
    if (mx0 > mx1)
        std::swap(mx0, mx1);
    if (my0 > my1)
        std::swap(my0, my1);

    // Apply width to the path by turning 90 degrees
    mx0 -= std::abs(range * DirX[(dir + 2) % ROWOW_RO_DIRECTION_COUNT]);
    my0 -= std::abs(range * DirY[(dir + 2) % ROWOW_RO_DIRECTION_COUNT]);
    mx1 += std::abs(range * DirX[(dir + 2) % ROWOW_RO_DIRECTION_COUNT]);
    my1 += std::abs(range * DirY[(dir + 2) % ROWOW_RO_DIRECTION_COUNT]);
    return MapForEachInArea(origin, mx0, my0, mx1, my1, [this, origin, x0, y0, dx, dy, isDiagonal, range, length, offset, &func](Unit* bl)
    {
        // What matters now is the relative x and y from the start point
        int32 rx = 0, ry = 0;
        GetUnitCell(bl, rx, ry);
        rx -= x0;
        ry -= y0;
        // Do not hit source cell
        if (BattleConfig.SkillEightpathSameCell == 0 && rx == 0 && ry == 0)
            return 0;
        // This turns it so that the area that is hit is always with positive rx and ry
        rx *= dx;
        ry *= dy;
        // These checks only need to be done for diagonal paths
        if (isDiagonal == true)
        {
            // Check for length, then for width
            if ((rx + ry < offset) || (rx + ry > 2 * (length + (offset / 2) - 1)))
                return 0;
            if (std::abs(rx - ry) > 2 * range)
                return 0;
        }
        // Everything else ok, check for line of sight from source
        if (bl != origin && origin->IsWithinLOSInMap(bl) == false)
            return 0;
        return func(bl);
    });
}

int32 ROWOWMod::SkillAreaSub(Unit* bl, Unit* src, uint16 skillID, uint16 skillLevel, uint64 tick, int32 flag, ROWOWSkillAreaFunc func)
{
    // skill_area_sub: each unit an area search finds that the flag's target rules accept.  (No pre-renewal skill sends the preamble packet)
    if (BattleCheckTarget(src, bl, flag) <= 0)
        return 0;
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    if ((flag & ROWOW_SD_SPLASH) != 0)
        ud->AreaTemp.Values[2]++;
    switch (func)
    {
        case ROWOWSkillAreaFunc::CastendDamageID: return SkillCastendDamageID(src, bl, skillID, skillLevel, tick, flag);
        case ROWOWSkillAreaFunc::CastendNoDamageID: return SkillCastendNoDamageID(src, bl, skillID, skillLevel, tick, flag);
        case ROWOWSkillAreaFunc::Count: return 1; // skill_area_sub_count
    }
    return 0;
}

int32 ROWOWMod::GetDeferredBonus(Unit const* unit, char const* type) const
{
    ROWOWStatus const* status = GetROStatus(unit);
    if (status == nullptr || status->IsPlayer == false)
        return 0;
    int32 total = 0;
    for (ROWOWDeferredBonus const& bonus : status->Bonus.Deferred)
        if (bonus.Values.empty() == false && IsSameName(bonus.Type, type) == true)
            total += int32(bonus.Values.back());
    return total;
}

int32 ROWOWMod::GetDeferredBonus2(Unit const* unit, char const* type, int64 key) const
{
    ROWOWStatus const* status = GetROStatus(unit);
    if (status == nullptr || status->IsPlayer == false)
        return 0;
    int32 total = 0;
    for (ROWOWDeferredBonus const& bonus : status->Bonus.Deferred)
        if (bonus.Values.size() >= 2 && bonus.Values[0] == key && IsSameName(bonus.Type, type) == true)
            total += int32(bonus.Values[1]);
    return total;
}

bool ROWOWMod::HasDeferredBonusKey(Unit const* unit, char const* type, int64 key) const
{
    ROWOWStatus const* status = GetROStatus(unit);
    if (status == nullptr || status->IsPlayer == false)
        return false;
    for (ROWOWDeferredBonus const& bonus : status->Bonus.Deferred)
        if (bonus.Values.empty() == false && bonus.Values[0] == key && IsSameName(bonus.Type, type) == true)
            return true;
    return false;
}

void ROWOWMod::ClifSkillFail(Player* sd, uint16 skillID, int32 cause, int32 btype, uint32 itemID) const
{
    // clif_skill_fail: RO's failure message.  display_skill_fail 2 hides nothing a player casts.  (Needing an item names it)
    if (sd == nullptr || cause == ROWOW_USESKILL_FAIL_MAX)
        return;
    std::string text = GetSkillFailText(cause);
    if (cause == ROWOW_USESKILL_FAIL_NEED_ITEM)
    {
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
        ItemTemplate const* itemTemplate = itemTemplateIDIter != ItemTemplateIDsByItemID.end() ? sObjectMgr->GetItemTemplate(itemTemplateIDIter->second) : nullptr;
        text = fmt::format("[{}] required '{}' amount.", itemTemplate != nullptr ? itemTemplate->Name1 : std::to_string(itemID), btype);
    }
    else if (cause == ROWOW_USESKILL_FAIL_LEVEL && skillID == ROWOW_SKILL_TF_STEAL)
        text = "Steal failed.";
    ChatHandler(sd->GetSession()).SendNotification("{}", text);
}

void ROWOWMod::UnitSetAttackDelay(Unit* unit, uint64 tick, bool isCastBegin)
{
    // unit_set_attackdelay, its act delay: a player's normal attack and the start of its casts hold the next skill back by its attack motion
    // plus the shortest motion (the attack delay itself is WoW's swing timer).  Monsters get theirs at cast end and cancel (MobSetDelay)
    Player* sd = unit->ToPlayer();
    ROWOWStatus const* status = GetROStatus(unit);
    int32 followerType = GetFollowerType(unit);
    if ((sd == nullptr && followerType == 0) || status == nullptr)
        return;
    ROWOWSkillUnitData* ud = GetSkillUnitData(unit);
    if (isCastBegin == true && ud->SkillID == 0)
        return;
    int64 actDelay = status->AttackMotion + BattleConfig.MaxAspdAmotion / 2; // pc_maxaspd / AMOTION_DIVIDER_PC
    if (followerType != 0)
    {
        // A homunculus's and mercenary's normal attack delay is its attack delay, WoW's swing timer.  For non-PCs that can be controlled from the client, there is a security delay of
        // 200ms at a cast's start; to prevent tricks to use skills faster, a config uses amotion instead.  A mercenary's ground skills go as a player's
        if (isCastBegin == false)
            return;
        ROWOWSkill const* skill = GetSkill(ud->SkillID);
        bool isGround = skill != nullptr && (skill->Inf & ROWOW_INF_GROUND_SKILL) != 0;
        if (followerType == ROWOW_BL_HOM || isGround == false)
            actDelay = BattleConfig.AmotionMinSkillDelay == 1 ? status->AttackMotion + ROWOW_MAX_ASPD_NOPC : ROWOW_MIN_DELAY_SLAVE;
        else
            actDelay = status->AttackMotion + ROWOW_MAX_ASPD_NOPC;
    }
    ud->CanActTime = std::max<uint64>(tick + uint64(actDelay), ud->CanActTime);
}

void ROWOWMod::SendSkillDelayToClient(Unit* unit, uint16 skillID) const
{
    // What clif_status_change(EFST_POSTDELAY) shows in RO: the action bars hold every skill for the after-cast delay, and this skill's
    // levels for its cooldown (a cooldown on the client only, which the server's own checks match)
    Player* player = unit->ToPlayer();
    if (player == nullptr)
        return;
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    ROWOWSkillUnitData const* ud = unit->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
    if (identity == nullptr || ud == nullptr)
        return;
    uint64 tick = GetTick();
    uint64 delay = ud->CanActTime > tick ? ud->CanActTime - tick : 0;
    auto cooldownIter = ud->CooldownEndTimesBySkillID.find(skillID);
    uint64 cooldown = (cooldownIter != ud->CooldownEndTimesBySkillID.end() && cooldownIter->second > tick) ? cooldownIter->second - tick : 0;
    std::vector<std::pair<uint32, uint32>> cooldownsBySpellID;
    for (uint32 spellID : identity->SkillSpellIDs)
    {
        uint16 spellSkillID = 0, spellSkillLevel = 0;
        if (GetSkillOfSpell(spellID, spellSkillID, spellSkillLevel) == false)
            continue;
        uint64 spellCooldown = std::max(delay, spellSkillID == skillID ? cooldown : 0);
        if (spellCooldown > 0)
            cooldownsBySpellID.emplace_back(spellID, uint32(spellCooldown));
    }
    if (cooldownsBySpellID.empty() == true)
        return;
    WorldPacket data(SMSG_SPELL_COOLDOWN, 8 + 1 + cooldownsBySpellID.size() * 8);
    data << player->GetGUID();
    data << uint8(0); // SPELL_COOLDOWN_FLAG_NONE
    for (auto const& cooldownBySpellID : cooldownsBySpellID)
    {
        data << uint32(cooldownBySpellID.first);
        data << uint32(cooldownBySpellID.second);
    }
    player->SendDirectMessage(&data);
}

SpellCastResult ROWOWMod::UnitSkillUse(Unit* src, Unit* target, WorldLocation const* dest, uint16 skillID, uint16 skillLevel)
{
    // clif_parse_UseSkillToId and unit_skilluse_id2 (or unit_skilluse_pos2, target being the caster and dest the ground spot), up to the
    // cast time: whether the skill may start.  It has no side effects; UnitSkillCastTime starts it.  (The other jobs' checks of
    // unit_skilluse_id2's switch come with their skills)
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr || GetROStatus(src) == nullptr)
        return SPELL_FAILED_DONT_REPORT;
    if (src->IsAlive() == false)
        return SPELL_FAILED_CASTER_DEAD; // Do not continue source is dead
    if (target == nullptr)
        target = src;
    Player* sd = src->ToPlayer();
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    uint64 tick = GetTick();
    if (sd != nullptr)
    {
        // never trust client: a skill item (Abracadabra's pick) is cast at its own level, other skills only as learned
        if (ud->SkillItemID == skillID)
            skillLevel = ud->SkillItemLevel;
        else if (PCCheckSkill(sd, skillID) < skillLevel)
            return SPELL_FAILED_NOT_KNOWN;

        // clif_parse_UseSkillToId's pc_cant_act: a trade holds the items a skill uses up
        if (PCCantAct(sd) == true)
            return SPELL_FAILED_DONT_REPORT;

        // skill_isNotOk: the map's rules, and the skill's cooldown (blockskill)
        if (SkillIsNotOk(sd, skillID) == true)
            return SPELL_FAILED_DONT_REPORT;
        auto cooldownIter = ud->CooldownEndTimesBySkillID.find(skillID);
        if (cooldownIter != ud->CooldownEndTimesBySkillID.end() && cooldownIter->second > tick)
        {
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_SKILLINTERVAL);
            return SPELL_FAILED_DONT_REPORT;
        }

        // canact_tick: no skill before the after-cast delay ends, but a skill item and the dance Encore starts (not a client's request).
        // While a cast runs only Cast Cancel can be used, which remembers the skill it stops (skill_id_old)
        if (ud->SkillID != 0 && skillID == ROWOW_SKILL_SA_CASTCANCEL)
        {
            ud->SkillIDOld = ud->SkillID;
            ud->SkillLevelOld = ud->SkillLevel;
        }
        else if (tick < ud->CanActTime && ud->SkillItemID != skillID && ud->EncoreSkillID != skillID)
        {
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_SKILLINTERVAL);
            return SPELL_FAILED_DONT_REPORT;
        }

        // Record the status of the previous skill
        switch (skillID)
        {
            case ROWOW_SKILL_BD_ENCORE:
                // Prevent using the dance skill if you no longer have the skill in your tree.
                if (ud->SkillIDDance == 0 || PCCheckSkill(sd, ud->SkillIDDance) <= 0)
                {
                    ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                    return SPELL_FAILED_DONT_REPORT;
                }
                ud->SkillIDOld = skillID;
                break;
            case ROWOW_SKILL_BA_PANGVOICE:
            case ROWOW_SKILL_DC_WINKCHARM:
                if (GetROStatus(target) != nullptr && GetROStatus(target)->Class == ROWOW_CLASS_BOSS)
                {
                    ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_TOTARGET);
                    return SPELL_FAILED_DONT_REPORT;
                }
                break;
        }
    }
    if (skill->Inf2[ROWOW_INF2_NOTARGETSELF] == true && src == target)
        return SPELL_FAILED_BAD_TARGETS;
    if (StatusCheckSkillUse(src, target, skillID, 0) == false)
        return SPELL_FAILED_BAD_TARGETS;
    if (sd != nullptr && SkillCheckConditionCastBegin(sd, skillID, skillLevel) == false)
        return SPELL_FAILED_DONT_REPORT;
    if (sd != nullptr && skillID == ROWOW_SKILL_CR_DEVOTION && target->IsPlayer() == true &&
        PCDevotionSlot(sd, target->GetGUID(), std::min<int32>(skillLevel, ROWOW_MAX_DEVOTION)) < 0)
    {
        // Devotion needs a free slot of the level's (or the target's own): no free slots, skill Fail
        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
        return SPELL_FAILED_DONT_REPORT;
    }
    if (skillID == ROWOW_SKILL_ALL_RESURRECTION)
    {
        // Resurrection is cast on the dead, or (offensively) on the undead
        ROWOWStatus const* tstatus = GetROStatus(target);
        if (tstatus == nullptr || (BattleCheckUndead(tstatus->Race, tstatus->DefElement) == false && target->IsAlive() == true))
            return SPELL_FAILED_TARGET_NOT_DEAD; // Can't cast on non-dead characters.
    }
    if (sd != nullptr && skillID == ROWOW_SKILL_MG_FIREWALL && dest != nullptr)
    {
        // Special check for Firewall only (unit_skilluse_pos2)
        int32 x = 0, y = 0;
        GetWorldCell(src->GetMapId(), dest->GetPositionX(), dest->GetPositionY(), x, y);
        if (SkillPosMaxcountCheck(src, x, y, skillID, skillLevel, true) == false)
            return SPELL_FAILED_DONT_REPORT;
    }

    // A cast holds a player still, but with Free Cast (unit_walktoxy refuses a walk while the skill timer runs): a walking player can't start
    // one.  (The skill spells don't stop on WoW's movement; UpdateSkillCastMove stops them)
    ROWOWMobSkill const* mobSkill = GetMobSkillInUse(src);
    int32 casttime = UnitSkillCastTimeFix(src, target, skillID, skillLevel, mobSkill != nullptr ? mobSkill->CastTime : SkillCastFix(src, skillID, skillLevel));
    if (sd != nullptr && sd->isMoving() == true && PCCheckSkill(sd, ROWOW_SKILL_SA_FREECAST) <= 0 && casttime > 0)
        return SPELL_FAILED_MOVING;

    // Any action ends the invincibility after arriving (pc_delinvincibletimer)
    if (sd != nullptr)
        PCDelInvincibleTimer(sd);

    // An instant skill ends at once (skill_castend_id from unit_skilluse_id2), so its castend checks are part of starting it
    if (casttime <= 0)
        return SkillCastEndCheck(src, target, dest, skillID, skillLevel);
    return SPELL_CAST_OK;
}

Unit* ROWOWMod::UnitSkillTarget(Unit* src, Unit* target, Unit* pickedTarget, uint16 skillID)
{
    // unit_skilluse_id2's target: a combo skill used in its combo goes at the combo's target (val2) or, as it auto-targets (val2 0), at what the
    // caster attacks (ud->target: the unit it swings at, else the one the player picked, the spell's own target); a self skill that can't
    // target itself (a combo's) at that too; Chain Combo under Blade Stop at the blade's other holder.  (A combo whose castbegin isn't met falls
    // to the same target, and castbegin refuses it after)
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr)
        return target;
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
    if (sc != nullptr && sc->IsEmpty() == true)
        sc = nullptr; // Unneeded
    Unit* attackTarget = src->GetVictim();
    if (attackTarget == nullptr && pickedTarget != src)
        attackTarget = pickedTarget;
    ROWOWStatusChangeEntry const* combo = sc != nullptr ? sc->GetSCE(ROWOW_SC_COMBO) : nullptr;
    if (combo != nullptr && SkillIsCombo(skillID) != 0)
    {
        // temp: used to signal combo-skills right now.
        if (combo->Val2 != 0)
            target = MapID2BL(src, combo->Val2);
        else if (target == src || attackTarget != nullptr)
            target = attackTarget;
        if ((skill->Inf & ROWOW_INF_SELF_SKILL) != 0 && skill->NK[ROWOW_NK_NODAMAGE] == true) // exploit fix
            target = src;
    }
    else if (target == src && (skill->Inf & ROWOW_INF_SELF_SKILL) != 0 && skill->Inf2[ROWOW_INF2_NOTARGETSELF] == true)
        target = attackTarget; // Auto-select target. [Skotlex]

    // Homun-auto-target skills.
    if (GetFollowerType(src) == ROWOW_BL_HOM && (skillID == ROWOW_SKILL_HLIF_HEAL || skillID == ROWOW_SKILL_HLIF_AVOID || skillID == ROWOW_SKILL_HAMI_DEFENCE || skillID == ROWOW_SKILL_HAMI_CASTLE))
        if (Player* master = GetFollowerMaster(src))
            return master;

    // Check for skills that auto-select target
    if (src->IsPlayer() == true && skillID == ROWOW_SKILL_MO_CHAINCOMBO && sc != nullptr)
        if (ROWOWStatusChangeEntry const* bladeStop = sc->GetSCE(ROWOW_SC_BLADESTOP))
            target = MapID2BL(src, bladeStop->Val4);
    return target != nullptr ? target : src;
}

int32 ROWOWMod::UnitSkillCastTimeFix(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 casttime)
{
    // unit_skilluse_id2's cast time switch, on skill_castfix's time: -1 casts at once
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
    switch (skillID)
    {
        case ROWOW_SKILL_MO_FINGEROFFENSIVE:
            // Each sphere thrown takes its own cast time
            if (src->IsPlayer() == true)
                casttime += casttime * std::min<int32>(skillLevel, PCGetSpiritBall(src));
            break;
        case ROWOW_SKILL_MO_EXTREMITYFIST:
            // At once from Combo Finish or Chain Crush Combo
            if (ROWOWStatusChangeEntry const* combo = sc != nullptr ? sc->GetSCE(ROWOW_SC_COMBO) : nullptr; combo != nullptr &&
                (combo->Val1 == ROWOW_SKILL_MO_COMBOFINISH || combo->Val1 == ROWOW_SKILL_CH_CHAINCRUSH))
                casttime = -1;
            break;
        case ROWOW_SKILL_HP_BASILICA:
            if (sc != nullptr && sc->HasSCE(ROWOW_SC_BASILICA) == true)
                casttime = -1; // No Casting time on basilica cancel
            break;
        case ROWOW_SKILL_ST_CHASEWALK:
            if (sc != nullptr && sc->HasSCE(ROWOW_SC_CHASEWALK) == true)
                casttime = -1; // Coming out of Chase Walk is at once
            break;
        case ROWOW_SKILL_TK_RUN:
            if (sc != nullptr && sc->HasSCE(ROWOW_SC_RUN) == true)
                casttime = -1; // Stopping a run is at once
            break;
        case ROWOW_SKILL_KN_CHARGEATK:
        {
            // Range 0-3: 500ms, Range 4-6: 1000ms, Range 7+: 1500ms (distance_math_bl: the straight distance in cells)
            if (target == nullptr)
                break;
            int32 k = DistanceMathBL(src, target);
            k = std::clamp((k - 1) / 3, 0, 2);
            casttime += casttime * k;
            break;
        }
    }
    return casttime;
}

int32 ROWOWMod::UnitSkillCastTime(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
{
    // The rest of unit_skilluse_id2: the cast time (skill_castfix, its skill switch, then skill_castfix_sc), the attack and act delays set at
    // cast begin, the cast's cancel flag, and the skill remembered as the one being cast.  A non-combo skill stops the normal attack (a combo
    // goes on swinging at its target)
    ROWOWSkill const* skill = GetSkill(skillID);
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    uint64 tick = GetTick();
    bool isCombo = (GetSCE(src, ROWOW_SC_COMBO) != nullptr && SkillIsCombo(skillID) != 0) ||
        (skill != nullptr && (skill->Inf & ROWOW_INF_SELF_SKILL) != 0 && skill->Inf2[ROWOW_INF2_NOTARGETSELF] == true);
    if (isCombo == false) // Stop attack on non-combo skills [Skotlex]
        src->AttackStop();
    ud->IsSkillCastCancel = skill != nullptr && skill->IsCastCancel == true;

    // A monster's cast time and cancel are its skill row's (mobskill_use's call of unit_skilluse_id2)
    ROWOWMobSkill const* mobSkill = GetMobSkillInUse(src);
    if (mobSkill != nullptr)
        ud->IsSkillCastCancel = mobSkill->IsCancel;
    int32 casttime = UnitSkillCastTimeFix(src, target, skillID, skillLevel, mobSkill != nullptr ? mobSkill->CastTime : SkillCastFix(src, skillID, skillLevel, true));
    casttime = SkillCastFixSC(src, casttime, uint8(skill != nullptr ? skill->CastTimeFlags : 0));

    // SC_MAGICPOWER needs to switch states at start of cast
    SkillToggleMagicPower(src, skillID);

    // A player's skill cast at a monster: a cast sensor monster turns on the caster, also when it takes no cast time in a combo
    if (src->IsPlayer() == true && target != nullptr && target->IsCreature() == true && GetROMobID(target->ToCreature()) != 0 && (casttime > 0 || isCombo == true))
        MobCastSensor(target->ToCreature(), src, tick);
    if (casttime <= 0)
        ud->IsSkillCastCancel = false;
    ud->SkillID = skillID;
    ud->SkillLevel = skillLevel;

    // Set attack and act delays (unit_set_attackdelay relies on the skill being set), then the cast time and general delays
    // (unit_set_castdelay, with the security delay that castend takes off again)
    UnitSetAttackDelay(src, tick, true);
    int64 castDelay = std::max<int64>((skill != nullptr && ROWOWSkill::GetLevelValue(skill->CastTime, skillLevel) != 0) ? casttime : 0, BattleConfig.MinSkillDelayLimit);
    ud->CanActTime = std::max<uint64>(ud->CanActTime, tick + uint64(castDelay)) + ROWOW_SECURITY_CASTTIME;

    // Using a skill ends Cloaking, unless the cloak check type lets it stay (val4&4: monster_cloak_check_type 4)
    if (ROWOWStatusChangeEntry const* sce = GetSCE(src, ROWOW_SC_CLOAKING); sce != nullptr && (sce->Val4 & 4) == 0 && skillID != ROWOW_SKILL_AS_CLOAKING)
        StatusChangeEnd(src, ROWOW_SC_CLOAKING);

    // Free Cast: the caster walks and attacks slower while it casts (status_calc_speed and status_calc_bl_main's skilltimer check)
    ud->IsSkillTimer = casttime > 0;
    if (casttime > 0 && src->IsPlayer() == true && PCCheckSkill(src, ROWOW_SKILL_SA_FREECAST) > 0)
        StatusCalcBL(src, std::bitset<ROWOW_SCB_MAX>().set(ROWOW_SCB_SPEED).set(ROWOW_SCB_ASPD));
    return casttime;
}

SpellCastResult ROWOWMod::SkillCastEndCheck(Unit* src, Unit* target, WorldLocation const* dest, uint16 skillID, uint16 skillLevel)
{
    // skill_castend_id's checks, before anything is paid (skill_castend_id_check, status_check_skilluse, skill_check_condition_castend), or
    // skill_castend_pos's for a ground skill (skill_pos_maxcount_check at its spot).  A failure ends the cast with no after-cast delay
    ROWOWSkill const* skill = GetSkill(skillID);
    Player* sd = src->ToPlayer();
    auto fail = [this, src, &target, sd, skillID, skillLevel](int32 cause)
    {
        if (sd != nullptr)
            ClifSkillFail(sd, skillID, cause);
        // When Asura fails... (except when it fails from Wall of Fog): its SP and spheres are spent all the same, and the Monk moves toward the
        // target anyway
        if (skillID == ROWOW_SKILL_MO_EXTREMITYFIST && sd != nullptr && GetSCE(src, ROWOW_SC_FOGWALL) == nullptr)
        {
            SkillConsumeRequirement(sd, skillID, skillLevel, 1);
            SkillExtremityFistEnd(src, target != nullptr && target->IsInWorld() == true && target->GetMap() == src->GetMap() ? target : nullptr, skillLevel);
        }
        ROWOWSkillUnitData* ud = GetSkillUnitData(src);
        bool wasSkillTimer = ud->IsSkillTimer;
        ud->IsSkillTimer = false;
        ud->CanActTime = GetTick();

        // These actions happen even if the skill fails: a monster's delays and its emotion
        if (src->IsCreature() == true && GetROMobID(src->ToCreature()) != 0)
        {
            ROWOWMobData* mob = GetMobData(src->ToCreature());
            if (ROWOWMobSkill const* mobSkill = GetMobSkillInUse(src))
                ClifEmotion(src, mobSkill->Emotion);
            MobSetDelay(src->ToCreature(), GetTick(), true);
            mob->SkillIdx = -1;
        }
        ud->SkillID = 0;
        ud->SkillLevel = 0;
        ud->SkillItemID = 0;
        ud->SkillItemLevel = 0;
        if (wasSkillTimer == true && sd != nullptr && PCCheckSkill(sd, ROWOW_SKILL_SA_FREECAST) > 0)
            StatusCalcBL(sd, std::bitset<ROWOW_SCB_MAX>().set(ROWOW_SCB_SPEED).set(ROWOW_SCB_ASPD)); // restore original walk speed
        return SPELL_FAILED_DONT_REPORT;
    };
    if (skill == nullptr || src->IsAlive() == false)
        return fail(ROWOW_USESKILL_FAIL_MAX);
    if (target == nullptr)
        target = src;
    if (target->IsInWorld() == false || src->GetMap() != target->GetMap())
        return fail(ROWOW_USESKILL_FAIL_MAX);

    if (SkillGetCastType(skillID) == ROWOW_CAST_GROUND)
    {
        // skill_castend_pos: a ground skill whose units can't go on its spot fails
        int32 x = 0, y = 0;
        if (dest != nullptr)
            GetWorldCell(src->GetMapId(), dest->GetPositionX(), dest->GetPositionY(), x, y);
        else
            GetUnitCell(src, x, y);
        if (SkillPosMaxcountCheck(src, x, y, skillID, skillLevel, true) == false)
            return fail(ROWOW_USESKILL_FAIL_MAX);
    }
    else
    {
        // skill_castend_id_check.  (The other jobs' cases of its switch come with their skills)
        bool isTargetChecked = false; // A case returned -1: the skill's target rules aren't checked
        if (src != target && target->IsCreature() == true && GetROStatus(target) != nullptr && (GetROStatus(target)->Mode & ROWOW_MODE_SKILL_IMMUNE) != 0 &&
            SkillGetCastType(skillID) == ROWOW_CAST_NODAMAGE)
            return fail(ROWOW_USESKILL_FAIL_MAX); // Don't show a skill fail message (NoDamage type doesn't consume requirements)
        switch (skillID)
        {
            case ROWOW_SKILL_RG_STEALCOIN:
            {
                // Does not work on non-monsters, bosses and targets already mugged
                ROWOWStealState const* stealState = target->CustomData.Get<ROWOWStealState>("ROWOWStealState");
                if (target->IsCreature() == false || (GetROStatus(target)->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0 || (stealState != nullptr && stealState->IsStealCoinFlag == true))
                    return fail(ROWOW_USESKILL_FAIL);
                break;
            }
            case ROWOW_SKILL_PF_SOULBURN:
                // Only works on players
                if (target->IsPlayer() == false)
                    return fail(ROWOW_USESKILL_FAIL_LEVEL);
                break;
            case ROWOW_SKILL_RG_BACKSTAP:
            {
                // From behind only (the target faces the way the Rogue looks at it: map_calc_dir and map_check_dir), and not from its own cell
                int32 srcX = 0, srcY = 0, targetX = 0, targetY = 0;
                GetUnitCell(src, srcX, srcY);
                GetUnitCell(target, targetX, targetY);
                if (MapCheckDir(GetRODirection(float(targetX - srcX), float(targetY - srcY)), UnitGetDir(target)) != 0)
                    return fail(ROWOW_USESKILL_FAIL_MAX);
                if (DistanceBL(src, target) == 0)
                    return fail(ROWOW_USESKILL_FAIL_MAX);
                break;
            }
            case ROWOW_SKILL_PR_TURNUNDEAD:
            {
                ROWOWStatus const* tstatus = GetROStatus(target);
                if (tstatus == nullptr || BattleCheckUndead(tstatus->Race, tstatus->DefElement) == false)
                    return fail(ROWOW_USESKILL_FAIL_MAX);
                break;
            }
            case ROWOW_SKILL_DC_WINKCHARM:
                // Charms only demons, demi-humans and angels
                switch (GetROStatus(target) != nullptr ? GetROStatus(target)->Race : -1)
                {
                    case ROWOW_RACE_DEMON:
                    case ROWOW_RACE_DEMIHUMAN:
                    case ROWOW_RACE_ANGEL:
                    case ROWOW_RACE_PLAYER_HUMAN:
                    case ROWOW_RACE_PLAYER_DORAM:
                        break;
                    default:
                        return fail(ROWOW_USESKILL_FAIL_LEVEL);
                }
                break;
            case ROWOW_SKILL_MER_LEXDIVINA:
            case ROWOW_SKILL_PR_LEXDIVINA:
                // If it's not an enemy, and not silenced, you can't use the skill on them. [Skotlex]
                if (BattleCheckTarget(src, target, ROWOW_BCT_ENEMY) <= 0 && GetSCE(target, ROWOW_SC_SILENCE) == nullptr)
                    return fail(ROWOW_USESKILL_FAIL_LEVEL);
                isTargetChecked = true; // Works on silenced allies
                break;
        }
        int32 inf = 0;
        if ((skill->Inf & ROWOW_INF_ATTACK_SKILL) != 0 || ((skill->Inf & ROWOW_INF_SELF_SKILL) != 0 && skill->Inf2[ROWOW_INF2_NOTARGETSELF] == true)) // Casted through combo.
            inf = ROWOW_BCT_ENEMY; // Offensive skill.
        else if (skill->Inf2[ROWOW_INF2_NOTARGETENEMY] == true)
            inf = ROWOW_BCT_NOENEMY;
        if ((skill->Inf2[ROWOW_INF2_PARTYONLY] == true || skill->Inf2[ROWOW_INF2_GUILDONLY] == true) && src != target)
        {
            inf |= (skill->Inf2[ROWOW_INF2_PARTYONLY] == true ? ROWOW_BCT_PARTY : 0) | (skill->Inf2[ROWOW_INF2_GUILDONLY] == true ? ROWOW_BCT_GUILD : 0);
            inf &= ~ROWOW_BCT_NEUTRAL; // Remove neutral targets (but allow enemy if skill is designed to be so)
        }
        if (isTargetChecked == false && inf != 0 && BattleCheckTarget(src, target, inf) <= 0)
            return fail(ROWOW_USESKILL_FAIL_LEVEL);
        // Fogwall makes all offensive-type targetted skills fail at 75%
        if ((inf & ROWOW_BCT_ENEMY) != 0 && GetSCE(target, ROWOW_SC_FOGWALL) != nullptr && urand(0, 99) < 75)
            return fail(ROWOW_USESKILL_FAIL_LEVEL);
    }
    if (StatusCheckSkillUse(src, target, skillID, 1) == false)
        return fail(ROWOW_USESKILL_FAIL_MAX);
    if (sd != nullptr && SkillCheckConditionCastEnd(sd, skillID, skillLevel) == false)
        return fail(ROWOW_USESKILL_FAIL_MAX);
    if (IsROFollower(src) == true && FollowerSkillCheckCondition(src, skillID, skillLevel, 0) == false)
        return fail(ROWOW_USESKILL_FAIL_MAX);
    return SPELL_CAST_OK;
}

void ROWOWMod::SkillCastEnd(Unit* src, Unit* target, WorldLocation const* dest, uint16 skillID, uint16 skillLevel)
{
    // The rest of skill_castend_id / skill_castend_pos once the checks passed: the costs, the after-cast delay (canact_tick, which takes off
    // the security delay of the cast begin) and the skill's cooldown, then the dispatch by cast type.  dest is a ground skill's spot
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr)
        return;
    Player* sd = src->ToPlayer();
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    uint64 tick = GetTick();
    if (target == nullptr)
        target = src;

    // These actions happen even if the skill fails, except when the caster is already dead: a monster's delays and its emotion
    Creature* md = src->IsCreature() == true && GetROMobID(src->ToCreature()) != 0 ? src->ToCreature() : nullptr;
    if (md != nullptr)
    {
        if (ROWOWMobSkill const* mobSkill = GetMobSkillInUse(src))
            ClifEmotion(src, mobSkill->Emotion);
        MobSetDelay(md, tick, true);
    }
    if (ud->IsSkillTimer == true && skillID != ROWOW_SKILL_SA_CASTCANCEL)
    {
        ud->IsSkillTimer = false;
        if (sd != nullptr && PCCheckSkill(sd, ROWOW_SKILL_SA_FREECAST) > 0)
            StatusCalcBL(sd, std::bitset<ROWOW_SCB_MAX>().set(ROWOW_SCB_SPEED).set(ROWOW_SCB_ASPD)); // restore original walk speed
    }
    if (sd != nullptr)
        SkillConsumeRequirement(sd, skillID, skillLevel, 1);

    if (IsROFollower(src) == true && FollowerSkillCheckCondition(src, skillID, skillLevel, 1) == false)
    {
        ud->CanActTime = tick;
        ud->SkillID = 0;
        FollowerClearSkill(src);
        return;
    }

    // Damage skills on a skill immune target fail after paying (a skill fail message)
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (src != target && tstatus != nullptr && tstatus->Class == ROWOW_CLASS_BATTLEFIELD && SkillGetCastType(skillID) == ROWOW_CAST_DAMAGE)
    {
        if (sd != nullptr)
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
        ud->CanActTime = tick;
        ud->SkillID = 0;
        return;
    }

    ud->CanActTime = std::max<uint64>(tick + uint64(SkillDelayFix(src, skillID, skillLevel)), ud->CanActTime > ROWOW_SECURITY_CASTTIME ? ud->CanActTime - ROWOW_SECURITY_CASTTIME : 0);

    // Cooldown application (pc_get_skillcooldown, skill_blockpc_start)
    if (sd != nullptr)
    {
        int32 cooldown = std::max(0, skill->Cooldown[ROWOWSkill::GetLevelIndex(skillLevel)] + GetDeferredBonus2(src, "bSkillCooldown", skillID));
        if (cooldown > 0)
            ud->CooldownEndTimesBySkillID[skillID] = tick + uint64(cooldown);
    }
    SendSkillDelayToClient(src, skillID);
    if (IsROFollower(src) == true)
        FollowerSetSkillBlock(src, skillID, skillLevel);
    if (sd != nullptr)
    {
        switch (skillID)
        {
            case ROWOW_SKILL_KN_BRANDISHSPEAR:
            case ROWOW_SKILL_CR_GRANDCROSS:
            {
                // The weapon (Brandish Spear) or the shield (Grand Cross) can't be changed for the skill's time, unless a strip already lasts
                // longer.  During Grand Cross you are in ankle state (cannot move or teleport)
                int32 type = skillID == ROWOW_SKILL_KN_BRANDISHSPEAR ? ROWOW_SC_STRIPWEAPON : ROWOW_SC_STRIPSHIELD;
                int64 time = SkillGetTime(skillID, skillLevel);
                ROWOWStatusChangeEntry const* sce = GetSCE(src, type);
                if (sce != nullptr && sce->EndTime != 0 && sce->EndTime > tick + uint64(time))
                    break;
                SCStart2(src, src, type, 100, 0, 1, time);
                if (skillID == ROWOW_SKILL_CR_GRANDCROSS)
                    SCStart(src, src, ROWOW_SC_ANKLE, 100, 0, time);
                break;
            }
            case ROWOW_SKILL_GS_DESPERADO:
                GetIdentity(sd)->CanEquipTick = tick + uint64(std::max<int64>(0, SkillGetTime(skillID, skillLevel)));
                break;
        }
    }
    // (unit_set_walkdelay: WoW's movement isn't held after a cast)

    // A Flying Side Kick while running ends the run and kicks with the running formula (its flag 1, the attack's miscflag)
    int32 castFlag = 0;
    if (ud->IsRunning == true && skillID == ROWOW_SKILL_TK_JUMPKICK)
    {
        ud->IsRunning = false;
        StatusChangeEnd(src, ROWOW_SC_RUN);
        castFlag = 1;
    }

    if (skillID == ROWOW_SKILL_PF_SPIDERWEB || skillID == ROWOW_SKILL_AM_RESURRECTHOMUN)
    {
        // These should become skill_castend_pos: Spider Web goes on a random cell its splash away from the target (its own cell when none), as a resurrected homunculus does
        int32 targetX = 0, targetY = 0;
        GetUnitCell(target, targetX, targetY);
        int32 splash = ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel);
        int32 x = targetX + splash, y = targetY + splash;
        if (splash != 0 && MapRandomDir(target, x, y) == false)
        {
            x = targetX;
            y = targetY;
        }
        ud->SkillZ = target->GetPositionZ();
        SkillCastendPos2(src, x, y, skillID, skillLevel, tick, 0);
    }
    else if (SkillGetCastType(skillID) == ROWOW_CAST_GROUND || (skill->Inf & ROWOW_INF_TRAP_SKILL) != 0)
    {
        // A ground skill at its spot.  A skill aimed at a trap (Remove Trap, Spring Trap) is cast at the trap's cell too: a WoW client can't
        // pick a skill unit (rAthena's BL_SKILL target), so the skill takes the trap on the cell it's cast at
        int32 x = 0, y = 0;
        if (dest != nullptr)
            GetWorldCell(src->GetMapId(), dest->GetPositionX(), dest->GetPositionY(), x, y);
        else
            GetUnitCell(src, x, y);
        ud->SkillZ = dest != nullptr ? dest->GetPositionZ() : src->GetPositionZ(); // skillx and skilly's ground, where the units go
        SkillCastendPos2(src, x, y, skillID, skillLevel, tick, 0);
    }
    else if (SkillGetCastType(skillID) == ROWOW_CAST_NODAMAGE)
        SkillCastendNoDamageID(src, target, skillID, skillLevel, tick, castFlag);
    else
        SkillCastendDamageID(src, target, skillID, skillLevel, tick, castFlag);

    // A song started while performing keeps Adaptation from ending it for 3 seconds (skill_blockpc_start)
    if (sd != nullptr && skill->Inf2[ROWOW_INF2_ISSONG] == true && GetSCE(src, ROWOW_SC_DANCING) != nullptr)
        ud->CooldownEndTimesBySkillID[ROWOW_SKILL_BD_ADAPTATION] = tick + 3000;
    ud->SkillID = 0;
    ud->SkillLevel = 0;
    FollowerClearSkill(src);

    // mobs can't clear this one as it is used for skill condition 'afterskill'
    if (md != nullptr && md->IsAlive() == true)
    {
        ROWOWMobData* mob = GetMobData(md);
        mob->SkillIdx = -1;
        mob->AfterSkillID = skillID;
    }
    if (sd != nullptr && skillID != ROWOW_SKILL_SA_ABRACADABRA) // they just set the data so leave it as it is.[Inkfish]
    {
        ud->SkillItemID = 0;
        ud->SkillItemLevel = 0;
    }
}

void ROWOWMod::UnitSkillCastCancel(Unit* bl, int32 type)
{
    // unit_skillcastcancel: a cast stopped (type&2: by damage, which only stops a cast that can be, and not with the no cast cancel
    // bonuses).  The unit may act at once
    ROWOWSkillUnitData* ud = bl->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
    if (ud == nullptr || ud->SkillID == 0)
        return; // Nothing to cancel.
    Spell* spell = bl->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    uint16 spellSkillID = 0, spellSkillLevel = 0;
    if (spell == nullptr || spell->getState() != SPELL_STATE_PREPARING || GetSkillOfSpell(spell->GetSpellInfo()->Id, spellSkillID, spellSkillLevel) == false)
        return;
    if ((type & 2) != 0)
    {
        // See if it can be cancelled.
        if (ud->IsSkillCastCancel == false)
            return;
        if (GetDeferredBonus(bl, "bNoCastCancel2") != 0 || GetDeferredBonus(bl, "bNoCastCancel") != 0)
            return;
    }
    UnitSkillCastReset(bl);
    bl->InterruptSpell(CURRENT_GENERIC_SPELL, false, false);
}

void ROWOWMod::UnitSkillCastReset(Unit* bl)
{
    // unit_skillcastcancel's end, for a cast that stopped however it stopped.  A spell cancelled by Esc or by losing its target has left the unit's spell
    // slot before the core's hook runs, so nothing here reads it
    ROWOWSkillUnitData* ud = bl->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
    if (ud == nullptr || ud->SkillID == 0)
        return;
    ud->CanActTime = GetTick();
    ud->SkillID = 0;
    ud->SkillLevel = 0;
    ud->IsSkillTimer = false;
    FollowerClearSkill(bl);
    if (bl->IsCreature() == true && GetROMobID(bl->ToCreature()) != 0)
    {
        // unit_set_attackdelay(DELAY_EVENT_CASTCANCEL), and the skill's row is done with
        MobSetDelay(bl->ToCreature(), GetTick(), false);
        GetMobData(bl->ToCreature())->SkillIdx = -1;
    }
    if (bl->IsPlayer() == true && PCCheckSkill(bl, ROWOW_SKILL_SA_FREECAST) > 0)
        StatusCalcBL(bl, std::bitset<ROWOW_SCB_MAX>().set(ROWOW_SCB_SPEED).set(ROWOW_SCB_ASPD)); // restore original walk speed
}

bool ROWOWMod::SkillIsNotOk(Player* sd, uint16 skillID)
{
    // skill_isNotOk, the map's rules: noskill, but for an item's skill, nowarp for Warp Portal and nomemo for the wedding calls.  (noteleport is Teleport's
    // own, and the shops' flags are skill_check_condition_castbegin's.  GM rights don't skip it here)
    ROWOWSkillUnitData const* ud = GetSkillUnitData(sd);
    if (skillID == ROWOW_SKILL_AL_TELEPORT && ud->SkillItemID == skillID && ud->SkillItemLevel > 2)
        return false; // Teleport lv 3 bypasses this check.[Inkfish]
    uint32 mapID = sd->GetMapId();
    if (HasMapFlag(mapID, "noskill") == true && skillID != ROWOW_SKILL_ALL_EQSWITCH && ud->SkillItemID == 0)
        return true; // Item skills bypass noskill
    if (ud->SkillItemID == skillID)
        return false;
    switch (skillID)
    {
        case ROWOW_SKILL_AL_WARP:
            if (HasMapFlag(mapID, "nowarp") == true)
            {
                ChatHandler(sd->GetSession()).SendNotification("You can't teleport in this area."); // clif_skill_teleportmessage NOTIFY_MAPINFO_CANT_TP
                return true;
            }
            break;
        case ROWOW_SKILL_WE_CALLPARTNER:
        case ROWOW_SKILL_WE_CALLPARENT:
        case ROWOW_SKILL_WE_CALLBABY:
            if (HasMapFlag(mapID, "nomemo") == true)
            {
                ChatHandler(sd->GetSession()).SendNotification("Saved point cannot be memorized."); // clif_skill_teleportmessage NOTIFY_MAPINFO_CANT_MEMO
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

void ROWOWMod::UpdateSkillCastMove(Player* player)
{
    // unit_walktoxy: a player's walk while its skill timer runs is refused but with Free Cast.  A WoW client walks anyway, so walking stops
    // the cast, from the player's update
    ROWOWSkillUnitData const* ud = player->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
    if (ud == nullptr || ud->IsSkillTimer == false || player->isMoving() == false || PCCheckSkill(player, ROWOW_SKILL_SA_FREECAST) > 0)
        return;
    UnitSkillCastCancel(player, 0);
}

ROWOWSkillCondition ROWOWMod::SkillGetRequirement(Player* sd, uint16 skillID, uint16 skillLevel) const
{
    // skill_get_requirement: what a skill level needs and uses up, with the bonuses and statuses that change it; Encore's dance costs half.
    // (The soul linker spirits and Kaina come with their jobs)
    ROWOWSkillCondition req;
    ROWOWSkill const* skill = GetSkill(skillID);
    ROWOWStatus const* status = GetROStatus(sd);
    if (sd == nullptr || skill == nullptr || status == nullptr)
        return req;
    if (GetSkillUnitData(sd)->SkillItemID == skillID)
        return req; // Item skills and Hocus-Pocus don't have requirements.[Inkfish]
    skillLevel = uint16(std::clamp<int32>(skillLevel, 1, ROWOW_MAX_SKILL_LEVEL));
    int32 levelIndex = skillLevel - 1;

    req.HP = skill->HPCost[levelIndex];
    int32 hpRate = skill->HPRateCost[levelIndex];
    if (hpRate > 0)
        req.HP += int32(sd->GetHealth()) * hpRate / 100;
    else
        req.HP += int32(sd->GetMaxHealth()) * (-hpRate) / 100;

    req.SP = skill->SPCost[levelIndex];
    ROWOWSkillUnitData const* ud = GetSkillUnitData(sd);
    if (ud->SkillIDOld == ROWOW_SKILL_BD_ENCORE && skillID == ud->SkillIDDance)
        req.SP /= 2;
    int32 spRate = skill->SPRateCost[levelIndex];
    if (spRate > 0)
        req.SP += int32(sd->GetPower(POWER_MANA)) * spRate / 100;
    else
        req.SP += int32(sd->GetMaxPower(POWER_MANA)) * (-spRate) / 100;
    if (status->Bonus.UseSPRate != 100)
        req.SP = req.SP * status->Bonus.UseSPRate / 100;
    int32 spSkillRateBonus = 100 - GetDeferredBonus2(sd, "bSkillUseSPrate", skillID);
    req.SP -= GetDeferredBonus2(sd, "bSkillUseSP", skillID);
    req.SP = std::clamp(req.SP * spSkillRateBonus / 100, 0, int32(SHRT_MAX));

    req.Zeny = skill->ZenyCost[levelIndex];
    req.SpiritBall = skill->SpiritSphereCost[levelIndex];
    req.State = skill->RequiredState;
    req.Weapon = skill->RequiredWeapons;
    req.AmmoAmount = skill->AmmoAmount[levelIndex];
    if (req.AmmoAmount != 0)
        req.Ammo = skill->RequiredAmmo;
    req.Statuses = skill->RequiredStatuses;
    req.EquipItemIDs = skill->RequiredEquipment;

    // Normal skill requirements and gemstone checks.  A per-level item cost (Level set) is the level's own; the potion throwers and Plant
    // Cultivation list one item a level and need only the level's (at its place in the list)
    size_t itemIndex = 0;
    bool isLevelDependent = std::any_of(skill->RequiredItems.begin(), skill->RequiredItems.end(), [](ROWOWSkillItemRequirement const& item) { return item.Level != 0; });
    bool isNoGemstone = GetDeferredBonus(sd, "bNoGemStone") != 0;
    for (ROWOWSkillItemRequirement const& item : skill->RequiredItems)
    {
        if (itemIndex >= ROWOW_MAX_SKILL_ITEM_REQUIRE)
            break;
        if (isLevelDependent == true && item.Level != skillLevel)
            continue;
        if ((skillID == ROWOW_SKILL_AM_POTIONPITCHER || skillID == ROWOW_SKILL_CR_SLIMPITCHER || skillID == ROWOW_SKILL_CR_CULTIVATION) &&
            int32(itemIndex) != skillLevel % 11 - 1)
        {
            itemIndex++;
            continue;
        }
        if (skillID == ROWOW_SKILL_AM_CALLHOMUN && GetHomunculus(sd) != nullptr)
            continue; // Don't delete items when hom is already out.
        uint32 itemID = item.ItemID;
        int32 amount = item.Amount;
        // Check requirement for gemstone (Blue, Red and Yellow Gemstones): the Mistress card's bonus saves them
        bool isGemstone = itemID == 715 || itemID == 716 || itemID == 717;
        if (isGemstone == true && isNoGemstone == true)
        {
            if (skillID != ROWOW_SKILL_SA_ABRACADABRA && skillID != ROWOW_SKILL_HW_GANBANTEIN)
                continue;
            if (--amount < 1)
                amount = 1; // Hocus Pocus always use at least 1 gem
        }
        req.ItemIDs[itemIndex] = itemID;
        req.ItemAmounts[itemIndex] = amount;
        itemIndex++;
    }

    // Check for cost reductions due to skills & SCs.  (The other jobs' cases come with them)
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(sd));
    switch (skillID)
    {
        case ROWOW_SKILL_MC_MAMMONITE:
            if (PCCheckSkill(sd, ROWOW_SKILL_BS_UNFAIRLYTRICK) > 0)
                req.Zeny -= req.Zeny * 10 / 100;
            break;
        case ROWOW_SKILL_MO_CHAINCOMBO:
        case ROWOW_SKILL_MO_COMBOFINISH:
        case ROWOW_SKILL_CH_TIGERFIST:
        case ROWOW_SKILL_CH_CHAINCRUSH:
            if (ROWOWStatusChangeEntry const* spirit = sc != nullptr ? sc->GetSCE(ROWOW_SC_SPIRIT) : nullptr; spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_MONK)
                req.SP = 2; // Monk Spirit makes monk/champion combo skills cost 2 SP regardless of original cost
            break;
        case ROWOW_SKILL_MO_BODYRELOCATION:
            if (sc != nullptr && sc->HasSCE(ROWOW_SC_EXPLOSIONSPIRITS) == true)
                req.SpiritBall = 0;
            break;
        case ROWOW_SKILL_SL_SMA:
        case ROWOW_SKILL_SL_STUN:
        case ROWOW_SKILL_SL_STIN:
        {
            // Kaina makes the Es spells cheaper from base level 70 (3%, 5% from 80, 7% from 90 a level of Kaina)
            int32 kainaLevel = PCCheckSkill(sd, ROWOW_SKILL_SL_KAINA);
            uint32 baseLevel = GetROStatus(sd)->Level;
            if (kainaLevel == 0 || baseLevel < 70)
                break;
            if (baseLevel >= 90)
                req.SP -= req.SP * 7 * kainaLevel / 100;
            else if (baseLevel >= 80)
                req.SP -= req.SP * 5 * kainaLevel / 100;
            else
                req.SP -= req.SP * 3 * kainaLevel / 100;
            break;
        }
        case ROWOW_SKILL_MO_EXTREMITYFIST:
            if (sc != nullptr)
            {
                if (sc->HasSCE(ROWOW_SC_BLADESTOP) == true)
                    req.SpiritBall--;
                else if (ROWOWStatusChangeEntry const* combo = sc->GetSCE(ROWOW_SC_COMBO))
                {
                    switch (combo->Val1)
                    {
                        case ROWOW_SKILL_MO_COMBOFINISH:
                            req.SpiritBall = 4;
                            break;
                        case ROWOW_SKILL_CH_CHAINCRUSH: // It should consume whatever is left as long as it's at least 1.
                            req.SpiritBall = PCGetSpiritBall(sd) != 0 ? PCGetSpiritBall(sd) : 1;
                            break;
                    }
                }
            }
            break;
    }
    return req;
}

bool ROWOWMod::SkillCheckConditionCastBegin(Player* sd, uint16 skillID, uint16 skillLevel)
{
    // skill_check_condition_castbegin: the state, statuses, equipment, HP, weapon, SP, zeny and spheres a skill needs to start; an item's
    // skill (Abracadabra's pick) needs nothing.  GM rights don't skip it here.  (The other jobs' checks of its switch come with them; water
    // cells come with their system)
    ROWOWSkillUnitData* ud = GetSkillUnitData(sd);
    if (PCCanMoveAt(sd) == false)
        return false; // clif_parse_UseSkillToId's pc_cant_act2, and the chat room's check
    if (ud->SkillItemID == skillID)
    {
        ud->IsAbraFlag = false; // Hocus-Pocus was used. [Inkfish]
        return true;
    }

    // MC_VENDING and ALL_BUYING_STORE: not on a novending map, nor beside an NPC
    if (skillID == ROWOW_SKILL_MC_VENDING || skillID == ROWOW_SKILL_ALL_BUYING_STORE)
    {
        auto mapIDsIter = MapIDsByMapFlag.find("novending");
        if (mapIDsIter != MapIDsByMapFlag.end() && mapIDsIter->second.find(sd->GetMapId()) != mapIDsIter->second.end())
        {
            ChatHandler(sd->GetSession()).SendSysMessage("You can't open a shop on this map.");
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
            return false;
        }
        if (NpcIsNear(sd) == true)
        {
            ChatHandler(sd->GetSession()).SendNotification("You can't do that this close to an NPC."); // USESKILL_FAIL_THERE_ARE_NPC_AROUND
            return false;
        }
    }
    ROWOWSkillCondition require = SkillGetRequirement(sd, skillID, skillLevel);
    ROWOWStatus const* status = GetROStatus(sd);

    // An ensemble needs its partner beside the performer
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill != nullptr && skill->Inf2[ROWOW_INF2_ISENSEMBLE] == true)
    {
        uint16 partnerLevel = skillLevel;
        if (SkillCheckPCPartner(sd, skillID, partnerLevel, 1, 0) < 1)
        {
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
            return false;
        }
    }

    // A brewing list open blocks brewing (and making arrows, mending) again until it's picked from
    ROWOWIdentity const* identity = GetIdentity(sd);
    if (identity->MenuSkillID == ROWOW_SKILL_AM_PHARMACY)
    {
        switch (skillID)
        {
            case ROWOW_SKILL_AM_PHARMACY:
            case ROWOW_SKILL_AC_MAKINGARROW:
            case ROWOW_SKILL_BS_REPAIRWEAPON:
            case ROWOW_SKILL_AM_TWILIGHT1:
            case ROWOW_SKILL_AM_TWILIGHT2:
            case ROWOW_SKILL_AM_TWILIGHT3:
                return false;
        }
    }

    // The checks of the skills themselves.  (The other jobs' cases of its switch come with them)
    switch (skillID)
    {
        case ROWOW_SKILL_AM_TWILIGHT2:
        case ROWOW_SKILL_AM_TWILIGHT3:
        {
            // party_skill_check: Twilight Pharmacy needs a Super Novice in the party (2) or a Taekwon (3), online anywhere
            if (PartySkillCheckJob(sd, skillID == ROWOW_SKILL_AM_TWILIGHT2 ? ROWOW_JOB_SUPER_NOVICE : ROWOW_JOB_TAEKWON) == false)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        }
        case ROWOW_SKILL_BD_ADAPTATION:
        {
            // Only while performing, and not in its last moments (rAthena's own check of the time left: val3>>16 of the seconds left, which is
            // always 0, so only a dance shorter than Adaptation's Duration2 refuses it)
            ROWOWStatusChangeEntry const* dancing = GetSCE(sd, ROWOW_SC_DANCING);
            if (dancing == nullptr)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            int64 time = 1000 * int64(dancing->Val3 >> 16);
            if (SkillGetTime(uint16(dancing->Val1 & 0xFFFF), uint16(dancing->Val1 >> 16)) - time < SkillGetTime2(skillID, skillLevel))
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        }
        case ROWOW_SKILL_HT_POWER:
        {
            // Beast Strafing only right after Double Strafe (its combo)
            ROWOWStatusChangeEntry const* combo = GetSCE(sd, ROWOW_SC_COMBO);
            if (combo == nullptr || combo->Val1 != ROWOW_SKILL_AC_DOUBLE)
                return false;
            break;
        }
        case ROWOW_SKILL_CG_HERMODE:
        {
            // Wand of Hermode only near a warp (npc_check_areanpc's warps, here the map's teleport area triggers)
            int32 sdX = 0, sdY = 0;
            GetUnitCell(sd, sdX, sdY);
            int32 range = ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel);
            bool isNearWarp = false;
            for (auto const& teleportByTriggerID : sObjectMgr->GetAllAreaTriggerTeleports())
            {
                AreaTrigger const* areaTrigger = sObjectMgr->GetAreaTrigger(teleportByTriggerID.first);
                if (areaTrigger == nullptr || areaTrigger->map != sd->GetMapId())
                    continue;
                int32 warpX = 0, warpY = 0;
                GetWorldCell(sd->GetMapId(), areaTrigger->x, areaTrigger->y, warpX, warpY);
                int32 warpReach = int32(std::max({ areaTrigger->radius, areaTrigger->length / 2.0f, areaTrigger->width / 2.0f }) / CellSize);
                if (std::max(std::abs(warpX - sdX), std::abs(warpY - sdY)) <= range + warpReach)
                {
                    isNearWarp = true;
                    break;
                }
            }
            if (isNearWarp == false)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        }
        case ROWOW_SKILL_CG_MOONLIT:
        {
            // Check there's no wall in the range+1 area around the caster. [Skotlex]
            int32 range = ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel) + 1;
            int32 size = range * 2 + 1;
            int32 sdX = 0, sdY = 0;
            GetUnitCell(sd, sdX, sdY);
            ROWOWMapGrid const* mapGrid = GetMapGrid(sd->GetMapId());
            for (int32 i = 0; mapGrid != nullptr && i < size * size; i++)
            {
                int32 x = sdX + (i % size - range);
                int32 y = sdY + (i / size - range);
                if ((x != sdX || y != sdY) && IsCellNoPass(sd, *mapGrid, x, y) == true)
                {
                    ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                    return false;
                }
            }
            break;
        }
        case ROWOW_SKILL_SA_CASTCANCEL:
            if (ud->SkillID == 0)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        case ROWOW_SKILL_PF_HPCONVERSION:
            if (sd->GetPower(POWER_MANA) >= sd->GetMaxPower(POWER_MANA))
                return false; // Unusable when at full SP.
            break;
        case ROWOW_SKILL_MO_CALLSPIRITS:
            if (PCGetSpiritBall(sd) >= skillLevel)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        case ROWOW_SKILL_MO_FINGEROFFENSIVE:
            // Throws the spheres there are, up to the level's
            if (PCGetSpiritBall(sd) > 0 && PCGetSpiritBall(sd) < require.SpiritBall)
                ud->SpiritBallOld = require.SpiritBall = PCGetSpiritBall(sd);
            else
                ud->SpiritBallOld = require.SpiritBall;
            break;
        case ROWOW_SKILL_MO_CHAINCOMBO:
        {
            ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(sd));
            if (sc == nullptr)
                return false;
            if (sc->HasSCE(ROWOW_SC_BLADESTOP) == true)
                break;
            if (ROWOWStatusChangeEntry const* combo = sc->GetSCE(ROWOW_SC_COMBO); combo != nullptr && combo->Val1 == ROWOW_SKILL_MO_TRIPLEATTACK)
                break;
            return false;
        }
        case ROWOW_SKILL_MO_COMBOFINISH:
        {
            ROWOWStatusChangeEntry const* combo = GetSCE(sd, ROWOW_SC_COMBO);
            if (combo == nullptr || combo->Val1 != ROWOW_SKILL_MO_CHAINCOMBO)
                return false;
            break;
        }
        case ROWOW_SKILL_CH_TIGERFIST:
        {
            // In Pre-Renewal Tiger Fist can be used after Combo Finish or after Chain Crush Combo
            ROWOWStatusChangeEntry const* combo = GetSCE(sd, ROWOW_SC_COMBO);
            if (combo == nullptr || (combo->Val1 != ROWOW_SKILL_MO_COMBOFINISH && combo->Val1 != ROWOW_SKILL_CH_CHAINCRUSH))
                return false;
            break;
        }
        case ROWOW_SKILL_CH_CHAINCRUSH:
        {
            ROWOWStatusChangeEntry const* combo = GetSCE(sd, ROWOW_SC_COMBO);
            if (combo == nullptr || (combo->Val1 != ROWOW_SKILL_MO_COMBOFINISH && combo->Val1 != ROWOW_SKILL_CH_TIGERFIST))
                return false;
            break;
        }
        case ROWOW_SKILL_MO_EXTREMITYFIST:
        {
            ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(sd));
            if (sc != nullptr && sc->HasSCE(ROWOW_SC_BLADESTOP) == true)
                break;
            if (ROWOWStatusChangeEntry const* combo = sc != nullptr ? sc->GetSCE(ROWOW_SC_COMBO) : nullptr)
            {
                if (combo->Val1 != ROWOW_SKILL_MO_COMBOFINISH && combo->Val1 != ROWOW_SKILL_CH_CHAINCRUSH)
                    return false;
            }
            else if (sd->HasUnitState(UNIT_STATE_ROOT) == true || (sc != nullptr && sc->Cant.Move == true))
            {
                // Placed here as ST_MOVE_ENABLE should not apply if rooted or on a combo. [Skotlex]
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            ud->SpiritBallOld = PCGetSpiritBall(sd);
            break;
        }
        case ROWOW_SKILL_PR_BENEDICTIO:
        {
            uint16 partnerLevel = skillLevel;
            if (SkillCheckPCPartner(sd, skillID, partnerLevel, 1, 0) < 2)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        }
        case ROWOW_SKILL_AM_CALLHOMUN:
            // Can't summon if a hom is already out
            if (ROWOWHomunData const* hd = GetHomunculus(sd); hd != nullptr && hd->Vaporize == ROWOW_HOM_ST_ACTIVE)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        case ROWOW_SKILL_AM_REST:
        {
            // Can't vapo homun if you don't have an active homunc or it's hp is < 80%
            ROWOWHomunData const* hd = GetHomunculus(sd);
            Creature* md = HomIsActive(hd) == true ? GetFollowerCreature(sd, *hd) : nullptr;
            if (md == nullptr || uint64(md->GetHealth()) < uint64(md->GetMaxHealth()) * 80 / 100)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        }
        case ROWOW_SKILL_PR_REDEMPTIO:
        {
            // Needs exp_cost_redemptio percent of the level's exp
            ROWOWIdentity const* identity = GetIdentity(sd);
            uint64 exp = PCNextBaseExp(*identity);
            if (BattleConfig.ExpCostRedemptio != 0 && exp > 0 && uint64(identity->BaseExp) * 100 / exp < uint64(BattleConfig.ExpCostRedemptio))
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL); // Not enough exp.
                return false;
            }
            break;
        }
        case ROWOW_SKILL_HP_BASILICA:
            if (GetSCE(sd, ROWOW_SC_BASILICA) == nullptr)
            {
                // When castbegin, needs 7x7 clear area (no walls, and no one but the caster)
                int32 range = SkillGetUnitLayoutType(skillID, skillLevel) + 1;
                int32 size = range * 2 + 1;
                int32 sdX = 0, sdY = 0;
                GetUnitCell(sd, sdX, sdY);
                ROWOWMapGrid const* mapGrid = GetMapGrid(sd->GetMapId());
                for (int32 i = 0; mapGrid != nullptr && i < size * size; i++)
                {
                    int32 x = sdX + (i % size - range);
                    int32 y = sdY + (i / size - range);
                    if ((x != sdX || y != sdY) && IsCellNoPass(sd, *mapGrid, x, y) == true)
                    {
                        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL);
                        return false;
                    }
                }
                if (MapForEachInRange(sd, range, [sd](Unit* bl) { return bl != sd && (bl->IsPlayer() == true || bl->IsAlive() == true) ? 1 : 0; }) > 0) // skill_count_wos
                {
                    ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL);
                    return false;
                }
            }
            break;
        case ROWOW_SKILL_RG_GRAFFITI:
        {
            // If a previous Graffiti exists skill fails to cast (map_foreachinmap(skill_graffitiremover): anyone's, on the map)
            ROWOWMapSkillUnits const* mapUnits = GetMapSkillUnits(sd->GetMap(), false);
            if (mapUnits != nullptr && std::any_of(mapUnits->GroupsByID.begin(), mapUnits->GroupsByID.end(), [](auto const& groupByID)
            {
                return groupByID.second->UnitID == ROWOW_UNT_GRAFFITI && groupByID.second->AliveCount > 0;
            }) == true)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        }
        case ROWOW_SKILL_ASC_EDP:
            // Bare hands can't enchant (switch_remove_edp&1, pre-renewal's bit)
            if (GetROStatus(sd)->RightWeaponType == ROWOW_WEAPON_TYPE_FIST && (BattleConfig.SwitchRemoveEdp & 1) != 0)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_THIS_WEAPON);
                return false;
            }
            break;
        case ROWOW_SKILL_GS_FLING:
            // As many coins as it has, up to the level's (the attack and SC_FLING count them)
            if (PCGetSpiritBall(sd) > 0 && PCGetSpiritBall(sd) < require.SpiritBall)
                ud->SpiritBallOld = require.SpiritBall = PCGetSpiritBall(sd);
            else
                ud->SpiritBallOld = require.SpiritBall;
            break;
        case ROWOW_SKILL_TK_MISSION:
            if (identity->JobID != ROWOW_JOB_TAEKWON)
            {
                // Cannot be used by Non-Taekwon classes
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        case ROWOW_SKILL_TK_READYCOUNTER:
        case ROWOW_SKILL_TK_READYDOWN:
        case ROWOW_SKILL_TK_READYSTORM:
        case ROWOW_SKILL_TK_READYTURN:
        case ROWOW_SKILL_TK_JUMPKICK:
            if (identity->JobID == ROWOW_JOB_SOUL_LINKER)
            {
                // Soul Linkers cannot use this skill
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        case ROWOW_SKILL_TK_TURNKICK:
        case ROWOW_SKILL_TK_STORMKICK:
        case ROWOW_SKILL_TK_DOWNKICK:
        case ROWOW_SKILL_TK_COUNTER:
        {
            if (identity->JobID == ROWOW_JOB_SOUL_LINKER)
                return false; // Anti-Soul Linker check in case you job-changed with Stances active.
            ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(sd));
            ROWOWStatusChangeEntry const* combo = sc != nullptr ? sc->GetSCE(ROWOW_SC_COMBO) : nullptr;
            if (combo == nullptr || combo->Val1 == ROWOW_SKILL_TK_JUMPKICK)
                return false; // Combo needs to be ready
            if (combo->Val3 != 0)
            {
                // Kick chain.  Do not repeat a kick.
                if (combo->Val3 != skillID)
                    break;
                StatusChangeEnd(sd, ROWOW_SC_COMBO);
                return false;
            }
            if (combo->Val1 != skillID && PCIsTaekwonRanker(*GetIdentity(sd)) == false)
            {
                // Cancel combo wait.
                UnitCancelCombo(sd);
                return false;
            }
            break; // Combo ready.
        }
        case ROWOW_SKILL_SL_SMA:
            // Only in the opening another Es spell or a spirit gave
            if (GetSCE(sd, ROWOW_SC_SMA) == nullptr)
                return false;
            break;
        case ROWOW_SKILL_SG_SUN_COMFORT:
        case ROWOW_SKILL_SG_MOON_COMFORT:
        case ROWOW_SKILL_SG_STAR_COMFORT:
        {
            // Only on the place of its sun, moon or star, on its day (or anywhere with Miracle)
            if (GetSCE(sd, ROWOW_SC_MIRACLE) != nullptr)
                break;
            int32 i = skillID - ROWOW_SKILL_SG_SUN_COMFORT;
            if (sd->GetMapId() == PCGetFeelMap(*identity, i) && SGIsDay(i) == true)
                break;
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
            return false;
        }
        case ROWOW_SKILL_SG_FUSION:
            // Only with the Star Gladiator spirit.  Auron insists we should implement SP consumption when you are not Soul Linked. [Skotlex]
            // Only invoke on skill begin cast (instant cast skill). [Kevin]
            if (ROWOWStatusChangeEntry const* spirit = GetSCE(sd, ROWOW_SC_SPIRIT); spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_STAR)
                break;
            if (require.SP > 0)
            {
                if (int32(sd->GetPower(POWER_MANA)) < require.SP)
                    ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_SP_INSUFFICIENT);
                else
                    StatusZap(sd, 0, uint32(require.SP));
            }
            return false;
        case ROWOW_SKILL_GS_GLITTERING:
            if (PCGetSpiritBall(sd) >= 10)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        case ROWOW_SKILL_NJ_ISSEN:
            if (sd->GetHealth() < 2)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            [[fallthrough]];
        case ROWOW_SKILL_NJ_BUNSINJYUTSU:
            // Only out of Soul
            if (GetSCE(sd, ROWOW_SC_NEN) == nullptr)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
    }

    // check state required
    switch (require.State)
    {
        case ROWOW_ST_HIDDEN:
        {
            ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(sd));
            if (sc == nullptr || (sc->Option & ROWOW_OPTION_HIDE) == 0) // pc_ishiding
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        }
        case ROWOW_ST_RIDING:
        case ROWOW_ST_PECO:
            // A Peco (pc_isriding; ST_RIDING's dragon is a third job's)
            if (PCIsRiding(sd) == false)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        case ROWOW_ST_FALCON:
            if (PCIsFalcon(sd) == false)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        case ROWOW_ST_CART:
            if (PCIsCartOn(sd) == false)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_CART);
                return false;
            }
            break;
        case ROWOW_ST_SHIELD:
            if (GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_HAND_L) == nullptr || (status->LeftWeaponType != ROWOW_WEAPON_TYPE_FIST))
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
        case ROWOW_ST_MOVE_ENABLE:
            if (sd->HasUnitState(UNIT_STATE_ROOT) == true)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
            break;
    }

    // check the status required (Sight Rasher's is checked later, in skill_check_condition_castend)
    if (skillID != ROWOW_SKILL_WZ_SIGHTRASHER && SkillCheckConditionSCRequired(sd, skillID, require) == false)
        return false;

    // Check for equipped item(s)
    for (uint32 itemID : require.EquipItemIDs)
    {
        bool isWorn = false;
        ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity != nullptr)
            for (auto const& itemGUIDByPosition : identity->ItemGUIDsByEquipPosition)
                if (Item* item = GetIdentityEquippedItem(sd, itemGUIDByPosition.first))
                    if (ROWOWItem const* roItem = GetROItem(item))
                        isWorn = isWorn == true || roItem->ItemID == itemID;
        if (isWorn == false)
        {
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_NEED_EQUIPMENT, 0, itemID);
            return false;
        }
    }

    if (require.Weapon != 0 && (require.Weapon & (1u << status->WeaponType)) == 0 && (require.Weapon & (1u << status->RightWeaponType)) == 0)
    {
        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_THIS_WEAPON);
        return false;
    }
    if (require.SP > 0 && int32(sd->GetPower(POWER_MANA)) < require.SP)
    {
        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_SP_INSUFFICIENT);
        return false;
    }
    if (require.Zeny > 0 && sd->GetMoney() < uint32(require.Zeny))
    {
        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_MONEY);
        return false;
    }
    if (require.SpiritBall > 0 && PCGetSpiritBall(sd) < require.SpiritBall)
    {
        // Skills that require spirit/coin spheres.
        ClifSkillFail(sd, skillID, identity->JobID == ROWOW_JOB_GUNSLINGER ? ROWOW_USESKILL_FAIL_COINS : ROWOW_USESKILL_FAIL_SPIRITS, require.SpiritBall);
        return false;
    }
    return true;
}

bool ROWOWMod::SkillCheckConditionCastEnd(Player* sd, uint16 skillID, uint16 skillLevel)
{
    // skill_check_condition_castend: HP, weapon, ammo, items and Sight Rasher's status, checked when the cast ends, what B.S. Sacramenti's
    // partners give, and how many plants or Marine Spheres an Alchemist may have out
    if (PCCantAct(sd) == true)
        return false; // sd.chatID, and a trade opened during the cast
    if (GetSkillUnitData(sd)->SkillItemID == skillID)
        return true; // Casting finished (Item skill or Hocus-Pocus)

    // perform skill-specific checks (and actions)
    switch (skillID)
    {
        case ROWOW_SKILL_PR_BENEDICTIO:
            SkillCheckPCPartner(sd, skillID, skillLevel, 1, 1);
            break;
        case ROWOW_SKILL_AM_CANNIBALIZE:
        case ROWOW_SKILL_AM_SPHEREMINE:
        {
            int32 c = 0;
            ROWOWSkill const* skill = GetSkill(skillID);
            int32 maxcount = skillID == ROWOW_SKILL_AM_CANNIBALIZE ? 6 - skillLevel : ROWOWSkill::GetLevelValue(skill->MaxCount, skillLevel);
            uint32 mobClass = skillID == ROWOW_SKILL_AM_CANNIBALIZE ? FloraSummonMobIDs[std::clamp<int32>(skillLevel, 1, 5) - 1] : ROWOW_MOBID_MARINE_SPHERE;
            if (BattleConfig.LandSkillLimit != 0 && maxcount > 0 && (BattleConfig.LandSkillLimit & ROWOW_BL_PC) != 0)
            {
                int32 i = SkillCheckConditionMobMaster(sd, skillID, mobClass, c);
                if (c >= maxcount || (skillID == ROWOW_SKILL_AM_CANNIBALIZE && c != i && (BattleConfig.AlchemistSummonSetting & 4) != 0))
                {
                    // Fails when: exceed max limit. There are other plant types already out.
                    ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                    return false;
                }
            }
            break;
        }
    }

    ROWOWSkillCondition require = SkillGetRequirement(sd, skillID, skillLevel);
    ROWOWStatus const* status = GetROStatus(sd);
    if (require.HP > 0 && int32(sd->GetHealth()) <= require.HP)
    {
        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_HP_INSUFFICIENT);
        return false;
    }
    if (require.Weapon != 0 && (require.Weapon & (1u << status->WeaponType)) == 0 && (require.Weapon & (1u << status->RightWeaponType)) == 0)
    {
        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_THIS_WEAPON);
        return false;
    }
    if (require.Ammo != 0)
    {
        // Skill requires stuff equipped in the ammo slot.
        Item* ammoItem = GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_AMMO);
        if (ammoItem == nullptr)
        {
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_NO_AMMO);
            return false;
        }
        if (int32(ammoItem->GetCount()) < require.AmmoAmount)
        {
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_NEED_MORE_BULLET);
            return false;
        }
        if ((require.Ammo & (1u << status->AmmoType)) == 0)
        {
            // Ammo type check. Send the "wrong weapon type" message which is the closest we have to wrong ammo type. [Skotlex]
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_NO_AMMO);
            return false;
        }
    }
    for (int32 i = 0; i < ROWOW_MAX_SKILL_ITEM_REQUIRE; ++i)
    {
        if (require.ItemIDs[i] == 0)
            continue;
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(require.ItemIDs[i]);
        if (itemTemplateIDIter == ItemTemplateIDsByItemID.end() || int32(sd->GetItemCount(itemTemplateIDIter->second)) < require.ItemAmounts[i])
        {
            if (require.ItemIDs[i] == 523) // Holy Water
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_HOLYWATER);
            else if (require.ItemIDs[i] == 716) // Red Gemstone
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_REDJAMSTONE);
            else if (require.ItemIDs[i] == 717) // Blue Gemstone
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_BLUEJAMSTONE);
            else
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_NEED_ITEM, require.ItemAmounts[i], require.ItemIDs[i]);
            return false;
        }
    }
    // check the status required
    if (skillID == ROWOW_SKILL_WZ_SIGHTRASHER && SkillCheckConditionSCRequired(sd, skillID, require) == false)
        return false;
    return true;
}

void ROWOWMod::SkillConsumeRequirement(Player* sd, uint16 skillID, uint16 skillLevel, int16 type)
{
    // skill_consume_requirement: type&1 the HP, SP, spheres and zeny when the cast ends, type&2 the items once the skill went off.  SP is the
    // form's mana, and zeny its money
    ROWOWSkillCondition require = SkillGetRequirement(sd, skillID, skillLevel);
    if ((type & 1) != 0)
    {
        ROWOWSkillUnitData* ud = GetSkillUnitData(sd);
        switch (skillID)
        {
            case ROWOW_SKILL_CG_TAROTCARD: // TarotCard will consume sp in skill_cast_nodamage_id [Inkfish]
            case ROWOW_SKILL_MC_IDENTIFY:
            case ROWOW_SKILL_SM_AUTOBERSERK:
            case ROWOW_SKILL_BD_ADAPTATION:
            case ROWOW_SKILL_BD_ENCORE:
                require.SP = 0;
                break;
            case ROWOW_SKILL_AL_HOLYLIGHT:
                if (ROWOWStatusChangeEntry const* spirit = GetSCE(sd, ROWOW_SC_SPIRIT); spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_PRIEST)
                    require.SP *= 5;
                break;
            case ROWOW_SKILL_MO_KITRANSLATION:
                // Spiritual Bestowment only uses spirit sphere when giving it to someone
                require.SpiritBall = 0;
                [[fallthrough]];
            default:
                if (ud->IsAutoCast == true)
                    require.SP = 0; // An autospell costs no SP
                // Need to remove remembered Encore skill to prevent permanent halving of SP cost
                if (ud->SkillIDOld == ROWOW_SKILL_BD_ENCORE && skillID == ud->SkillIDDance)
                    ud->SkillIDOld = 0;
                break;
        }

        // skill_consume_hpspap: Magnum Break needs HP but doesn't use it up
        if (skillID == ROWOW_SKILL_SM_MAGNUM || skillID == ROWOW_SKILL_MS_MAGNUM)
            require.HP = 0;
        if (require.HP > 0)
            sd->SetHealth(sd->GetHealth() > uint32(require.HP) ? sd->GetHealth() - uint32(require.HP) : 1);
        if (require.SP > 0)
            sd->SetPower(POWER_MANA, sd->GetPower(POWER_MANA) > uint32(require.SP) ? sd->GetPower(POWER_MANA) - uint32(require.SP) : 0);
        if (require.SpiritBall > 0) // Skills that require spirit spheres to use
            PCDelSpiritBall(sd, require.SpiritBall, 0);
        if (require.Zeny > 0 && skillID != ROWOW_SKILL_NJ_ZENYNAGE) // Zeny is reduced on skill_attack.
            sd->ModifyMoney(-std::min<int32>(require.Zeny, int32(std::min<uint32>(sd->GetMoney(), INT32_MAX))));
    }
    if ((type & 2) != 0)
    {
        if (skillID == ROWOW_SKILL_MC_IDENTIFY && require.SP > 0)
            sd->SetPower(POWER_MANA, sd->GetPower(POWER_MANA) > uint32(require.SP) ? sd->GetPower(POWER_MANA) - uint32(require.SP) : 0);
        for (int32 i = 0; i < ROWOW_MAX_SKILL_ITEM_REQUIRE; ++i)
        {
            if (require.ItemIDs[i] == 0 || require.ItemAmounts[i] <= 0)
                continue;
            auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(require.ItemIDs[i]);
            if (itemTemplateIDIter != ItemTemplateIDsByItemID.end())
                sd->DestroyItemCount(itemTemplateIDIter->second, uint32(require.ItemAmounts[i]), true);
        }
    }
}

int32 ROWOWMod::SkillCastFix(Unit* bl, uint16 skillID, uint16 skillLevel, bool isCastBegin)
{
    // skill_castfix: the skill's cast time, reduced by DEX (castrate_dex_scale DEX is instant), the cast rate bonuses, Magic Strings and
    // Foresight.  Only the cast's start (isCastBegin) uses up a Foresight count; the other calls just ask the time
    ROWOWSkill const* skill = GetSkill(skillID);
    ROWOWStatus const* status = GetROStatus(bl);
    if (skill == nullptr || status == nullptr)
        return 0;
    double time = ROWOWSkill::GetLevelValue(skill->CastTime, skillLevel);
    int32 flag = skill->CastTimeFlags;
    int32 reduceCastRate = 0;

    // Calculate base cast time (reduced by dex)
    if ((flag & ROWOW_SKILL_CAST_IGNOREDEX) == 0)
    {
        int32 scale = BattleConfig.CastrateDexScale - status->Stats[ROWOW_STAT_DEX];
        if (scale > 0) // not instant cast
            time = time * float(scale) / BattleConfig.CastrateDexScale;
        else
            return 0; // instant cast
    }

    // Calculate cast time reduced by item/card bonuses
    if (bl->IsPlayer() == true)
    {
        if ((flag & ROWOW_SKILL_CAST_IGNOREITEMBONUS) == 0)
        {
            if (status->Bonus.CastRate != 100)
                reduceCastRate += 100 - status->Bonus.CastRate;
            time += GetDeferredBonus(bl, "bVariableCast"); // bonus bVariableCast
        }
        // Skill-specific reductions work regardless of flag
        time += time * (GetDeferredBonus2(bl, "bCastrate", skillID) + GetDeferredBonus2(bl, "bVariableCastrate", skillID)) / 100; // bonus2 bCastrate, bVariableCastrate
        time += GetDeferredBonus2(bl, "bSkillVariableCast", skillID); // bonus2 bSkillVariableCast
    }

    // These cast time reductions are processed even if the skill fails
    ROWOWStatusChange* sc = GetStatusChange(bl);
    if (sc != nullptr && sc->IsEmpty() == false)
    {
        // Magic Strings stacks additively with item bonuses
        if ((flag & ROWOW_SKILL_CAST_IGNORESTATUS) == 0 && sc->HasSCE(ROWOW_SC_POEMBRAGI) == true)
            reduceCastRate += sc->GetSCE(ROWOW_SC_POEMBRAGI)->Val2;
        // Foresight halves the cast time, it does not stack additively
        if (ROWOWStatusChangeEntry* sce = sc->GetSCE(ROWOW_SC_MEMORIZE))
        {
            if (bl->IsPlayer() == false || PCCheckSkill(bl, skillID) > 0) // Foresight only decreases cast times from learned skills, not skills granted by items
            {
                if ((flag & ROWOW_SKILL_CAST_IGNORESTATUS) == 0)
                    time -= time * 50 / 100;
                // Foresight counter gets reduced even if the skill is not affected by it
                if (isCastBegin == true && (--sce->Val2) <= 0)
                    StatusChangeEnd(bl, ROWOW_SC_MEMORIZE);
            }
        }
    }
    time = time * (1 - float(reduceCastRate) / 100);

    // config cast time multiplier
    if (BattleConfig.CastRate != 100)
        time = time * BattleConfig.CastRate / 100;
    // return final cast time
    return std::max(int32(time), 0);
}

int32 ROWOWMod::SkillCastFixSC(Unit* bl, double time, uint8 flag)
{
    // skill_castfix_sc: the statuses' part of the cast time (Slow Cast, Suffragium, which the cast uses up).  Monsters keep their time
    if (time < 0)
        return 0;
    if (bl->IsPlayer() == false)
        return int32(time);
    ROWOWStatusChange* sc = GetStatusChange(bl);
    if (sc != nullptr && sc->IsEmpty() == false)
    {
        if ((flag & ROWOW_SKILL_CAST_IGNORESTATUS) == 0)
        {
            if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_SLOWCAST))
                time += time * sce->Val2 / 100;
        }
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_SUFFRAGIUM))
        {
            if ((flag & ROWOW_SKILL_CAST_IGNORESTATUS) == 0)
                time -= time * sce->Val2 / 100;
            // Suffragium ends even if the skill is not affected by it
            StatusChangeEnd(bl, ROWOW_SC_SUFFRAGIUM);
        }
    }
    time = std::max(time, 0.0);
    return int32(time);
}

int32 ROWOWMod::SkillDelayFix(Unit* bl, uint16 skillID, uint16 skillLevel) const
{
    // skill_delayfix: the after-cast delay, reduced by DEX (delay_dependon_dex), Bragi, the Soul Linker spirits and the delay bonuses
    ROWOWSkill const* skill = GetSkill(skillID);
    ROWOWStatus const* status = GetROStatus(bl);
    if (skill == nullptr || status == nullptr)
        return 0;
    if (skillID == ROWOW_SKILL_SA_ABRACADABRA)
        return 0; // Will use picked skill's delay.
    if ((GetBattleObjectType(*status) & BattleConfig.NoSkillDelay) != 0)
        return BattleConfig.MinSkillDelayLimit;
    int32 delaynodex = skill->CastDelayFlags;
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(bl));
    if (sc != nullptr && sc->IsEmpty() == true)
        sc = nullptr;
    double time = ROWOWSkill::GetLevelValue(skill->AfterCastActDelay, skillLevel);
    if (time < 0)
        time = -time + status->AttackMotion; // If set to <0, add to attack motion.

    // Delay reductions
    switch (skillID)
    {
        // Monk combo skills have their delay reduced by agi/dex.
        case ROWOW_SKILL_MO_TRIPLEATTACK:
        case ROWOW_SKILL_MO_CHAINCOMBO:
        case ROWOW_SKILL_MO_COMBOFINISH:
        case ROWOW_SKILL_CH_TIGERFIST:
        case ROWOW_SKILL_CH_CHAINCRUSH:
            // If delay not specified, it will be 1000 - 4*agi - 2*dex
            if (time == 0)
                time = 1000;
            time -= (4 * status->Stats[ROWOW_STAT_AGI] + 2 * status->Stats[ROWOW_STAT_DEX]);
            break;
        case ROWOW_SKILL_HP_BASILICA:
            if (sc != nullptr && sc->HasSCE(ROWOW_SC_BASILICA) == false)
                time = 0; // There is no Delay on Basilica creation, only on cancel
            break;
        default:
            if (BattleConfig.DelayDependOnDex != 0 && (delaynodex & ROWOW_SKILL_CAST_IGNOREDEX) == 0)
            {
                // if skill delay is allowed to be reduced by dex
                int32 scale = BattleConfig.CastrateDexScale - status->Stats[ROWOW_STAT_DEX];
                time = scale > 0 ? time * scale / BattleConfig.CastrateDexScale : 0;
            }
            if (BattleConfig.DelayDependOnAgi != 0 && (delaynodex & ROWOW_SKILL_CAST_IGNOREDEX) == 0)
            {
                // if skill delay is allowed to be reduced by agi
                int32 scale = BattleConfig.CastrateDexScale - status->Stats[ROWOW_STAT_AGI];
                time = scale > 0 ? time * scale / BattleConfig.CastrateDexScale : 0;
            }
            break;
    }

    if (ROWOWStatusChangeEntry const* sce = sc != nullptr ? sc->GetSCE(ROWOW_SC_SPIRIT) : nullptr)
    {
        if ((skillID == ROWOW_SKILL_CR_SHIELDBOOMERANG && sce->Val2 == ROWOW_SKILL_SL_CRUSADER) || (skillID == ROWOW_SKILL_AS_SONICBLOW && sce->Val2 == ROWOW_SKILL_SL_ASSASIN))
            time /= 2; // (no War of Emperium or battleground here)
    }

    int32 delay = 0;
    if ((delaynodex & ROWOW_SKILL_CAST_IGNORESTATUS) == 0 && sc != nullptr)
    {
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_POEMBRAGI))
            delay += sce->Val3;
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_MAGICMUSHROOM); sce != nullptr && sce->Val3 == 0)
            delay += sce->Val2;
    }
    if ((delaynodex & ROWOW_SKILL_CAST_IGNOREITEMBONUS) == 0 && bl->IsPlayer() == true)
    {
        delay += GetDeferredBonus(bl, "bDelayRate"); // bonus bDelayRate
        time += GetDeferredBonus2(bl, "bSkillDelay", skillID); // bonus2 bSkillDelay
    }
    if (delay != 0)
        time = time * (1 - float(std::min(delay, 100)) / 100);
    if (BattleConfig.DelayRate != 100)
        time = time * BattleConfig.DelayRate / 100;
    return std::max(int32(time), 0);
}

int32 ROWOWMod::SkillCastendDamageID(Unit* src, Unit* bl, uint16 skillID, uint16 skillLevel, uint64 tick, int32 flag)
{
    // skill_castend_damage_id: hands a damage skill to its impl, then (on the first call of an area) uses up the ammo and items
    if (skillID > 0 && skillLevel == 0)
        return 0;
    if (src->GetMap() != bl->GetMap() || bl->IsInWorld() == false || bl->IsAlive() == false)
        return 1;
    // (Golden Thief Bug's magic immunity, status)
    ROWOWSkillImpl const* impl = GetSkillImpl(skillID);
    if (impl == nullptr)
        return 1; // An unknown skill (one whose class isn't in yet) does nothing after its costs
    impl->CastendDamageID(src, bl, skillLevel, tick, flag);

    if (Player* sd = src->ToPlayer(); sd != nullptr && (flag & 1) == 0)
    {
        // consume arrow on last invocation to this skill.
        if (IsSkillUsingArrow(src, skillID) == true)
            BattleConsumeAmmo(sd, skillID, skillLevel);
        // perform skill requirement consumption
        if ((flag & ROWOW_SKILL_NOCONSUME_REQ) == 0)
            SkillConsumeRequirement(sd, skillID, skillLevel, 2);
    }
    return 0;
}

int32 ROWOWMod::SkillCastendNoDamageID(Unit* src, Unit* bl, uint16 skillID, uint16 skillLevel, uint64 tick, int32 flag)
{
    // skill_castend_nodamage_id
    if (skillID > 0 && skillLevel == 0)
        return 0;
    if (src->GetMap() != bl->GetMap() || bl->IsInWorld() == false || src->IsAlive() == false)
        return 1;
    if (src != bl && bl->IsAlive() == false)
    {
        // Skills that may be cast on dead targets
        switch (skillID)
        {
            case ROWOW_SKILL_NPC_WIDESOULDRAIN:
            case ROWOW_SKILL_PR_REDEMPTIO:
            case ROWOW_SKILL_ALL_RESURRECTION:
                break;
            default:
                return 1;
        }
    }
    ROWOWStatus const* tstatus = GetROStatus(bl);
    Player* sd = src->ToPlayer();
    ROWOWSkill const* skill = GetSkill(skillID);
    if (tstatus == nullptr || skill == nullptr)
        return 1;

    // Check for undead skills that convert a no-damage skill into a damage one. [Skotlex]
    switch (skillID)
    {
        case ROWOW_SKILL_AL_HEAL:
        case ROWOW_SKILL_ALL_RESURRECTION:
        case ROWOW_SKILL_PR_ASPERSIO:
            // Apparently only player casted skills can be offensive like this.
            if (sd != nullptr && BattleCheckUndead(tstatus->Race, tstatus->DefElement) == true)
            {
                if (BattleCheckTarget(src, bl, ROWOW_BCT_ENEMY) < 1)
                {
                    // Offensive heal does not works on non-enemies. [Skotlex]
                    ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                    return 0;
                }
                return SkillCastendDamageID(src, bl, skillID, skillLevel, tick, flag);
            }
            break;
        case ROWOW_SKILL_NPC_SMOKING: // Since it is a self skill, this one ends here rather than in damage_id. [Skotlex]
            return SkillCastendDamageID(src, bl, skillID, skillLevel, tick, flag);
        default:
            // Skill is actually ground placed.
            if (src == bl && skill->UnitTypeID != 0)
            {
                int32 x = 0, y = 0;
                GetUnitCell(bl, x, y);
                GetSkillUnitData(src)->SkillZ = bl->GetPositionZ();
                return SkillCastendPos2(src, x, y, skillID, skillLevel, tick, 0);
            }
            break;
    }

    // Skills that cause an status should be blocked if the target element blocks its element.
    int32 element = ROWOWSkill::GetLevelValue(skill->Element, skillLevel);
    if (src != bl && skill->StatusID > ROWOW_SC_NONE && element > ROWOW_ELEMENT_NEUTRAL && element < ROWOW_ELEMENT_COUNT && skill->Inf != ROWOW_INF_SUPPORT_SKILL &&
        BattleAttrFix(nullptr, nullptr, 100, element, tstatus->DefElement, tstatus->ElementLevel, 0) <= 0)
        return 1;

    ROWOWSkillImpl const* impl = GetSkillImpl(skillID);
    if (impl == nullptr)
        return 1; // An unknown skill (one whose class isn't in yet) does nothing after its costs
    impl->CastendNoDamageID(src, bl, skillLevel, tick, flag);

    // Mob skill event for no damage skills (damage ones are handled in battle_damage/skill_attack) [Skotlex]
    if (bl->IsCreature() == true && GetROMobID(bl->ToCreature()) != 0)
    {
        if (BattleConfig.ExpBonusNodamageAttacker != 0)
            MobLogDamage(bl->ToCreature(), src, 0); // Log interaction (counts as 'attacker' for the exp bonus)
        MobSkillEvent(bl->ToCreature(), src, tick, ROWOW_MSC_SKILLUSED | (int32(skillID) << 16), 0);
    }

    if (sd != nullptr && (flag & 1) == 0)
    {
        // consume arrow on last invocation to this skill.
        if (IsSkillUsingArrow(src, skillID) == true)
            BattleConsumeAmmo(sd, skillID, skillLevel);
        SkillOnSkillUsage(sd, bl, skillID, tick);
        // perform skill requirement consumption
        if ((flag & ROWOW_SKILL_NOCONSUME_REQ) == 0)
            SkillConsumeRequirement(sd, skillID, skillLevel, 2);
    }
    return 0;
}

int32 ROWOWMod::SkillCastendPos2(Unit* src, int32 x, int32 y, uint16 skillID, uint16 skillLevel, uint64 tick, int32 flag)
{
    // skill_castend_pos2: a ground skill at an RO cell, through its impl (which places its units with skill_unitsetting)
    if (skillID > 0 && skillLevel == 0)
        return 0;
    if (src->IsAlive() == false)
        return 0;
    ROWOWSkillImpl const* impl = GetSkillImpl(skillID);
    if (impl == nullptr)
        return 1; // An unknown skill (one whose class isn't in yet) does nothing after its costs
    impl->CastendPos2(src, x, y, skillLevel, tick, flag);
    if (Player* sd = src->ToPlayer())
    {
        // consume arrow if this is a ground skill
        if (IsSkillUsingArrow(src, skillID) == true && (flag & 1) == 0)
            BattleConsumeAmmo(sd, skillID, skillLevel);
        SkillOnSkillUsage(sd, nullptr, skillID, tick);
        // perform skill requirement consumption
        if ((flag & ROWOW_SKILL_NOCONSUME_REQ) == 0)
            SkillConsumeRequirement(sd, skillID, skillLevel, 2);
    }
    return 0;
}

int32 ROWOWMod::SkillMagicReflect(Unit* src, Unit* bl, int32 type)
{
    // skill_magic_reflect: checks if bl should reflect back a spell cast by src.  type is the type of magic attack: 0 indirect (aoe), 1 direct
    // (targetted).  In case of success returns the type of reflection, otherwise 0
    ROWOWStatusChange* sc = GetStatusChange(bl);
    if (sc != nullptr && sc->IsEmpty() == true)
        sc = nullptr;

    // Item-based reflection - Bypasses Boss check
    if (bl->IsPlayer() == true && type != 0 && int32(urand(0, 99)) < GetDeferredBonus(bl, "bMagicDamageReturn"))
        return 1;

    // Magic Mirror reflection - Bypasses Boss check
    if (ROWOWStatusChangeEntry const* mirror = sc != nullptr ? sc->GetSCE(ROWOW_SC_MAGICMIRROR) : nullptr; mirror != nullptr && int32(urand(0, 99)) < mirror->Val2)
        return 3;
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (sstatus == nullptr || sstatus->Class == ROWOW_CLASS_BOSS || sc == nullptr)
        return 0;

    // Kaite reflection - Does not bypass Boss check.  Kaite only works against non-players if they are low-level
    if (ROWOWStatusChangeEntry* kaite = sc->GetSCE(ROWOW_SC_KAITE); kaite != nullptr && (src->IsPlayer() == true || sstatus->Level <= 80))
    {
        if (--kaite->Val2 <= 0)
            StatusChangeEnd(bl, ROWOW_SC_KAITE);
        return 2;
    }
    return 0;
}

Unit* ROWOWMod::BattleGetTargeted(Unit* target) const
{
    // battle_gettargeted: one of the (first 24) units around that swing at the target or cast at it
    std::vector<Unit*> units;
    MapForEachInRange(target, BattleConfig.AreaSize, [target, &units](Unit* bl)
    {
        if (units.size() >= 24)
            return 0;
        ROWOWSkillUnitData const* ud = bl->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
        if (bl->GetVictim() != target && (ud == nullptr || ud->SkillTargetGUID != target->GetGUID()))
            return 0;
        units.push_back(bl);
        return 1;
    });
    return units.empty() == false ? units[urand(0, uint32(units.size() - 1))] : nullptr;
}

int64 ROWOWMod::SkillAttack(int32 attackType, Unit* src, Unit* dsrc, Unit* bl, uint16 skillID, uint16 skillLevel, uint64 tick, int32 flag,
    ROWOWSkillUnit const* dsrcUnit)
{
    // skill_attack: one skill hit on one target, worked out (battle_calc_attack), delivered after the attack motion (battle_delay_damage),
    // then the knockback.  flag&0xFFF goes to battle_calc_attack (the targets of a split skill, Magnum Break's distance), flag&4 skips the
    // knockback, and the SD_ flags are how the hit shows.  A ground skill's unit (dsrcUnit) is the damage's source as rAthena's BL_SKILL dsrc:
    // its hits aren't delayed and knock back from its cell.  A devoted target's Crusader takes the hit, Magic Rod takes in a spell, magic may be
    // reflected at its caster, a Monk's hit opens its combo (skill_combo), a Rogue hit by a skill may copy it (Plagiarism), Intimidate may warp both,
    // and Double Casting may cast a bolt again
    if (skillID > 0 && skillLevel == 0)
        return 0;
    ROWOWStatus const* tstatus = GetROStatus(bl);
    if (tstatus == nullptr || GetROStatus(src) == nullptr)
        return 0;
    bool isUnitSource = src != dsrc || dsrcUnit != nullptr;
    if (isUnitSource == true)
    {
        // When caster is not the src of attack, this is a ground skill, and as such, do the relevant target checking. [Skotlex]
        if (StatusCheckSkillUse(BattleConfig.SkillCasterCheck != 0 ? src : nullptr, bl, skillID, 2) == false)
            return 0;
    }
    else if ((flag & ROWOW_SD_ANIMATION) != 0 && GetSkill(skillID) != nullptr && GetSkill(skillID)->NK[ROWOW_NK_SPLASH] == true)
    {
        // Note that splash attacks often only check versus the targetted mob, those around the splash area normally don't get checked for
        // being hidden/cloaked/etc. [Skotlex]
        if (StatusCheckSkillUse(src, bl, skillID, 2) == false)
            return 0;
    }

    // When Gravitational Field is active, damage can only be dealt by Gravitational Field and Autospells
    ROWOWSkillUnitData const* sud = GetSkillUnitData(src);
    if (ROWOWStatusChangeEntry const* gravitation = src->IsPlayer() == true ? GetSCE(src, ROWOW_SC_GRAVITATION) : nullptr; gravitation != nullptr &&
        gravitation->Val3 == ROWOW_BCT_SELF && skillID != ROWOW_SKILL_HW_GRAVITATION && sud->IsAutoCast == false)
        return 0;

    ROWOWDamage dmg = BattleCalcAttack(attackType, src, bl, skillID, skillLevel, flag & 0xFFF);

    // If the damage source is a unit, the damage is not delayed.  Self Destruction's isn't either: its source is dead by the time it takes
    // effect. [Skotlex]
    if (isUnitSource == true || skillID == ROWOW_SKILL_NPC_SELFDESTRUCTION)
        dmg.AMotion = 0;

    // Earthquake on multiple targets is not counted as a target skill. [Inkfish]
    bool isMagicReflected = false; // rmdamage
    if ((dmg.Flag & ROWOW_BF_MAGIC) != 0 && (skillID != ROWOW_SKILL_NPC_EARTHQUAKE || (BattleConfig.EqSingleTargetReflectable != 0 && (flag & 0xFFF) == 1)))
    {
        int32 reflectType = dmg.Damage != 0 || dmg.Damage2 != 0 ? SkillMagicReflect(src, bl, isUnitSource == false ? 1 : 0) : 0;
        if (reflectType != 0)
        {
            // Magic reflection, switch caster/target
            Unit* tbl = bl;
            isMagicReflected = true;
            bl = src;
            src = tbl;
            dsrc = tbl;
            dsrcUnit = nullptr;
            tstatus = GetROStatus(bl);
            flag |= 2; // bugreport:2564 flag&2 disables double casting trigger
            flag |= 4; // Reflected magic damage will not cause the caster to be knocked back [Playtester]

            // Spirit of Wizard blocks Kaite's reflection
            ROWOWStatusChange* casterSC = GetStatusChange(bl);
            if (ROWOWStatusChangeEntry* spirit = reflectType == 2 && casterSC != nullptr ? casterSC->GetSCE(ROWOW_SC_SPIRIT) : nullptr; spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_WIZARD)
            {
                // Consume one Fragment per hit of the casted skill? [Skotlex]
                Item* fragment = bl->IsPlayer() == true ? PCSearchInventory(bl->ToPlayer(), ROWOW_ITEMID_FRAGMENT_OF_CRYSTAL) : nullptr;
                if (bl->IsPlayer() == false || fragment != nullptr)
                {
                    if (fragment != nullptr)
                        bl->ToPlayer()->DestroyItemCount(fragment->GetEntry(), 1, true);
                    dmg.Damage = 0;
                    dmg.Damage2 = 0;
                    dmg.DmgLv = ROWOW_ATK_MISS;
                    spirit->Val3 = skillID;
                    spirit->Val4 = int32(dsrc->GetGUID().GetCounter());
                }
            }

            // Official Magic Reflection Behavior: damage reflected depends on gears caster wears, not target.  Issue:6415 in pre-renewal Kaite
            // reflected the entire damage received regardless of caster's equipment (Aegis 11.1)
            if (dmg.DmgLv != ROWOW_ATK_MISS && reflectType == 1) // Wiz SL cancelled and consumed fragment
            {
                ROWOWStatus const* casterStatus = GetROStatus(bl);
                ROWOWSkill const* castSkill = GetSkill(skillID);
                int32 element = castSkill != nullptr ? ROWOWSkill::GetLevelValue(castSkill->Element, skillLevel) : ROWOW_ELEMENT_NEUTRAL;
                if (element == ROWOW_ELEMENT_WEAPON) // the skill takes the weapon's element
                    element = GetROStatus(tbl)->WeaponElement;
                else if (element == ROWOW_ELEMENT_ENDOWED) // Use status element
                    element = StatusCalcAttackElement(GetStatusChange(static_cast<Unit const*>(tbl)), ROWOW_ELEMENT_NEUTRAL);
                else if (element == ROWOW_ELEMENT_RANDOM) // Use random element
                    element = int32(urand(0, ROWOW_ELEMENT_COUNT - 1));
                dmg.Damage = BattleAttrFix(bl, bl, dmg.Damage, element, casterStatus->DefElement, casterStatus->ElementLevel, 0);
                if (GetSCE(bl, ROWOW_SC_ENERGYCOAT) != nullptr)
                {
                    int32 per = 100 * int32(bl->GetPower(POWER_MANA)) / std::max<int32>(1, bl->GetMaxPower(POWER_MANA)) - 1; // 100% should be counted as the 80~99% interval
                    per /= 20; // Uses 20% SP intervals.
                    // SP Cost: 1% + 0.5% per every 20% SP
                    if (StatusCharge(bl, 0, (10 + 5 * per) * bl->GetMaxPower(POWER_MANA) / 1000) == false)
                        StatusChangeEnd(bl, ROWOW_SC_ENERGYCOAT);
                    // Reduction: 6% + 6% every 20%
                    dmg.Damage -= dmg.Damage * (6 * (1 + per)) / 100;
                }
                int64 reduce = GetDeferredBonus(bl, "bReduceDamageReturn");
                if (dmg.Damage > 0)
                {
                    dmg.Damage -= dmg.Damage * std::min<int64>(100, reduce) / 100;
                    dmg.Damage = std::max<int64>(dmg.Damage, dmg.Div);
                }
            }
        }
    }

    // Magic Rod takes in a magic skill cast at its user (not a ground skill's unit): no damage, and SP back from the skill's cost
    if ((dmg.Flag & ROWOW_BF_MAGIC) != 0 && isUnitSource == false)
    {
        if (ROWOWStatusChangeEntry const* magicRod = GetSCE(bl, ROWOW_SC_MAGICROD))
        {
            ROWOWSkill const* castSkill = GetSkill(skillID);
            int32 sp = castSkill != nullptr ? castSkill->SPCost[ROWOWSkill::GetLevelIndex(skillLevel)] : 0;
            ClifSkillNoDamage(bl, bl, ROWOW_SKILL_SA_MAGICROD, uint16(magicRod->Val1));
            dmg.Damage = 0;
            dmg.Damage2 = 0;
            dmg.DmgLv = ROWOW_ATK_MISS; // This will prevent skill additional effect from taking effect. [Skotlex]
            sp = sp * magicRod->Val2 / 100;
            if (skillID == ROWOW_SKILL_WZ_WATERBALL && skillLevel > 1)
                sp = sp / ((skillLevel | 1) * (skillLevel | 1)); // Estimate SP cost of a single water-ball
            StatusHeal(bl, 0, sp, 2, bl, 0);
        }
    }
    int64 damage = dmg.Damage + dmg.Damage2;
    if (damage < dmg.Div && skillID != ROWOW_SKILL_CH_PALMSTRIKE)
        dmg.BlewCount = 0; // only pushback when it hit for other

    // Blessing and Increase AGI on someone who's undead from Change Undead (a player; the skill class makes the attack) do 1 damage
    if ((skillID == ROWOW_SKILL_AL_INCAGI || skillID == ROWOW_SKILL_AL_BLESSING) && GetSCE(bl, ROWOW_SC_CHANGEUNDEAD) != nullptr)
        damage = 1;

    // Skills with can't walk delay also stop normal attacking for that duration when the attack connects. [Skotlex]  (WoW's swing timer)
    ROWOWSkill const* skill = GetSkill(skillID);
    if (dmg.DmgLv >= ROWOW_ATK_MISS && skill != nullptr && ROWOWSkill::GetLevelValue(skill->AfterCastWalkDelay, skillLevel) > 0 && src->IsPlayer() == true)
        src->ToPlayer()->setAttackTimer(BASE_ATTACK, std::max<uint32>(src->getAttackTimer(BASE_ATTACK), uint32(ROWOWSkill::GetLevelValue(skill->AfterCastWalkDelay, skillLevel))));

    bool isCritical = dmg.Type == ROWOW_DMG_CRITICAL;
    ROWOWStatusChange const* tsc = GetStatusChange(static_cast<Unit const*>(bl));
    if (tsc != nullptr && tsc->IsEmpty() == true)
        tsc = nullptr;

    switch (skillID)
    {
        case ROWOW_SKILL_TK_COUNTER:
            // bonus from SG_FRIEND [Komurka]
            if (Player* sd = src->ToPlayer(); sd != nullptr && PCCheckSkill(sd, ROWOW_SKILL_SG_FRIEND) > 0)
                PartySkillCheck(sd, ROWOW_SKILL_TK_COUNTER, uint16(PCCheckSkill(sd, ROWOW_SKILL_SG_FRIEND)));
            break;
        case ROWOW_SKILL_SL_STIN:
        case ROWOW_SKILL_SL_STUN:
            // From level 7 Estin and Estun open Esma
            if (skillLevel >= 7 && GetSCE(src, ROWOW_SC_SMA) == nullptr)
                SCStart(src, src, ROWOW_SC_SMA, 100, skillLevel, SkillGetTime(ROWOW_SKILL_SL_SMA, skillLevel));
            break;
    }

    // Only copy skill that copyable [Cydh].  Cannot copy skills if the blow will kill you. [Skotlex]
    if (bl->IsPlayer() == true && skill != nullptr && skill->CopyOption != 0 && (dmg.Flag & ROWOW_BF_SKILL) != 0 && dmg.Damage + dmg.Damage2 > 0 &&
        damage < int64(bl->GetHealth()))
        SkillDoCopy(src, bl, skillID, skillLevel);

    // combo handling
    SkillCombo(src, isUnitSource == true ? nullptr : dsrc, bl, skillID, skillLevel, tick);

    // Instant damage
    if (dmg.AMotion == 0)
    {
        // Deal damage before knockback to allow stuff like firewall+storm gust combo.  A devoted target's Crusader takes it instead (below)
        if (tsc == nullptr || (tsc->HasSCE(ROWOW_SC_DEVOTION) == false && skillID != ROWOW_SKILL_CR_REFLECTSHIELD) || skillID == ROWOW_SKILL_HW_GRAVITATION ||
            skillID == ROWOW_SKILL_NPC_EVILLAND)
            BattleDamage(src, bl, damage, dmg.Div, skillLevel, skillID, dmg.DmgLv, dmg.Flag, false, tick, isCritical);
        if (bl->IsAlive() == true)
            SkillAdditionalEffect(src, bl, skillID, skillLevel, dmg.Flag, dmg.DmgLv, tick);
        if (damage > 0) // Counter status effects [Skotlex]
            SkillCounterAdditionalEffect(src, bl, skillID, skillLevel, dmg.Flag, tick);
    }

    // Blow!
    if ((flag & 4) == 0)
        SkillAttackBlow(src, dsrc, bl, uint8(std::clamp(dmg.BlewCount, 0, 255)), skillID, skillLevel, damage, tick, flag, dsrcUnit);

    // Delayed damage must be dealt after the knockback (it needs to know actual position of target)
    if (dmg.AMotion != 0)
        BattleDelayDamage(tick, dmg.AMotion, src, bl, dmg.Flag, skillID, skillLevel, damage, dmg.DmgLv, dmg.Div, true, isCritical);

    // Devotion: while the link holds, the devoted target's Crusader takes the whole hit (battle_delay_damage kept a delayed one off the
    // target, and an instant one skipped it above).  A broken link ends the status, and an instant hit lands on the target after all
    tsc = GetStatusChange(static_cast<Unit const*>(bl));
    if (tsc != nullptr && tsc->HasSCE(ROWOW_SC_DEVOTION) == true && skillID != ROWOW_SKILL_NPC_EVILLAND && skillID != ROWOW_SKILL_PA_PRESSURE &&
        skillID != ROWOW_SKILL_HW_GRAVITATION)
    {
        if (Unit* d_bl = GetDevotionSource(bl, true))
        {
            // If it isn't devotion_rdamage's, reflected magics are done directly on the target not on the Crusader
            if (isMagicReflected == false)
                BattleFixDamage(src, d_bl, damage, ROWOW_SKILL_CR_DEVOTION);
            else if (BattleConfig.DevotionRDamage != 0 && BattleConfig.DevotionRDamage > int32(urand(0, 99)))
                BattleFixDamage(bl, d_bl, damage, ROWOW_SKILL_CR_DEVOTION);
            else
                BattleFixDamage(bl, bl, damage, ROWOW_SKILL_CR_DEVOTION);
        }
        else
        {
            StatusChangeEnd(bl, ROWOW_SC_DEVOTION);
            if (dmg.AMotion == 0)
                BattleDamage(src, bl, damage, dmg.Div, skillLevel, skillID, dmg.DmgLv, dmg.Flag, false, tick, isCritical);
        }
    }

    // Intimidate: a hit may warp the caster and the target together 800 ms later (50% + 5% a level + the level difference), not a status
    // immune target
    if (damage > 0 && (tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) == 0 && skillID == ROWOW_SKILL_RG_INTIMIDATE)
    {
        int32 rate = 50 + skillLevel * 5;
        rate = rate + (int32(GetROStatus(src)->Level) - int32(tstatus->Level));
        if (int32(urand(0, 99)) < rate)
            SkillAddTimerSkill(src, tick + 800, bl->GetGUID(), 0, 0, skillID, skillLevel, 0, flag);
    }

    // Post-damage effects
    if (damage > 0)
        if (Player* sd = src->ToPlayer())
            SkillOnSkillUsage(sd, bl, skillID, tick);
    // (battle_drain, the drain bonuses of weapon skills, come with the deferred drain bonuses)

    // Double Casting: a chance of the same bolt again (flag&2 keeps the second from casting a third)
    if ((flag & 2) == 0)
    {
        switch (skillID)
        {
            case ROWOW_SKILL_MG_COLDBOLT:
            case ROWOW_SKILL_MG_FIREBOLT:
            case ROWOW_SKILL_MG_LIGHTNINGBOLT:
                if (ROWOWStatusChangeEntry const* sce = GetSCE(src, ROWOW_SC_DOUBLECAST); sce != nullptr && int32(urand(0, 99)) < sce->Val2)
                    SkillAddTimerSkill(src, tick + uint64(dmg.AMotion), bl->GetGUID(), 0, 0, skillID, skillLevel, ROWOW_BF_MAGIC, flag | 2);
                break;
        }
    }
    return damage;
}

void ROWOWMod::SkillAttackBlow(Unit* src, Unit* dsrc, Unit* target, uint8 blewcount, uint16 skillID, uint16 /*skillLevel*/, int64 damage, uint64 /*tick*/,
    int32 /*flag*/, ROWOWSkillUnit const* dsrcUnit)
{
    // skill_attack_blow: knockback the target on skill_attack.  Only knockback if it's still alive, otherwise a "ghost" is left behind.
    // [Skotlex]  Reflected spells do not bounce back (src == dsrc since it only happens for direct skills).  Phantasmic Arrow doesn't push
    // what it kills.  (Knuckle Arrow's and the third jobs' cases come with them)
    if (blewcount == 0 || (target == dsrc && dsrcUnit == nullptr) || target->IsAlive() == false)
        return;
    int8 dir = -1; // Default direction
    // Skill specific direction
    switch (skillID)
    {
        case ROWOW_SKILL_MG_FIREWALL:
            dir = int8(UnitGetDir(target)); // Backwards
            break;
        // This ensures the storm randomly pushes instead of exactly a cell backwards per official mechanics.
        case ROWOW_SKILL_NPC_STORMGUST2:
        case ROWOW_SKILL_WZ_STORMGUST:
            if (BattleConfig.StormgustKnockback == 0)
                dir = int8(urand(0, ROWOW_RO_DIRECTION_COUNT - 1));
            break;
        case ROWOW_SKILL_MC_CARTREVOLUTION:
            if (BattleConfig.CartRevoKnockback != 0)
                dir = ROWOW_RO_DIRECTION_WEST; // Official servers push target to the West
            break;
        case ROWOW_SKILL_AC_SHOWER:
        {
            // map_calc_dir: from the target to the caster, or (arrow_shower_knockback) to the arrows' spot
            int32 targetX = 0, targetY = 0, srcX = 0, srcY = 0;
            GetUnitCell(target, targetX, targetY);
            if (BattleConfig.ArrowShowerKnockback == 0)
                GetUnitCell(src, srcX, srcY);
            else
            {
                ROWOWSkillUnitData const* ud = GetSkillUnitData(src);
                srcX = ud->AreaTemp.Values[4];
                srcY = ud->AreaTemp.Values[5];
            }
            dir = int8((srcX == targetX && srcY == targetY) ? UnitGetDir(target) : GetRODirection(float(srcX - targetX), float(srcY - targetY)));
            break;
        }
        case ROWOW_SKILL_HT_PHANTASMIC: // issue #1378
            if (int64(target->GetHealth()) - damage <= 0)
                return;
            break;
    }
    SkillBlown(dsrc, target, blewcount, dir, ROWOW_BLOWN_NONE, dsrcUnit);
}

bool ROWOWMod::IsCellNoPass(Unit const* unit, ROWOWMapGrid const& mapGrid, int32 cellX, int32 cellY) const
{
    // map_getcell(CELL_CHKNOPASS) beside the unit: a cell is a wall when there's no ground there, or it's too steep from the unit's ground to
    // step to (a wall or a cliff), or something solid stands between (the same rules as a monster's cell path)
    if (mapGrid.HasCells() == true)
        return MapGetCellNoPass(unit->GetMap(), mapGrid, cellX, cellY);
    float x = mapGrid.GetCellCenterWorldX(cellY);
    float y = mapGrid.GetCellCenterWorldY(cellX);
    if (MapGetCellIceWall(unit->GetMap(), cellX, cellY) == true)
        return true; // An Ice Wall cell is a wall
    float distance = std::sqrt((x - unit->GetPositionX()) * (x - unit->GetPositionX()) + (y - unit->GetPositionY()) * (y - unit->GetPositionY()));
    float unitZ = unit->GetPositionZ();
    float groundZ = unit->GetMap()->GetHeight(unit->GetPhaseMask(), x, y, unitZ + ConfigMoveMaxRisePerStep * distance + 0.5f, true, ConfigMoveFloorSearchDistance);
    if (groundZ <= INVALID_HEIGHT || std::fabs(groundZ - unitZ) > ConfigMoveMaxRisePerStep * distance)
        return true;
    return unit->GetMap()->isInLineOfSight(unit->GetPositionX(), unit->GetPositionY(), unitZ + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT, x, y, groundZ + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT,
        unit->GetPhaseMask(), LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing) == false;
}

bool ROWOWMod::MapRandomDir(Unit const* bl, int32& x, int32& y) const
{
    // map_random_dir: a spot as far from the unit as x,y but off along a random diagonal (Confusion's walk), one that isn't a wall, or
    // false (x,y kept) after 100 tries.  (The path search to it is the walk's own)
    static int32 const dirx[] = { 0, -1, -1, -1, 0, 1, 1, 1 };
    static int32 const diry[] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    ROWOWMapGrid const* mapGrid = GetMapGrid(bl->GetMapId());
    if (mapGrid == nullptr)
        return false;
    int32 blX = 0, blY = 0;
    GetUnitCell(bl, blX, blY);
    int32 xi = x - blX;
    int32 yi = y - blY;
    int32 dist2 = xi * xi + yi * yi;
    int32 dist = int32(std::sqrt(float(dist2)));
    if (dist < 1)
        dist = 1;
    int32 i = 0;
    do
    {
        int32 j = 1 + 2 * int32(urand(0, 3)); // Pick a random diagonal direction
        int32 segment = int32(urand(1, uint32(dist))); // Pick a random interval from the whole vector in that direction
        xi = blX + segment * dirx[j];
        segment = int32(std::sqrt(float(std::max(0, dist2 - segment * segment)))); // The complement of the previously picked segment
        yi = blY + segment * diry[j];
    } while (IsCellNoPass(bl, *mapGrid, xi, yi) == true && (++i) < 100);
    if (i < 100)
    {
        x = xi;
        y = yi;
        return true;
    }
    return false;
}

bool ROWOWMod::SkillCheckCloaking(Unit* bl, ROWOWStatusChangeEntry* sce)
{
    // skill_check_cloaking: whether a wall is next to the unit (any of the 8 cells around it), with the cloak check type asking for one.
    // With the unit's Cloaking: below level 3 it ends away from a wall, and from level 3 the wall's speed bonus (val4&1) follows
    bool wall = true;
    if ((bl->IsPlayer() == true && (BattleConfig.PCCloakCheckType & 1) != 0) || (bl->IsPlayer() == false && (BattleConfig.MonsterCloakCheckType & 1) != 0))
    {
        // Check for walls.
        static int32 const dx[] = { 0, 1, 0, -1, -1, 1, 1, -1 };
        static int32 const dy[] = { -1, 0, 1, 0, -1, -1, 1, 1 };
        if (ROWOWMapGrid const* mapGrid = GetMapGrid(bl->GetMapId()))
        {
            int32 x = 0, y = 0;
            GetUnitCell(bl, x, y);
            int32 i = 0;
            while (i < 8 && IsCellNoPass(bl, *mapGrid, x + dx[i], y + dy[i]) == false)
                i++;
            if (i == 8)
                wall = false;
        }
    }
    if (sce != nullptr)
    {
        std::bitset<ROWOW_SCB_MAX> speedFlag;
        speedFlag.set(ROWOW_SCB_SPEED);
        if (wall == false)
        {
            if (sce->Val1 < 3) // End cloaking.
                StatusChangeEnd(bl, ROWOW_SC_CLOAKING);
            else if ((sce->Val4 & 1) != 0)
            {
                // Remove wall bonus
                sce->Val4 &= ~1;
                StatusCalcBL(bl, speedFlag);
            }
        }
        else if ((sce->Val4 & 1) == 0)
        {
            // Add wall speed bonus
            sce->Val4 |= 1;
            StatusCalcBL(bl, speedFlag);
        }
    }
    return wall;
}

int32 ROWOWMod::SkillBlown(Unit* src, Unit* target, int32 count, int8 dir, int32 flag, ROWOWSkillUnit const* srcUnit)
{
    // skill_blown: knocks the target back count RO cells, away from src (or its ground unit's cell; dir -1) or in a direction.  A cell it
    // can't stand on stops it (path_blown_halt); it slides there.  Bosses and the knockback-immune don't move (unit_blown_immune)
    if (count == 0)
        return count; // Actual knockback distance is 0.
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (tstatus == nullptr)
        return 0;
    bool isSelf = src == target && srcUnit == nullptr;
    if (isSelf == false && target->IsCreature() == true && (tstatus->Mode & ROWOW_MODE_KNOCKBACK_IMMUNE) != 0)
        return (flag & ROWOW_BLOWN_MD_KNOCKBACK_IMMUNE) != 0 ? count : 0; // Immune can't be knocked back
    if (isSelf == false && (flag & ROWOW_BLOWN_IGNORE_NO_KNOCKBACK) == 0 && target->IsPlayer() == true && GetDeferredBonus(target, "bNoKnockback") != 0)
        return (flag & ROWOW_BLOWN_TARGET_NO_KNOCKBACK) != 0 ? count : 0; // Target has special_state.no_knockback (equip)

    int32 srcX = 0, srcY = 0, targetX = 0, targetY = 0;
    if (srcUnit != nullptr)
    {
        srcX = srcUnit->X;
        srcY = srcUnit->Y;
    }
    else
        GetUnitCell(src, srcX, srcY);
    GetUnitCell(target, targetX, targetY);
    if (dir == -1)
    {
        // <optimized>: do the computation here instead of outside.  Direction from src to target, reversed
        if (targetX == srcX && targetY == srcY)
            dir = int8(BattleConfig.KnockbackLeft != 0 ? ROWOW_RO_DIRECTION_EAST : 0);
        else
            dir = int8(GetRODirection(float(srcX - targetX), float(srcY - targetY)));
    }
    int32 dx = 0, dy = 0;
    if (dir >= 0 && dir < ROWOW_RO_DIRECTION_COUNT)
    {
        // Take the reversed 'direction' and reverse it
        dx = -DirX[dir];
        dy = -DirY[dir];
    }
    if (dx == 0 && dy == 0)
        return 0;

    // unit_blown with path_blownpos: step cell by cell while the next cell has ground at a walkable height and nothing in the way
    count = std::min(count, 25); // Cap to prevent too much processing...?
    Map* map = target->GetMap();
    ROWOWMapGrid const* mapGrid = GetMapGrid(target->GetMapId());
    float cellSize = mapGrid != nullptr ? mapGrid->CellSize : CellSize;
    float x = target->GetPositionX();
    float y = target->GetPositionY();
    float z = target->GetPositionZ();
    int32 moved = 0;
    for (; moved < count; moved++)
    {
        // RO's x east is WoW's -Y, RO's y north is WoW's X
        float nextX = x + float(dy) * cellSize;
        float nextY = y - float(dx) * cellSize;
        float nextZ = map->GetHeight(target->GetPhaseMask(), nextX, nextY, z + KnockbackMaxStepHeight + 0.5f, true, KnockbackMaxStepHeight * 2.0f + 1.0f);
        if (nextZ <= INVALID_HEIGHT || std::fabs(nextZ - z) > KnockbackMaxStepHeight)
            break;
        int32 nextCellX = 0, nextCellY = 0;
        GetWorldCell(target->GetMapId(), nextX, nextY, nextCellX, nextCellY);
        if (MapGetCellIceWall(map, nextCellX, nextCellY) == true)
            break; // An Ice Wall stops it like a wall
        if (map->isInLineOfSight(x, y, z + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT, nextX, nextY, nextZ + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT, target->GetPhaseMask(),
            LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing) == false)
            break;
        x = nextX;
        y = nextY;
        z = nextZ;
    }
    if (moved == 0)
        return 0;
    target->StopMoving();
    target->GetMotionMaster()->MoveCharge(x, y, z, KnockbackSpeed);
    ROWOWSkillUnitData* targetUD = GetSkillUnitData(target);
    targetUD->SkillBlownTick = std::max(GetTick(), targetUD->SkillBlownTick + 1); // Always changes, so a hit can tell it knocked back
    return moved; // Return amount of knocked back cells
}

bool ROWOWMod::UnitMovePos(Unit* bl, int32 x, int32 y)
{
    // skill_check_unit_movepos with unit_movepos: the unit goes at once (the quick slide of a knockback) onto an RO cell of its map that has
    // ground near its own height and nothing solid in the way; else it stays and it's false.  (No map here forbids it, as War of Emperium's)
    float cellX = 0, cellY = 0;
    GetCellWorldPosition(bl->GetMapId(), x, y, cellX, cellY);
    Map* map = bl->GetMap();
    if (MapGetCellIceWall(map, x, y) == true)
        return false; // An Ice Wall's cell can't be stood on
    float z = map->GetHeight(bl->GetPhaseMask(), cellX, cellY, bl->GetPositionZ() + ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT, true, ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE);
    if (z <= INVALID_HEIGHT || map->isInLineOfSight(bl->GetPositionX(), bl->GetPositionY(), bl->GetPositionZ() + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT, cellX, cellY,
        z + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT, bl->GetPhaseMask(), LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing) == false)
        return false;
    bl->StopMoving();
    bl->GetMotionMaster()->MoveCharge(cellX, cellY, z, KnockbackSpeed);
    return true;
}

bool ROWOWMod::UnitJumpPos(Unit* bl, int32 x, int32 y)
{
    // unit_movepos(bl, x, y, 1, 0): the unit goes at once onto an RO cell with ground, whatever lies between (Leap's jump, clif_skill_nodamage
    // with the jump look)
    float cellX = 0, cellY = 0;
    GetCellWorldPosition(bl->GetMapId(), x, y, cellX, cellY);
    Map* map = bl->GetMap();
    if (MapGetCellIceWall(map, x, y) == true)
        return false; // An Ice Wall's cell can't be stood on
    float z = map->GetHeight(bl->GetPhaseMask(), cellX, cellY, bl->GetPositionZ() + ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT, true, ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE);
    if (z <= INVALID_HEIGHT)
        return false;
    bl->StopMoving();
    bl->GetMotionMaster()->MoveJump(cellX, cellY, z, KnockbackSpeed, 8.0f);
    return true;
}

int32 ROWOWMod::SkillAdditionalEffect(Unit* src, Unit* bl, uint16 skillID, uint16 skillLevel, int32 attackType, uint8 dmgLv, uint64 tick)
{
    // skill_additional_effect: what a hit does past its damage: the statuses the attacker's items give (bAddEff, bAddEffOnSkill, which go
    // through even when the damage was blocked), the skill's own, Enchant Poison's and Enchant Deadly Poison's on a normal attack, equipment
    // breaking, the autospells, a falcon's Blitz Beat, Gank and the Taekwon stances' kicks
    if (skillID > 0 && skillLevel == 0)
        return 0; // don't forget auto attacks! - celest
    if (dmgLv < ROWOW_ATK_BLOCK) // Don't apply effect if miss.
        return 0;
    ROWOWStatus const* sstatus = GetROStatus(src);

    // Taekwon combos activate on traps, so we need to check them even for targets that don't have status.  A normal attack in a stance may
    // ready its kick (15%, Counter Kick 20% and more with Friend's bonus), for 2 s less 4 ms an AGI and 2 a DEX
    if (src->IsPlayer() == true && skillID == 0 && (attackType & ROWOW_BF_SKILL) == 0 && sstatus != nullptr)
    {
        // Chance to trigger Taekwon kicks
        int32 comboTime = 2000 - 4 * sstatus->Stats[ROWOW_STAT_AGI] - 2 * sstatus->Stats[ROWOW_STAT_DEX];
        if (GetSCE(src, ROWOW_SC_READYSTORM) != nullptr && SCStart4(src, src, ROWOW_SC_COMBO, 15, ROWOW_SKILL_TK_STORMKICK, 0, 2, 0, comboTime) == true)
            ; // Stance triggered
        else if (GetSCE(src, ROWOW_SC_READYDOWN) != nullptr && SCStart4(src, src, ROWOW_SC_COMBO, 15, ROWOW_SKILL_TK_DOWNKICK, 0, 2, 0, comboTime) == true)
            ; // Stance triggered
        else if (GetSCE(src, ROWOW_SC_READYTURN) != nullptr && SCStart4(src, src, ROWOW_SC_COMBO, 15, ROWOW_SKILL_TK_TURNKICK, 0, 2, 0, comboTime) == true)
            ; // Stance triggered
        else if (GetSCE(src, ROWOW_SC_READYCOUNTER) != nullptr)
        {
            // additional chance from SG_FRIEND [Komurka]
            int32 rate = 20;
            if (ROWOWStatusChangeEntry const* skillRateUp = GetSCE(src, ROWOW_SC_SKILLRATE_UP); skillRateUp != nullptr && skillRateUp->Val1 == ROWOW_SKILL_TK_COUNTER)
            {
                rate += rate * skillRateUp->Val2 / 100;
                StatusChangeEnd(src, ROWOW_SC_SKILLRATE_UP);
            }
            SCStart4(src, src, ROWOW_SC_COMBO, rate, ROWOW_SKILL_TK_COUNTER, 0, 2, 0, comboTime); // Stance triggered
        }
    }
    if (sstatus == nullptr || GetROStatus(bl) == nullptr)
        return 0; // skill additional effect is about adding effects to the target...  So if the target can't be inflicted with statuses, this is pointless.
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
    if (sc != nullptr && sc->IsEmpty() == true)
        sc = nullptr;

    if (src->IsPlayer() == true)
    {
        // These statuses would be applied anyway even if the damage was blocked by some skills. [Inkfish]
        if (skillID != ROWOW_SKILL_WS_CARTTERMINATION && skillID != ROWOW_SKILL_AM_DEMONSTRATION && skillID != ROWOW_SKILL_CR_REFLECTSHIELD &&
            skillID != ROWOW_SKILL_MS_REFLECTSHIELD && skillID != ROWOW_SKILL_ASC_BREAKER)
        {
            // Trigger status effects
            for (ROWOWAddEffect const& effect : sstatus->Bonus.AddEff)
            {
                int32 rate = effect.Rate;
                if ((attackType & ROWOW_BF_LONG) != 0) // Any ranged physical attack takes status arrows into account (Grimtooth...) [DracoRPG]
                    rate += effect.ArrowRate;
                if (rate == 0)
                    continue;
                if (IsAddEffectTriggered(effect.Flag, attackType) == false)
                    continue;
                if ((effect.Flag & ROWOW_ATF_TARGET) != 0)
                    StatusChangeStart(src, bl, effect.Type, rate, 7, 0, 0, 0, effect.Duration, ROWOW_SCSTART_NONE, 100);
                if ((effect.Flag & ROWOW_ATF_SELF) != 0)
                    StatusChangeStart(src, src, effect.Type, rate, 7, 0, 0, 0, effect.Duration, ROWOW_SCSTART_NONE, 100);
            }
        }
        if (skillID != 0)
        {
            // Trigger status effects on skills
            for (ROWOWAddEffectOnSkill const& effect : sstatus->Bonus.AddEffOnSkill)
            {
                if (skillID != effect.SkillID || effect.Rate == 0)
                    continue;
                if ((effect.Target & ROWOW_ATF_TARGET) != 0)
                    StatusChangeStart(src, bl, effect.Type, effect.Rate, 7, 0, 0, 0, effect.Duration, ROWOW_SCSTART_NONE, 100);
                if ((effect.Target & ROWOW_ATF_SELF) != 0)
                    StatusChangeStart(src, src, effect.Type, effect.Rate, 7, 0, 0, 0, effect.Duration, ROWOW_SCSTART_NONE, 100);
            }
        }
        // "While the damage can be blocked by Pneuma, the chance to break armor remains", irowiki. [Cydh]
        if (dmgLv == ROWOW_ATK_BLOCK && skillID == ROWOW_SKILL_AM_ACIDTERROR)
        {
            SCStart2(src, bl, ROWOW_SC_BLEEDING, skillLevel * 3, skillLevel, int32(src->GetGUID().GetCounter()), SkillGetTime2(skillID, skillLevel));
            SkillBreakEquip(src, bl, ROWOW_EQUIP_POSITION_ARMOR, int32(100 * SkillGetTime(skillID, skillLevel)), ROWOW_BCT_ENEMY);
        }
    }
    if (dmgLv < ROWOW_ATK_DEF) // no damage, return;
        return 0;
    if (ROWOWSkillImpl const* impl = GetSkillImpl(skillID))
        impl->ApplyAdditionalEffects(src, bl, skillLevel, tick, attackType, dmgLv);

    // Normal attacks (no skill used); a normal attack that is a skill is splash damage. [Inkfish]
    if (skillID == 0 && (attackType & ROWOW_BF_SKILL) == 0)
    {
        // Automatic trigger of Blitz Beat: a Hunter with its falcon and a bow, LUK/3 in 100 (+0.1), at the level its job level allows (1 a
        // ten levels), its damage split among the targets (SD_LEVEL).  Its look is the skill's own (clif_skill_damage), the falcon flying
        // at the target.  Gank: a Rogue's hit on a monster (not with a bow) may steal from it, at Steal's level
        if (Player* sd = src->ToPlayer())
        {
            int32 skill = PCCheckSkill(sd, ROWOW_SKILL_HT_BLITZBEAT);
            if (PCIsFalcon(sd) == true && sstatus->WeaponType == ROWOW_WEAPON_TYPE_BOW && skill > 0 && int32(urand(0, 999)) <= sstatus->Stats[ROWOW_STAT_LUK] * 10 / 3 + 1)
            {
                int32 rate = (int32(GetIdentity(sd)->JobLevel) + 9) / 10;
                uint16 blitzLevel = uint16(skill < rate ? skill : rate);
                ClifSkillNoDamage(src, bl, ROWOW_SKILL_HT_BLITZBEAT, blitzLevel);
                SkillCastendDamageID(src, bl, ROWOW_SKILL_HT_BLITZBEAT, blitzLevel, tick, ROWOW_SD_LEVEL);
            }
            // Gank
            if (bl->IsCreature() == true && sstatus->WeaponType != ROWOW_WEAPON_TYPE_BOW && (skill = PCCheckSkill(sd, ROWOW_SKILL_RG_SNATCHER)) > 0 &&
                (skill * 15 + 55) + PCCheckSkill(sd, ROWOW_SKILL_TF_STEAL) * 10 > int32(urand(0, 999)))
            {
                if (PCStealItem(sd, bl, uint16(PCCheckSkill(sd, ROWOW_SKILL_TF_STEAL))) == true)
                    ClifSkillNoDamage(src, bl, ROWOW_SKILL_TF_STEAL, uint16(skill));
                else
                    ClifSkillFail(sd, ROWOW_SKILL_RG_SNATCHER, ROWOW_USESKILL_FAIL_LEVEL);
            }
        }
    }
    if (skillID == 0 && (attackType & ROWOW_BF_SKILL) == 0 && sc != nullptr)
    {
        // Enchant Poison gives a chance to poison attacked enemies (don't use sc_start since chance comes in 1/10000 rate)
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_ENCPOISON))
            StatusChangeStart(src, bl, ROWOW_SC_POISON, sce->Val2, sce->Val1, int32(src->GetGUID().GetCounter()), 0, 0,
                SkillGetTime2(ROWOW_SKILL_AS_ENCHANTPOISON, uint16(sce->Val1)), ROWOW_SCSTART_NONE);
        // Enchant Deadly Poison gives a chance to deadly poison attacked enemies
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_EDP))
            SCStart4(src, bl, ROWOW_SC_DPOISON, sce->Val2, sce->Val1, int32(src->GetGUID().GetCounter()), 0, 0, SkillGetTime2(ROWOW_SKILL_ASC_EDP, uint16(sce->Val1)));
    }

    if ((attackType & ROWOW_BF_WEAPON) != 0)
    {
        // Breaking Equipment
        Player* sd = src->ToPlayer();
        if (sd != nullptr && BattleConfig.EquipSelfBreakRate != 0)
        {
            // Self weapon breaking
            int32 rate = BattleConfig.EquipNaturalBreakRate;
            if (sc != nullptr)
            {
                if (sc->HasSCE(ROWOW_SC_OVERTHRUST) == true)
                    rate += 10;
                if (sc->HasSCE(ROWOW_SC_MAXOVERTHRUST) == true)
                    rate += 10;
            }
            if (rate != 0)
                SkillBreakEquip(src, src, ROWOW_EQUIP_POSITION_HAND_R, rate, ROWOW_BCT_SELF);
        }
        if (BattleConfig.EquipSkillBreakRate != 0 && skillID != ROWOW_SKILL_WS_CARTTERMINATION && skillID != ROWOW_SKILL_ITM_TOMAHAWK)
        {
            // Cart Termination/Tomahawk won't trigger breaking data. Why? No idea, go ask Gravity.  Target weapon breaking
            int32 rate = 0;
            if (sd != nullptr)
                rate += GetDeferredBonus(src, "bBreakWeaponRate");
            if (sc != nullptr)
            {
                if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_MELTDOWN))
                    rate += sce->Val2;
                if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_WEAPONBREAKER))
                    rate += sce->Val2;
            }
            if (rate != 0)
                SkillBreakEquip(src, bl, ROWOW_EQUIP_POSITION_HAND_R, rate, ROWOW_BCT_ENEMY);
            // Target armor breaking
            rate = 0;
            if (sd != nullptr)
                rate += GetDeferredBonus(src, "bBreakArmorRate");
            if (ROWOWStatusChangeEntry const* sce = sc != nullptr ? sc->GetSCE(ROWOW_SC_MELTDOWN) : nullptr)
                rate += sce->Val3;
            if (rate != 0)
                SkillBreakEquip(src, bl, ROWOW_EQUIP_POSITION_ARMOR, rate, ROWOW_BCT_ENEMY);
        }
    }

    // Autospell when attacking
    SkillAutoSpellAdditionalEffect(src, bl, skillID, attackType, tick);
    return 0;
}

int32 ROWOWMod::SkillCounterAdditionalEffect(Unit* src, Unit* bl, uint16 skillID, uint16 skillLevel, int32 attackType, uint64 tick)
{
    // skill_counter_additional_effect: what being hit does to the attacker (the statuses the target's items give when hit, bAddEffWhenHit),
    // what a kill gives it (Soul Drain, the gain bonuses), the counter autospells and the autobonuses
    if (skillID > 0 && skillLevel == 0)
        return 0; // don't forget auto attacks! - celest
    ROWOWStatus const* dstStatus = GetROStatus(bl);
    if (bl->IsPlayer() == true && dstStatus != nullptr && (attackType & (ROWOW_BF_WEAPON | ROWOW_BF_MAGIC | ROWOW_BF_MISC)) != 0)
    {
        // Counter effects.
        for (ROWOWAddEffect const& effect : dstStatus->Bonus.AddEffAtked)
        {
            int32 rate = effect.Rate;
            if ((attackType & ROWOW_BF_LONG) != 0)
                rate += effect.ArrowRate;
            if (rate == 0)
                continue;
            if (IsAddEffectTriggered(effect.Flag, attackType) == false)
                continue;
            if ((effect.Flag & ROWOW_ATF_TARGET) != 0 && src != bl)
                StatusChangeStart(src, src, effect.Type, rate, 7, 0, 0, 0, effect.Duration, ROWOW_SCSTART_NONE, 100);
            if ((effect.Flag & ROWOW_ATF_SELF) != 0 && bl->IsAlive() == true)
                StatusChangeStart(src, bl, effect.Type, rate, 7, 0, 0, 0, effect.Duration, ROWOW_SCSTART_NONE, 100);
        }
    }
    if (ROWOWSkillImpl const* impl = GetSkillImpl(skillID))
        impl->ApplyCounterAdditionalEffects(src, bl, skillLevel, tick, attackType);

    // SG_MIRACLE [Komurka]: a Star Gladiator's hit may bring Miracle of the Sun, Moon and Stars (0.005% per sg_miracle_skill_ratio, less with
    // AGI over 46).  (No RO map has nosunmoonstarmiracle)
    Player* sd = src->ToPlayer();
    if (sd != nullptr && GetIdentity(sd)->JobID == ROWOW_JOB_STAR_GLADIATOR)
    {
        int32 agi = GetROStatus(sd)->Stats[ROWOW_STAT_AGI];
        if (int32(urand(0, 19999)) < BattleConfig.SgMiracleSkillRatio && (agi <= 46 || int32(urand(0, uint32(agi - 1))) < 46))
            SCStart(src, src, ROWOW_SC_MIRACLE, 100, 1, BattleConfig.SgMiracleSkillDuration);
    }

    // Soul Drain should only work on targetted spells [Skotlex]: a magic skill's kill gives SP back by the monster's level
    ROWOWSkill const* skill = GetSkill(skillID);
    int32 rate = 0;
    if (sd != nullptr && skill != nullptr && (attackType & ROWOW_BF_MAGIC) != 0 && bl->IsAlive() == false &&
        (skill->Inf & (ROWOW_INF_GROUND_SKILL | ROWOW_INF_SELF_SKILL)) == 0 && (rate = PCCheckSkill(sd, ROWOW_SKILL_HW_SOULDRAIN)) > 0)
    {
        if (sd->IsSitState() == true)
            sd->SetStandState(UNIT_STAND_STATE_STAND); // Character stuck in attacking animation while 'sitting' fix. [Skotlex]
        if (ROWOWStatus const* tstatus = GetROStatus(bl))
            StatusHeal(src, 0, int64(tstatus->Level) * (95 + 15 * rate) / 100, 2, src, 0);
    }

    // HP and SP gained on a kill
    if (sd != nullptr && bl->IsAlive() == false)
    {
        int32 sp = 0, hp = 0;
        ROWOWStatus const* tstatus = GetROStatus(bl);
        if ((attackType & (ROWOW_BF_WEAPON | ROWOW_BF_SHORT)) == (ROWOW_BF_WEAPON | ROWOW_BF_SHORT))
        {
            sp += GetDeferredBonus(src, "bSPGainValue");
            if (tstatus != nullptr)
                sp += GetDeferredBonus2(src, "bSPGainRace", tstatus->Race) + GetDeferredBonus2(src, "bSPGainRace", ROWOW_RACE_ALL);
            hp += GetDeferredBonus(src, "bHPGainValue");
        }
        if ((attackType & ROWOW_BF_MAGIC) != 0)
        {
            sp += GetDeferredBonus(src, "bMagicSPGainValue");
            hp += GetDeferredBonus(src, "bMagicHPGainValue");
        }
        if (hp != 0 || sp != 0) // updated to force healing to allow healing through berserk
            StatusHeal(src, hp, sp, 3, src, GetSkillSpellID(skillID, skillLevel));
    }

    // Trigger counter-spells to retaliate against damage causing skills, and the autobonuses of the attack
    SkillAutoSpellCounterAdditionalEffect(src, bl, skillID, attackType, tick);
    return 0;
}

int32 ROWOWMod::SkillCalcHeal(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, bool heal) const
{
    // skill_calc_heal: a heal skill's amount (halved when it's damage to the undead), raised by the heal bonuses and the target's Recovery
    // Potion, and cut by its Critical Wound.  (Offertorium is a third job's)
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (sstatus == nullptr)
        return 0;
    int32 hp = 0;
    int32 skill = 0;
    switch (skillID)
    {
        case ROWOW_SKILL_BA_APPLEIDUN:
            hp = 30 + 5 * skillLevel + (sstatus->Stats[ROWOW_STAT_VIT] / 2); // HP recovery
            hp += 5 * PCCheckSkill(src, ROWOW_SKILL_BA_MUSICALLESSON);
            break;
        case ROWOW_SKILL_PR_SANCTUARY:
            hp = (skillLevel > 6) ? 777 : skillLevel * 100;
            break;
        case ROWOW_SKILL_NPC_EVILLAND:
            hp = (skillLevel > 6) ? 666 : skillLevel * 100;
            break;
        default:
            if (skillLevel >= BattleConfig.MaxHealLv)
                return BattleConfig.MaxHeal;
            hp = (int32(sstatus->Level) + sstatus->Stats[ROWOW_STAT_INT]) / 8 * (4 + (skillLevel * 8));
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_HP_MEDITATIO)) > 0)
                hp += hp * skill * 2 / 100;
            else if ((skill = HomCheckSkill(src, ROWOW_SKILL_HLIF_BRAIN)) > 0)
                hp += hp * skill * 2 / 100;
            break;
    }
    if ((heal == false || GetFollowerType(target) == ROWOW_BL_MER) && skillID != ROWOW_SKILL_NPC_EVILLAND)
        hp /= 2;
    if (src->IsPlayer() == true && (skill = PCSkillHealBonus(src, skillID)) > 0)
        hp += hp * skill / 100;
    if (target != nullptr && target->IsPlayer() == true && (skill = PCSkillHeal2Bonus(target, skillID)) > 0)
        hp += hp * skill / 100;
    ROWOWStatusChange const* tsc = target != nullptr ? GetStatusChange(target) : nullptr;
    if (tsc != nullptr && tsc->IsEmpty() == false)
    {
        if (skillID != ROWOW_SKILL_NPC_EVILLAND && skillID != ROWOW_SKILL_BA_APPLEIDUN)
        {
            if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_INCHEALRATE))
                hp += hp * sce->Val1 / 100;
        }
        if (heal == true)
        {
            uint8 penalty = 0;
            if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_CRITICALWOUND))
                penalty += uint8(sce->Val2);
            if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_DEATHHURT); sce != nullptr && sce->Val3 == 1)
                penalty += 20;
            if (tsc->HasSCE(ROWOW_SC_NORECOVER_STATE) == true)
                penalty = 100;
            if (penalty > 0)
                hp -= hp * penalty / 100;
        }
    }
    return hp;
}

int32 ROWOWMod::PCSkillHealBonus(Unit const* unit, uint16 skillID) const
{
    // pc_skillheal_bonus: bHealPower on any heal skill, but the ones skill_add_heal_rate leaves out (1 Heal, 2 Sanctuary, 4 Aid Potion, 8 Slim
    // Potion Pitcher, 16 Apple of Idun), and the skill's bSkillHeal
    int32 bonus = GetDeferredBonus(unit, "bHealPower");
    if (bonus != 0)
    {
        switch (skillID)
        {
            case ROWOW_SKILL_AL_HEAL: if ((BattleConfig.SkillAddHealRate & 1) == 0) bonus = 0; break;
            case ROWOW_SKILL_PR_SANCTUARY: if ((BattleConfig.SkillAddHealRate & 2) == 0) bonus = 0; break;
            case ROWOW_SKILL_AM_POTIONPITCHER: if ((BattleConfig.SkillAddHealRate & 4) == 0) bonus = 0; break;
            case ROWOW_SKILL_CR_SLIMPITCHER: if ((BattleConfig.SkillAddHealRate & 8) == 0) bonus = 0; break;
            case ROWOW_SKILL_BA_APPLEIDUN: if ((BattleConfig.SkillAddHealRate & 16) == 0) bonus = 0; break;
        }
    }
    return bonus + GetDeferredBonus2(unit, "bSkillHeal", skillID);
}

int32 ROWOWMod::PCSkillHeal2Bonus(Unit const* unit, uint16 skillID) const
{
    // pc_skillheal2_bonus: bHealPower2 and the skill's bSkillHeal2
    return GetDeferredBonus(unit, "bHealPower2") + GetDeferredBonus2(unit, "bSkillHeal2", skillID);
}

int64 ROWOWMod::StatusHeal(Unit* bl, int64 hp, int64 sp, int32 flag, Unit* src, uint32 spellID)
{
    // status_heal: heals HP and SP, never past the maximums.  The heal shows whole as the spell's (RO's green number shows what the skill
    // heals, not what was missing).  Berserk blocks HP heals but the forced ones (flag&1).  (A negative heal is Akaitsuki's, a third job's)
    if (bl->IsAlive() == false)
        return 0;
    if (hp > 0 && (flag & 1) == 0)
    {
        if (ROWOWStatusChangeEntry const* sce = GetSCE(bl, ROWOW_SC_BERSERK); sce != nullptr)
            hp = 0;
    }
    if (hp > 0)
    {
        uint32 missingHP = bl->GetMaxHealth() - bl->GetHealth();
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID != 0 ? spellID : ROWOW_SPELL_USE_ITEM);
        if (spellInfo != nullptr)
        {
            HealInfo healInfo(src != nullptr ? src : bl, bl, uint32(std::min<int64>(hp, std::numeric_limits<uint32>::max())), spellInfo, SPELL_SCHOOL_MASK_NORMAL);
            (src != nullptr ? src : bl)->HealBySpell(healInfo);
        }
        hp = std::min<int64>(hp, missingHP);
    }
    else
        hp = 0;
    if (sp > 0 && bl->GetMaxPower(POWER_MANA) > 0)
    {
        sp = std::min<int64>(sp, int64(bl->GetMaxPower(POWER_MANA) - bl->GetPower(POWER_MANA)));
        bl->SetPower(POWER_MANA, bl->GetPower(POWER_MANA) + uint32(sp));
    }
    else
        sp = 0;
    return hp + sp;
}

void ROWOWMod::BattleDelayDamage(uint64 tick, int32 amotion, Unit* src, Unit* target, int32 attackType, uint16 skillID, uint16 skillLevel, int64 damage, uint8 dmgLv,
    int16 div, bool additionalEffects, bool isCritical)
{
    // battle_delay_damage: the hit lands after the attack motion (Aegis caps it at 1 second for non players), in the target's update, if
    // both are still on the map and the attacker didn't teleport away.  A devoted target whose Crusader is in reach takes none of it (the
    // Crusader takes it in skill_attack), which also keeps it from stopping a cast or triggering autospells (ATK_BLOCK)
    if (damage > 0 && skillID != ROWOW_SKILL_CR_REFLECTSHIELD && skillID != ROWOW_SKILL_PA_PRESSURE && GetDevotionSource(target, true) != nullptr)
    {
        Player* tsd = target->ToPlayer();
        if (tsd != nullptr && tsd->IsSitState() == true && BattleConfig.DevotionStandupFix != 0)
            tsd->SetStandState(UNIT_STAND_STATE_STAND); // pc_setstand
        damage = 0;
        if (dmgLv == ROWOW_ATK_DEF)
            dmgLv = ROWOW_ATK_BLOCK;
    }
    if (BattleConfig.DelayBattleDamage == 0)
        amotion = 1;
    else if (src->IsPlayer() == false && amotion > 1000)
        amotion = 1000;
    if (amotion <= 1)
    {
        BattleDamage(src, target, damage, div, skillLevel, skillID, dmgLv, attackType, additionalEffects, GetTick(), isCritical);
        return;
    }
    // The event is the target's own: it goes with the target, and runs in its update, where the attacker is looked up again
    ObjectGuid srcGUID = src->GetGUID();
    int32 distance = DistanceBL(src, target) + BattleConfig.AreaSize;
    target->m_Events.AddEventAtOffset([this, target, srcGUID, distance, damage, div, skillLevel, skillID, dmgLv, attackType, additionalEffects, isCritical]()
    {
        // battle_delay_damage_sub
        if (target->IsAlive() == false)
            return;
        Unit* delayedSrc = ObjectAccessor::GetUnit(*target, srcGUID);
        if (delayedSrc == nullptr || delayedSrc->IsInWorld() == false || delayedSrc->GetMap() != target->GetMap() || DistanceBL(delayedSrc, target) > distance)
            return; // Check to see if you haven't teleported. [Skotlex]
        BattleDamage(delayedSrc, target, damage, div, skillLevel, skillID, dmgLv, attackType, additionalEffects, GetTick(), isCritical);
    }, Milliseconds(std::max<int64>(1, int64(amotion) - int64(GetTick() - tick))));
}

void ROWOWMod::BattleDamage(Unit* src, Unit* target, int64 damage, int16 div, uint16 skillLevel, uint16 skillID, uint8 dmgLv, int32 attackType, bool additionalEffects,
    uint64 tick, bool isCritical)
{
    // battle_damage: status_fix_damage as WoW damage carrying the RO number (the client's RO digits, MobLogDamage through the damage hook,
    // death and exp), shown as the skill's spell; a miss shows RO's "Miss".  A coma hit (battle_check_coma) leaves 1 HP and 1 SP.  Then the
    // additional effects
    if (target == nullptr || src == nullptr || target->IsAlive() == false)
        return;
    uint32 spellID = GetSkillSpellID(skillID, skillLevel);
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID != 0 ? spellID : GetWeaponHitSoundSpellID(ROWOW_WEAPON_TYPE_FIST));
    bool isComa = src->IsPlayer() == true && BattleCheckComa(src, target, attackType) == true;
    if (spellInfo != nullptr && (dmgLv > ROWOW_ATK_BLOCK || isComa == true))
    {
        if (target->IsCreature() == true && target->ToCreature()->IsEvadingAttacks() == true)
            src->SendSpellMiss(target, spellInfo->Id, SPELL_MISS_EVADE);
        else if (damage <= 0 && isComa == false)
            src->SendSpellMiss(target, spellInfo->Id, SPELL_MISS_MISS);
        else
        {
            src->AtTargetAttacked(target, true);
            uint32 dealt = uint32(std::clamp<int64>(damage, 0, std::numeric_limits<uint32>::max()));
            SpellNonMeleeDamage log(src, target, spellInfo, SPELL_SCHOOL_MASK_NORMAL);
            log.damage = dealt;
            if (isCritical == true)
                log.HitInfo |= SPELL_HIT_TYPE_CRIT;
            src->SendSpellNonMeleeDamageLog(&log);
            if (isComa == true)
            {
                // Coma: the damage is logged as it was, but HP and SP are set to 1
                dealt = target->GetHealth() > 1 ? target->GetHealth() - 1 : 0;
                if (target->GetMaxPower(POWER_MANA) > 0)
                    target->SetPower(POWER_MANA, 1);
            }
            if (dealt > 0)
                Unit::DealDamage(src, target, dealt, nullptr, SPELL_DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, spellInfo, false);
        }
    }
    else if (spellInfo != nullptr)
        src->SendSpellMiss(target, spellInfo->Id, dmgLv == ROWOW_ATK_LUCKY ? SPELL_MISS_DODGE : SPELL_MISS_MISS);
    if (attackType != 0 && target->IsAlive() == true && additionalEffects == true)
        SkillAdditionalEffect(src, target, skillID, skillLevel, attackType, dmgLv, tick);
    if (dmgLv > ROWOW_ATK_BLOCK && attackType != 0 && additionalEffects == true)
        SkillCounterAdditionalEffect(src, target, skillID, skillLevel, attackType, tick);

    // These events trigger when the attack connects
    if (dmgLv > ROWOW_ATK_BLOCK)
        MobBattleAttacked(src, target, damage, div, tick, 0, false, attackType, skillID);
}

bool ROWOWMod::BattleCheckComa(Unit const* src, Unit const* target, int32 attackType) const
{
    // battle_check_coma: the coma bonuses (bComaClass, bComaRace, and for weapon attacks bWeaponComaClass, bWeaponComaEle, bWeaponComaRace;
    // the race and element ones not on status immune monsters), in 10000.  (The guild castle's monsters come with War of Emperium)
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (tstatus == nullptr || tstatus->Class == ROWOW_CLASS_BATTLEFIELD)
        return false;
    bool isStatusImmune = (tstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0;
    int32 rate = GetDeferredBonus2(src, "bComaClass", tstatus->Class) + GetDeferredBonus2(src, "bComaClass", ROWOW_CLASS_ALL);
    if (isStatusImmune == false)
        rate += GetDeferredBonus2(src, "bComaRace", tstatus->Race) + GetDeferredBonus2(src, "bComaRace", ROWOW_RACE_ALL);
    if ((attackType & ROWOW_BF_WEAPON) != 0)
    {
        rate += GetDeferredBonus2(src, "bWeaponComaClass", tstatus->Class) + GetDeferredBonus2(src, "bWeaponComaClass", ROWOW_CLASS_ALL);
        if (isStatusImmune == false)
        {
            rate += GetDeferredBonus2(src, "bWeaponComaEle", tstatus->DefElement) + GetDeferredBonus2(src, "bWeaponComaEle", ROWOW_ELEMENT_ALL);
            rate += GetDeferredBonus2(src, "bWeaponComaRace", tstatus->Race) + GetDeferredBonus2(src, "bWeaponComaRace", ROWOW_RACE_ALL);
        }
    }
    return rate > 0 && int32(urand(0, 9999)) < rate;
}

int32 ROWOWMod::SkillAddTimerSkill(Unit* src, uint64 tick, ObjectGuid targetGUID, int32 x, int32 y, uint16 skillID, uint16 skillLevel, int32 type, int32 flag)
{
    // skill_addtimerskill: queued on the caster and run by an event of the caster's at the tick, so in its own update; 1 when it has no room
    if (src->IsInWorld() == false)
        return 0;
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    if (ud->TimerSkills.size() >= ROWOW_MAX_SKILLTIMERSKILL)
        return 1;
    ROWOWSkillTimerSkill timerSkill;
    timerSkill.ID = ++ud->NextTimerSkillID;
    timerSkill.Tick = tick;
    timerSkill.TargetGUID = targetGUID;
    timerSkill.SkillID = skillID;
    timerSkill.SkillLevel = skillLevel;
    timerSkill.MapID = src->GetMapId();
    timerSkill.X = x;
    timerSkill.Y = y;
    timerSkill.Type = type;
    timerSkill.Flag = flag;
    ud->TimerSkills.push_back(timerSkill);
    uint32 timerSkillID = timerSkill.ID;
    src->m_Events.AddEventAtOffset([this, src, timerSkillID]()
    {
        SkillTimerSkill(src, timerSkillID);
    }, Milliseconds(std::max<int64>(1, int64(tick) - int64(GetTick()))));
    return 0;
}

void ROWOWMod::SkillTimerSkill(Unit* src, uint32 timerSkillID)
{
    // skill_timerskill: a queued part of a skill, if it's still queued (skill_cleartimerskill drops them), the caster and its target are on
    // one map, the caster is alive and the target too (but for Water Ball's, which go on).  Jupitel Thunder's hit, Water Ball's balls (each
    // uses up one of its cells while the target lives), Frost Joker's and Scream's effect around where they were used, Lex Divina's silence,
    // Status Recovery's blindness on the undead and Hammer Fall's stun (type their rate), Raging Palm Strike's hit, and the default: the skill's attack again
    // (Double Casting's bolt).  (No pre-renewal skill queues one on a cell)
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    auto timerSkillIter = std::find_if(ud->TimerSkills.begin(), ud->TimerSkills.end(), [timerSkillID](ROWOWSkillTimerSkill const& timerSkill)
    {
        return timerSkill.ID == timerSkillID;
    });
    if (timerSkillIter == ud->TimerSkills.end())
        return;
    ROWOWSkillTimerSkill skl = *timerSkillIter;
    ud->TimerSkills.erase(timerSkillIter);
    if (src->IsInWorld() == false || skl.TargetGUID.IsEmpty() == true)
        return; // Source not on Map
    Unit* target = ObjectAccessor::GetUnit(*src, skl.TargetGUID);
    if (skl.SkillID == ROWOW_SKILL_RG_INTIMIDATE && (target == nullptr || target->IsInWorld() == false || src->GetMap() != target->GetMap() ||
        DistanceBL(src, target) > BattleConfig.AreaSize))
        target = src; // Required since it has to warp.
    if (target == nullptr || target->IsInWorld() == false || src->GetMap() != target->GetMap())
        return; // Target offline, not on Map, or on another map
    if (src->IsAlive() == false)
        return; // Caster is Dead
    if (target->IsAlive() == false && skl.SkillID != ROWOW_SKILL_RG_INTIMIDATE && skl.SkillID != ROWOW_SKILL_WZ_WATERBALL)
        return;
    uint64 tick = GetTick();
    switch (skl.SkillID)
    {
        case ROWOW_SKILL_RG_INTIMIDATE:
        {
            // The caster to a random spot of the map, and the target (alive, and not the caster) onto a free cell beside it
            Position dest;
            if (UnitWarpRandom(src, dest) == true && target != src && target->IsAlive() == true)
                UnitWarpNear(target, dest);
            break;
        }
        case ROWOW_SKILL_WZ_WATERBALL:
        case ROWOW_SKILL_WZ_JUPITEL:
        {
            // Get the next waterball cell to consume
            std::shared_ptr<ROWOWSkillUnitGroup> unitGroup;
            int32 unitIndex = -1;
            if (skl.SkillID == ROWOW_SKILL_WZ_WATERBALL)
            {
                ROWOWSkillUnitLayout const* layout = SkillGetUnitLayout(skl.SkillID, skl.SkillLevel, src, skl.X, skl.Y);
                for (int32 i = skl.Type; i >= 0 && i < int32(layout->Cells.size()); i++)
                {
                    unitGroup = MapFindSkillUnitOnCell(src, skl.X + layout->Cells[i].first, skl.Y + layout->Cells[i].second, ROWOW_SKILL_WZ_WATERBALL, 0, unitIndex);
                    if (unitGroup != nullptr)
                        break;
                }
            }
            // Official behaviour is to hit as long as there is a line of sight, regardless of distance
            if (skl.Type > 0 && target->IsAlive() == true && src->IsWithinLOSInMap(target) == true)
            {
                // Apply canact delay here to prevent hacks (unlimited casting)
                if (ROWOWStatus const* status = GetROStatus(src))
                    ud->CanActTime = std::max<uint64>(tick + uint64(status->AttackMotion), ud->CanActTime);
                SkillAttack(ROWOW_BF_MAGIC, src, src, target, skl.SkillID, skl.SkillLevel, tick, skl.Flag);
            }
            if (unitGroup != nullptr && target->IsAlive() == true && src->IsAlive() == true)
            {
                // Consume unit for next waterball.  (The timer goes on until the target dies, even with no line of sight now)
                SkillDelUnit(src->GetMap(), *unitGroup, unitIndex);
                SkillAddTimerSkill(src, tick + ROWOW_TIMERSKILL_INTERVAL, skl.TargetGUID, skl.X, skl.Y, skl.SkillID, skl.SkillLevel, skl.Type + 1, skl.Flag);
            }
            break;
        }
        case ROWOW_SKILL_BA_FROSTJOKER:
        case ROWOW_SKILL_DC_SCREAM:
        {
            // skill_frostjoke_scream on everyone around (enemies and the party alike, the caster aside), but the invisible
            int32 range = SkillGetSplash(skl.SkillID, skl.SkillLevel);
            MapForEachInArea(src, skl.X - range, skl.Y - range, skl.X + range, skl.Y + range, [this, src, &skl, tick](Unit* bl)
            {
                if (src == bl || bl->IsAlive() == false)
                    return 0;
                // It has been reported that Scream/Joke works the same regardless of woe-setting. [Skotlex]
                if (BattleCheckTarget(src, bl, ROWOW_BCT_ENEMY | ROWOW_BCT_PARTY) > 0)
                    SkillAdditionalEffect(src, bl, skl.SkillID, skl.SkillLevel, ROWOW_BF_MISC, ROWOW_ATK_DEF, tick);
                return 0;
            });
            break;
        }
        case ROWOW_SKILL_MER_LEXDIVINA:
        case ROWOW_SKILL_PR_LEXDIVINA:
            if (src->IsPlayer() == false)
            {
                // Monsters use the default duration when casting Lex Divina
                SCStart(src, target, ROWOW_SC_SILENCE, skl.Type, skl.SkillLevel, SkillGetTime2(StatusDBGetSkill(ROWOW_SC_SILENCE), 1));
                break;
            }
            SCStart(src, target, ROWOW_SC_SILENCE, skl.Type, skl.SkillLevel, SkillGetTime2(skl.SkillID, skl.SkillLevel));
            break;
        case ROWOW_SKILL_PR_STRECOVERY:
            SCStart(src, target, ROWOW_SC_BLIND, skl.Type, skl.SkillLevel, SkillGetTime2(skl.SkillID, skl.SkillLevel));
            break;
        case ROWOW_SKILL_HVAN_EXPLOSION:
            StatusKill(src);
            break;
        case ROWOW_SKILL_BS_HAMMERFALL:
            SCStart(src, target, ROWOW_SC_STUN, skl.Type, skl.SkillLevel, SkillGetTime2(skl.SkillID, skl.SkillLevel));
            break;
        case ROWOW_SKILL_CH_PALMSTRIKE:
        {
            // A hidden target or caster isn't hit, only pushed back
            ROWOWStatusChange const* tsc = GetStatusChange(static_cast<Unit const*>(target));
            ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
            if ((tsc != nullptr && (tsc->Option & ROWOW_OPTION_HIDE) != 0) || (sc != nullptr && (sc->Option & ROWOW_OPTION_HIDE) != 0))
            {
                ROWOWSkill const* skill = GetSkill(skl.SkillID);
                SkillBlown(src, target, skill != nullptr ? ROWOWSkill::GetLevelValue(skill->Knockback, skl.SkillLevel) : 0, -1, ROWOW_BLOWN_NONE);
                break;
            }
            SkillAttack(skl.Type, src, src, target, skl.SkillID, skl.SkillLevel, tick, skl.Flag);
            break;
        }
        default:
            SkillAttack(skl.Type, src, src, target, skl.SkillID, skl.SkillLevel, tick, skl.Flag);
            break;
    }
}

int32 ROWOWMod::SkillIsCombo(uint16 skillID)
{
    // skill_is_combo: 1 a normal combo, 2 one that prioritizes auto-target even if val2 is set (no pre-renewal skill)
    switch (skillID)
    {
        case ROWOW_SKILL_MO_CHAINCOMBO:
        case ROWOW_SKILL_MO_COMBOFINISH:
        case ROWOW_SKILL_CH_TIGERFIST:
        case ROWOW_SKILL_CH_CHAINCRUSH:
        case ROWOW_SKILL_MO_EXTREMITYFIST:
        case ROWOW_SKILL_TK_TURNKICK:
        case ROWOW_SKILL_TK_STORMKICK:
        case ROWOW_SKILL_TK_DOWNKICK:
        case ROWOW_SKILL_TK_COUNTER:
        case ROWOW_SKILL_TK_JUMPKICK:
        case ROWOW_SKILL_HT_POWER:
            return 1;
    }
    return 0;
}

bool ROWOWMod::UnitCancelCombo(Unit* bl)
{
    // unit_cancel_combo: the combo window ends, and the unit can't swing again until its attack motion passed (WoW's swing timer)
    if (StatusChangeEnd(bl, ROWOW_SC_COMBO) == false)
        return false; // Combo wasn't active.
    if (ROWOWStatus const* status = GetROStatus(bl); status != nullptr && bl->IsPlayer() == true)
        bl->ToPlayer()->setAttackTimer(BASE_ATTACK, std::max<uint32>(bl->getAttackTimer(BASE_ATTACK), uint32(status->AttackMotion)));
    return true;
}

void ROWOWMod::SkillCombo(Unit* src, Unit* dsrc, Unit* bl, uint16 skillID, uint16 skillLevel, uint64 tick)
{
    // skill_combo: a skill hit ends the combo it was used in (but a ground skill's unit's), then a Monk's or Champion's skill opens the next
    // window when the next skill is learned and the spheres it needs are there: SC_COMBO from the after-cast delay's end (canact_tick, never
    // less than the attack motion), val1 the skill, val2 0 (the next skill auto-targets).  The window's 300 ms are SC_COMBO's own.  A Hunter's
    // Double Strafe opens Beast Strafing for 2 seconds.  A Taekwon's kick ends the window its stance opened (unit_cancel_combo); a Taekwon
    // ranker's goes on, as a kick chain
    int64 duration = 0; // Set to duration the user can use a combo skill or 1 for aftercast delay of pre-skill
    int32 nodelay = 0; // Set to 1 for no walk/attack delay, set to 2 for no walk delay
    int32 targetID = int32(bl->GetGUID().GetCounter()); // Set to 0 if combo skill should not autotarget

    // End previous combo state after skill is invoked
    if (GetSCE(src, ROWOW_SC_COMBO) != nullptr)
    {
        switch (skillID)
        {
            case ROWOW_SKILL_TK_TURNKICK:
            case ROWOW_SKILL_TK_STORMKICK:
            case ROWOW_SKILL_TK_DOWNKICK:
            case ROWOW_SKILL_TK_COUNTER:
                UnitCancelCombo(src); // Cancel combo wait
                break;
            default:
                if (src == dsrc) // Ground skills are exceptions. [Inkfish]
                    StatusChangeEnd(src, ROWOW_SC_COMBO);
                break;
        }
    }

    // start new combo
    if (Player* sd = src->ToPlayer())
    {
        // player only
        int32 spheres = PCGetSpiritBall(sd);
        bool hasFury = GetSCE(sd, ROWOW_SC_EXPLOSIONSPIRITS) != nullptr;
        switch (skillID)
        {
            case ROWOW_SKILL_MO_TRIPLEATTACK:
                if (PCCheckSkill(sd, ROWOW_SKILL_MO_CHAINCOMBO) > 0)
                {
                    duration = 1;
                    targetID = 0; // Will target current auto-target instead
                }
                break;
            case ROWOW_SKILL_MO_CHAINCOMBO:
                if (PCCheckSkill(sd, ROWOW_SKILL_MO_COMBOFINISH) > 0 && spheres >= 1)
                {
                    duration = 1;
                    targetID = 0; // Will target current auto-target instead
                }
                break;
            case ROWOW_SKILL_MO_COMBOFINISH:
                PartySkillCheck(sd, skillID, skillLevel); // bonus from SG_FRIEND [Komurka]
                if (PCCheckSkill(sd, ROWOW_SKILL_CH_TIGERFIST) > 0 && spheres >= 1)
                {
                    duration = 1;
                    targetID = 0; // Will target current auto-target instead
                }
                else if (PCCheckSkill(sd, ROWOW_SKILL_CH_CHAINCRUSH) > 0 && spheres >= 2)
                {
                    duration = 1;
                    targetID = 0;
                }
                else if (PCCheckSkill(sd, ROWOW_SKILL_MO_EXTREMITYFIST) > 0 && spheres >= 4 && hasFury == true)
                {
                    duration = 1;
                    targetID = 0;
                }
                break;
            case ROWOW_SKILL_CH_TIGERFIST:
                if (PCCheckSkill(sd, ROWOW_SKILL_CH_CHAINCRUSH) > 0 && spheres >= 2)
                {
                    duration = 1;
                    targetID = 0; // Will target current auto-target instead
                }
                break;
            case ROWOW_SKILL_CH_CHAINCRUSH:
                if (PCCheckSkill(sd, ROWOW_SKILL_MO_EXTREMITYFIST) > 0 && spheres >= 1 && hasFury == true)
                {
                    duration = 1;
                    targetID = 0; // Will target current auto-target instead
                }
                else if (PCCheckSkill(sd, ROWOW_SKILL_CH_TIGERFIST) > 0 && spheres >= 1)
                {
                    duration = 1;
                    targetID = 0;
                }
                break;
            case ROWOW_SKILL_AC_DOUBLE:
                // Double Strafe opens Beast Strafing for 2 seconds
                if (PCCheckSkill(sd, ROWOW_SKILL_HT_POWER) > 0)
                {
                    duration = 2000;
                    nodelay = 1; // Neither gives walk nor attack delay
                    targetID = 0; // Does not need to be used on previous target
                }
                break;
        }
    }

    if (duration != 0)
    {
        // Possible to chain
        ROWOWSkillUnitData const* ud = GetSkillUnitData(src);
        if (src->IsPlayer() == true && duration == 1)
            duration = int64(ud->CanActTime) - int64(tick); // Auto calc duration
        ROWOWStatus const* status = GetROStatus(src);
        duration = std::max<int64>(status != nullptr ? status->AttackMotion : 0, duration); // Never less than aMotion
        SCStart4(src, src, ROWOW_SC_COMBO, 100, skillID, targetID, nodelay, 0, duration);
    }
}

void ROWOWMod::SkillClearTimerSkill(Unit* src)
{
    // skill_cleartimerskill: the caster's queued timer skills are dropped (it died or left its map); their events find nothing to run
    if (ROWOWSkillUnitData* ud = src->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData"))
        ud->TimerSkills.clear();
}

void ROWOWStatusSkillImpl::CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const
{
    // StatusSkillImpl::castendNoDamageId: the skill's status on the target for its Duration1, or ended when it runs and that's asked
    ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
    if (skill == nullptr || skill->StatusID == ROWOW_SC_NONE)
        return;
    if (IsEndIfRunning == true)
    {
        ROWOWStatusChange const* tsc = ROWOW->GetStatusChange(static_cast<Unit const*>(target));
        if (tsc != nullptr && tsc->HasSCE(skill->StatusID) == true)
        {
            ROWOW->StatusChangeEnd(target, skill->StatusID);
            return;
        }
    }
    ROWOW->SCStart(src, target, skill->StatusID, 100, skillLevel, ROWOWSkill::GetLevelValue(skill->Duration1, skillLevel));
}

void ROWOWWeaponSkillImpl::CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const
{
    // WeaponSkillImpl::castendDamageId
    ROWOW->SkillAttack(ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, flag);
}

void ROWOWRecursiveDamageSplashSkillImpl::CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const
{
    // SkillImplRecursiveDamageSplash::castendDamageId: the first call searches the area around the target, which calls back with flag&1 for
    // each unit found.  The search's skill_area_temp is the caster's, put back after it
    ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
    ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
    if (skill == nullptr)
        return;
    if ((flag & 1) != 0)
    {
        // Recursive invocation.  (Hovering's immunity to ground and trap skills, status)
        int32 sflag = ud->AreaTemp.Values[0] & 0xFFF;
        if ((flag & ROWOW_SD_LEVEL) != 0)
            sflag |= ROWOW_SD_LEVEL; // -1 will be used in packets instead of the skill level
        if (ud->AreaTemp.TargetGUID != target->GetGUID() && skill->Inf2[ROWOW_INF2_ISNPC] == false)
            sflag |= ROWOW_SD_ANIMATION; // original target gets no animation (as well as all NPC skills)
        SplashDamage(src, target, skillLevel, tick, sflag);
    }
    else
    {
        ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
        ud->AreaTemp = ROWOWSkillAreaTemp();
        ud->AreaTemp.TargetGUID = target->GetGUID();
        SplashSearch(src, target, skillLevel, tick, flag);
        ud->AreaTemp = outerAreaTemp;
    }
}

void ROWOWRecursiveDamageSplashSkillImpl::CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 tick, int32& flag) const
{
    // SkillImplRecursiveDamageSplash::castendPos2: every enemy in the splash area around the ground spot (the cast center kept for the
    // knockback direction)
    ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
    ROWOWSkillAreaTemp outerAreaTemp = ud->AreaTemp;
    ud->AreaTemp.Values[4] = x;
    ud->AreaTemp.Values[5] = y;
    int16 size = GetSplashSearchSize(src, skillLevel);
    int32 splashFlag = flag | ROWOW_BCT_ENEMY | 1;
    ROWOW->MapForEachInArea(src, x - size, y - size, x + size, y + size, [this, src, skillLevel, tick, splashFlag](Unit* bl)
    {
        return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, splashFlag, ROWOWSkillAreaFunc::CastendDamageID);
    });
    ROWOW->SkillTrapsInSplash(src, SkillID, skillLevel, x, y, size);
    ud->AreaTemp = outerAreaTemp;
}

int16 ROWOWRecursiveDamageSplashSkillImpl::GetSearchSize(Unit* /*src*/, uint16 skillLevel) const
{
    ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
    return int16(skill != nullptr ? ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel) : 0);
}

int16 ROWOWRecursiveDamageSplashSkillImpl::GetSplashSearchSize(Unit* /*src*/, uint16 skillLevel) const
{
    ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
    return int16(skill != nullptr ? ROWOWSkill::GetLevelValue(skill->SplashArea, skillLevel) : 0);
}

void ROWOWRecursiveDamageSplashSkillImpl::SplashSearch(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32 flag) const
{
    // SkillImplRecursiveDamageSplash::splashSearch.  If skill damage should be split among targets, count them (SD_LEVEL: forced splash
    // damage counts targets).  If there are no characters in the area, then it always counts as if there was one target
    ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
    ROWOWSkillUnitData* ud = ROWOW->GetSkillUnitData(src);
    if ((flag & ROWOW_SD_LEVEL) != 0 || (skill != nullptr && skill->NK[ROWOW_NK_SPLASHSPLIT] == true))
    {
        ud->AreaTemp.Values[0] = ROWOW->MapForEachInRange(target, GetSearchSize(src, skillLevel), [this, src, skillLevel, tick](Unit* bl)
        {
            return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, ROWOW_BCT_ENEMY, ROWOWSkillAreaFunc::Count);
        });
        ud->AreaTemp.Values[0] = std::max(1, ud->AreaTemp.Values[0]);
    }

    // recursive invocation of skill_castend_damage_id() with flag|1
    int32 splashFlag = flag | ROWOW_BCT_ENEMY | ROWOW_SD_SPLASH | 1;
    ROWOW->MapForEachInRange(target, GetSplashSearchSize(src, skillLevel), [this, src, skillLevel, tick, splashFlag](Unit* bl)
    {
        return ROWOW->SkillAreaSub(bl, src, SkillID, skillLevel, tick, splashFlag, ROWOWSkillAreaFunc::CastendDamageID);
    });
    int32 centerX = 0, centerY = 0;
    ROWOW->GetUnitCell(target, centerX, centerY);
    ROWOW->SkillTrapsInSplash(src, SkillID, skillLevel, centerX, centerY, GetSplashSearchSize(src, skillLevel));
}

int64 ROWOWRecursiveDamageSplashSkillImpl::SplashDamage(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32 flag) const
{
    // SkillImplRecursiveDamageSplash::splashDamage
    ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
    return ROWOW->SkillAttack(skill != nullptr ? skill->AttackType : ROWOW_BF_WEAPON, src, src, target, SkillID, skillLevel, tick, flag);
}
