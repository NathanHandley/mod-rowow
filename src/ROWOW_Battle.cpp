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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/battle.cpp, status.cpp and mob.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Creature.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellMgr.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// RO combat between RO combatants: rAthena's pre-renewal battle code, keeping its function names (in the comments beside each and in
// ROWOW.h) and structure so the port can be checked against it: the normal attack, and the weapon, magic and misc attacks of skills.
// The skill-specific parts are each skill's ROWOWSkillImpl (CalculateSkillRatio, ModifyHitRate, ModifyElement, ModifyDamageData); the
// cases rAthena still keeps in this file's switches come with their jobs (R6.5), and the status changes' parts are those of rAthena's
// pre-renewal statuses (the third jobs' are left out).  All of it runs in the map's thread, which the attacker and its target share
//
// rAthena's macros, written out: ATK_RATE (damage * rate / 100, the left hand's too when attacking with it), ATK_ADDRATE (+ damage *
// rate / 100), ATK_ADD (+ value), and the *2 forms with a separate value for the left hand.  rnd() % n is urand(0, n - 1)

namespace
{
    // rAthena's names for the elements, sizes, races and monster classes, as mob_db and item scripts write them
    char const* const ElementNames[ROWOW_ELEMENT_COUNT] = { "Neutral", "Water", "Earth", "Fire", "Wind", "Poison", "Holy", "Dark", "Ghost", "Undead" };
    char const* const SizeNames[ROWOW_SIZE_COUNT] = { "Small", "Medium", "Large" };
    char const* const RaceNames[] = { "Formless", "Undead", "Brute", "Plant", "Insect", "Fish", "Demon", "Demihuman", "Angel", "Dragon" };
    char const* const ClassNames[] = { "Normal", "Boss", "Guardian", "Battlefield", "Event" };

    // rAthena's e_aegis_monstertype: the modes of each monster AI (mob_db's Ai)
    std::unordered_map<int32, uint32> const MonsterTypeModes = { { 1, 0x81 }, { 2, 0x83 }, { 3, 0x1089 }, { 4, 0x3885 }, { 5, 0x2085 }, { 6, 0 },
        { 7, 0x108B }, { 8, 0x7085 }, { 9, 0x3095 }, { 10, 0x84 }, { 11, 0x84 }, { 12, 0x2085 }, { 13, 0x308D }, { 17, 0x91 }, { 19, 0x3095 },
        { 20, 0x3295 }, { 21, 0x3695 }, { 24, 0xA1 }, { 25, 0x1 }, { 26, 0xB695 }, { 27, 0x8084 } };

    // A name's index in one of the tables above (any case), or -1
    int32 GetNameIndex(char const* const* names, size_t nameCount, std::string const& name)
    {
        for (size_t nameIndex = 0; nameIndex < nameCount; nameIndex++)
        {
            std::string const tableName = names[nameIndex];
            if (tableName.size() == name.size() && std::equal(tableName.begin(), tableName.end(), name.begin(),
                [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); }))
                return int32(nameIndex);
        }
        return -1;
    }

    // ATK_ADD2: the left hand's only when the attack is left-handed
    void AttackAdd(ROWOWDamage& wd, bool isLeftHanded, int64 value, int64 leftValue)
    {
        wd.Damage += value;
        if (isLeftHanded == true)
            wd.Damage2 += leftValue;
    }

    // ATK_RATE and ATK_ADDRATE
    void AttackRate(ROWOWDamage& wd, bool isLeftHanded, int64 rate)
    {
        wd.Damage = wd.Damage * rate / 100;
        if (isLeftHanded == true)
            wd.Damage2 = wd.Damage2 * rate / 100;
    }
    void AttackAddRate(ROWOWDamage& wd, bool isLeftHanded, int64 rate)
    {
        wd.Damage += wd.Damage * rate / 100;
        if (isLeftHanded == true)
            wd.Damage2 += wd.Damage2 * rate / 100;
    }

    // The jobs in the Thief line (rAthena's MAPID_THIEF with the first-job mask): Thief, Assassin, Rogue and their transcendent jobs
    bool IsThiefClass(uint16 jobID)
    {
        switch (jobID)
        {
            case 6: case 12: case 17: case 4007: case 4013: case 4018:
                return true;
            default:
                return false;
        }
    }

    // battle_check_undead with undead_detect_type 0: the undead element
    bool IsUndead(ROWOWStatus const& status)
    {
        return ROWOWMod::BattleCheckUndead(status.Race, status.DefElement);
    }

    // The weapon hit sounds' spells, as the converter numbers them from ROWOW_SPELL_WEAPON_HIT_SOUND_START (its RO_WEAPON_HIT_SOUND_NAMES)
    enum WeaponHitSound
    {
        WEAPON_HIT_SOUND_SWORD = 0,
        WEAPON_HIT_SOUND_SPEAR = 1,
        WEAPON_HIT_SOUND_AXE = 2,
        WEAPON_HIT_SOUND_MACE = 3,
        WEAPON_HIT_SOUND_ROD = 4,
        WEAPON_HIT_SOUND_ARROW = 5,
        WEAPON_HIT_SOUND_HANDGUN = 6,
        WEAPON_HIT_SOUND_RIFLE = 7,
    };

    // DAMAGE_DIV_FIX
    void DamageDivFix(int64& damage, int16& div)
    {
        if (div < 0)
        {
            div *= -1;
            damage /= div;
        }
        damage *= div;
    }
}

int32 ROWOWMod::GetElementByName(std::string const& name)
{
    return GetNameIndex(ElementNames, ROWOW_ELEMENT_COUNT, name);
}

void ROWOWMod::LoadBattleData()
{
    MobStatusesByMobID.clear();
    for (uint8 level = 0; level < ROWOW_ELEMENT_LEVEL_MAX; level++)
        for (uint8 attackElement = 0; attackElement < ROWOW_ELEMENT_COUNT; attackElement++)
            for (uint8 defenseElement = 0; defenseElement < ROWOW_ELEMENT_COUNT; defenseElement++)
                AttrFix[level][attackElement][defenseElement] = 100;
    if (IsEnabled == false)
        return;

    // attr_fix.yml: a row for each element level, holding for each attacking element its damage against each defending element
    if (ROWOWDataTable const* attrFixTable = GetDataTable(ROWOW_DATA_TABLE_ATTR_FIX))
    {
        int32 levelColumnIndex = attrFixTable->GetColumnIndex("Level");
        for (size_t rowIndex = 0; rowIndex < attrFixTable->Rows.size(); rowIndex++)
        {
            int64 level = attrFixTable->GetInt(rowIndex, levelColumnIndex, 0);
            if (level < 1 || level > ROWOW_ELEMENT_LEVEL_MAX)
                continue;
            for (uint8 attackElement = 0; attackElement < ROWOW_ELEMENT_COUNT; attackElement++)
            {
                for (auto const& ratioField : ROWOWDataTable::ParseMap(attrFixTable->GetCell(rowIndex, attrFixTable->GetColumnIndex(ElementNames[attackElement]))))
                {
                    int32 defenseElement = GetElementByName(ratioField.first);
                    if (defenseElement >= 0)
                        AttrFix[level - 1][attackElement][defenseElement] = int32(strtol(ratioField.second.c_str(), nullptr, 10));
                }
            }
        }
    }
    else
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadBattleData found no {} table, so every element deals full damage to every other", ROWOW_DATA_TABLE_ATTR_FIX);

    // Each monster's status, as MobDatabase::parseBodyNode and loadingFinished leave it (mob_db's defaults for what a row leaves out)
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    if (mobTable == nullptr)
        return;
    static char const* const statColumnNames[ROWOW_STAT_COUNT] = { "Str", "Agi", "Vit", "Int", "Dex", "Luk" };
    int32 idColumnIndex = mobTable->GetColumnIndex("Id");
    int32 levelColumnIndex = mobTable->GetColumnIndex("Level");
    int32 hpColumnIndex = mobTable->GetColumnIndex("Hp");
    int32 spColumnIndex = mobTable->GetColumnIndex("Sp");
    int32 attackColumnIndex = mobTable->GetColumnIndex("Attack");
    int32 attack2ColumnIndex = mobTable->GetColumnIndex("Attack2");
    int32 defenseColumnIndex = mobTable->GetColumnIndex("Defense");
    int32 magicDefenseColumnIndex = mobTable->GetColumnIndex("MagicDefense");
    int32 attackRangeColumnIndex = mobTable->GetColumnIndex("AttackRange");
    int32 sizeColumnIndex = mobTable->GetColumnIndex("Size");
    int32 raceColumnIndex = mobTable->GetColumnIndex("Race");
    int32 elementColumnIndex = mobTable->GetColumnIndex("Element");
    int32 elementLevelColumnIndex = mobTable->GetColumnIndex("ElementLevel");
    int32 attackDelayColumnIndex = mobTable->GetColumnIndex("AttackDelay");
    int32 attackMotionColumnIndex = mobTable->GetColumnIndex("AttackMotion");
    int32 damageMotionColumnIndex = mobTable->GetColumnIndex("DamageMotion");
    int32 damageTakenColumnIndex = mobTable->GetColumnIndex("DamageTaken");
    int32 classColumnIndex = mobTable->GetColumnIndex("Class");
    int32 aiColumnIndex = mobTable->GetColumnIndex("Ai");
    int32 modesColumnIndex = mobTable->GetColumnIndex("Modes");
    int32 walkSpeedColumnIndex = mobTable->GetColumnIndex("WalkSpeed");
    for (size_t rowIndex = 0; rowIndex < mobTable->Rows.size(); rowIndex++)
    {
        uint32 mobID = uint32(mobTable->GetInt(rowIndex, idColumnIndex, 0));
        ROWOWStatus& status = MobStatusesByMobID[mobID];
        status = ROWOWStatus();
        status.IsPlayer = false;
        status.Level = uint32(std::max<int64>(1, mobTable->GetInt(rowIndex, levelColumnIndex, 1)));
        status.MaxHP = uint32(std::max<int64>(1, mobTable->GetInt(rowIndex, hpColumnIndex, 1)));
        status.MaxSP = uint32(std::max<int64>(1, mobTable->GetInt(rowIndex, spColumnIndex, 1)));
        status.WeaponAttack = int32(mobTable->GetInt(rowIndex, attackColumnIndex, 0));
        status.WeaponRefineAttack = int32(mobTable->GetInt(rowIndex, attack2ColumnIndex, 0));
        status.Def = std::clamp<int32>(int32(mobTable->GetInt(rowIndex, defenseColumnIndex, 0)), CHAR_MIN, CHAR_MAX); // DEFTYPE_MIN/MAX, pre-renewal
        status.MDef = std::clamp<int32>(int32(mobTable->GetInt(rowIndex, magicDefenseColumnIndex, 0)), CHAR_MIN, CHAR_MAX);
        for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
            status.Stats[statIndex] = std::max<int32>(0, int32(mobTable->GetInt(rowIndex, mobTable->GetColumnIndex(statColumnNames[statIndex]), 1)));
        status.Range = int32(mobTable->GetInt(rowIndex, attackRangeColumnIndex, 0));
        status.Range2 = int32(mobTable->GetInt(rowIndex, mobTable->GetColumnIndex("SkillRange"), 0));
        status.Range3 = int32(mobTable->GetInt(rowIndex, mobTable->GetColumnIndex("ChaseRange"), 0));
        status.Size = uint8(std::max(0, GetNameIndex(SizeNames, ROWOW_SIZE_COUNT, mobTable->GetCell(rowIndex, sizeColumnIndex))));
        status.Race = uint8(std::max(0, GetNameIndex(RaceNames, std::size(RaceNames), mobTable->GetCell(rowIndex, raceColumnIndex))));
        status.DefElement = uint8(std::max(0, GetElementByName(mobTable->GetCell(rowIndex, elementColumnIndex))));
        status.ElementLevel = uint8(std::clamp<int64>(mobTable->GetInt(rowIndex, elementLevelColumnIndex, 1), 1, ROWOW_ELEMENT_LEVEL_MAX));
        status.Class = uint8(std::max(0, GetNameIndex(ClassNames, std::size(ClassNames), mobTable->GetCell(rowIndex, classColumnIndex))));

        // The Ai's modes (MONSTER_TYPE_<Ai>; none or an unknown one is 06, no modes), then the Modes list turns single modes on or off
        auto aiIter = MonsterTypeModes.find(int32(mobTable->GetInt(rowIndex, aiColumnIndex, 6)));
        status.Mode = aiIter != MonsterTypeModes.end() ? aiIter->second : 0;
        // MAX_ASPD_NOPC .. MIN_ASPD, and amotion 1 .. MIN_ASPD / AMOTION_DIVIDER_NOPC
        status.AttackDelay = std::clamp<int32>(int32(mobTable->GetInt(rowIndex, attackDelayColumnIndex, 100)), 100, 8000);
        status.AttackMotion = std::clamp<int32>(int32(mobTable->GetInt(rowIndex, attackMotionColumnIndex, 100)), 1, 8000);
        status.DamageMotion = int32(mobTable->GetInt(rowIndex, damageMotionColumnIndex, 0));
        if (BattleConfig.MonsterDamageDelayRate != 100)
            status.DamageMotion = status.DamageMotion * BattleConfig.MonsterDamageDelayRate / 100;
        status.DamageTaken = int32(mobTable->GetInt(rowIndex, damageTakenColumnIndex, 100));
        status.Speed = std::clamp<int32>(int32(mobTable->GetInt(rowIndex, walkSpeedColumnIndex, ROWOW_DEFAULT_WALK_SPEED)), ROWOW_MIN_WALK_SPEED, ROWOW_MAX_WALK_SPEED);
        for (auto const& modeField : ROWOWDataTable::ParseMap(mobTable->GetCell(rowIndex, modesColumnIndex)))
        {
            uint32 mode = 0;
            if (modeField.first == "CanMove")
                mode = ROWOW_MODE_CANMOVE;
            else if (modeField.first == "Looter")
                mode = ROWOW_MODE_LOOTER;
            else if (modeField.first == "Aggressive")
                mode = ROWOW_MODE_AGGRESSIVE;
            else if (modeField.first == "Assist")
                mode = ROWOW_MODE_ASSIST;
            else if (modeField.first == "CastSensorIdle")
                mode = ROWOW_MODE_CASTSENSORIDLE;
            else if (modeField.first == "NoRandomWalk")
                mode = ROWOW_MODE_NORANDOMWALK;
            else if (modeField.first == "NoCast")
                mode = ROWOW_MODE_NOCAST;
            else if (modeField.first == "CanAttack")
                mode = ROWOW_MODE_CANATTACK;
            else if (modeField.first == "CastSensorChase")
                mode = ROWOW_MODE_CASTSENSORCHASE;
            else if (modeField.first == "ChangeChase")
                mode = ROWOW_MODE_CHANGECHASE;
            else if (modeField.first == "Angry")
                mode = ROWOW_MODE_ANGRY;
            else if (modeField.first == "ChangeTargetMelee")
                mode = ROWOW_MODE_CHANGETARGETMELEE;
            else if (modeField.first == "ChangeTargetChase")
                mode = ROWOW_MODE_CHANGETARGETCHASE;
            else if (modeField.first == "TargetWeak")
                mode = ROWOW_MODE_TARGETWEAK;
            else if (modeField.first == "RandomTarget")
                mode = ROWOW_MODE_RANDOMTARGET;
            else if (modeField.first == "TeleportBlock")
                mode = ROWOW_MODE_TELEPORTBLOCK;
            else if (modeField.first == "FixedItemDrop")
                mode = ROWOW_MODE_FIXEDITEMDROP;
            else if (modeField.first == "Detector")
                mode = ROWOW_MODE_DETECTOR;
            else if (modeField.first == "StatusImmune")
                mode = ROWOW_MODE_STATUS_IMMUNE;
            else if (modeField.first == "SkillImmune")
                mode = ROWOW_MODE_SKILL_IMMUNE;
            else if (modeField.first == "IgnoreMelee")
                mode = ROWOW_MODE_IGNORE_MELEE;
            else if (modeField.first == "IgnoreMagic")
                mode = ROWOW_MODE_IGNORE_MAGIC;
            else if (modeField.first == "IgnoreRanged")
                mode = ROWOW_MODE_IGNORE_RANGED;
            else if (modeField.first == "IgnoreMisc")
                mode = ROWOW_MODE_IGNORE_MISC;
            else if (modeField.first == "Mvp")
                mode = ROWOW_MODE_MVP;
            else if (modeField.first == "KnockBackImmune")
                mode = ROWOW_MODE_KNOCKBACK_IMMUNE;
            if (modeField.second == "true")
                status.Mode |= mode;
            else
                status.Mode &= ~mode;
        }

        // loadingFinished: the class's modes, an attack can't come sooner than its motion, the rest of the status follows from the stats,
        // and the HP rates
        switch (status.Class)
        {
            case ROWOW_CLASS_BOSS:
                status.Mode |= ROWOW_MODE_DETECTOR | ROWOW_MODE_STATUS_IMMUNE | ROWOW_MODE_KNOCKBACK_IMMUNE;
                break;
            case ROWOW_CLASS_GUARDIAN:
                status.Mode |= ROWOW_MODE_STATUS_IMMUNE;
                break;
            case ROWOW_CLASS_BATTLEFIELD:
                status.Mode |= ROWOW_MODE_STATUS_IMMUNE | ROWOW_MODE_SKILL_IMMUNE;
                break;
            case ROWOW_CLASS_EVENT:
                status.Mode |= ROWOW_MODE_FIXEDITEMDROP;
                break;
        }
        if (BattleConfig.ViewRangeRate != 100)
            status.Range2 = std::max(1, status.Range2 * BattleConfig.ViewRangeRate / 100);
        if (BattleConfig.ChaseRangeRate != 100)
            status.Range3 = std::max(status.Range2, status.Range3 * BattleConfig.ChaseRangeRate / 100);
        status.AttackDelay = std::max(status.AttackDelay, status.AttackMotion);
        status.AspdRate = 1000;
        if (BattleConfig.MonsterActiveEnable == 0)
            status.Mode &= ~ROWOW_MODE_AGGRESSIVE;
        StatusCalcMisc(status, status.Level);
        double maxHP = double(status.MaxHP);
        if ((status.Mode & ROWOW_MODE_MVP) != 0)
        {
            if (BattleConfig.MvpHPRate != 100)
                maxHP = maxHP * double(BattleConfig.MvpHPRate) / 100.;
        }
        else if (BattleConfig.MonsterHPRate != 100)
            maxHP = maxHP * double(BattleConfig.MonsterHPRate) / 100.;
        status.MaxHP = std::max<uint32>(1, uint32(maxHP));
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadBattleData loaded {} RO monster statuses", MobStatusesByMobID.size());
}

ROWOWStatus const* ROWOWMod::GetROStatus(Unit const* unit) const
{
    if (unit == nullptr)
        return nullptr;
    if (unit->IsPlayer() == true)
    {
        ROWOWIdentity const* identity = unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity == nullptr || identity->IsInForm == false)
            return nullptr;
        return &identity->Status;
    }
    if (unit->IsCreature() == true)
    {
        // Its own while statuses change it, otherwise its mob_db status
        if (ROWOWMobStatus const* mobStatus = unit->CustomData.Get<ROWOWMobStatus>("ROWOWMobStatus"))
            return &mobStatus->Status;
        auto statusIter = MobStatusesByMobID.find(GetROMobID(unit->ToCreature()));
        if (statusIter == MobStatusesByMobID.end())
            return nullptr;
        return &statusIter->second;
    }
    return nullptr;
}

void ROWOWMod::ApplyMobStatus(Creature* creature)
{
    // The end of Creature::SelectLevel (spawn, respawn and entry changes): the monster's exact RO HP, in place of the Warcraft health the
    // converter fitted to its level, and its attack delay.  A respawn starts from its mob_db status, with no statuses, and a detector (insects,
    // demons, bosses) sees through Hiding and Cloaking (the stealth they put on WoW's side)
    StatusChangeClear(creature, 1);
    creature->CustomData.Erase("ROWOWMobStatus");
    ROWOWStatus const* status = GetROStatus(creature);
    if (status == nullptr)
        return;
    if ((status->Mode & ROWOW_MODE_DETECTOR) != 0 && creature->m_stealthDetect.GetValue(STEALTH_GENERAL) < ROWOW_HIDING_STEALTH_VALUE)
    {
        creature->m_stealthDetect.AddFlag(STEALTH_GENERAL);
        creature->m_stealthDetect.AddValue(STEALTH_GENERAL, ROWOW_HIDING_STEALTH_VALUE);
    }
    creature->SetCreateHealth(status->MaxHP);
    creature->SetMaxHealth(status->MaxHP);
    creature->SetHealth(status->MaxHP);
    creature->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(status->MaxHP));
    creature->SetAttackTime(BASE_ATTACK, uint32(status->AttackDelay));
    creature->SetAttackTime(OFF_ATTACK, uint32(status->AttackDelay));
}

uint32 ROWOWMod::GetWeaponHitSoundSpellID(uint8 weaponType)
{
    // The client's hit sound of each weapon type (roBrowser's WeaponSoundTable): fists, the dual wield types and the rest sound like a mace
    uint32 hitSound = WEAPON_HIT_SOUND_MACE;
    switch (weaponType)
    {
        case ROWOW_WEAPON_TYPE_DAGGER:
        case ROWOW_WEAPON_TYPE_1HSWORD:
        case ROWOW_WEAPON_TYPE_2HSWORD:
            hitSound = WEAPON_HIT_SOUND_SWORD;
            break;
        case ROWOW_WEAPON_TYPE_1HSPEAR:
        case ROWOW_WEAPON_TYPE_2HSPEAR:
            hitSound = WEAPON_HIT_SOUND_SPEAR;
            break;
        case ROWOW_WEAPON_TYPE_1HAXE:
        case ROWOW_WEAPON_TYPE_2HAXE:
            hitSound = WEAPON_HIT_SOUND_AXE;
            break;
        case ROWOW_WEAPON_TYPE_STAFF:
        case ROWOW_WEAPON_TYPE_2HSTAFF:
            hitSound = WEAPON_HIT_SOUND_ROD;
            break;
        case ROWOW_WEAPON_TYPE_BOW:
            hitSound = WEAPON_HIT_SOUND_ARROW;
            break;
        case ROWOW_WEAPON_TYPE_REVOLVER:
            hitSound = WEAPON_HIT_SOUND_HANDGUN;
            break;
        case ROWOW_WEAPON_TYPE_RIFLE:
            hitSound = WEAPON_HIT_SOUND_RIFLE;
            break;
    }
    return ROWOW_SPELL_WEAPON_HIT_SOUND_START + hitSound;
}

float ROWOWMod::GetAutoAttackExtraRange(Player* player, Unit const* victim) const
{
    // An RO weapon reaches its range in cells (a bow 5 and more), which past melee range is further than Warcraft's
    ROWOWStatus const* status = GetROStatus(player);
    if (status == nullptr || victim == nullptr)
        return 0.0f;
    float rangeYards = float(status->Range) * CellSize + player->GetCombatReach() + victim->GetCombatReach();
    return std::max(0.0f, rangeYards - player->GetMeleeRange(victim));
}

bool ROWOWMod::CalculateMeleeDamage(Unit* attacker, Unit* victim, CalcDamageInfo* damageInfo)
{
    // A swing between RO combatants is rAthena's normal attack (battle_weapon_attack), and nothing of Warcraft's melee: no miss, dodge,
    // parry, block, glancing or crushing table, armor, resilience, absorb or level difference
    if (IsEnabled == false || IsROCombatant(attacker) == false || IsROCombatant(victim) == false)
        return false;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
    {
        damageInfo->damages[i].damageSchoolMask = SPELL_SCHOOL_MASK_NORMAL;
        damageInfo->damages[i].damage = 0;
        damageInfo->damages[i].absorb = 0;
        damageInfo->damages[i].resist = 0;
    }
    damageInfo->cleanDamage = 0;
    damageInfo->blocked_amount = 0;
    if (victim->IsCreature() == true && victim->ToCreature()->IsEvadingAttacks() == true)
    {
        damageInfo->HitInfo |= HITINFO_MISS | HITINFO_SWINGNOHITSOUND;
        damageInfo->TargetState = VICTIMSTATE_EVADES;
        damageInfo->hitOutCome = MELEE_HIT_EVADE;
        return true;
    }

    // unit_attack_timer_sub's status_check_skilluse: an attacker a status stops from attacking (stun, freeze, Hiding...), or a target it can't
    // see, swings for nothing
    ROWOWDamage wd;
    wd.DmgLv = ROWOW_ATK_NONE;
    if (StatusCheckSkillUse(attacker, victim, 0, 0) == true)
        BattleWeaponAttack(attacker, victim, wd);

    // The client shows a perfect dodge as RO's "Lucky" (dodge, which RO combat otherwise never has), a miss as "Miss", and a critical.  A
    // swing that became something else (a skill's hit, Poison React's counter) shows nothing of its own
    switch (wd.DmgLv)
    {
        case ROWOW_ATK_NONE:
            damageInfo->hitOutCome = MELEE_HIT_NORMAL;
            damageInfo->TargetState = VICTIMSTATE_INTACT;
            damageInfo->HitInfo |= HITINFO_SWINGNOHITSOUND;
            break;
        case ROWOW_ATK_LUCKY:
            damageInfo->hitOutCome = MELEE_HIT_DODGE;
            damageInfo->TargetState = VICTIMSTATE_DODGE;
            break;
        case ROWOW_ATK_DEF:
        {
            damageInfo->damages[0].damage = uint32(std::clamp<int64>(wd.Damage + wd.Damage2, 0, std::numeric_limits<uint32>::max()));
            damageInfo->TargetState = VICTIMSTATE_HIT;
            damageInfo->HitInfo |= HITINFO_AFFECTS_VICTIM;
            if (IsAttackCritical(wd, attacker, victim, 0, 0, false) == true)
            {
                damageInfo->hitOutCome = MELEE_HIT_CRIT;
                damageInfo->HitInfo |= HITINFO_CRITICALHIT;
            }
            else
                damageInfo->hitOutCome = MELEE_HIT_NORMAL;
            if (damageInfo->damages[0].damage > 0)
                damageInfo->procVictim |= PROC_FLAG_TAKEN_DAMAGE;

            // A player's weapon makes its RO hit sound on the target (missing until the converter has made the spells)
            ROWOWStatus const* sstatus = GetROStatus(attacker);
            uint32 hitSoundSpellID = GetWeaponHitSoundSpellID(sstatus->WeaponType);
            if (attacker->IsPlayer() == true && damageInfo->damages[0].damage > 0 && sSpellMgr->GetSpellInfo(hitSoundSpellID) != nullptr)
                victim->CastSpell(victim, hitSoundSpellID, true);
        } break;
        default:
            damageInfo->hitOutCome = MELEE_HIT_MISS;
            damageInfo->HitInfo |= HITINFO_MISS;
            damageInfo->TargetState = VICTIMSTATE_INTACT;
            break;
    }
    return true;
}

bool ROWOWMod::HasNormalAttackAmmo(Player* player) const
{
    // battle_weapon_attack's ammo check: a bow or gun needs its ammo worn, of the right type (Ishizu-chan's check, with ammo_check_weapon)
    ROWOWStatus const* status = GetROStatus(player);
    if (status == nullptr || status->IsArrowAttack == false)
        return true;
    if (GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_AMMO) == nullptr)
        return false;
    if (BattleConfig.AmmoCheckWeapon == 0)
        return true;
    uint8 neededAmmoType = ROWOW_AMMO_TYPE_BULLET;
    if (status->WeaponType == ROWOW_WEAPON_TYPE_BOW)
        neededAmmoType = ROWOW_AMMO_TYPE_ARROW;
    else if (status->WeaponType == ROWOW_WEAPON_TYPE_GRENADE)
        neededAmmoType = ROWOW_AMMO_TYPE_GRENADE;
    return status->AmmoType == neededAmmoType;
}

uint8 ROWOWMod::BattleWeaponAttack(Unit* src, Unit* target, ROWOWDamage& wdOut)
{
    // battle_weapon_attack, for a normal attack: the statuses that end with it or turn it into something else, the attack, and its
    // additional effects once WoW has dealt it.  wdOut is what the client shows of the swing: ATK_NONE when it became something that shows
    // itself (Triple Attack, Sacrifice).  (The third jobs' triggers are left out)
    wdOut.DmgLv = ROWOW_ATK_NONE;
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (sstatus == nullptr || tstatus == nullptr)
        return ROWOW_ATK_NONE;
    Player* sd = src->ToPlayer();
    Item* ammoItem = nullptr;
    if (sd != nullptr && sstatus->IsArrowAttack == true)
    {
        // A bow or gun shoots its ammo (the auto attack hook stops the attack without it)
        if (HasNormalAttackAmmo(sd) == false)
            return ROWOW_ATK_NONE;
        ammoItem = GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_AMMO);
    }
    uint64 tick = GetTick();
    ROWOWStatusChange* sc = GetStatusChange(src);
    ROWOWStatusChange* tsc = GetStatusChange(target);
    if (sc != nullptr && sc->IsEmpty() == true) // Avoid sc checks when there's none to check for. [Skotlex]
        sc = nullptr;
    if (tsc != nullptr && tsc->IsEmpty() == true)
        tsc = nullptr;

    // Attacking reveals a cloaked attacker, unless player_cloak_check_type/monster_cloak_check_type&2 lets it keep Cloaking
    if (sc != nullptr)
    {
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_CLOAKING); sce != nullptr && (sce->Val4 & 2) == 0)
            StatusChangeEnd(src, ROWOW_SC_CLOAKING);
    }
    if (tsc != nullptr && tsc->HasSCE(ROWOW_SC_AUTOCOUNTER) == true && StatusCheckSkillUse(target, src, ROWOW_SKILL_KN_AUTOCOUNTER, 1) == true)
    {
        // Auto Counter: an attack from the front (map_check_dir: within one of the eight directions of where the target faces, 135 degrees)
        // within the target's reach is countered
        int32 dist = DistanceBL(src, target);
        if (dist <= 0 || (target->HasInArc(float(M_PI) * 3.0f / 4.0f, src) == true && dist <= tstatus->Range + 1))
        {
            uint16 skillLevel = uint16(tsc->GetSCE(ROWOW_SC_AUTOCOUNTER)->Val1);
            wdOut.DmgLv = ROWOW_ATK_MISS; // Display MISS.
            StatusChangeEnd(target, ROWOW_SC_AUTOCOUNTER);
            SkillAttack(ROWOW_BF_WEAPON, target, target, src, ROWOW_SKILL_KN_AUTOCOUNTER, skillLevel, tick, 0);
            return ROWOW_ATK_BLOCK;
        }
    }

    // Poison React counter activates on attacks from poison-element enemies as well as normal poison attacks
    if (tsc != nullptr && ((src->IsPlayer() == false && sstatus->DefElement == ROWOW_ELEMENT_POISON) || sstatus->WeaponElement == ROWOW_ELEMENT_POISON))
    {
        if (ROWOWStatusChangeEntry* sce = tsc->GetSCE(ROWOW_SC_POISONREACT); sce != nullptr && sce->Val4 == 0)
        {
            // Next normal attack will receive a damage boost
            sce->Val4 = 1;
            // The target will start attacking instead of the source (unit_attack)
            if (Creature* tmd = target->ToCreature(); tmd != nullptr && GetROMobID(tmd) != 0)
                GetMobData(tmd)->TargetID = src->GetGUID();
            else
                target->Attack(src, true);
            return ROWOW_ATK_BLOCK;
        }
    }

    // Blade Stop's catch: both are held for its time (check_distance_bl 2 for a monster's attack)
    if (tsc != nullptr && tsc->HasSCE(ROWOW_SC_BLADESTOP_WAIT) == true && sstatus->Class != ROWOW_CLASS_BOSS &&
        (src->IsPlayer() == true || DistanceBL(src, target) <= 2))
    {
        uint16 skillLevel = uint16(tsc->GetSCE(ROWOW_SC_BLADESTOP_WAIT)->Val1);
        int64 duration = SkillGetTime2(ROWOW_SKILL_MO_BLADESTOP, skillLevel);
        StatusChangeEnd(target, ROWOW_SC_BLADESTOP_WAIT);
        if (SCStart4(src, src, ROWOW_SC_BLADESTOP, 100, sd != nullptr ? PCCheckSkill(sd, ROWOW_SKILL_MO_BLADESTOP) : 5, 0, 0,
            int32(target->GetGUID().GetCounter()), duration) == true)
        {
            // Target locked.
            wdOut.DmgLv = ROWOW_ATK_MISS; // Display MISS.
            SCStart4(src, target, ROWOW_SC_BLADESTOP, 100, skillLevel, 0, 0, int32(src->GetGUID().GetCounter()), duration);
            return ROWOW_ATK_BLOCK;
        }
    }

    // Triple Attack: a chance (30% less its level) that the swing is the skill instead, with the skill's after-cast delay (it doesn't go
    // through skill_castend_id), which opens the Chain Combo window
    if (int32 skillv = sd != nullptr ? PCCheckSkill(sd, ROWOW_SKILL_MO_TRIPLEATTACK) : 0; skillv > 0)
    {
        int32 tripleRate = 30 - skillv; // Base Rate
        if (ROWOWStatusChangeEntry const* sce = sc != nullptr ? sc->GetSCE(ROWOW_SC_SKILLRATE_UP) : nullptr; sce != nullptr && sce->Val1 == ROWOW_SKILL_MO_TRIPLEATTACK)
        {
            tripleRate += tripleRate * sce->Val2 / 100;
            StatusChangeEnd(src, ROWOW_SC_SKILLRATE_UP);
        }
        if (int32(urand(0, 99)) < tripleRate)
        {
            // Need to apply canact_tick here because it doesn't go through skill_castend_id
            ROWOWSkillUnitData* ud = GetSkillUnitData(sd);
            ud->CanActTime = std::max<uint64>(tick + uint64(SkillDelayFix(src, ROWOW_SKILL_MO_TRIPLEATTACK, uint16(skillv))), ud->CanActTime);
            SendSkillDelayToClient(sd, ROWOW_SKILL_MO_TRIPLEATTACK);
            if (SkillAttack(ROWOW_BF_WEAPON, src, src, target, ROWOW_SKILL_MO_TRIPLEATTACK, uint16(skillv), tick, 0) != 0)
                return ROWOW_ATK_DEF;
            return ROWOW_ATK_MISS;
        }
    }

    if (sc != nullptr)
    {
        if (ROWOWStatusChangeEntry* sce = sc->GetSCE(ROWOW_SC_SACRIFICE))
        {
            // Sacrifice: the next normal attacks are its hits, each costing 9% of the attacker's max HP
            uint16 skillLevel = uint16(sce->Val1);
            if (--sce->Val2 <= 0)
                StatusChangeEnd(src, ROWOW_SC_SACRIFICE);

            // We need to calculate the DMG before the hp reduction, because it can kill the source.  For further information: bugreport:4950
            int64 dealt = SkillAttack(ROWOW_BF_WEAPON, src, src, target, ROWOW_SKILL_PA_SACRIFICE, skillLevel, tick, 0);
            StatusZap(src, src->GetMaxHealth() * 9 / 100, 0); // Damage to self is always 9%
            return dealt == 0 ? ROWOW_ATK_MISS : ROWOW_ATK_DEF;
        }
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_MAGICALATTACK))
        {
            // A monster's magical attacks (NPC_MAGICALATTACK) in place of its normal attack
            if (SkillAttack(ROWOW_BF_MAGIC, src, src, target, ROWOW_SKILL_NPC_MAGICALATTACK, uint16(sce->Val1), tick, 0) != 0)
                return ROWOW_ATK_DEF;
            return ROWOW_ATK_MISS;
        }
    }

    // Kaahi: being hit heals, for SP
    if (tsc != nullptr && target->GetHealth() < target->GetMaxHealth())
    {
        ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_KAAHI);
        if (sce != nullptr && StatusCharge(target, 0, sce->Val3) == true)
        {
            int64 hpHeal = std::min<int64>(target->GetMaxHealth() - target->GetHealth(), sce->Val2);
            if (hpHeal > 0)
                StatusHeal(target, hpHeal, 0, 2, target, GetSkillSpellID(ROWOW_SKILL_SL_KAAHI, uint16(sce->Val1)));
        }
    }

    wdOut = BattleCalcAttack(ROWOW_BF_WEAPON, src, target, 0, 0, 0);

    // Consume arrow
    if (ammoItem != nullptr)
        BattleConsumeAmmo(sd, 0, 0);

    // Devotion: while the link holds, the devoted target's Crusader takes the hit and the target none (battle_delay_damage's part: as
    // ATK_BLOCK, which keeps it from stopping a cast or triggering autospells).  A broken link ends the status
    if (GetSCE(target, ROWOW_SC_DEVOTION) != nullptr)
    {
        int64 damage = wdOut.Damage + wdOut.Damage2;
        if (Unit* d_bl = GetDevotionSource(target, true))
        {
            // Only trigger if the devoted player was hit
            if (damage > 0)
            {
                // The devoting player needs to stand up, and the devoted one (devotion_standup_fix)
                Player* tsd = target->ToPlayer();
                if (tsd != nullptr && tsd->IsSitState() == true && BattleConfig.DevotionStandupFix != 0)
                    tsd->SetStandState(UNIT_STAND_STATE_STAND);
                if (d_bl->IsSitState() == true)
                    d_bl->SetStandState(UNIT_STAND_STATE_STAND);
                BattleFixDamage(src, d_bl, damage, ROWOW_SKILL_CR_DEVOTION);
                wdOut.Damage = 0;
                wdOut.Damage2 = 0;
                if (wdOut.DmgLv == ROWOW_ATK_DEF)
                    wdOut.DmgLv = ROWOW_ATK_BLOCK;
            }
        }
        else
            StatusChangeEnd(target, ROWOW_SC_DEVOTION);
    }

    // Monsters trigger a special event when they are hit in Berserk state by a normal attack within [attack range+1].  Certain AI types only switch
    // target on this trigger.  This event only triggers if the target is not already the current attacker.  We need to do this here because it needs
    // to be calculated before waiting for attack motion
    if (wdOut.DmgLv > ROWOW_ATK_BLOCK && target->IsCreature() == true && GetROMobID(target->ToCreature()) != 0)
    {
        ROWOWMobData const* tmob = GetMobData(target->ToCreature());
        bool isNormAttacked = tmob->SkillState == ROWOW_MSS_BERSERK && tmob->TargetID != src->GetGUID() &&
            ((BattleConfig.MobAI & 0x4) != 0 || CheckDistanceBL(src, target, tstatus->Range + 1) == true);
        MobBattleAttacked(src, target, wdOut.Damage + wdOut.Damage2, wdOut.Div, tick, wdOut.AMotion, isNormAttacked, wdOut.Flag, 0);
    }

    // battle_delay_damage's additional effects: WoW deals the swing right after this, and they follow it in the target's update (where the
    // attacker is looked up again)
    ObjectGuid srcGUID = src->GetGUID();
    int32 attackType = wdOut.Flag;
    uint8 dmgLv = wdOut.DmgLv;
    target->m_Events.AddEventAtOffset([this, target, srcGUID, attackType, dmgLv]()
    {
        Unit* delayedSrc = ObjectAccessor::GetUnit(*target, srcGUID);
        if (delayedSrc == nullptr || delayedSrc->IsInWorld() == false || delayedSrc->GetMap() != target->GetMap())
            return;
        uint64 effectTick = GetTick();
        if (delayedSrc->IsPlayer() == true && target->IsAlive() == true && BattleCheckComa(delayedSrc, target, attackType) == true)
        {
            // battle_damage's coma attack (status_damage flag 16): the swing leaves 1 HP and 1 SP
            if (target->GetHealth() > 1)
                Unit::DealDamage(delayedSrc, target, target->GetHealth() - 1, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            if (target->GetMaxPower(POWER_MANA) > 0)
                target->SetPower(POWER_MANA, 1);
        }
        if (target->IsAlive() == true)
            SkillAdditionalEffect(delayedSrc, target, 0, 0, attackType, dmgLv, effectTick);
        if (dmgLv > ROWOW_ATK_BLOCK)
            SkillCounterAdditionalEffect(delayedSrc, target, 0, 0, attackType, effectTick);
    }, Milliseconds(1));

    // Hindsight: a chance for each normal attack to cast one of the remembered bolts, at a lower level most of the time, for 2/3 of its SP
    sc = GetStatusChange(src);
    if (ROWOWStatusChangeEntry const* sce = sc != nullptr ? sc->GetSCE(ROWOW_SC_AUTOSPELL) : nullptr; sce != nullptr && int32(urand(0, 99)) < sce->Val4)
    {
        uint16 skillID = uint16(sce->Val2);
        uint16 skillLevel = uint16(sce->Val3);
        int32 i = int32(urand(0, 99));
        if (ROWOWStatusChangeEntry const* spirit = sc->GetSCE(ROWOW_SC_SPIRIT); spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_SAGE)
            i = 0; // Max chance, no skill_lv reduction. [Skotlex]
        // reduction only for skill_lv > 1
        if (skillLevel > 1)
        {
            if (i >= 50)
                skillLevel /= 2;
            else if (i >= 15)
                skillLevel--;
        }
        ROWOWSkill const* skill = GetSkill(skillID);
        int32 sp = skill != nullptr ? skill->SPCost[ROWOWSkill::GetLevelIndex(skillLevel)] * 2 / 3 : 0;
        if (skill != nullptr && StatusCharge(src, 0, sp) == true)
        {
            switch (SkillGetCastType(skillID))
            {
                case ROWOW_CAST_GROUND:
                {
                    int32 x = 0, y = 0;
                    GetUnitCell(target, x, y);
                    SkillCastendPos2(src, x, y, skillID, skillLevel, tick, 0);
                    break;
                }
                case ROWOW_CAST_NODAMAGE:
                    SkillCastendNoDamageID(src, target, skillID, skillLevel, tick, 0);
                    break;
                case ROWOW_CAST_DAMAGE:
                    SkillCastendDamageID(src, target, skillID, skillLevel, tick, 0);
                    break;
            }
            ROWOWSkillUnitData* ud = GetSkillUnitData(src);
            int32 autospellTick = SkillDelayFix(src, skillID, skillLevel);
            if (ud != nullptr && ud->CanActTime < tick + uint64(autospellTick))
            {
                ud->CanActTime = std::max<uint64>(tick + uint64(autospellTick), ud->CanActTime);
                SendSkillDelayToClient(src, skillID);
            }
        }
    }

    // A normal attack holds the next skill back (unit_set_attackdelay, DELAY_EVENT_ATTACK)
    UnitSetAttackDelay(src, tick, false);
    return wdOut.DmgLv;
}

void ROWOWMod::BattleConsumeAmmo(Player* sd, uint16 skillID, uint16 skillLevel)
{
    // battle_consume_ammo: one ammo for a normal attack, the skill's AmmoAmount (at least one) for a skill, and the last one is taken off
    if (BattleConfig.ArrowDecrement == 0)
        return;
    Item* ammoItem = GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_AMMO);
    if (ammoItem == nullptr)
        return; // Qty check should have been done in skill_check_condition
    uint32 count = 1;
    if (skillID != 0)
    {
        ROWOWSkill const* skill = GetSkill(skillID);
        int32 skillCount = skill != nullptr ? skill->AmmoAmount[ROWOWSkill::GetLevelIndex(skillLevel)] : 0;
        count = skillCount > 0 ? uint32(skillCount) : 1;
    }
    bool isLastAmmo = ammoItem->GetCount() <= count;
    sd->DestroyItemCount(ammoItem, count, true);
    if (isLastAmmo == true)
    {
        ROWOWIdentity* identity = GetIdentity(sd);
        identity->ItemGUIDsByEquipPosition.erase(ROWOW_EQUIP_POSITION_AMMO);
        identity->IsEquipmentChanged = true;
        RefreshROForm(sd);
    }
}

ROWOWDamage ROWOWMod::BattleCalcAttack(int32 attackType, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 flag)
{
    // battle_calc_attack
    ROWOWDamage d;
    switch (attackType)
    {
        case ROWOW_BF_WEAPON: d = BattleCalcWeaponAttack(src, target, skillID, skillLevel, flag); break;
        case ROWOW_BF_MAGIC: d = BattleCalcMagicAttack(src, target, skillID, skillLevel, flag); break;
        case ROWOW_BF_MISC: d = BattleCalcMiscAttack(src, target, skillID, skillLevel, flag); break;
        default:
            LOG_ERROR("module.ROWOW", "ROWOWMod::BattleCalcAttack: unknown attack type {} (skill_id={}, skill_lv={})", attackType, skillID, skillLevel);
            d.DmgLv = ROWOW_ATK_NONE;
            return d;
    }
    if (d.Damage + d.Damage2 < 1)
    {
        // Miss/Absorbed
        if (d.DmgLv == ROWOW_ATK_DEF)
            d.DmgLv = ROWOW_ATK_MISS;
        d.DMotion = 0;
        if (src->IsPlayer() == true)
            d.Div = 1;
    }
    else // Some skills like Weaponry Research will cause damage even if attack is dodged
        d.DmgLv = ROWOW_ATK_DEF;
    return d;
}

ROWOWDamage ROWOWMod::InitializeWeaponData(Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel, int32 wflag) const
{
    // initialize_weapon_data
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    ROWOWSkill const* skill = GetSkill(skillID);
    ROWOWDamage wd;
    wd.Type = ROWOW_DMG_NORMAL; // Normal attack
    wd.Div = int16(skillID != 0 && skill != nullptr ? ROWOWSkill::GetLevelValue(skill->HitCount, skillLevel) : 1);
    wd.AMotion = (skill != nullptr && (skill->Inf & ROWOW_INF_GROUND_SKILL) != 0) ? 0 : sstatus->AttackMotion; // Amotion should be 0 for ground skills.
    wd.DMotion = tstatus->DamageMotion;
    wd.BlewCount = skill != nullptr ? ROWOWSkill::GetLevelValue(skill->Knockback, skillLevel) : 0;
    wd.MiscFlag = wflag;
    wd.Flag = ROWOW_BF_WEAPON; // Initial Flag
    wd.Flag |= (skillID != 0 || wd.MiscFlag != 0) ? ROWOW_BF_SKILL : ROWOW_BF_NORMAL; // Baphomet card's splash damage is counted as a skill. [Inkfish]
    wd.Damage = wd.Damage2 = wd.BaseDamage = 0;
    wd.DmgLv = ROWOW_ATK_DEF; // This assumption simplifies the assignation later
    if (src->IsPlayer() == true)
        wd.BlewCount += BattleBlewCountBonus(src, skillID);

    if (skillID != 0)
    {
        // wd.flag from skills
        wd.Flag |= BattleRangeType(src, target, skillID, skillLevel);
    }
    else
    {
        // wd.flag from basic attacks.  (Soul Attack is a fourth job's status)
        if (IsSkillUsingArrow(src, skillID) == true)
            wd.Flag |= ROWOW_BF_LONG;
        else
            wd.Flag |= ROWOW_BF_SHORT;
    }
    return wd;
}

int32 ROWOWMod::BattleRangeType(Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel) const
{
    // battle_range_type.  [Akinari] , [Xynvaroth]: Traps are always short range.
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill != nullptr && skill->Inf2[ROWOW_INF2_ISTRAP] == true)
        return ROWOW_BF_SHORT;
    switch (skillID)
    {
        case ROWOW_SKILL_AC_SHOWER:
        case ROWOW_SKILL_AM_DEMONSTRATION:
            // When monsters use Arrow Shower or Bomb, it is always short range
            if (src->IsCreature() == true)
                return ROWOW_BF_SHORT;
            break;
        case ROWOW_SKILL_NJ_KIRIKAGE:
            // Cast range mimics NJ_SHADOWJUMP but damage is considered melee
            return ROWOW_BF_SHORT;
    }

    // Skill Range Criteria: based on distance between src/target [Skotlex], or on the used skill's range
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (BattleConfig.SkillrangeByDistance != 0 && (BattleConfig.SkillrangeByDistance & GetBattleObjectType(*sstatus)) != 0)
    {
        if (DistanceBL(src, target) <= 3)
            return ROWOW_BF_SHORT;
        return ROWOW_BF_LONG;
    }
    if (SkillGetRange2(src, skillID, skillLevel, true) < 4)
        return ROWOW_BF_SHORT;
    return ROWOW_BF_LONG;
}

int32 ROWOWMod::BattleBlewCountBonus(Unit const* src, uint16 skillID) const
{
    // battle_blewcount_bonus: Apply the bonus blewcount. [Skotlex]
    return GetDeferredBonus2(src, "bAddSkillBlow", skillID);
}

std::bitset<ROWOW_NK_MAX> ROWOWMod::BattleSkillGetDamageProperties(uint16 skillID, int32 isSplash) const
{
    // battle_skill_get_damage_properties: a normal attack's splash (the Baphomet card's) ignores the attacker's cards and FLEE
    std::bitset<ROWOW_NK_MAX> nk;
    if (skillID == 0)
    {
        if (isSplash != 0)
        {
            nk.set(ROWOW_NK_IGNOREATKCARD);
            nk.set(ROWOW_NK_IGNOREFLEE);
        }
        return nk;
    }
    if (ROWOWSkill const* skill = GetSkill(skillID))
        return skill->NK;
    return nk;
}

bool ROWOWMod::BattleCheckUndead(int32 /*race*/, int32 element)
{
    // battle_check_undead with undead_detect_type 0: the undead element
    return element == ROWOW_ELEMENT_UNDEAD;
}

ROWOWDamage ROWOWMod::BattleCalcWeaponAttack(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 wflag)
{
    // battle_calc_weapon_attack
    ROWOWDamage wd;
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (sstatus == nullptr || tstatus == nullptr)
    {
        wd.DmgLv = ROWOW_ATK_NONE;
        return wd;
    }
    wd = InitializeWeaponData(src, target, skillID, skillLevel, wflag);

    // Update Damage data based on skill
    if (ROWOWSkillImpl const* impl = GetSkillImpl(skillID))
        impl->ModifyDamageData(wd, *src, *target, skillLevel);

    int32 rightElement = BattleGetWeaponElement(wd, src, target, skillID, skillLevel, true, false);
    int32 leftElement = BattleGetWeaponElement(wd, src, target, skillID, skillLevel, false, false);
    bool isLeftHanded = IsAttackLeftHanded(src, skillID);
    bool infdef = false;
    Player* sd = src->ToPlayer();
    Player* tsd = target->ToPlayer();

    // Check for Lucky Dodge
    if ((skillID == 0 || skillID == ROWOW_SKILL_PA_SACRIFICE) && tstatus->Flee2 != 0 && int32(urand(0, 999)) < tstatus->Flee2)
    {
        wd.Type = ROWOW_DMG_LUCY_DODGE;
        wd.DmgLv = ROWOW_ATK_LUCKY;
        if (wd.Div < 0)
            wd.Div *= -1;
        return wd;
    }

    // on official check for multi hit first so we can override crit on double attack [helvetica]
    BattleCalcMultiAttack(wd, src, target, skillID, skillLevel);

    // crit check is next since crits always hit on official [helvetica]
    if (IsAttackCritical(wd, src, target, skillID, skillLevel, true) == true)
        wd.Type = ROWOW_DMG_CRITICAL;

    std::bitset<ROWOW_NK_MAX> nk = BattleSkillGetDamageProperties(skillID, wd.MiscFlag);

    // check if we're landing a hit
    if (IsAttackHitting(wd, src, target, skillID, skillLevel, true) == false)
        wd.DmgLv = ROWOW_ATK_FLEE;
    else if ((infdef = IsInfiniteDefense(target, wd.Flag)) == false) // no need for math against plants
    {
        // First call function with skill_id 0 to get base damage of a normal attack
        BattleCalcSkillBaseDamage(wd, src, target, 0, 0); // base damage
        wd.BaseDamage = wd.Damage;
        // Now get actual skill damage
        if (skillID != 0)
            BattleCalcSkillBaseDamage(wd, src, target, skillID, skillLevel); // base skill damage

        // Skill ratio
        AttackRate(wd, isLeftHanded, BattleCalcAttackSkillRatio(wd, src, target, skillID, skillLevel));

        // Additive damage bonus
        int64 constantAddition = BattleCalcSkillConstantAddition(wd, src, target, skillID, skillLevel);
        AttackAdd(wd, isLeftHanded, constantAddition, constantAddition);

        // add any miscellaneous player ATK bonuses
        int32 i = 0;
        if (sd != nullptr && skillID != 0 && (i = GetDeferredBonus2(src, "bSkillAtk", skillID)) != 0)
            AttackAddRate(wd, isLeftHanded, i);
        if (tsd != nullptr && (i = GetDeferredBonus2(target, "bSubSkill", skillID)) != 0)
            AttackAddRate(wd, isLeftHanded, -i);

        // final attack bonuses that aren't affected by cards
        BattleAttackSCBonus(wd, src, target, skillID, skillLevel);

        if (wd.Damage + wd.Damage2 != 0)
        {
            // Shield Boomerang and Rapid Smiting already calculated the defense before the skill ratio was applied
            if (skillID != ROWOW_SKILL_PA_SHIELDCHAIN && skillID != ROWOW_SKILL_CR_SHIELDBOOMERANG)
                BattleCalcDefenseReduction(wd, src, target, skillID, skillLevel);
            BattleCalcAttackPostDefense(wd, src, target, skillID, skillLevel);
        }
    }

    BattleCalcElementDamage(wd, src, target, skillID, skillLevel);

    // The Taekwon's kicks with bare hands: 10 a level of Running
    switch (skillID)
    {
        case ROWOW_SKILL_TK_DOWNKICK:
        case ROWOW_SKILL_TK_STORMKICK:
        case ROWOW_SKILL_TK_TURNKICK:
        case ROWOW_SKILL_TK_COUNTER:
            if (sd != nullptr && sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_FIST && sstatus->LeftWeaponType == ROWOW_WEAPON_TYPE_FIST)
                AttackAdd(wd, isLeftHanded, 10 * PCCheckSkill(sd, ROWOW_SKILL_TK_RUN), 10 * PCCheckSkill(sd, ROWOW_SKILL_TK_RUN));
            break;
    }
    if (sd != nullptr)
    {
        int32 skill = 0;
        if ((skill = PCCheckSkill(sd, ROWOW_SKILL_BS_WEAPONRESEARCH)) > 0)
            AttackAdd(wd, isLeftHanded, skill * 2, skill * 2);
        if (skillID == ROWOW_SKILL_GS_GROUNDDRIFT)
            AttackAdd(wd, isLeftHanded, 50 * skillLevel, 50 * skillLevel);
        if (skillID != ROWOW_SKILL_MC_CARTREVOLUTION && PCCheckSkill(sd, ROWOW_SKILL_BS_HILTBINDING) > 0)
            AttackAdd(wd, isLeftHanded, 4, 4);

        // Card Fix for attacker (sd), 2 is added to the "left" flag meaning "attacker cards only"
        wd.Damage += BattleCalcCardFix(ROWOW_BF_WEAPON, src, target, nk, skillID, rightElement, leftElement, wd.Damage, 2, wd.Flag);
        if (isLeftHanded == true)
            wd.Damage2 += BattleCalcCardFix(ROWOW_BF_WEAPON, src, target, nk, skillID, rightElement, leftElement, wd.Damage2, 3, wd.Flag);
    }
    if (tsd != nullptr)
    {
        // Card Fix for target (tsd), 2 is not added to the "left" flag meaning "target cards only"
        wd.Damage += BattleCalcCardFix(ROWOW_BF_WEAPON, src, target, nk, skillID, rightElement, leftElement, wd.Damage, 0, wd.Flag);
        if (isLeftHanded == true)
            wd.Damage2 += BattleCalcCardFix(ROWOW_BF_WEAPON, src, target, nk, skillID, rightElement, leftElement, wd.Damage2, 1, wd.Flag);
    }

    // only do 1 dmg to plant, no need to calculate rest
    if (infdef == true)
    {
        BattleCalcAttackPlant(wd, src, target, skillID, skillLevel);
        return wd;
    }

    // Apply DAMAGE_DIV_FIX and check for min damage
    BattleApplyDivFix(wd, skillID);

    BattleCalcAttackLeftRightHands(wd, src, target, skillID, skillLevel);

    // battle_calc_attack_gvg_bg: the damage the target's cards bounce back (Item reflect gets calculated before any mapflag reducing is
    // applicated), then the final damage of each hand (there is no War of Emperium or battleground here)
    if (wd.Damage + wd.Damage2 != 0)
    {
        // Don't reflect your own damage (Grand Cross)
        if (src != target)
        {
            int64 rdamage = BattleCalcReturnDamage(target, src, wd.Damage + wd.Damage2, wd.Flag, skillID, false);
            if (rdamage > 0)
            {
                // Use Reflect Shield to signal this kind of skill trigger [Skotlex].  A devoted attacker's Crusader may take it
                // (devotion_rdamage).  (The drain of the target's cards on it, battle_drain, comes with the deferred bonuses)
                Unit* d_bl = BattleCheckDevotion(src);
                uint64 tick = GetTick();
                BattleDelayDamage(tick, wd.AMotion, target, d_bl == nullptr ? src : d_bl, 0, ROWOW_SKILL_CR_REFLECTSHIELD, 0, rdamage, ROWOW_ATK_DEF, 1, true, false);
                SkillAdditionalEffect(target, d_bl == nullptr ? src : d_bl, ROWOW_SKILL_CR_REFLECTSHIELD, 1, ROWOW_BF_WEAPON | ROWOW_BF_SHORT | ROWOW_BF_NORMAL,
                    ROWOW_ATK_DEF, tick);
            }
        }

        if (wd.Damage2 == 0)
            wd.Damage = BattleCalcDamage(src, target, wd, wd.Damage, skillID, skillLevel);
        else if (wd.Damage == 0)
            wd.Damage2 = BattleCalcDamage(src, target, wd, wd.Damage2, skillID, skillLevel);
        else
        {
            wd.Damage = BattleCalcDamage(src, target, wd, wd.Damage, skillID, skillLevel);
            wd.Damage2 = BattleCalcDamage(src, target, wd, wd.Damage2, skillID, skillLevel);
            if (wd.Damage > 1 && wd.Damage2 < 1)
                wd.Damage2 = 1;
        }
    }

    BattleCalcWeaponFinalAtkModifiers(wd, src, target, skillID, skillLevel);

    // (battle_absorb_damage is the gear's, bAbsorbDmgMaxHP, which no pre-renewal item has)
    BattleDoReflect(ROWOW_BF_WEAPON, wd, src, target, skillID, skillLevel);
    return wd;
}

int32 ROWOWMod::BattleCalcAttackSkillRatio(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
{
    // battle_calc_attack_skill_ratio.  (bSkillRatio is a renewal bonus; Crush Strike and Giant Growth are third jobs')
    int32 skillratio = 100;
    ROWOWStatusChange* sc = GetStatusChange(src);

    // Skill damage modifiers that stack linearly
    if (sc != nullptr && skillID != ROWOW_SKILL_PA_SACRIFICE)
    {
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_OVERTHRUST))
            skillratio += sce->Val3;
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_MAXOVERTHRUST))
            skillratio += sce->Val2;
        if (sc->HasSCE(ROWOW_SC_BERSERK) == true)
            skillratio += 100;
        if (skillID == 0 || skillID == ROWOW_SKILL_KN_AUTOCOUNTER)
        {
            if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_POISONREACT); sce != nullptr && sce->Val4 == 1)
            {
                // Damage boost from poison react (for players bonus depends on level learned)
                if (src->IsPlayer() == true)
                    skillratio += 30 * PCCheckSkill(src, ROWOW_SKILL_AS_POISONREACT);
                else
                    skillratio += 30 * sce->Val1;
                // This attack has a chance to cause poison
                int32 poisonRate = sce->Val3;
                int32 poisonLevel = sce->Val1;
                ROWOWStatus const* sstatus = GetROStatus(src);
                SCStart2(src, target, ROWOW_SC_POISON, poisonRate, poisonLevel, int32(src->GetGUID().GetCounter()),
                    SkillGetTime2(ROWOW_SKILL_AS_POISONREACT, uint16(poisonLevel)), sstatus->AttackMotion);
                StatusChangeEnd(src, ROWOW_SC_POISONREACT);
            }
        }
    }

    if (ROWOWSkillImpl const* impl = GetSkillImpl(skillID))
        impl->CalculateSkillRatio(&wd, src, target, skillLevel, skillratio, 0);
    return skillratio;
}

int64 ROWOWMod::BattleCalcSkillConstantAddition(ROWOWDamage& /*wd*/, Unit* src, Unit* /*target*/, uint16 skillID, uint16 skillLevel) const
{
    // battle_calc_skill_constant_addition: Constant/misc additions from skills
    ROWOWStatus const* sstatus = GetROStatus(src);
    int64 atk = 0;
    switch (skillID)
    {
        case ROWOW_SKILL_MO_EXTREMITYFIST:
            atk = 250 + 150 * skillLevel;
            break;
        case ROWOW_SKILL_PA_SHIELDCHAIN:
            if (Player* sd = src->ToPlayer())
            {
                // Bonus damage: [max(100, Random(100, 0.7*weight + pow(skill level + refine)))]
                Item* shield = GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_HAND_L);
                ROWOWItem const* roShield = GetROItem(shield);
                if (roShield != nullptr && roShield->Type == ROWOW_ITEM_TYPE_ARMOR)
                {
                    // First calculate the random part of the bonus
                    int32 bonus = (7 * roShield->Weight) / 100;
                    bonus += int32(std::pow(skillLevel + int32(GetItemRefine(shield)), 2));
                    // Now get a random value between 100 and the random part
                    atk = std::max(100, int32(urand(uint32(std::min(100, bonus)), uint32(std::max(100, bonus)))));
                }
            }
            break;
        case ROWOW_SKILL_GS_MAGICALBULLET:
            if (sstatus->MagicAttackMax > sstatus->MagicAttackMin)
                atk = sstatus->MagicAttackMin + int32(urand(0, sstatus->MagicAttackMax - sstatus->MagicAttackMin - 1));
            else
                atk = sstatus->MagicAttackMin;
            break;
    }
    return atk;
}

bool ROWOWMod::BattleSkillStacksMasteriesVVS(uint16 skillID, bool isRefineOrStar)
{
    // battle_skill_stacks_masteries_vvs: the skills unaffected by refine bonus, masteries, Star Crumbs and Spirit Spheres
    switch (skillID)
    {
        // PC skills that are unaffected
        case ROWOW_SKILL_PA_SHIELDCHAIN:
        case ROWOW_SKILL_CR_SHIELDBOOMERANG:
        case ROWOW_SKILL_AM_ACIDTERROR:
        case ROWOW_SKILL_MO_INVESTIGATE:
        case ROWOW_SKILL_MO_EXTREMITYFIST:
        case ROWOW_SKILL_PA_SACRIFICE:
        case ROWOW_SKILL_NPC_DRAGONBREATH:
            return false;
        case ROWOW_SKILL_LK_SPIRALPIERCE:
            // In Pre-Renewal Spiral Pierce is influenced only by refine bonus and Star Crumbs for players
            if (isRefineOrStar == false)
                return false;
            break;
    }
    return true;
}

ROWOWDamage ROWOWMod::BattleCalcMagicAttack(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 mflag)
{
    // battle_calc_magic_attack: Calculate "magic"-type attacks and skills
    ROWOWDamage ad;
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    ROWOWSkill const* skill = GetSkill(skillID);
    if (sstatus == nullptr || tstatus == nullptr || skill == nullptr)
    {
        ad.DmgLv = ROWOW_ATK_NONE;
        return ad;
    }
    Player* sd = src->ToPlayer();
    Player* tsd = target->ToPlayer();

    // Initial Values
    // Set to 1 because magic damage on plants is 1 per hit; if target is not a plant this gets reinitialized to 0 later
    ad.Damage = 1;
    ad.Div = int16(ROWOWSkill::GetLevelValue(skill->HitCount, skillLevel));
    ad.AMotion = (skill->Inf & ROWOW_INF_GROUND_SKILL) != 0 ? 0 : sstatus->AttackMotion; // Amotion should be 0 for ground skills.
    ad.DMotion = tstatus->DamageMotion;
    ad.BlewCount = ROWOWSkill::GetLevelValue(skill->Knockback, skillLevel);
    ad.Flag = ROWOW_BF_MAGIC | ROWOW_BF_SKILL;
    ad.MiscFlag = mflag;
    ad.DmgLv = ROWOW_ATK_DEF;
    std::bitset<ROWOW_NK_MAX> nk = skill->NK;
    bool isIgnoringMdef = nk[ROWOW_NK_IGNOREDEFENSE];

    // Initialize variables that will be used afterwards
    int32 element = BattleGetMagicElement(ad, src, target, skillID, skillLevel);

    // Set miscellaneous data that needs be filled
    if (sd != nullptr)
        ad.BlewCount += BattleBlewCountBonus(src, skillID);

    // Skill Range Criteria
    ad.Flag |= BattleRangeType(src, target, skillID, skillLevel);

    // Update Damage data based on skill
    ROWOWSkillImpl const* impl = GetSkillImpl(skillID);
    if (impl != nullptr)
        impl->ModifyDamageData(ad, *src, *target, skillLevel);

    // Infinite defense (plant mode)
    if (IsInfiniteDefense(target, ad.Flag) == false)
    {
        // No need to do the math for plants
        int32 skillratio = 100; // Skill dmg modifiers.

        // Damage was set to 1 to simulate plant damage; if not plant, need to reinitialize damage with 0
        ad.Damage = 0;

        // Calc base damage according to skill
        int32 i = 0;
        switch (skillID)
        {
            case ROWOW_SKILL_AL_HEAL:
            case ROWOW_SKILL_PR_BENEDICTIO:
            case ROWOW_SKILL_PR_SANCTUARY:
                ad.Damage = SkillCalcHeal(src, target, skillID, skillLevel, false);
                break;
            case ROWOW_SKILL_PR_ASPERSIO:
                ad.Damage = 40;
                break;
            case ROWOW_SKILL_ALL_RESURRECTION:
            case ROWOW_SKILL_PR_TURNUNDEAD:
                // Undead check is on skill_castend_damageid code.
                i = 20 * skillLevel + sstatus->Stats[ROWOW_STAT_LUK] + sstatus->Stats[ROWOW_STAT_INT] + int32(sstatus->Level) + 200 -
                    int32(200 * int64(target->GetHealth()) / std::max<int64>(1, target->GetMaxHealth()));
                if (i > 700)
                    i = 700;
                if (int32(urand(0, 999)) < i && (tstatus->Mode & ROWOW_MODE_MVP) == 0)
                    ad.Damage = target->GetHealth();
                else
                    ad.Damage = int32(sstatus->Level) + sstatus->Stats[ROWOW_STAT_INT] + skillLevel * 10;
                break;
            case ROWOW_SKILL_NPC_DARKBREATH:
                ad.Damage = int64(target->GetHealth()) * (skillLevel <= 5 ? 100 / (2 * (6 - skillLevel)) : 50) / 100;
                break;
            case ROWOW_SKILL_PF_SOULBURN:
                ad.Damage = int64(target->GetPower(POWER_MANA)) * 2;
                break;
            case ROWOW_SKILL_NPC_EARTHQUAKE:
                if ((mflag & ROWOW_NPC_EARTHQUAKE_FLAG) != 0)
                {
                    ad.Flag |= ROWOW_NPC_EARTHQUAKE_FLAG; // Pass flag to battle_calc_damage
                    mflag &= ~ROWOW_NPC_EARTHQUAKE_FLAG; // Remove before NK_SPLASHSPLIT check
                }
                if (sd != nullptr)
                    ad.Damage = sstatus->BaseAttack + sstatus->WeaponAttack;
                else
                {
                    ad.Damage = BattleCalcBaseDamage(src, *sstatus, false, tstatus->Size, 0);
                    if (ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src)); sc != nullptr && sc->IsEmpty() == false)
                        ad.Damage = ad.Damage * BattleGetAtkPercent(src, skillID, *sc) / 100; // MATK_RATE
                }
                ad.Damage = ad.Damage * (200 + 100 * skillLevel + 100 * (skillLevel / 2) + (skillLevel > 4 ? 100 : 0)) / 100; // MATK_RATE
                if (nk[ROWOW_NK_SPLASHSPLIT] == true && mflag > 1)
                    ad.Damage /= mflag;
                break;
            default:
            {
                if (sstatus->MagicAttackMax > sstatus->MagicAttackMin)
                    ad.Damage += sstatus->MagicAttackMin + int32(urand(0, sstatus->MagicAttackMax - sstatus->MagicAttackMin - 1));
                else
                    ad.Damage += sstatus->MagicAttackMin;

                if (nk[ROWOW_NK_SPLASHSPLIT] == true)
                {
                    // Divide MATK in case of multiple targets skill
                    if (mflag > 0)
                        ad.Damage /= mflag;
                    else
                        LOG_ERROR("module.ROWOW", "ROWOWMod::BattleCalcMagicAttack: 0 enemies targeted by {}, divide per 0 avoided!", skillID);
                }

                if (impl != nullptr)
                    impl->CalculateSkillRatio(&ad, src, target, skillLevel, skillratio, mflag);
                ad.Damage = ad.Damage * skillratio / 100; // MATK_RATE

                // Constant/misc additions from skills
                if (skillID == ROWOW_SKILL_WZ_FIREPILLAR)
                    ad.Damage += 100 + 50 * skillLevel;
                break;
            }
        }

        if (sd != nullptr)
        {
            // Damage bonuses
            if ((i = GetDeferredBonus2(src, "bSkillAtk", skillID)) != 0)
                ad.Damage += ad.Damage * i / 100;

            // Ignore Defense?
            if (isIgnoringMdef == false && (HasDeferredBonusKey(src, "bIgnoreMdefEle", tstatus->DefElement) == true ||
                HasDeferredBonusKey(src, "bIgnoreMdefEle", ROWOW_ELEMENT_ALL) == true || HasDeferredBonusKey(src, "bIgnoreMdefRace", tstatus->Race) == true ||
                HasDeferredBonusKey(src, "bIgnoreMdefRace", ROWOW_RACE_ALL) == true || HasDeferredBonusKey(src, "bIgnoreMdefClass", tstatus->Class) == true ||
                HasDeferredBonusKey(src, "bIgnoreMdefClass", ROWOW_CLASS_ALL) == true))
                isIgnoringMdef = true;
        }
        if (tsd != nullptr && (i = GetDeferredBonus2(target, "bSubSkill", skillID)) != 0)
            ad.Damage -= ad.Damage * i / 100;

        if (isIgnoringMdef == false)
        {
            // Bonuses ignoring Mdef are added together.  (Expiatio's is a third job's)
            int32 mdef = tstatus->MDef;
            int32 mdef2 = tstatus->MDef2;
            i = 0; // Bonus ratio that ignores Mdef
            if (sd != nullptr)
                i += GetDeferredBonus2(src, "bIgnoreMdefRaceRate", tstatus->Race) + GetDeferredBonus2(src, "bIgnoreMdefRaceRate", ROWOW_RACE_ALL) +
                    GetDeferredBonus2(src, "bIgnoreMdefClassRate", tstatus->Class) + GetDeferredBonus2(src, "bIgnoreMdefClassRate", ROWOW_CLASS_ALL);
            i = std::clamp(i, 0, 100);

            // On pre-renewal, Mdef reduction is rounded down before being subtracted from Mdef
            if (i > 0)
                mdef -= mdef * i / 100;
            if (BattleConfig.MagicDefenseType != 0)
                ad.Damage = ad.Damage - mdef * BattleConfig.MagicDefenseType - mdef2;
            else
                ad.Damage = ad.Damage * (100 - mdef) / 100 - mdef2;
        }

        // Apply the physical part of the skill's damage. [Skotlex]  (The elemental options are the third jobs')
        switch (skillID)
        {
            case ROWOW_SKILL_CR_GRANDCROSS:
            case ROWOW_SKILL_NPC_GRANDDARKNESS:
            {
                // Pre-re ATK = Take atk, apply def reduction and add refine bonus; Final Damage = (ATK+MATK)*RATIO.  No need to go through
                // the whole physical damage code
                ROWOWDamage wd = InitializeWeaponData(src, target, skillID, skillLevel, mflag);
                BattleCalcSkillBaseDamage(wd, src, target, skillID, skillLevel);
                // Calculate ATK
                BattleCalcDefenseReduction(wd, src, target, skillID, skillLevel);
                if (sd != nullptr)
                    wd.Damage += sstatus->WeaponRefineAttack;
                // Combine ATK and MATK
                ad.Damage = std::max<int64>(1, wd.Damage + ad.Damage);
                // Ratio
                skillratio += 40 * skillLevel;
                ad.Damage = ad.Damage * skillratio / 100; // MATK_RATE
                break;
            }
        }

        if (ad.Damage < 1)
            ad.Damage = 1;

        if (nk[ROWOW_NK_IGNOREELEMENT] == false)
            ad.Damage = BattleAttrFix(src, target, ad.Damage, element, tstatus->DefElement, tstatus->ElementLevel, 0);

        // Card Fix for attacker and target
        ad.Damage += BattleCalcCardFix(ROWOW_BF_MAGIC, src, target, nk, skillID, element, 0, ad.Damage, 0, ad.Flag);

        switch (skillID)
        {
            case ROWOW_SKILL_CR_GRANDCROSS:
            case ROWOW_SKILL_NPC_GRANDDARKNESS:
                if (src == target)
                {
                    // Grand Cross on self first applies attr_fix, then cardfix and finally halves the damage
                    if (src->IsPlayer() == true)
                        ad.Damage = ad.Damage / 2;
                    else
                        ad.Damage = 0;
                }
                else // Grand Cross on target applies attr_fix, then cardfix and then attr_fix a second time
                    ad.Damage = BattleAttrFix(src, target, ad.Damage, element, tstatus->DefElement, tstatus->ElementLevel, 0);
                break;
        }
    } // Hint: Against plants damage will still be 1 at this point

    // Apply DAMAGE_DIV_FIX and check for min damage
    BattleApplyDivFix(ad, skillID);
    ad.Damage = BattleCalcDamage(src, target, ad, ad.Damage, skillID, skillLevel);

    // (battle_absorb_damage is the gear's, bAbsorbDmgMaxHP, which no pre-renewal item has; magic is reflected by its own statuses in
    // skill_attack)
    return ad;
}

ROWOWDamage ROWOWMod::BattleCalcMiscAttack(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 mflag)
{
    // battle_calc_misc_attack: Calculate "misc"-type attacks and skills
    ROWOWDamage md;
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    ROWOWSkill const* skill = GetSkill(skillID);
    if (sstatus == nullptr || tstatus == nullptr || skill == nullptr)
    {
        md.DmgLv = ROWOW_ATK_NONE;
        return md;
    }
    int32 levelIndex = ROWOWSkill::GetLevelIndex(skillLevel);
    Player* sd = src->ToPlayer();
    Player* tsd = target->ToPlayer();

    // Some initial values
    md.AMotion = (skill->Inf & ROWOW_INF_GROUND_SKILL) != 0 ? 0 : sstatus->AttackMotion;
    md.DMotion = tstatus->DamageMotion;
    md.Div = int16(ROWOWSkill::GetLevelValue(skill->HitCount, skillLevel));
    md.BlewCount = ROWOWSkill::GetLevelValue(skill->Knockback, skillLevel);
    md.DmgLv = ROWOW_ATK_DEF;
    md.Flag = ROWOW_BF_MISC | ROWOW_BF_SKILL;
    md.MiscFlag = mflag;
    std::bitset<ROWOW_NK_MAX> nk = skill->NK;
    if (sd != nullptr)
        md.BlewCount += BattleBlewCountBonus(src, skillID);
    int32 element = BattleGetMiscElement(md, src, target, skillID, skillLevel);

    // Skill Range Criteria
    md.Flag |= BattleRangeType(src, target, skillID, skillLevel);

    // Update Damage data based on skill
    if (ROWOWSkillImpl const* impl = GetSkillImpl(skillID))
        impl->ModifyDamageData(md, *src, *target, skillLevel);

    // (The third jobs' and the homunculi's come with them)
    switch (skillID)
    {
        case ROWOW_SKILL_TF_THROWSTONE:
            md.Damage = sd != nullptr ? 50 : 30;
            md.Flag |= ROWOW_BF_WEAPON;
            break;
        case ROWOW_SKILL_HT_LANDMINE:
        case ROWOW_SKILL_MA_LANDMINE:
            md.Damage = int64(skillLevel * (sstatus->Stats[ROWOW_STAT_DEX] + 75.0) * (100.0 + sstatus->Stats[ROWOW_STAT_INT]) / 100.0);
            break;
        case ROWOW_SKILL_HVAN_EXPLOSION:
            md.Damage = int64(sstatus->MaxHP) * (50 + 50 * skillLevel) / 100;
            break;
        case ROWOW_SKILL_HT_BLASTMINE:
            md.Damage = int64(skillLevel * (sstatus->Stats[ROWOW_STAT_DEX] / 2.0 + 50.0) * (100.0 + sstatus->Stats[ROWOW_STAT_INT]) / 100.0);
            break;
        case ROWOW_SKILL_HT_CLAYMORETRAP:
            md.Damage = int64(skillLevel * (sstatus->Stats[ROWOW_STAT_DEX] / 2.0 + 75.0) * (100.0 + sstatus->Stats[ROWOW_STAT_INT]) / 100.0);
            break;
        case ROWOW_SKILL_HT_BLITZBEAT:
        case ROWOW_SKILL_SN_FALCONASSAULT:
        {
            // Blitz-beat Damage
            int32 steelCrow = PCCheckSkill(src, ROWOW_SKILL_HT_STEELCROW);
            md.Damage = (sstatus->Stats[ROWOW_STAT_DEX] / 10 + sstatus->Stats[ROWOW_STAT_INT] / 2 + steelCrow * 3 + 40) * 2;
            if (mflag > 1) // Autocasted Blitz
                nk.set(ROWOW_NK_SPLASHSPLIT);
            if (skillID == ROWOW_SKILL_SN_FALCONASSAULT)
            {
                // Div fix of Blitzbeat (DAMAGE_DIV_FIX2)
                ROWOWSkill const* blitzBeat = GetSkill(ROWOW_SKILL_HT_BLITZBEAT);
                int32 blitzHits = blitzBeat != nullptr ? ROWOWSkill::GetLevelValue(blitzBeat->HitCount, 5) : 1;
                if (blitzHits > 1)
                    md.Damage *= blitzHits;
                // Falcon Assault Modifier
                md.Damage = md.Damage * (150 + 70 * skillLevel) / 100;
            }
            break;
        }
        case ROWOW_SKILL_BA_DISSONANCE:
            md.Damage = 30 + 10 * skillLevel;
            md.Damage += skillLevel * PCCheckSkill(src, ROWOW_SKILL_BA_MUSICALLESSON);
            break;
        case ROWOW_SKILL_NPC_SELFDESTRUCTION:
            md.Damage = src->GetHealth();
            break;
        case ROWOW_SKILL_NPC_SMOKING:
            md.Damage = 3;
            break;
        case ROWOW_SKILL_NPC_EVILLAND:
            md.Damage = SkillCalcHeal(src, target, skillID, skillLevel, false);
            break;
        case ROWOW_SKILL_ASC_BREAKER:
            md.Damage = 500 + int32(urand(0, 499)) + 5 * skillLevel * sstatus->Stats[ROWOW_STAT_INT];
            nk.set(ROWOW_NK_IGNOREFLEE);
            nk.set(ROWOW_NK_IGNOREELEMENT); // These two are not properties of the weapon based part.
            break;
        case ROWOW_SKILL_HW_GRAVITATION:
            md.Damage = 200 + 200 * skillLevel;
            md.DMotion = 0; // No flinch animation
            break;
        case ROWOW_SKILL_PA_PRESSURE:
            md.Damage = 500 + 300 * skillLevel;
            break;
        case ROWOW_SKILL_PA_GOSPEL:
            if (mflag > 0)
                md.Damage = int32(urand(0, 3999)) + 1500;
            else
            {
                md.Damage = int32(urand(0, 4999)) + 3000;
                md.Damage -= (md.Damage * int64(tstatus->Def)) / 100;
                md.Damage -= tstatus->Def2;
                if (md.Damage < 0)
                    md.Damage = 0;
            }
            break;
        case ROWOW_SKILL_CR_ACIDDEMONSTRATION:
            if (tstatus->Stats[ROWOW_STAT_VIT] + sstatus->Stats[ROWOW_STAT_INT] != 0) // crash fix
                md.Damage = int32(int64(7) * tstatus->Stats[ROWOW_STAT_VIT] * sstatus->Stats[ROWOW_STAT_INT] * sstatus->Stats[ROWOW_STAT_INT] /
                    (10 * (tstatus->Stats[ROWOW_STAT_VIT] + sstatus->Stats[ROWOW_STAT_INT])));
            else
                md.Damage = 0;
            if (tsd != nullptr)
                md.Damage /= 2;
            break;
        case ROWOW_SKILL_NJ_ZENYNAGE:
            md.Damage = skill->ZenyCost[levelIndex];
            if (md.Damage == 0)
                md.Damage = 2;
            md.Damage += urand(0, uint32(md.Damage));
            // Specific to Boss Class
            if (tstatus->Class == ROWOW_CLASS_BOSS)
                md.Damage /= 3;
            if (tsd != nullptr)
                md.Damage /= 2;
            break;
        case ROWOW_SKILL_GS_FLING:
        {
            ROWOWIdentity const* identity = sd != nullptr ? sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
            md.Damage = identity != nullptr ? identity->JobLevel : sstatus->Level;
            break;
        }
    }

    if (nk[ROWOW_NK_SPLASHSPLIT] == true)
    {
        // Divide ATK among targets
        if (mflag > 0)
            md.Damage /= mflag;
        else
            LOG_ERROR("module.ROWOW", "ROWOWMod::BattleCalcMiscAttack: 0 enemies targeted by {}, divide per 0 avoided!", skillID);
    }

    if (nk[ROWOW_NK_IGNOREFLEE] == false)
    {
        // Temp for "hit or no hit": a frozen, stoned, stunned or sleeping target is always hit
        ROWOWStatusChange const* tsc = GetStatusChange(static_cast<Unit const*>(target));
        if (tsc == nullptr || tsc->Opt1 == ROWOW_OPT1_NONE || tsc->Opt1 == ROWOW_OPT1_STONEWAIT || tsc->Opt1 == ROWOW_OPT1_BURNING)
        {
            int32 flee = tstatus->Flee;
            int32 hitrate = 80; // Default hitrate
            if (BattleConfig.AgiPenaltyType != 0 && (BattleConfig.AgiPenaltyTarget & GetBattleObjectType(*tstatus)) != 0)
            {
                int32 attackerCount = int32(std::min<size_t>(target->getAttackers().size(), 255));
                if (attackerCount >= BattleConfig.AgiPenaltyCount)
                {
                    if (BattleConfig.AgiPenaltyType == 1)
                        flee = (flee * (100 - (attackerCount - (BattleConfig.AgiPenaltyCount - 1)) * BattleConfig.AgiPenaltyNum)) / 100;
                    else // assume type 2: absolute reduction
                        flee -= (attackerCount - (BattleConfig.AgiPenaltyCount - 1)) * BattleConfig.AgiPenaltyNum;
                    if (flee < 1)
                        flee = 1;
                }
            }
            hitrate += sstatus->Hit - flee;
            hitrate = std::clamp(hitrate, BattleConfig.MinHitRate, BattleConfig.MaxHitRate);
            if (int32(urand(0, 99)) >= hitrate)
            {
                md.Damage = 0;
                md.DmgLv = ROWOW_ATK_FLEE;
            }
        }
    }

    md.Damage += BattleCalcCardFix(ROWOW_BF_MISC, src, target, nk, skillID, element, 0, md.Damage, 0, md.Flag);
    int32 i = 0;
    if (sd != nullptr && (i = GetDeferredBonus2(src, "bSkillAtk", skillID)) != 0)
        md.Damage += md.Damage * i / 100;
    if (tsd != nullptr && (i = GetDeferredBonus2(target, "bSubSkill", skillID)) != 0)
        md.Damage -= md.Damage * i / 100;
    if (nk[ROWOW_NK_IGNOREELEMENT] == false)
        md.Damage = BattleAttrFix(src, target, md.Damage, element, tstatus->DefElement, tstatus->ElementLevel, 0);

    // Plant damage
    if (md.Damage < 0)
        md.Damage = 0;
    else if (md.Damage != 0 && IsInfiniteDefense(target, md.Flag) == true)
        md.Damage = 1;

    // Apply DAMAGE_DIV_FIX and check for min damage
    BattleApplyDivFix(md, skillID);

    // Throwing Zeny pays what it deals
    if (skillID == ROWOW_SKILL_NJ_ZENYNAGE && sd != nullptr)
    {
        if (md.Damage > int64(sd->GetMoney()))
            md.Damage = sd->GetMoney();
        sd->ModifyMoney(-int32(md.Damage));
    }

    md.Damage = BattleCalcDamage(src, target, md, md.Damage, skillID, skillLevel);

    // (battle_absorb_damage is the gear's, bAbsorbDmgMaxHP, which no pre-renewal item has)
    BattleDoReflect(ROWOW_BF_MISC, md, src, target, skillID, skillLevel);
    return md;
}

bool ROWOWMod::IsAttackCritical(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool firstCall)
{
    // is_attack_critical
    if (firstCall == false)
        return wd.Type == ROWOW_DMG_CRITICAL;

    if (skillID == ROWOW_SKILL_NPC_CRITICALSLASH) // Always critical skills
        return true;
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skillID != 0 && (skill == nullptr || skill->NK[ROWOW_NK_CRITICAL] == false))
        return false;

    ROWOWStatus const* sstatus = GetROStatus(src);
    if (sstatus->Critical == 0)
        return false;
    Player* sd = src->ToPlayer();
    if (wd.Type == ROWOW_DMG_MULTI_HIT)
    {
        // Multiple Hit Attack Skills: Chain Action and Double Attack aren't critical skills
        ROWOWSkill const* chainAction = GetSkill(ROWOW_SKILL_GS_CHAINACTION);
        ROWOWSkill const* doubleAttack = GetSkill(ROWOW_SKILL_TF_DOUBLE);
        if (PCCheckSkill(sd, ROWOW_SKILL_GS_CHAINACTION) > 0 && (chainAction == nullptr || chainAction->NK[ROWOW_NK_CRITICAL] == false))
            return false;
        if (PCCheckSkill(sd, ROWOW_SKILL_TF_DOUBLE) > 0 && (doubleAttack == nullptr || doubleAttack->NK[ROWOW_NK_CRITICAL] == false))
            return false;
    }

    ROWOWStatus const* tstatus = GetROStatus(target);
    int32 cri = sstatus->Critical;
    if (sd != nullptr)
    {
        cri += sstatus->Bonus.CriticalAddRace[tstatus->Race] + sstatus->Bonus.CriticalAddRace[ROWOW_RACE_ALL];
        if (skillID == 0 && IsSkillUsingArrow(src, skillID) == true)
        {
            cri += sstatus->Bonus.ArrowCri;
            cri += sstatus->Bonus.CriticalRangeAtk;
        }
    }

    // The official equation is *2, but that only applies when sd's do critical.
    // Therefore, we use the old value 3 on cases when an sd gets attacked by a mob
    cri -= tstatus->Stats[ROWOW_STAT_LUK] * ((sd == nullptr && target->IsPlayer() == true) ? 3 : 2);

    ROWOWStatusChange const* tsc = GetStatusChange(target);
    if (tsc != nullptr && tsc->HasSCE(ROWOW_SC_SLEEP) == true)
        cri *= 2;

    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
    switch (skillID)
    {
        case 0:
            // A normal attack under the Auto Counter status is Auto Counter's
            if (sc == nullptr || sc->HasSCE(ROWOW_SC_AUTOCOUNTER) == false)
                break;
            StatusChangeEnd(src, ROWOW_SC_AUTOCOUNTER);
            [[fallthrough]];
        case ROWOW_SKILL_KN_AUTOCOUNTER:
            if (BattleConfig.AutoCounterType != 0 && (BattleConfig.AutoCounterType & GetBattleObjectType(*sstatus)) != 0)
                return true;
            cri *= 2;
            break;
        case ROWOW_SKILL_SN_SHARPSHOOTING:
        case ROWOW_SKILL_MA_SHARPSHOOTING:
            cri += 200;
            break;
        case ROWOW_SKILL_NJ_KIRIKAGE:
            cri += 250 + 50 * skillLevel;
            break;
    }
    if (target->IsPlayer() == true && tstatus->Bonus.CriticalDef != 0)
        cri = cri * (100 - tstatus->Bonus.CriticalDef) / 100;
    return int32(urand(0, 999)) < cri;
}

bool ROWOWMod::IsAttackHitting(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, bool firstCall)
{
    // is_attack_hitting
    if (firstCall == false)
        return wd.DmgLv != ROWOW_ATK_FLEE;
    ROWOWStatus const* sstatus = GetROStatus(src);
    std::bitset<ROWOW_NK_MAX> nk = BattleSkillGetDamageProperties(skillID, wd.MiscFlag);
    if (IsAttackCritical(wd, src, target, skillID, skillLevel, false) == true)
        return true;
    else if (src->IsPlayer() == true && sstatus->Bonus.PerfectHit > 0 && int32(urand(0, 99)) < sstatus->Bonus.PerfectHit)
        return true;
    else if (skillID == ROWOW_SKILL_AS_SPLASHER && wd.MiscFlag == 0)
        return true;
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
    ROWOWStatusChange const* tsc = GetStatusChange(static_cast<Unit const*>(target));
    if (sc != nullptr && sc->HasSCE(ROWOW_SC_FUSION) == true)
        return true;
    if (skillID == ROWOW_SKILL_CR_SHIELDBOOMERANG && sc != nullptr && sc->HasSCE(ROWOW_SC_SPIRIT) == true && sc->GetSCE(ROWOW_SC_SPIRIT)->Val2 == ROWOW_SKILL_SL_CRUSADER)
        return true;
    if (tsc != nullptr && tsc->Opt1 != ROWOW_OPT1_NONE && tsc->Opt1 != ROWOW_OPT1_STONEWAIT && tsc->Opt1 != ROWOW_OPT1_BURNING)
        return true; // A frozen, stoned, stunned or sleeping target
    if (nk[ROWOW_NK_IGNOREFLEE] == true)
        return true;

    ROWOWStatus const* tstatus = GetROStatus(target);
    int32 flee = tstatus->Flee;
    int16 hitrate = 80; // Default hitrate

    // The target's FLEE drops for each attacker past agi_penalty_count (unit_counttargeted: the units attacking it)
    if (BattleConfig.AgiPenaltyType != 0 && (BattleConfig.AgiPenaltyTarget & GetBattleObjectType(*tstatus)) != 0)
    {
        int32 attackerCount = int32(std::min<size_t>(target->getAttackers().size(), 255));
        if (attackerCount >= BattleConfig.AgiPenaltyCount)
        {
            if (BattleConfig.AgiPenaltyType == 1)
                flee = (flee * (100 - (attackerCount - (BattleConfig.AgiPenaltyCount - 1)) * BattleConfig.AgiPenaltyNum)) / 100;
            else // assume type 2: absolute reduction
                flee -= (attackerCount - (BattleConfig.AgiPenaltyCount - 1)) * BattleConfig.AgiPenaltyNum;
            if (flee < 1)
                flee = 1;
        }
    }

    hitrate += int16(sstatus->Hit - flee);

    // Fogwall's hit penalty is only for normal ranged attacks.
    if ((wd.Flag & (ROWOW_BF_LONG | ROWOW_BF_MAGIC)) == ROWOW_BF_LONG && skillID == 0 && tsc != nullptr && tsc->HasSCE(ROWOW_SC_FOGWALL) == true)
        hitrate -= 50;

    Player* sd = src->ToPlayer();
    if (sd != nullptr && IsSkillUsingArrow(src, skillID) == true)
        hitrate += int16(sstatus->Bonus.ArrowHit);
    if (skillID != 0)
    {
        if (ROWOWSkillImpl const* impl = GetSkillImpl(skillID))
            impl->ModifyHitRate(hitrate, src, target, skillLevel);
    }
    else if (sd != nullptr && (wd.Type & ROWOW_DMG_MULTI_HIT) != 0 && wd.Div == 2)
        hitrate += int16(PCCheckSkill(sd, ROWOW_SKILL_TF_DOUBLE)); // +1 hit per level of Double Attack on a successful double attack [helvetica]

    if (sd != nullptr)
    {
        // Weaponry Research hidden bonus
        int32 skill = 0;
        if ((skill = PCCheckSkill(sd, ROWOW_SKILL_BS_WEAPONRESEARCH)) > 0)
            hitrate += int16(hitrate * (2 * skill) / 100);
    }

    hitrate = int16(std::clamp<int32>(hitrate, BattleConfig.MinHitRate, BattleConfig.MaxHitRate));
    if (skillID == ROWOW_SKILL_PA_SHIELDCHAIN)
        hitrate += 20; // Rapid Smiting gives a flat +20 hit after the hitrate was capped
    return int32(urand(0, 99)) < hitrate;
}

bool ROWOWMod::AttackIgnoresDef(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand)
{
    // attack_ignores_def
    if (IsAttackCritical(wd, src, target, skillID, skillLevel, false) == true)
        return true;
    if (ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src)); sc != nullptr && sc->HasSCE(ROWOW_SC_FUSION) == true)
        return true;

    // Ignore Defense?  (Each hand's own bonuses: left_cardfix_to_right is off)
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (src->IsPlayer() == true && skillID != ROWOW_SKILL_CR_GRANDCROSS && skillID != ROWOW_SKILL_NPC_GRANDDARKNESS)
    {
        ROWOWWeaponBonus const& weaponBonus = isRightHand == true ? sstatus->Bonus.RightWeapon : sstatus->Bonus.LeftWeapon;
        if ((weaponBonus.IgnoreDefEle & (1 << tstatus->DefElement)) != 0 || (weaponBonus.IgnoreDefEle & (1 << ROWOW_ELEMENT_ALL)) != 0
            || (weaponBonus.IgnoreDefRace & (1 << tstatus->Race)) != 0 || (weaponBonus.IgnoreDefRace & (1 << ROWOW_RACE_ALL)) != 0
            || (weaponBonus.IgnoreDefClass & (1 << tstatus->Class)) != 0 || (weaponBonus.IgnoreDefClass & (1 << ROWOW_CLASS_ALL)) != 0)
            return true;
    }
    return BattleSkillGetDamageProperties(skillID, wd.MiscFlag)[ROWOW_NK_IGNOREDEFENSE] == true;
}

int32 ROWOWMod::IsAttackPiercing(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand)
{
    // is_attack_piercing: Investigate, and the defense-ratio bonuses of cards (Ice Pick and the like)
    if (skillID == ROWOW_SKILL_MO_INVESTIGATE)
        return 2;
    if (src->IsPlayer() == false || skillID == ROWOW_SKILL_PA_SACRIFICE || skillID == ROWOW_SKILL_CR_GRANDCROSS || skillID == ROWOW_SKILL_NPC_GRANDDARKNESS ||
        skillID == ROWOW_SKILL_PA_SHIELDCHAIN || IsAttackCritical(wd, src, target, skillID, skillLevel, false) == true)
        return 0;
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    ROWOWWeaponBonus const& weaponBonus = isRightHand == true ? sstatus->Bonus.RightWeapon : sstatus->Bonus.LeftWeapon;
    // Elemental/Racial adjustments
    if ((weaponBonus.DefRatioAtkEle & (1 << tstatus->DefElement)) != 0 || (weaponBonus.DefRatioAtkEle & (1 << ROWOW_ELEMENT_ALL)) != 0
        || (weaponBonus.DefRatioAtkRace & (1 << tstatus->Race)) != 0 || (weaponBonus.DefRatioAtkRace & (1 << ROWOW_RACE_ALL)) != 0
        || (weaponBonus.DefRatioAtkClass & (1 << tstatus->Class)) != 0 || (weaponBonus.DefRatioAtkClass & (1 << ROWOW_CLASS_ALL)) != 0)
        return 1;
    return 0;
}

bool ROWOWMod::IsInfiniteDefense(Unit const* target, int32 flag) const
{
    // is_infinite_defense: plants and the like take 1 damage from each kind of attack they ignore, and so does an invincible target
    ROWOWStatus const* tstatus = GetROStatus(target);
    if ((tstatus->Mode & ROWOW_MODE_IGNORE_MELEE) != 0 && (flag & (ROWOW_BF_WEAPON | ROWOW_BF_SHORT)) == (ROWOW_BF_WEAPON | ROWOW_BF_SHORT))
        return true;
    if ((tstatus->Mode & ROWOW_MODE_IGNORE_MAGIC) != 0 && (flag & ROWOW_BF_MAGIC) != 0)
        return true;
    if ((tstatus->Mode & ROWOW_MODE_IGNORE_RANGED) != 0 && (flag & (ROWOW_BF_WEAPON | ROWOW_BF_LONG)) == (ROWOW_BF_WEAPON | ROWOW_BF_LONG))
        return true;
    if ((tstatus->Mode & ROWOW_MODE_IGNORE_MISC) != 0 && (flag & ROWOW_BF_MISC) != 0)
        return true;
    ROWOWStatusChange const* tsc = GetStatusChange(target);
    if (tsc != nullptr && tsc->HasSCE(ROWOW_SC_INVINCIBLE) == true)
        return true;
    return false;
}

bool ROWOWMod::IsSkillUsingArrow(Unit const* src, uint16 skillID) const
{
    // is_skill_using_arrow.  A player's sd->state.arrow_atk is set by what attacks: a normal attack with a bow or gun (battle_weapon_attack),
    // a skill that needs ammo (skill_check_condition_castbegin's require.ammo), so it's worked out from the same here
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (sstatus == nullptr)
        return false;
    ROWOWSkill const* skill = GetSkill(skillID);
    if (src->IsPlayer() == true)
    {
        if (skillID == 0 ? sstatus->IsArrowAttack : (skill != nullptr && skill->RequiredAmmo != 0 && skill->AmmoAmount[0] > 0))
            return true;
    }
    else
    {
        if (skillID != 0 && skill != nullptr && skill->RequiredAmmo != 0)
            return true;
        if (sstatus->Range > 3)
            return true;
    }
    switch (skillID)
    {
        case ROWOW_SKILL_HT_FREEZINGTRAP:
        case ROWOW_SKILL_HT_PHANTASMIC:
        case ROWOW_SKILL_GS_GROUNDDRIFT:
            return true;
    }
    return false;
}

bool ROWOWMod::IsAttackRightHanded(Unit const* src, uint16 skillID) const
{
    // is_attack_right_handed: skills ALWAYS use ONLY your right-hand weapon (tested on Aegis 10.2)
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (skillID == 0 && src->IsPlayer() == true && sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_FIST && sstatus->LeftWeaponType != ROWOW_WEAPON_TYPE_FIST)
        return false;
    return true;
}

bool ROWOWMod::IsAttackLeftHanded(Unit const* src, uint16 skillID) const
{
    // is_attack_left_handed: skills ALWAYS use ONLY your right-hand weapon (tested on Aegis 10.2)
    if (skillID != 0)
        return false;
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (src->IsPlayer() == true)
    {
        if (sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_FIST && sstatus->LeftWeaponType != ROWOW_WEAPON_TYPE_FIST)
            return true;
        if (sstatus->WeaponType == ROWOW_WEAPON_TYPE_KATAR)
            return true;
    }
    return sstatus->LeftWeaponAttack != 0;
}

int32 ROWOWMod::BattleGetWeaponElement(ROWOWDamage const& wd, Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand,
    bool isCalcForDamageOnly) const
{
    // battle_get_weapon_element: the skill's element, or the weapon's (the ammo's on a weapon that isn't a bow or gun), which an endow
    // overrides.  (There are no spirit charms in pre-renewal jobs)
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWSkill const* skill = GetSkill(skillID);
    int32 element = skill != nullptr ? ROWOWSkill::GetLevelValue(skill->Element, skillLevel) : ROWOW_ELEMENT_NEUTRAL;

    // Take weapon's element
    if (skillID == 0 || element == ROWOW_ELEMENT_WEAPON)
    {
        element = isRightHand == true ? sstatus->WeaponElement : sstatus->LeftWeaponElement;
        if (IsSkillUsingArrow(src, skillID) == true && src->IsPlayer() == true && sstatus->ArrowElement != ROWOW_ELEMENT_NEUTRAL && isRightHand == true)
            element = sstatus->ArrowElement;
        // on official endows override all other elements [helvetica]
        if (ROWOWStatusChangeEntry const* sce = GetSCE(src, ROWOW_SC_ENCHANTARMS)) // Check for endows
            element = sce->Val1;
    }
    else if (element == ROWOW_ELEMENT_ENDOWED) // Use enchantment's element
        element = StatusCalcAttackElement(GetStatusChange(src), ROWOW_ELEMENT_NEUTRAL);
    else if (element == ROWOW_ELEMENT_RANDOM) // Use random element
        element = int32(urand(0, ROWOW_ELEMENT_COUNT - 1));

    // Modify the element type of skill attack
    if (ROWOWSkillImpl const* impl = GetSkillImpl(skillID))
        impl->ModifyElement(wd, *src, *target, skillLevel, element, isCalcForDamageOnly == true ? 1 : 0);
    return element;
}

int32 ROWOWMod::BattleGetMagicElement(ROWOWDamage const& d, Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel) const
{
    // battle_get_magic_element
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWSkill const* skill = GetSkill(skillID);
    int32 element = skill != nullptr ? ROWOWSkill::GetLevelValue(skill->Element, skillLevel) : ROWOW_ELEMENT_NEUTRAL;
    switch (element)
    {
        case ROWOW_ELEMENT_WEAPON: // pl = -1 : the skill takes the weapon's element
            element = sstatus->WeaponElement;
            break;
        case ROWOW_ELEMENT_ENDOWED: // Use status element
            element = StatusCalcAttackElement(GetStatusChange(src), ROWOW_ELEMENT_NEUTRAL);
            break;
        case ROWOW_ELEMENT_RANDOM: // Use random element
            element = int32(urand(0, ROWOW_ELEMENT_COUNT - 1));
            break;
    }

    // Modify the element type of skill attack
    if (ROWOWSkillImpl const* impl = GetSkillImpl(skillID))
        impl->ModifyElement(d, *src, *target, skillLevel, element, 0);
    return element;
}

int32 ROWOWMod::BattleGetMiscElement(ROWOWDamage const& d, Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel) const
{
    // battle_get_misc_element
    ROWOWSkill const* skill = GetSkill(skillID);
    int32 element = skill != nullptr ? ROWOWSkill::GetLevelValue(skill->Element, skillLevel) : ROWOW_ELEMENT_NEUTRAL;
    switch (element)
    {
        case ROWOW_ELEMENT_WEAPON:
        case ROWOW_ELEMENT_ENDOWED: // Attack that takes weapon's element for misc attacks? Make it neutral [Skotlex]
            element = ROWOW_ELEMENT_NEUTRAL;
            break;
        case ROWOW_ELEMENT_RANDOM: // Use random element
            element = int32(urand(0, ROWOW_ELEMENT_COUNT - 1));
            break;
    }

    // Modify the element type of skill attack
    if (ROWOWSkillImpl const* impl = GetSkillImpl(skillID))
        impl->ModifyElement(d, *src, *target, skillLevel, element, 0);
    return element;
}

void ROWOWMod::BattleCalcMultiAttack(ROWOWDamage& wd, Unit* src, Unit* /*target*/, uint16 skillID, uint16 /*skillLevel*/) const
{
    // battle_calc_multi_attack: a player's normal attack may hit twice.  (Fear Breeze, Kagemusya and Eternal Chain are third jobs' statuses)
    Player* sd = src->ToPlayer();
    if (sd == nullptr || skillID != 0)
        return;
    ROWOWStatus const* sstatus = GetROStatus(src);
    int32 skillLevel = 0;
    if (wd.Div == 1 && (((skillLevel = PCCheckSkill(sd, ROWOW_SKILL_TF_DOUBLE)) > 0 && sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_DAGGER)
        || (sstatus->Bonus.DoubleRate > 0 && sstatus->RightWeaponType != ROWOW_WEAPON_TYPE_FIST))) // Will fail bare-handed
    {
        // Success chance is not added, the higher one is used [Skotlex]
        int32 maxRate = std::max(5 * skillLevel, sstatus->Bonus.DoubleRate);
        if (int32(urand(0, 99)) < maxRate)
        {
            wd.Div = 2; // Double Attack's hits
            wd.Type = ROWOW_DMG_MULTI_HIT;
        }
    }
    if (wd.Div == 1 && sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_REVOLVER && (skillLevel = PCCheckSkill(sd, ROWOW_SKILL_GS_CHAINACTION)) > 0 &&
        int32(urand(0, 99)) < 5 * skillLevel) // Normal Chain Action effect
    {
        wd.Div = 2; // Chain Action's hits
        wd.Type = ROWOW_DMG_MULTI_HIT;
    }
}

void ROWOWMod::BattleCalcSkillBaseDamage(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
{
    // battle_calc_skill_base_damage: the skills with their own base damage, then the default case (a normal attack's base damage, which most
    // weapon skills start from).  (The other jobs' skills of its switch come with them)
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    Player* sd = src->ToPlayer();
    std::bitset<ROWOW_NK_MAX> nk = BattleSkillGetDamageProperties(skillID, wd.MiscFlag);
    bool isLeftHanded = IsAttackLeftHanded(src, skillID);

    // Calc base damage according to skill
    switch (skillID)
    {
        case ROWOW_SKILL_NJ_ISSEN:
            // Final Strike: 40 a point of STR and 8% a level of the HP the Ninja has
            wd.Damage = 40 * sstatus->Stats[ROWOW_STAT_STR] + int64(src->GetHealth()) * 8 * skillLevel / 100;
            wd.Damage2 = 0;
            return;
        case ROWOW_SKILL_PA_SACRIFICE:
            wd.Damage = sstatus->MaxHP * 9 / 100;
            wd.Damage2 = 0;
            return;
        case ROWOW_SKILL_HFLI_SBR44:
            if (GetFollowerType(src) == ROWOW_BL_HOM)
                if (ROWOWFollower const* fd = GetFollower(src))
                    wd.Damage = static_cast<ROWOWHomunData const*>(fd)->Intimacy;
            return;
        case ROWOW_SKILL_LK_SPIRALPIERCE:
        case ROWOW_SKILL_ML_SPIRALPIERCE:
        {
            if (sd != nullptr)
            {
                ROWOWItem const* weapon = GetROItem(GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_HAND_R));
                if (weapon != nullptr && weapon->Type == ROWOW_ITEM_TYPE_WEAPON)
                    wd.Damage = weapon->Weight * 8 / 100; // 80% of weight
                AttackAddRate(wd, isLeftHanded, 50 * skillLevel); // Skill modifier applies to weight only.
            }
            else
                wd.Damage = BattleCalcBaseDamage(src, *sstatus, false, tstatus->Size, 0); // Monsters have no weight and use ATK instead
            int32 i = sstatus->Stats[ROWOW_STAT_STR] / 10;
            i *= i;
            AttackAdd(wd, isLeftHanded, i, i); // Add str bonus.
            switch (tstatus->Size)
            {
                // Size-fix.  Is this modified by weapon perfection?
                case ROWOW_SIZE_SMALL: // Small: 125%
                    AttackRate(wd, isLeftHanded, 125);
                    break;
                case ROWOW_SIZE_BIG: // Large: 75%
                    AttackRate(wd, isLeftHanded, 75);
                    break;
            }
            return;
        }
        case ROWOW_SKILL_CR_SHIELDBOOMERANG:
        case ROWOW_SKILL_PA_SHIELDCHAIN:
            wd.Damage = sstatus->BaseAttack;
            if (sd != nullptr)
            {
                // Base damage of shield skills is [batk + 4*refine + weight]
                Item* shield = GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_HAND_L);
                ROWOWItem const* roShield = GetROItem(shield);
                if (roShield != nullptr && roShield->Type == ROWOW_ITEM_TYPE_ARMOR)
                {
                    AttackAdd(wd, isLeftHanded, 4 * int64(GetItemRefine(shield)), 4 * int64(GetItemRefine(shield)));
                    AttackAdd(wd, isLeftHanded, roShield->Weight / 10, roShield->Weight / 10);
                }
                // Shield Boomerang and Rapid Smiting calculate DEF before the skill ratio
                BattleCalcDefenseReduction(wd, src, target, skillID, skillLevel);
            }
            else
                AttackAdd(wd, isLeftHanded, sstatus->WeaponRefineAttack, sstatus->WeaponRefineAttack); // Else use Atk2
            return;
    }

    // Flags that apply to both pre-renewal and renewal
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
    int32 bflag = ROWOW_BDMG_NONE;
    if (IsAttackCritical(wd, src, target, skillID, skillLevel, false) == true)
        bflag |= ROWOW_BDMG_CRIT;
    if (skillID == 0 && sc != nullptr && sc->HasSCE(ROWOW_SC_CHANGE) == true)
        bflag |= ROWOW_BDMG_MAGIC;

    // Pre-renewal exclusive flags
    if (skillID == ROWOW_SKILL_HW_MAGICCRASHER)
        bflag |= ROWOW_BDMG_MAGIC;
    if (sc != nullptr && sc->HasSCE(ROWOW_SC_WEAPONPERFECTION) == true)
        bflag |= ROWOW_BDMG_NOSIZE;
    if (IsSkillUsingArrow(src, skillID) == true && sd != nullptr)
    {
        switch (sstatus->WeaponType)
        {
            case ROWOW_WEAPON_TYPE_BOW:
                bflag |= ROWOW_BDMG_ARROW;
                break;
            case ROWOW_WEAPON_TYPE_REVOLVER:
            case ROWOW_WEAPON_TYPE_RIFLE:
            case ROWOW_WEAPON_TYPE_GATLING:
            case ROWOW_WEAPON_TYPE_SHOTGUN:
            case ROWOW_WEAPON_TYPE_GRENADE:
                // Criticals with any guns are calculated like melee criticals
                if ((bflag & ROWOW_BDMG_CRIT) == 0)
                    bflag |= ROWOW_BDMG_ARROW;
                break;
            default:
                // Attacks that use ammo are calculated like melee attacks as long as no ranged weapon is equipped
                break;
        }
    }
    if (skillID == ROWOW_SKILL_SN_SHARPSHOOTING || skillID == ROWOW_SKILL_MA_SHARPSHOOTING)
        bflag &= ~ROWOW_BDMG_CRIT; // Sharpshooting just ignores DEF/FLEE but damage is like a normal attack
    wd.Damage = BattleCalcBaseDamage(src, *sstatus, false, tstatus->Size, bflag);
    if (isLeftHanded == true)
        wd.Damage2 = BattleCalcBaseDamage(src, *sstatus, true, tstatus->Size, bflag);

    if (nk[ROWOW_NK_SPLASHSPLIT] == true)
    {
        // Divide ATK among targets
        if (wd.MiscFlag > 0)
            wd.Damage /= wd.MiscFlag;
        else
            LOG_ERROR("module.ROWOW", "ROWOWMod::BattleCalcSkillBaseDamage: 0 enemies targeted by {}, divide per 0 avoided!", skillID);
    }

    // Add any bonuses that modify the base atk (pre-skills): +crit damage bonuses in pre-renewal mode, and Taekwon Mastery's 2% a level for
    // each other party member on the map
    if (sd != nullptr && (bflag & ROWOW_BDMG_CRIT) != 0 && sstatus->Bonus.CritAtkRate > 0)
        AttackAddRate(wd, isLeftHanded, sstatus->Bonus.CritAtkRate);
    if (int32 skill = sd != nullptr && sd->GetGroup() != nullptr ? PCCheckSkill(sd, ROWOW_SKILL_TK_POWER) : 0; skill > 0)
    {
        // party_foreachsamemap(party_sub_count): exclude the player himself [Inkfish].  Reduce count by one (self) [Tydus1]
        int32 i = PartyForEachSameMap(sd, 0, [](Unit* /*member*/) { return 1; });
        if (i > 1)
            AttackAddRate(wd, isLeftHanded, 2 * skill * (i - 1));
    }

    // Acid Terror ignores DEF but will substract VIT from base attack value instead
    if (skillID == ROWOW_SKILL_AM_ACIDTERROR)
        AttackAdd(wd, isLeftHanded, -tstatus->Def2, -tstatus->Def2);
}

int64 ROWOWMod::BattleCalcBaseDamage(Unit const* src, ROWOWStatus const& status, bool isLeftHand, uint8 targetSize, int32 flag) const
{
    // battle_calc_base_damage: the standard damage of a normal attack assuming it hits
    uint32 atkmin = 0;
    uint32 atkmax = 0;
    int64 damage = 0;
    bool isPlayer = src->IsPlayer();
    int32 weaponAttack = isLeftHand == true ? status.LeftWeaponAttack : status.WeaponAttack;
    int32 weaponAttack2 = isLeftHand == true ? status.LeftWeaponRefineAttack : status.WeaponRefineAttack;
    ROWOWStatusChange const* sc = GetStatusChange(src);
    if (isPlayer == false)
    {
        // Mobs/Pets
        if (sc != nullptr && sc->HasSCE(ROWOW_SC_CHANGE) == true)
            return status.MagicAttackMax; // [Aegis] simply uses raw max matk for base damage when Mental Charge active
        if ((flag & ROWOW_BDMG_MAGIC) != 0)
        {
            atkmin = uint32(std::max(0, status.MagicAttackMin));
            atkmax = uint32(std::max(0, status.MagicAttackMax));
        }
        else
        {
            atkmin = uint32(std::max(0, weaponAttack));
            atkmax = uint32(std::max(0, weaponAttack2));
        }
        if (atkmin > atkmax)
            atkmin = atkmax;
    }
    else
    {
        // PCs
        if ((flag & ROWOW_BDMG_MAGIC) != 0)
        {
            atkmin = uint32(std::max(0, status.MagicAttackMin));
            atkmax = uint32(std::max(0, status.MagicAttackMax));
        }
        else
        {
            atkmax = uint32(std::max(0, weaponAttack));
            if ((flag & ROWOW_BDMG_CRIT) == 0 || (flag & ROWOW_BDMG_ARROW) != 0)
            {
                // Normal attacks: DEX sets the low end, more of it with a higher level weapon in the hand
                atkmin = uint32(std::max(0, status.Stats[ROWOW_STAT_DEX]));
                int32 weaponLevel = isLeftHand == true ? status.LeftWeaponLevel : status.WeaponLevel;
                if (weaponLevel > 0)
                    atkmin = atkmin * uint32(80 + weaponLevel * 20) / 100;
                if (atkmin > atkmax)
                    atkmin = atkmax;
                if ((flag & ROWOW_BDMG_ARROW) != 0)
                {
                    // Bows
                    atkmin = atkmin * atkmax / 100;
                    if (atkmin > atkmax)
                        atkmax = atkmin;
                }
            }
        }
    }

    if (sc != nullptr && sc->HasSCE(ROWOW_SC_MAXIMIZEPOWER) == true)
        atkmin = atkmax;

    // Weapon Damage calculation
    if ((flag & ROWOW_BDMG_CRIT) == 0)
        damage = (atkmax > atkmin ? urand(0, atkmax - atkmin - 1) : 0) + atkmin;
    else
        damage = atkmax;

    if (isPlayer == true)
    {
        // rodatazone says the range is 0~arrow_atk-1 for non crit
        if ((flag & ROWOW_BDMG_ARROW) != 0 && status.ArrowAttack > 0)
            damage += ((flag & ROWOW_BDMG_CRIT) != 0) ? status.ArrowAttack : urand(0, status.ArrowAttack - 1);

        // Size fix only for players
        if (status.Bonus.NoSizeFix == false && (flag & ROWOW_BDMG_NOSIZE) == 0)
            damage = damage * (isLeftHand == true ? status.LeftWeaponSizeMods[targetSize] : status.WeaponSizeMods[targetSize]) / 100;
    }

    // Finally, add baseatk
    if ((flag & ROWOW_BDMG_MAGIC) == 0)
        damage += status.BaseAttack;

    // battle_add_weapon_damage: the overrefine bonus and the weapon damage rate of cards
    if (isPlayer == true)
    {
        int32 overrefine = isLeftHand == true ? status.Bonus.LeftWeapon.Overrefine : status.Bonus.RightWeapon.Overrefine;
        uint8 weaponType = isLeftHand == true ? status.LeftWeaponType : status.RightWeaponType;
        if (overrefine > 0)
            damage = damage + urand(0, overrefine - 1) + 1;
        if (weaponType < ROWOW_WEAPON_TYPE_MAX && status.Bonus.WeaponDamageRate[weaponType] != 0)
            damage += damage * status.Bonus.WeaponDamageRate[weaponType] / 100;
    }
    return damage;
}

void ROWOWMod::BattleCalcDefenseReduction(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
{
    // battle_calc_defense_reduction
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    Player* tsd = target->ToPlayer();

    // Defense reduction.  (Expiatio and the target statuses that change it here are third jobs')
    ROWOWStatusChange const* tsc = GetStatusChange(static_cast<Unit const*>(target));
    bool hasSteelBody = tsc != nullptr && tsc->HasSCE(ROWOW_SC_STEELBODY) == true;
    int32 def1 = tstatus->Def;
    int32 def2 = tstatus->Def2;
    int32 vitDef = 0;
    if (src->IsPlayer() == true)
    {
        int32 ignoreRate = sstatus->Bonus.IgnoreDefByRace[tstatus->Race] + sstatus->Bonus.IgnoreDefByRace[ROWOW_RACE_ALL];
        ignoreRate += sstatus->Bonus.IgnoreDefByClass[tstatus->Class] + sstatus->Bonus.IgnoreDefByClass[ROWOW_CLASS_ALL];
        if (ignoreRate != 0)
        {
            ignoreRate = std::min(ignoreRate, 100); // cap it to 100 for 0 def min
            def1 -= def1 * ignoreRate / 100;
            def2 -= def2 * ignoreRate / 100;
        }
    }

    // A player's DEF drops for each attacker past vit_penalty_count (official servers limit the count to 22 targets)
    if (BattleConfig.VitPenaltyType != 0 && (BattleConfig.VitPenaltyTarget & GetBattleObjectType(*tstatus)) != 0)
    {
        int32 targetCount = std::min<int32>(int32(std::min<size_t>(target->getAttackers().size(), 255)), (100 / BattleConfig.VitPenaltyNum) + (BattleConfig.VitPenaltyCount - 1));
        if (targetCount >= BattleConfig.VitPenaltyCount)
        {
            int32 penalty = (targetCount - (BattleConfig.VitPenaltyCount - 1)) * BattleConfig.VitPenaltyNum;
            if (BattleConfig.VitPenaltyType == 1)
            {
                if (hasSteelBody == false)
                    def1 = (def1 * (100 - penalty)) / 100;
                def2 = (def2 * (100 - penalty)) / 100;
            }
            else
            {
                // Assume type 2
                if (hasSteelBody == false)
                    def1 -= penalty;
                def2 -= penalty;
            }
        }
        if (def2 < 1)
            def2 = 1;
    }

    // Damage reduction based on vitality
    if (tsd != nullptr)
    {
        // Sd vit-eq.  Damage reduction: [VIT*0.3] + RND(0, [VIT^2/150] - [VIT*0.3] - 1) + [VIT*0.5]
        vitDef = ((3 * def2) / 10);
        vitDef += int32(urand(0, uint32(std::max(0, (def2 * def2) / 150 - ((3 * def2) / 10) - 1))));
        vitDef += (def2 / 2);
        int32 skill = 0;
        if (src->IsCreature() == true && (IsUndead(*sstatus) == true || sstatus->Race == ROWOW_RACE_DEMON) && // This bonus already doesn't work vs players
            (skill = PCCheckSkill(tsd, ROWOW_SKILL_AL_DP)) > 0)
            vitDef += int32(((float)tsd->GetLevel() / 25.0 + 3.0) * skill + 0.5);
    }
    else
    {
        // Mob-Pet vit-eq: VIT + rnd(0,[VIT/20]^2-1)
        if (tsc != nullptr && tsc->HasSCE(ROWOW_SC_SKA) == true)
            vitDef = 90; // Eska sets the random part of the formula to 90
        else
            vitDef = (def2 / 20) * (def2 / 20);
        vitDef = def2 + (vitDef > 0 ? int32(urand(0, uint32(vitDef - 1))) : 0);
    }

    if (BattleConfig.WeaponDefenseType != 0)
    {
        vitDef += def1 * BattleConfig.WeaponDefenseType;
        def1 = 0;
    }

    if (def1 > 100)
        def1 = 100;
    bool isLeftHanded = IsAttackLeftHanded(src, skillID);
    bool ignoresRight = AttackIgnoresDef(wd, src, target, skillID, skillLevel, true);
    bool ignoresLeft = AttackIgnoresDef(wd, src, target, skillID, skillLevel, false);
    int32 piercingRight = IsAttackPiercing(wd, src, target, skillID, skillLevel, true);
    int32 piercingLeft = IsAttackPiercing(wd, src, target, skillID, skillLevel, false);
    int64 rateRight = ignoresRight == true ? 100 : (piercingRight != 0 ? int64(piercingRight) * (def1 + vitDef) : (100 - def1));
    int64 rateLeft = ignoresLeft == true ? 100 : (piercingLeft != 0 ? int64(piercingLeft) * (def1 + vitDef) : (100 - def1));
    wd.Damage = wd.Damage * rateRight / 100;
    if (isLeftHanded == true)
        wd.Damage2 = wd.Damage2 * rateLeft / 100;
    wd.BaseDamage = wd.BaseDamage * (100 - def1) / 100;
    AttackAdd(wd, isLeftHanded, (ignoresRight == true || piercingRight != 0) ? 0 : -vitDef, (ignoresLeft == true || piercingLeft != 0) ? 0 : -vitDef);
    wd.BaseDamage -= vitDef;
}

void ROWOWMod::BattleCalcAttackPostDefense(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const
{
    // battle_calc_attack_post_defense: post skill/vit reduction damage increases
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (src->IsPlayer() == true)
    {
        // Refine bonus
        if (BattleSkillStacksMasteriesVVS(skillID, true) == true)
            AttackAdd(wd, IsAttackLeftHanded(src, skillID), sstatus->WeaponRefineAttack, sstatus->LeftWeaponRefineAttack);
        wd.BaseDamage += sstatus->WeaponRefineAttack;
    }

    // After DEF reduction, damage can be negative, refine bonus works against that value
    // After refinement bonus was applied, damage is capped to 1, then masteries are applied
    BattleMinDamage(wd, src, skillID, 1);

    BattleCalcAttackMasteries(wd, src, target, skillID, skillLevel);

    // SC skill damages
    if (ROWOWStatusChangeEntry const* sce = GetSCE(src, ROWOW_SC_AURABLADE); sce != nullptr && skillID != ROWOW_SKILL_LK_SPIRALPIERCE &&
        skillID != ROWOW_SKILL_ML_SPIRALPIERCE)
        AttackAdd(wd, IsAttackLeftHanded(src, skillID), 20 * sce->Val1, 20 * sce->Val1);

    // Set to min of 1
    BattleMinDamage(wd, src, skillID, 1);
}

void ROWOWMod::BattleCalcAttackMasteries(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 /*skillLevel*/) const
{
    // battle_calc_attack_masteries.  (The third jobs' masteries are left out: no pre-renewal job has them)
    if (src->IsPlayer() == false)
        return;
    wd.BaseDamage = BattleAddMastery(src, target, wd.BaseDamage, false);

    // Check if mastery damage applies to current skill
    if (BattleSkillStacksMasteriesVVS(skillID, false) == false)
        return;
    wd.Damage = BattleAddMastery(src, target, wd.Damage, false);
    if (IsAttackLeftHanded(src, skillID) == true)
        wd.Damage2 = BattleAddMastery(src, target, wd.Damage2, true);
    // (The statuses considered as masteries, Camouflage and the like, are third jobs')
}

int64 ROWOWMod::BattleAddMastery(Unit const* src, Unit const* target, int64 dmg, bool isLeftHand) const
{
    // battle_addmastery.  (The third jobs' masteries are left out: no pre-renewal job has them)
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* status = GetROStatus(target);
    int64 damage = dmg;
    int32 skill = 0;

    if ((skill = PCCheckSkill(src, ROWOW_SKILL_AL_DEMONBANE)) > 0 && target->IsCreature() == true && // This bonus doesn't work against players.
        (IsUndead(*status) == true || status->Race == ROWOW_RACE_DEMON))
        damage += int64(skill * (src->GetLevel() / 20.0 + 3.0));
    if ((skill = PCCheckSkill(src, ROWOW_SKILL_HT_BEASTBANE)) > 0 && (status->Race == ROWOW_RACE_INSECT || status->Race == ROWOW_RACE_BRUTE || status->Race == ROWOW_RACE_PLAYER_DORAM))
    {
        damage += (skill * 4);
        if (ROWOWStatusChangeEntry const* sce = GetSCE(src, ROWOW_SC_SPIRIT); sce != nullptr && sce->Val2 == ROWOW_SKILL_SL_HUNTER)
            damage += sstatus->Stats[ROWOW_STAT_STR];
    }

    uint8 weapon = isLeftHand == true ? sstatus->LeftWeaponType : sstatus->RightWeaponType;
    switch (weapon)
    {
        case ROWOW_WEAPON_TYPE_1HSWORD:
        case ROWOW_WEAPON_TYPE_DAGGER:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_SM_SWORD)) > 0)
                damage += (skill * 4);
            break;
        case ROWOW_WEAPON_TYPE_2HSWORD:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_SM_TWOHAND)) > 0)
                damage += (skill * 4);
            break;
        case ROWOW_WEAPON_TYPE_1HSPEAR:
        case ROWOW_WEAPON_TYPE_2HSPEAR:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_KN_SPEARMASTERY)) > 0)
                damage += skill * (PCIsRiding(src) == false ? 4 : 5);
            break;
        case ROWOW_WEAPON_TYPE_1HAXE:
        case ROWOW_WEAPON_TYPE_2HAXE:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_AM_AXEMASTERY)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_MACE:
        case ROWOW_WEAPON_TYPE_2HMACE:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_PR_MACEMASTERY)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_FIST:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_TK_RUN)) > 0)
                damage += (skill * 10);
            [[fallthrough]];
        case ROWOW_WEAPON_TYPE_KNUCKLE:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_MO_IRONHAND)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_MUSICAL:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_BA_MUSICALLESSON)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_WHIP:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_DC_DANCINGLESSON)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_BOOK:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_SA_ADVANCEDBOOK)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_KATAR:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_AS_KATAR)) > 0)
                damage += (skill * 3);
            break;
    }
    return damage;
}

void ROWOWMod::BattleCalcElementDamage(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
{
    // battle_calc_element_damage
    std::bitset<ROWOW_NK_MAX> nk = BattleSkillGetDamageProperties(skillID, wd.MiscFlag);
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    bool isLeftHanded = IsAttackLeftHanded(src, skillID);
    int32 rightElement = BattleGetWeaponElement(wd, src, target, skillID, skillLevel, true, true);

    // Elemental attribute fix
    if (nk[ROWOW_NK_IGNOREELEMENT] == false && (wd.Damage > 0 || wd.Damage2 > 0))
    {
        int32 leftElement = BattleGetWeaponElement(wd, src, target, skillID, skillLevel, false, true);
        switch (skillID)
        {
            case ROWOW_SKILL_PA_SACRIFICE:
            case ROWOW_SKILL_HFLI_SBR44:
                wd.Damage = BattleAttrFix(src, target, wd.Damage, rightElement, tstatus->DefElement, tstatus->ElementLevel, 1);
                if (isLeftHanded == true)
                    wd.Damage2 = BattleAttrFix(src, target, wd.Damage2, leftElement, tstatus->DefElement, tstatus->ElementLevel, 1);
                break;
            default:
                if (skillID == 0 && (BattleConfig.AttackAttrNone & GetBattleObjectType(*sstatus)) != 0)
                    return; // Non-player basic attacks are non-elemental, they deal 100% against all defense elements
                wd.Damage = BattleAttrFix(src, target, wd.Damage, rightElement, tstatus->DefElement, tstatus->ElementLevel, 1);
                if (isLeftHanded == true)
                    wd.Damage2 = BattleAttrFix(src, target, wd.Damage2, leftElement, tstatus->DefElement, tstatus->ElementLevel, 1);
                break;
        }

        // Forced to neutral skills [helvetica]
        // Skills forced to neutral gain benefits from weapon element but final damage is considered "neutral" and resistances are applied again
        switch (skillID)
        {
            case ROWOW_SKILL_MC_CARTREVOLUTION:
            case ROWOW_SKILL_HW_MAGICCRASHER:
                wd.Damage = BattleAttrFix(src, target, wd.Damage, ROWOW_ELEMENT_NEUTRAL, tstatus->DefElement, tstatus->ElementLevel, 1);
                if (isLeftHanded == true)
                    wd.Damage2 = BattleAttrFix(src, target, wd.Damage2, ROWOW_ELEMENT_NEUTRAL, tstatus->DefElement, tstatus->ElementLevel, 1);
                break;
        }
    }

    // These mastery bonuses are non-elemental and should apply even if the attack misses
    // They are still increased by the EDP/Magnum Break bonus damage (WATK_ELEMENT)
    if (src->IsPlayer() == true && BattleSkillStacksMasteriesVVS(skillID, true) == true)
    {
        // Star Crumb bonus damage
        AttackAdd(wd, isLeftHanded, sstatus->Bonus.RightWeapon.Star, sstatus->Bonus.LeftWeapon.Star);
    }
    // Check if general mastery bonuses apply (above check is only for Star Crumb)
    if (BattleSkillStacksMasteriesVVS(skillID, false) == true)
    {
        // Spirit Sphere bonus damage
        int32 spiritBallDamage = BattleGetSpiritBallDamage(wd, src, skillID);
        AttackAdd(wd, isLeftHanded, spiritBallDamage, spiritBallDamage);

        // Skill-specific bonuses
        switch (skillID)
        {
            case ROWOW_SKILL_TF_POISON:
                AttackAdd(wd, isLeftHanded, 15 * skillLevel, 15 * skillLevel);
                // Envenom applies the attribute table to the base damage and then again to the final damage
                wd.Damage = BattleAttrFix(src, target, wd.Damage, rightElement, tstatus->DefElement, tstatus->ElementLevel, 1);
                break;
            case ROWOW_SKILL_NJ_SYURIKEN:
                AttackAdd(wd, isLeftHanded, 4 * skillLevel, 4 * skillLevel);
                if (src->IsPlayer() == true)
                {
                    int32 skill = 3 * PCCheckSkill(src, ROWOW_SKILL_NJ_TOBIDOUGU);
                    AttackAdd(wd, isLeftHanded, skill, skill);
                    AttackAdd(wd, isLeftHanded, sstatus->ArrowAttack, sstatus->ArrowAttack);
                }
                // Applies attribute table on neutral element to the final damage
                wd.Damage = BattleAttrFix(src, target, wd.Damage, ROWOW_ELEMENT_NEUTRAL, tstatus->DefElement, tstatus->ElementLevel, 1);
                break;
            case ROWOW_SKILL_NJ_KUNAI:
                if (src->IsPlayer() == true)
                    AttackAdd(wd, isLeftHanded, 3 * sstatus->ArrowAttack, 3 * sstatus->ArrowAttack);
                // Applies attribute table on neutral element to the final damage
                wd.Damage = BattleAttrFix(src, target, wd.Damage, ROWOW_ELEMENT_NEUTRAL, tstatus->DefElement, tstatus->ElementLevel, 1);
                break;
        }
    }

    // These bonuses do not apply to skills that ignore +% damage cards
    // If damage was reduced below 0 and was not increased again to a positive value through mastery bonuses, these bonuses are ignored
    // Any of these are only applied to your right hand weapon in pre-renewal
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
    if (nk[ROWOW_NK_IGNOREELEMENT] == false && nk[ROWOW_NK_IGNOREATKCARD] == false && (wd.Damage > 0 || wd.Damage2 > 0) && sc != nullptr)
    {
        // EDP bonus damage
        // This has to be applied after mastery bonuses but still before the elemental extra damage
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_EDP))
            wd.Damage += (wd.Damage * sce->Val3) / 100;

        // This adds a percentual damage bonus based on the damage you would deal with a normal attack
        // Applies only to player damage; monsters and mercenaries don't get this damage boost
        ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_WATK_ELEMENT);
        if (src->IsPlayer() == true && sce != nullptr)
        {
            // Pretend the normal attack was of the element stored in the status change
            wd.BaseDamage = BattleAttrFix(src, target, wd.BaseDamage, sce->Val1, tstatus->DefElement, tstatus->ElementLevel, 1);
            // Star Crumb bonus damage
            wd.BaseDamage += sstatus->Bonus.RightWeapon.Star;
            // Spirit Sphere bonus damage
            wd.BaseDamage += BattleGetSpiritBallDamage(wd, src, skillID);
            // Add percent of the base damage to the damage
            wd.Damage += (wd.BaseDamage * sce->Val2) / 100;
        }
    }

    // Cap damage to 0
    if (BattleConfig.AttrRecover == 0)
        BattleMinDamage(wd, src, skillID, 0);
}

int32 ROWOWMod::BattleGetSpiritBallDamage(ROWOWDamage const& wd, Unit const* src, uint16 skillID) const
{
    // battle_get_spiritball_damage: a player's Spirit Spheres add 3 each (pre-renewal Occult Impaction and Throw Spirit Sphere count the
    // spheres they used as their hits)
    if (src->IsPlayer() == false)
        return 0; // Return 0 for non-players
    switch (skillID)
    {
        case ROWOW_SKILL_MO_INVESTIGATE:
        case ROWOW_SKILL_MO_FINGEROFFENSIVE:
            // These skills used as many spheres as they do hits
            return (wd.Div + PCGetSpiritBall(src)) * 3;
        case ROWOW_SKILL_MO_EXTREMITYFIST:
        {
            // These skills store the number of spheres the player had before cast
            ROWOWSkillUnitData const* ud = src->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
            return ud != nullptr ? ud->SpiritBallOld * 3 : 0;
        }
        default:
            // Any skills that do not consume spheres or do not care
            return PCGetSpiritBall(src) * 3;
    }
}

int64 ROWOWMod::BattleAttrFix(Unit* src, Unit* target, int64 damage, int32 atkElement, int32 defElement, int32 defElementLevel, int32 flag)
{
    // battle_attr_fix: the element table, with the attacker's statuses that raise an element's damage (Volcano, Violent Gale, Deluge) and the
    // target's that do (Fiber Lock, which fire burns away).  (The third jobs' statuses are left out)
    if (atkElement < 0 || atkElement >= ROWOW_ELEMENT_COUNT)
        atkElement = int32(urand(0, ROWOW_ELEMENT_COUNT - 1));
    if (defElement < 0 || defElement >= ROWOW_ELEMENT_COUNT || defElementLevel < 1 || defElementLevel > ROWOW_ELEMENT_LEVEL_MAX)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::BattleAttrFix: unknown attribute type: atk={} def_type={} def_lv={}", atkElement, defElement, defElementLevel);
        return damage;
    }
    int32 ratio = AttrFix[defElementLevel - 1][atkElement][defElement];
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
    if (sc != nullptr && sc->IsEmpty() == false)
    {
        // increase dmg by src status
        ROWOWStatusChangeEntry const* sce = nullptr;
        switch (atkElement)
        {
            case ROWOW_ELEMENT_FIRE:
                sce = sc->GetSCE(ROWOW_SC_VOLCANO);
                break;
            case ROWOW_ELEMENT_WIND:
                sce = sc->GetSCE(ROWOW_SC_VIOLENTGALE);
                break;
            case ROWOW_ELEMENT_WATER:
                sce = sc->GetSCE(ROWOW_SC_DELUGE);
                break;
        }
        if (sce != nullptr)
            damage += (damage * sce->Val3) / 100;
    }
    ROWOWStatusChange const* tsc = GetStatusChange(static_cast<Unit const*>(target));
    if (tsc != nullptr && atkElement == ROWOW_ELEMENT_FIRE && tsc->HasSCE(ROWOW_SC_SPIDERWEB) == true)
    {
        // Double damage
        damage *= 2;
        // Remove a unit group or end whole status change
        StatusChangeEnd(target, ROWOW_SC_SPIDERWEB);
    }

    if (BattleConfig.AttrRecover == 0 && (flag & 1) == 0 && ratio < 0)
        ratio = 0;
    damage = (damage * ratio) / 100;

    // Damage can be negative, see battle_config.attr_recover
    return damage;
}

void ROWOWMod::BattleCalcAttackPlant(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
{
    // battle_calc_attack_plant: "plant"-type monsters only take 1 damage from all sources, which the statuses that block damage block too.
    // (There is no Emperium here)
    bool attackHits = IsAttackHitting(wd, src, target, skillID, skillLevel, false);

    // Plants receive 1 damage when hit
    if (attackHits == true || wd.Damage > 0)
        wd.Damage = 1; // In some cases, right hand no need to have a weapon to deal a damage
    if (IsAttackLeftHanded(src, skillID) == true && (attackHits == true || wd.Damage2 > 0))
    {
        ROWOWStatus const* sstatus = GetROStatus(src);
        if (src->IsPlayer() == true && sstatus->WeaponType == ROWOW_WEAPON_TYPE_KATAR)
            wd.Damage2 = 0; // No backhand damage against plants
        else
            wd.Damage2 = 1; // Deal 1 HP damage as long as there is a weapon in the left hand
    }

    if (attackHits == true && target->IsCreature() == true)
    {
        ROWOWStatusChange* tsc = GetStatusChange(target);
        int64 damageDummy = 1;
        if (tsc != nullptr && BattleStatusBlockDamage(src, target, tsc, wd, damageDummy, skillID, skillLevel) == false)
        {
            // Statuses that reduce damage to 0.
            wd.Damage = wd.Damage2 = 0;
            return;
        }
    }

    // For plants we don't continue with the weapon attack code, so we have to apply DAMAGE_DIV_FIX here
    BattleApplyDivFix(wd, skillID);

    // If there is left hand damage, total damage can never exceed 2, even on multiple hits
    if (wd.Damage > 1 && wd.Damage2 > 0)
    {
        wd.Damage = 1;
        wd.Damage2 = 1;
    }
}

void ROWOWMod::BattleApplyDivFix(ROWOWDamage& wd, uint16 /*skillID*/) const
{
    // battle_apply_div_fix
    if (wd.Damage != 0)
    {
        DamageDivFix(wd.Damage, wd.Div);
        // Min damage
        if (wd.Damage < wd.Div && (BattleConfig.SkillMinDamage & wd.Flag) != 0)
            wd.Damage = wd.Div;
    }
    else if (wd.Div < 0)
        wd.Div *= -1;
}

void ROWOWMod::BattleCalcAttackLeftRightHands(ROWOWDamage& wd, Unit* src, Unit* /*target*/, uint16 skillID, uint16 /*skillLevel*/) const
{
    // battle_calc_attack_left_right_hands
    Player* sd = src->ToPlayer();
    bool isLeftHanded = IsAttackLeftHanded(src, skillID);
    if (sd != nullptr)
    {
        ROWOWStatus const* sstatus = GetROStatus(src);
        ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        bool isThief = identity != nullptr && IsThiefClass(identity->JobID);
        int32 skill = 0;
        if (sstatus->WeaponType == ROWOW_WEAPON_TYPE_KATAR && skillID == 0)
        {
            // Katars (offhand damage only applies to normal attacks, tested on Aegis 10.2)
            skill = PCCheckSkill(sd, ROWOW_SKILL_TF_DOUBLE);
            wd.Damage2 = wd.Damage * (1 + (skill * 2)) / 100;
        }
        else if (isLeftHanded == true)
        {
            // Dual-wield.  If you only have a weapon in the left hand, then your main hand damage will be identical to an unarmed attack
            if (IsAttackRightHanded(src, skillID) == true && wd.Damage != 0)
            {
                if (isThief == true)
                {
                    skill = PCCheckSkill(sd, ROWOW_SKILL_AS_RIGHT);
                    wd.Damage = wd.Damage * (50 + (skill * 10)) / 100;
                }
                if (wd.Damage < 1)
                    wd.Damage = 1;
            }
            // Left hand damage will always be adjusted, even if you don't have a weapon in the main hand
            if (wd.Damage2 != 0)
            {
                if (isThief == true)
                {
                    skill = PCCheckSkill(sd, ROWOW_SKILL_AS_LEFT);
                    wd.Damage2 = wd.Damage2 * (30 + (skill * 10)) / 100;
                }
                if (wd.Damage2 < 1)
                    wd.Damage2 = 1;
            }
        }
    }

    if (IsAttackRightHanded(src, skillID) == false && isLeftHanded == false && wd.Damage != 0)
        wd.Damage = 0;
    if (isLeftHanded == false && wd.Damage2 != 0)
        wd.Damage2 = 0;
}

int64 ROWOWMod::BattleCalcDamage(Unit* src, Unit* target, ROWOWDamage& d, int64 damage, uint16 skillID, uint16 skillLevel)
{
    // battle_calc_damage, the final damage: the statuses that raise, lower or block it.  (Kill steal protection and the PK mode are off; the
    // third jobs' statuses are left out)
    int16 div = d.Div;
    int32 flag = d.Flag;
    if (damage == 0)
        return 0;
    if (target->IsPlayer() == true)
    {
        // Special no damage states
        if ((flag & ROWOW_BF_WEAPON) != 0)
            damage -= damage * GetDeferredBonus(target, "bNoWeaponDamage") / 100;
        if ((flag & ROWOW_BF_MAGIC) != 0)
            damage -= damage * GetDeferredBonus(target, "bNoMagicDamage") / 100;
        if ((flag & ROWOW_BF_MISC) != 0)
            damage -= damage * GetDeferredBonus(target, "bNoMiscDamage") / 100;
        if (damage == 0)
            return 0;
    }
    switch (skillID)
    {
        case ROWOW_SKILL_PA_PRESSURE:
        case ROWOW_SKILL_HW_GRAVITATION:
            return damage; // These skills bypass everything else.
    }

    // check target status
    ROWOWStatusChange* tsc = GetStatusChange(target);
    if (tsc != nullptr && tsc->IsEmpty() == false)
    {
        // Barrier just sets damage to 1 per hit and prevents the rest of the code from being processed.  But only if the damage at this point
        // is greater than 0
        if (damage > 0 && tsc->HasSCE(ROWOW_SC_BARRIER) == true)
            return div;

        // Damage increasing effects
        if (tsc->HasSCE(ROWOW_SC_AETERNA) == true && skillID != ROWOW_SKILL_PF_SOULBURN)
        {
            if (GetFollowerType(src) != ROWOW_BL_MER || skillID == 0)
                damage *= 2; // Lex Aeterna only doubles damage of regular attacks from mercenaries
            if (skillID != ROWOW_SKILL_ASC_BREAKER || (flag & ROWOW_BF_WEAPON) == 0)
                StatusChangeEnd(target, ROWOW_SC_AETERNA); // Shouldn't end until Breaker's non-weapon part connects.
        }

        // Damage reductions.  Assumptio increases DEF on RE mode, otherwise gives a reduction on the final damage. [Igniz]  (No RO map is a
        // versus map, where it's 66%)
        if (tsc->HasSCE(ROWOW_SC_ASSUMPTIO) == true)
            damage /= 2; // Receive 50% damage
        if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_DEFENDER); sce != nullptr && skillID != ROWOW_SKILL_NJ_ZENYNAGE &&
            (flag & (ROWOW_BF_LONG | ROWOW_BF_WEAPON)) == (ROWOW_BF_LONG | ROWOW_BF_WEAPON))
            damage -= damage * sce->Val2 / 100;
        if (tsc->HasSCE(ROWOW_SC_ADJUSTMENT) == true && (flag & (ROWOW_BF_LONG | ROWOW_BF_WEAPON)) == (ROWOW_BF_LONG | ROWOW_BF_WEAPON))
            damage -= damage * 20 / 100;
        if (tsc->HasSCE(ROWOW_SC_FOGWALL) == true && skillID != ROWOW_SKILL_NPC_DRAGONBREATH)
        {
            if ((flag & ROWOW_BF_SKILL) != 0) // 25% reduction
                damage -= damage * 25 / 100;
            else if ((flag & (ROWOW_BF_LONG | ROWOW_BF_WEAPON)) == (ROWOW_BF_LONG | ROWOW_BF_WEAPON))
                damage /= 4; // 75% reduction
        }
        if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_ARMORCHANGE))
        {
            // On official servers, SC_ARMORCHANGE does not change DEF/MDEF but rather increases/decreases the damage
            if ((flag & ROWOW_BF_WEAPON) != 0)
                damage -= damage * sce->Val2 / 100;
            else if ((flag & ROWOW_BF_MAGIC) != 0)
                damage -= damage * sce->Val3 / 100;
        }
        if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_ARMOR); sce != nullptr && (sce->Val3 & flag) != 0 && (sce->Val4 & flag) != 0 &&
            sce->Val2 != 0) // NPC_DEFENDER
            damage /= sce->Val2;
        if (tsc->HasSCE(ROWOW_SC_ENERGYCOAT) == true && (flag & ROWOW_BF_WEAPON) != 0 && skillID != ROWOW_SKILL_WS_CARTTERMINATION)
        {
            int64 maxSP = std::max<int64>(1, target->GetMaxPower(POWER_MANA));
            int32 per = int32(100 * int64(target->GetPower(POWER_MANA)) / maxSP - 1); // 100% should be counted as the 80~99% interval
            per /= 20; // Uses 20% SP intervals.
            // SP Cost: 1% + 0.5% per every 20% SP
            if (StatusCharge(target, 0, (10 + 5 * per) * maxSP / 1000) == false)
                StatusChangeEnd(target, ROWOW_SC_ENERGYCOAT);
            damage -= damage * 6 * (1 + per) / 100; // Reduction: 6% + 6% every 20%
        }
        if (damage == 0)
            return 0;
    }

    // SC effects from caster side.  (Bloodlust is a homunculus's: its share of the damage heals it)
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
    if (ROWOWStatusChangeEntry const* sce = sc != nullptr ? sc->GetSCE(ROWOW_SC_BLOODLUST) : nullptr; sce != nullptr && (flag & ROWOW_BF_WEAPON) != 0 &&
        damage > 0 && int32(urand(0, 99)) < sce->Val3)
        StatusHeal(src, damage * sce->Val4 / 100, 0, 1, src, 0);

    if (BattleConfig.SkillMinDamage != 0 && damage > 0 && damage < div)
    {
        if (((flag & ROWOW_BF_WEAPON) != 0 && (BattleConfig.SkillMinDamage & 1) != 0) || ((flag & ROWOW_BF_MAGIC) != 0 && (BattleConfig.SkillMinDamage & 2) != 0) ||
            ((flag & ROWOW_BF_MISC) != 0 && (BattleConfig.SkillMinDamage & 4) != 0))
            damage = div;
    }

    // Reduces damage received for Green Aura MVP
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (target->IsCreature() == true && tstatus->DamageTaken != 100)
        damage = std::max<int64>(damage * tstatus->DamageTaken / 100, 1);

    if (tsc != nullptr && tsc->IsEmpty() == false)
    {
        if (BattleStatusBlockDamage(src, target, tsc, d, damage, skillID, skillLevel) == false) // Statuses that reduce damage to 0.
            return 0;
    }
    return damage;
}

bool ROWOWMod::BattleStatusBlockDamage(Unit* src, Unit* target, ROWOWStatusChange* sc, ROWOWDamage& d, int64& damage, uint16 skillID, uint16 skillLevel)
{
    // battle_status_block_damage: the statuses that block a hit or part of it.  True: damage inflicted, false: missed.  (Its element check
    // is for the third jobs' immunity statuses, which are left out with the rest of the third jobs' statuses)
    if (src == nullptr || target == nullptr || sc == nullptr)
        return true;
    int32 flag = d.Flag;
    ROWOWSkill const* skill = GetSkill(skillID);

    // SC Types that must be first because they may or may not block damage
    if (ROWOWStatusChangeEntry* sce = sc->GetSCE(ROWOW_SC_KYRIE); sce != nullptr && damage > 0)
    {
        sce->Val2 -= int32(std::clamp<int64>(damage, INT_MIN, INT_MAX));
        if ((flag & ROWOW_BF_WEAPON) != 0 || skillID == ROWOW_SKILL_TF_THROWSTONE)
        {
            if (sce->Val2 >= 0)
                damage = 0;
            else
                damage = -sce->Val2;
        }
        // Pressure usually won't reach this code in pre-renewal and does consequently not remove Kyrie.  But it's still coded to do that -
        // this reflects how it is done officially
        if ((--sce->Val3) <= 0 || sce->Val2 <= 0 || skillID == ROWOW_SKILL_AL_HOLYLIGHT || skillID == ROWOW_SKILL_PA_PRESSURE)
            StatusChangeEnd(target, ROWOW_SC_KYRIE);
    }

    if (damage == 0)
        return false;

    // ATK_BLOCK Type.  Safety Wall's hits are its unit group's (val3): each blocked hit uses one up, and the wall goes with the last.  Without
    // its group the status ends.  (Steinwand's comes with the homunculi)
    if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_SAFETYWALL); sce != nullptr && (flag & (ROWOW_BF_SHORT | ROWOW_BF_MAGIC)) == ROWOW_BF_SHORT)
    {
        if (std::shared_ptr<ROWOWSkillUnitGroup> group = SkillID2Group(target->GetMap(), sce->Val3))
        {
            d.DmgLv = ROWOW_ATK_BLOCK;
            switch (sce->Val2)
            {
                case ROWOW_SKILL_MG_SAFETYWALL:
                    if (--group->Val2 <= 0)
                        SkillDelUnitGroup(target->GetMap(), group);
                    break;
            }
            return false;
        }
        StatusChangeEnd(target, ROWOW_SC_SAFETYWALL);
    }

    ROWOWStatus const* sstatus = GetROStatus(src);
    if ((sc->HasSCE(ROWOW_SC_PNEUMA) == true && (flag & (ROWOW_BF_MAGIC | ROWOW_BF_LONG)) == ROWOW_BF_LONG) ||
        (sc->HasSCE(ROWOW_SC_BASILICA) == true && (sstatus->Mode & ROWOW_MODE_STATUS_IMMUNE) == 0))
    {
        d.DmgLv = ROWOW_ATK_BLOCK;
        return false;
    }

    // ATK_MISS Type.  (Auto Guard's walk delay, unit_set_walkdelay, has no WoW counterpart: a player's client moves it)
    if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_AUTOGUARD); sce != nullptr && (flag & ROWOW_BF_WEAPON) != 0 && int32(urand(0, 99)) < sce->Val2 &&
        (skill == nullptr || skill->Inf2[ROWOW_INF2_IGNOREAUTOGUARD] == false))
    {
        Player* tsd = target->ToPlayer();
        if (tsd != nullptr && tsd->IsSitState() == true)
            tsd->SetStandState(UNIT_STAND_STATE_STAND);
        // A devoted player's guard is its Crusader's (rAthena shows it on the Crusader): no Shrink then
        if (GetDevotionSource(target, true) == nullptr && sc->HasSCE(ROWOW_SC_SHRINK) == true && int32(urand(0, 99)) < 5 * sce->Val1)
        {
            ROWOWSkill const* shrink = GetSkill(ROWOW_SKILL_CR_SHRINK);
            SkillBlown(target, src, shrink != nullptr ? shrink->Knockback[0] : 0, -1, 0);
        }
        d.DmgLv = ROWOW_ATK_MISS;
        return false;
    }

    // Other
    if ((sc->HasSCE(ROWOW_SC_HERMODE) == true && (flag & ROWOW_BF_MAGIC) != 0) ||
        (sc->HasSCE(ROWOW_SC_TATAMIGAESHI) == true && (flag & (ROWOW_BF_MAGIC | ROWOW_BF_LONG)) == ROWOW_BF_LONG))
        return false;

    if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_PARRYING); sce != nullptr && (flag & ROWOW_BF_WEAPON) != 0 &&
        skillID != ROWOW_SKILL_WS_CARTTERMINATION && int32(urand(0, 99)) < sce->Val2)
    {
        // unit_set_attackdelay(DELAY_EVENT_PARRY): a player's next swing and skill wait as after its own attack
        if (Player* tsd = target->ToPlayer())
        {
            UnitSetAttackDelay(tsd, GetTick(), false);
            tsd->setAttackTimer(BASE_ATTACK, std::max<uint32>(tsd->getAttackTimer(BASE_ATTACK), uint32(std::max(0, GetROStatus(tsd)->AttackDelay))));
        }
        return false;
    }

    if (sc->HasSCE(ROWOW_SC_DODGE) == true && ((flag & ROWOW_BF_LONG) != 0 || sc->HasSCE(ROWOW_SC_SPURT) == true) &&
        (skillID != ROWOW_SKILL_NPC_EARTHQUAKE || (flag & ROWOW_NPC_EARTHQUAKE_FLAG) != 0) && int32(urand(0, 99)) < 20)
    {
        Player* tsd = target->ToPlayer();
        if (tsd != nullptr && tsd->IsSitState() == true)
            tsd->SetStandState(UNIT_STAND_STATE_STAND); // Stand it to dodge.
        SCStart4(src, target, ROWOW_SC_COMBO, 100, ROWOW_SKILL_TK_JUMPKICK, int32(src->GetGUID().GetCounter()), 1, 0, 2000);
        return false;
    }

    if (ROWOWStatusChangeEntry* sce = sc->GetSCE(ROWOW_SC_KAUPE); sce != nullptr && (skillID != ROWOW_SKILL_NPC_EARTHQUAKE || (flag & ROWOW_NPC_EARTHQUAKE_FLAG) != 0) &&
        int32(urand(0, 99)) < sce->Val2)
    {
        // Kaupe blocks damage (skill or otherwise) from players, mobs, homuns, mercenaries.  Shouldn't end until Breaker's non-weapon part
        // connects.
        if (skillID != ROWOW_SKILL_ASC_BREAKER || (flag & ROWOW_BF_WEAPON) == 0)
        {
            if (--sce->Val3 <= 0) // We make it work like Safety Wall, even though it only blocks 1 time.
                StatusChangeEnd(target, ROWOW_SC_KAUPE);
        }
        return false;
    }

    ROWOWStatusChangeEntry const* utsusemi = sc->GetSCE(ROWOW_SC_UTSUSEMI);
    if ((utsusemi != nullptr || sc->HasSCE(ROWOW_SC_BUNSINJYUTSU) == true) && (flag & ROWOW_BF_WEAPON) != 0 &&
        (skill == nullptr || skill->Inf2[ROWOW_INF2_IGNORECICADA] == false))
    {
        int32 blowCount = utsusemi != nullptr ? utsusemi->Val3 : 0;
        bool hasUtsusemi = utsusemi != nullptr;
        uint64 tick = GetTick();
        SkillAdditionalEffect(src, target, skillID, skillLevel, flag, ROWOW_ATK_BLOCK, tick);
        if (src->IsAlive() == true)
            SkillCounterAdditionalEffect(src, target, skillID, skillLevel, flag, tick);
        if (hasUtsusemi == true)
            SkillBlown(src, target, blowCount, -1, 0);
        // Both need to be consumed if they are active.
        if (ROWOWStatusChangeEntry* sce = sc->GetSCE(ROWOW_SC_UTSUSEMI); hasUtsusemi == true && sce != nullptr && --sce->Val2 <= 0)
            StatusChangeEnd(target, ROWOW_SC_UTSUSEMI);
        if (ROWOWStatusChangeEntry* sce = sc->GetSCE(ROWOW_SC_BUNSINJYUTSU); sce != nullptr && --sce->Val2 <= 0)
            StatusChangeEnd(target, ROWOW_SC_BUNSINJYUTSU);
        return false;
    }
    return true;
}

void ROWOWMod::BattleCalcWeaponFinalAtkModifiers(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
{
    // battle_calc_weapon_final_atk_modifiers: Reject Sword, Poison React's Envenom, Fusion's HP cost and Breaker's magic part.  (Crescent
    // Elbow, Vigor, Enchant Blade and Camouflage are third jobs'; there's no skill damage map adjustment)
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatusChange* tsc = GetStatusChange(target);
    uint64 tick = GetTick();

    // Reject Sword bugreport:4493 by Daegaladh
    if (wd.Damage != 0 && tsc != nullptr)
    {
        ROWOWStatusChangeEntry* sce = tsc->GetSCE(ROWOW_SC_REJECTSWORD);
        if (sce != nullptr && (src->IsPlayer() == false || sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_DAGGER ||
            sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_1HSWORD || sstatus->WeaponType == ROWOW_WEAPON_TYPE_2HSWORD) && int32(urand(0, 99)) < sce->Val2)
        {
            wd.Damage = wd.Damage * 50 / 100;
            BattleDamage(target, src, wd.Damage, wd.Div, uint16(sce->Val1), ROWOW_SKILL_ST_REJECTSWORD, ROWOW_ATK_DEF, 0, false, tick, false);
            if (target->IsAlive() == false)
                return;
            if (--sce->Val3 <= 0)
                StatusChangeEnd(target, ROWOW_SC_REJECTSWORD);
        }
    }

    // Poison React Envenom Level 5 Autocast
    if (tsc != nullptr && wd.Damage > 0)
    {
        if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_POISONREACT); sce != nullptr && int32(urand(0, 99)) < sce->Val3)
        {
            if (StatusCheckSkillUse(target, src, ROWOW_SKILL_TF_POISON, 0) == true)
                SkillAttack(ROWOW_BF_WEAPON, target, target, src, ROWOW_SKILL_TF_POISON, 5, tick, 0);
            // Counter is reduced even if the autocast fails
            if (ROWOWStatusChangeEntry* counter = tsc->GetSCE(ROWOW_SC_POISONREACT); counter != nullptr && --counter->Val2 <= 0)
                StatusChangeEnd(target, ROWOW_SC_POISONREACT);
        }
    }

    // SC_FUSION hp penalty [Komurka]
    if (ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src)); sc != nullptr && sc->HasSCE(ROWOW_SC_FUSION) == true)
    {
        uint32 hp = src->GetMaxHealth();
        if (src->IsPlayer() == true && target->IsPlayer() == true)
        {
            hp = hp / 13;
            if (uint64(src->GetHealth()) * 100 <= uint64(src->GetMaxHealth()) * 20)
                hp = src->GetHealth();
        }
        else
            hp = 2 * hp / 100; // 2% hp loss per hit
        StatusZap(src, hp, 0);
    }

    if (skillID == ROWOW_SKILL_ASC_BREAKER)
    {
        // Breaker's int-based damage (a misc attack?)
        ROWOWDamage md = BattleCalcMiscAttack(src, target, skillID, skillLevel, wd.MiscFlag);
        wd.Damage += md.Damage;
    }
}

int64 ROWOWMod::BattleCalcReturnDamage(Unit* tbl, Unit* src, int64 damage, int32 flag, uint16 skillID, bool isStatusReflect) const
{
    // battle_calc_return_damage: what a hit bounces back onto its attacker, the gear's (bShortWeaponDamageReturn, bLongWeaponDamageReturn)
    // or Reflect Shield's, which Devotion passes on (val4: only skills, with devotion_rdamage_skill_only, and only within the link's
    // reach).  (The third jobs' statuses and bReduceDamageReturn are left out, and there's no PvP or skill damage map adjustment)
    ROWOWStatusChange const* tsc = GetStatusChange(static_cast<Unit const*>(tbl));
    ROWOWSkill const* skill = GetSkill(skillID);
    int64 rdamage = 0;
    if ((flag & ROWOW_BF_SHORT) != 0)
    {
        // Bounces back part of the damage.
        int32 shortReturn = GetDeferredBonus(tbl, "bShortWeaponDamageReturn");
        if (((skill != nullptr && skill->Inf2[ROWOW_INF2_ISTRAP] == true) || isStatusReflect == false) && shortReturn != 0)
            rdamage += damage * shortReturn / 100;
        else if (isStatusReflect == true && tsc != nullptr)
        {
            if (ROWOWStatusChangeEntry const* sce = tsc->GetSCE(ROWOW_SC_REFLECTSHIELD))
            {
                bool isInheritedSkillOnly = skillID == 0 && BattleConfig.DevotionRDamageSkillOnly != 0 && sce->Val4 != 0;
                if (Unit* d_bl = GetDevotionSource(tbl, false))
                {
                    // Don't reflect non-skill attack if has SC_REFLECTSHIELD from Devotion bonus inheritance
                    if (isInheritedSkillOnly == true || DistanceBL(tbl, d_bl) > tsc->GetSCE(ROWOW_SC_DEVOTION)->Val3)
                        return 0;
                }
                if (skillID != ROWOW_SKILL_WS_CARTTERMINATION && skillID != ROWOW_SKILL_NPC_MAXPAIN_ATK && isInheritedSkillOnly == false)
                    rdamage += damage * sce->Val2 / 100;
            }
        }
    }
    else if (isStatusReflect == false)
        rdamage += damage * GetDeferredBonus(tbl, "bLongWeaponDamageReturn") / 100;

    if (rdamage <= 0)
        return 0; // No reflecting damage calculated.
    return std::clamp<int64>(rdamage, 1, tbl->GetMaxHealth());
}

void ROWOWMod::BattleDoReflect(int32 attackType, ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 /*skillLevel*/)
{
    // battle_do_reflect: Reflect Shield's share of a hit, back onto the attacker (or a devoted attacker's Crusader, devotion_rdamage) after
    // its attack motion.  (unit_data's immune_attack is never set here, so a skill immune target reflects too; Max Pain, Reflect Damage and
    // Vitality Activation are third jobs'; the drain of the target's cards on it, battle_drain, comes with the deferred bonuses)
    // Don't reflect your own damage (Grand Cross)
    if (wd.Damage + wd.Damage2 == 0 || src == nullptr || target == nullptr || src == target)
        return;
    if (GetStatusChange(static_cast<Unit const*>(target)) == nullptr)
        return;
    int64 rdamage = BattleCalcReturnDamage(target, src, wd.Damage + wd.Damage2, wd.Flag, skillID, true);
    if (rdamage > 0 && (attackType == ROWOW_BF_WEAPON || attackType == ROWOW_BF_MISC))
    {
        Unit* d_bl = BattleCheckDevotion(src);
        uint64 tick = GetTick();
        BattleDelayDamage(tick, wd.AMotion, target, d_bl == nullptr ? src : d_bl, 0, ROWOW_SKILL_CR_REFLECTSHIELD, 0, rdamage, ROWOW_ATK_DEF, 1, true, false);
        SkillAdditionalEffect(target, d_bl == nullptr ? src : d_bl, ROWOW_SKILL_CR_REFLECTSHIELD, 1, ROWOW_BF_WEAPON | ROWOW_BF_SHORT | ROWOW_BF_NORMAL,
            ROWOW_ATK_DEF, tick);
    }
}

int32 ROWOWMod::BattleGetAtkPercent(Unit const* bl, uint16 skillID, ROWOWStatusChange const& sc) const
{
    // battle_get_atkpercent: the percentual attack modificator of statuses, added together and applied to the damage before the defense
    // reduction in pre-renewal.  A player's skills that masteries don't stack with don't get it
    if (bl->IsPlayer() == true && BattleSkillStacksMasteriesVVS(skillID, false) == false)
        return 100;
    int32 atkpercent = 100;
    if (sc.HasSCE(ROWOW_SC_CURSE) == true)
        atkpercent -= 25;
    if (ROWOWStatusChangeEntry const* sce = sc.GetSCE(ROWOW_SC_PROVOKE))
        atkpercent += sce->Val2;
    if (ROWOWStatusChangeEntry const* sce = sc.GetSCE(ROWOW_SC_STRIPWEAPON); sce != nullptr && bl->IsPlayer() == false)
        atkpercent -= sce->Val2;
    if (ROWOWStatusChangeEntry const* sce = sc.GetSCE(ROWOW_SC_CONCENTRATION))
        atkpercent += sce->Val2;
    if (ROWOWStatusChangeEntry const* sce = sc.GetSCE(ROWOW_SC_TRUESIGHT))
        atkpercent += 2 * sce->Val1;
    if (ROWOWStatusChangeEntry const* sce = sc.GetSCE(ROWOW_SC_JOINTBEAT); sce != nullptr && (sce->Val2 & ROWOW_BREAK_WAIST) != 0)
        atkpercent -= 25;
    if (ROWOWStatusChangeEntry const* sce = sc.GetSCE(ROWOW_SC_INCATKRATE))
        atkpercent += sce->Val1;
    if (ROWOWStatusChangeEntry const* sce = sc.GetSCE(ROWOW_SC_POWERUP))
        atkpercent += sce->Val1;
    if (sc.HasSCE(ROWOW_SC_SKE) == true)
        atkpercent += 300;
    if (ROWOWStatusChangeEntry const* sce = sc.GetSCE(ROWOW_SC_BLOODLUST))
        atkpercent += sce->Val2;
    if (ROWOWStatusChangeEntry const* sce = sc.GetSCE(ROWOW_SC_FLEET))
        atkpercent += sce->Val3;
    if (ROWOWStatusChangeEntry const* sce = sc.GetSCE(ROWOW_SC_INVINCIBLE))
        atkpercent += sce->Val2;
    return std::clamp<int32>(atkpercent, 0, USHRT_MAX);
}

void ROWOWMod::BattleAttackSCBonus(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 /*skillLevel*/) const
{
    // battle_attack_sc_bonus: the final attack bonuses that aren't affected by cards, applied on top of current damage and stackable, then
    // the Star Gladiator's Anger of the Sun, Moon or Star at its hated monster (Miracle makes every monster a star's): base level + DEX + LUK
    // (+ STR for the star) percent, divided by 12 - 3 a level below level 4.  (The third jobs' statuses and spirit charms are left out, and
    // there's no War of Emperium)
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(src));
    auto applyAnger = [this, &wd, src, target, skillID, sc]()
    {
        static uint16 const angerSkillIDs[ROWOW_MAX_PC_FEELHATE] = { ROWOW_SKILL_SG_SUN_ANGER, ROWOW_SKILL_SG_MOON_ANGER, ROWOW_SKILL_SG_STAR_ANGER };
        Player* sd = src->ToPlayer();
        if (sd == nullptr)
            return;
        ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity == nullptr)
            return;
        int32 angerID = 0;
        if (sc != nullptr && sc->HasSCE(ROWOW_SC_MIRACLE) == true)
            angerID = 2; // Always treat all monsters as star flagged monster when in miracle state
        else
        {
            int32 targetClass = target->IsCreature() == true ? int32(GetROMobID(target->ToCreature())) :
                (target->IsPlayer() == true && target->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") != nullptr ?
                int32(target->CustomData.Get<ROWOWIdentity>("ROWOWIdentity")->JobID) : -2);
            for (; angerID < ROWOW_MAX_PC_FEELHATE; angerID++)
                if (targetClass == PCGetHateMob(*identity, angerID))
                    break;
        }
        int32 angerLevel = angerID < ROWOW_MAX_PC_FEELHATE ? PCCheckSkill(sd, angerSkillIDs[angerID]) : 0;
        if (angerLevel <= 0)
            return;
        ROWOWStatus const* sstatus = GetROStatus(src);
        int32 skillratio = int32(identity->BaseLevel) + sstatus->Stats[ROWOW_STAT_DEX] + sstatus->Stats[ROWOW_STAT_LUK];
        if (angerID == 2)
            skillratio += sstatus->Stats[ROWOW_STAT_STR]; // SG_STAR_ANGER additionally has STR added in its formula.
        if (angerLevel < 4)
            skillratio /= 12 - 3 * angerLevel;
        AttackAddRate(wd, IsAttackLeftHanded(src, skillID), skillratio);
    };
    if (sc == nullptr)
    {
        applyAnger();
        return;
    }
    bool isLeftHanded = IsAttackLeftHanded(src, skillID);

    // ATK percent modifier (in pre-renewal, it's applied multiplicatively after the skill ratio)
    AttackRate(wd, isLeftHanded, BattleGetAtkPercent(src, skillID, *sc));
    wd.BaseDamage = wd.BaseDamage * BattleGetAtkPercent(src, 0, *sc) / 100;
    if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_SPIRIT))
    {
        if (skillID == ROWOW_SKILL_AS_SONICBLOW && sce->Val2 == ROWOW_SKILL_SL_ASSASIN)
            AttackAddRate(wd, isLeftHanded, 100); // +25% dmg on woe/+100% dmg on nonwoe
        else if (skillID == ROWOW_SKILL_CR_SHIELDBOOMERANG && sce->Val2 == ROWOW_SKILL_SL_CRUSADER)
            AttackAddRate(wd, isLeftHanded, 100);
    }
    applyAnger();
}

void ROWOWMod::BattleMinDamage(ROWOWDamage& wd, Unit const* src, uint16 skillID, int64 minimum) const
{
    // battle_min_damage: cap both damage and basedamage of damage struct to a minimum value
    if (IsAttackRightHanded(src, skillID) == true)
    {
        wd.Damage = std::max(wd.Damage, minimum);
        wd.BaseDamage = std::max(wd.BaseDamage, minimum);
    }
    // Left-hand damage is always capped to 0
    if (IsAttackLeftHanded(src, skillID) == true)
        wd.Damage2 = std::max<int64>(wd.Damage2, 0);
}
