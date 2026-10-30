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

#ifndef ROWOW_SKILL_UNIT_IDS_H
#define ROWOW_SKILL_UNIT_IDS_H

#include "Define.h"
#include <utility>

// rAthena's ground skill units (e_skill_unit_id, the client's view of each unit), with its values, named as skill_db's Unit Id names them
// (UNT_ without the prefix, ROWOW_UNT_ here)
#define ROWOW_UNT_SAFETYWALL                           0x7e
#define ROWOW_UNT_FIREWALL                             0x7f
#define ROWOW_UNT_WARP_WAITING                         0x80
#define ROWOW_UNT_WARP_ACTIVE                          0x81
#define ROWOW_UNT_BENEDICTIO                           0x82
#define ROWOW_UNT_SANCTUARY                            0x83
#define ROWOW_UNT_MAGNUS                               0x84
#define ROWOW_UNT_PNEUMA                               0x85
#define ROWOW_UNT_DUMMYSKILL                           0x86
#define ROWOW_UNT_FIREPILLAR_WAITING                   0x87
#define ROWOW_UNT_FIREPILLAR_ACTIVE                    0x88
#define ROWOW_UNT_HIDDEN_TRAP                          0x89
#define ROWOW_UNT_TRAP                                 0x8a
#define ROWOW_UNT_HIDDEN_WARP_NPC                      0x8b
#define ROWOW_UNT_USED_TRAPS                           0x8c
#define ROWOW_UNT_ICEWALL                              0x8d
#define ROWOW_UNT_QUAGMIRE                             0x8e
#define ROWOW_UNT_BLASTMINE                            0x8f
#define ROWOW_UNT_SKIDTRAP                             0x90
#define ROWOW_UNT_ANKLESNARE                           0x91
#define ROWOW_UNT_VENOMDUST                            0x92
#define ROWOW_UNT_LANDMINE                             0x93
#define ROWOW_UNT_SHOCKWAVE                            0x94
#define ROWOW_UNT_SANDMAN                              0x95
#define ROWOW_UNT_FLASHER                              0x96
#define ROWOW_UNT_FREEZINGTRAP                         0x97
#define ROWOW_UNT_CLAYMORETRAP                         0x98
#define ROWOW_UNT_TALKIEBOX                            0x99
#define ROWOW_UNT_VOLCANO                              0x9a
#define ROWOW_UNT_DELUGE                               0x9b
#define ROWOW_UNT_VIOLENTGALE                          0x9c
#define ROWOW_UNT_LANDPROTECTOR                        0x9d
#define ROWOW_UNT_LULLABY                              0x9e
#define ROWOW_UNT_RICHMANKIM                           0x9f
#define ROWOW_UNT_ETERNALCHAOS                         0xa0
#define ROWOW_UNT_DRUMBATTLEFIELD                      0xa1
#define ROWOW_UNT_RINGNIBELUNGEN                       0xa2
#define ROWOW_UNT_ROKISWEIL                            0xa3
#define ROWOW_UNT_INTOABYSS                            0xa4
#define ROWOW_UNT_SIEGFRIED                            0xa5
#define ROWOW_UNT_DISSONANCE                           0xa6
#define ROWOW_UNT_WHISTLE                              0xa7
#define ROWOW_UNT_ASSASSINCROSS                        0xa8
#define ROWOW_UNT_POEMBRAGI                            0xa9
#define ROWOW_UNT_APPLEIDUN                            0xaa
#define ROWOW_UNT_UGLYDANCE                            0xab
#define ROWOW_UNT_HUMMING                              0xac
#define ROWOW_UNT_DONTFORGETME                         0xad
#define ROWOW_UNT_FORTUNEKISS                          0xae
#define ROWOW_UNT_SERVICEFORYOU                        0xaf
#define ROWOW_UNT_GRAFFITI                             0xb0
#define ROWOW_UNT_DEMONSTRATION                        0xb1
#define ROWOW_UNT_CALLFAMILY                           0xb2
#define ROWOW_UNT_GOSPEL                               0xb3
#define ROWOW_UNT_BASILICA                             0xb4
#define ROWOW_UNT_MOONLIT                              0xb5
#define ROWOW_UNT_FOGWALL                              0xb6
#define ROWOW_UNT_SPIDERWEB                            0xb7
#define ROWOW_UNT_GRAVITATION                          0xb8
#define ROWOW_UNT_HERMODE                              0xb9
#define ROWOW_UNT_KAENSIN                              0xba
#define ROWOW_UNT_SUITON                               0xbb
#define ROWOW_UNT_TATAMIGAESHI                         0xbc
#define ROWOW_UNT_KAEN                                 0xbd
#define ROWOW_UNT_GROUNDDRIFT_WIND                     0xbe
#define ROWOW_UNT_GROUNDDRIFT_DARK                     0xbf
#define ROWOW_UNT_GROUNDDRIFT_POISON                   0xc0
#define ROWOW_UNT_GROUNDDRIFT_WATER                    0xc1
#define ROWOW_UNT_GROUNDDRIFT_FIRE                     0xc2
#define ROWOW_UNT_DEATHWAVE                            0xc3
#define ROWOW_UNT_WATERATTACK                          0xc4
#define ROWOW_UNT_WINDATTACK                           0xc5
#define ROWOW_UNT_EARTHQUAKE                           0xc6
#define ROWOW_UNT_EVILLAND                             0xc7
#define ROWOW_UNT_DARK_RUNNER                          0xc8
#define ROWOW_UNT_DARK_TRANSFER                        0xc9
#define ROWOW_UNT_EPICLESIS                            0xca
#define ROWOW_UNT_EARTHSTRAIN                          0xcb
#define ROWOW_UNT_MANHOLE                              0xcc
#define ROWOW_UNT_DIMENSIONDOOR                        0xcd
#define ROWOW_UNT_CHAOSPANIC                           0xce
#define ROWOW_UNT_MAELSTROM                            0xcf
#define ROWOW_UNT_BLOODYLUST                           0xd0
#define ROWOW_UNT_FEINTBOMB                            0xd1
#define ROWOW_UNT_MAGENTATRAP                          0xd2
#define ROWOW_UNT_COBALTTRAP                           0xd3
#define ROWOW_UNT_MAIZETRAP                            0xd4
#define ROWOW_UNT_VERDURETRAP                          0xd5
#define ROWOW_UNT_FIRINGTRAP                           0xd6
#define ROWOW_UNT_ICEBOUNDTRAP                         0xd7
#define ROWOW_UNT_ELECTRICSHOCKER                      0xd8
#define ROWOW_UNT_CLUSTERBOMB                          0xd9
#define ROWOW_UNT_REVERBERATION                        0xda
#define ROWOW_UNT_SEVERE_RAINSTORM                     0xdb
#define ROWOW_UNT_FIREWALK                             0xdc
#define ROWOW_UNT_ELECTRICWALK                         0xdd
#define ROWOW_UNT_NETHERWORLD                          0xde
#define ROWOW_UNT_PSYCHIC_WAVE                         0xdf
#define ROWOW_UNT_CLOUD_KILL                           0xe0
#define ROWOW_UNT_POISONSMOKE                          0xe1
#define ROWOW_UNT_NEUTRALBARRIER                       0xe2
#define ROWOW_UNT_STEALTHFIELD                         0xe3
#define ROWOW_UNT_WARMER                               0xe4
#define ROWOW_UNT_THORNS_TRAP                          0xe5
#define ROWOW_UNT_WALLOFTHORN                          0xe6
#define ROWOW_UNT_DEMONIC_FIRE                         0xe7
#define ROWOW_UNT_FIRE_EXPANSION_SMOKE_POWDER          0xe8
#define ROWOW_UNT_FIRE_EXPANSION_TEAR_GAS              0xe9
#define ROWOW_UNT_HELLS_PLANT                          0xea
#define ROWOW_UNT_VACUUM_EXTREME                       0xeb
#define ROWOW_UNT_BANDING                              0xec
#define ROWOW_UNT_FIRE_MANTLE                          0xed
#define ROWOW_UNT_WATER_BARRIER                        0xee
#define ROWOW_UNT_ZEPHYR                               0xef
#define ROWOW_UNT_POWER_OF_GAIA                        0xf0
#define ROWOW_UNT_FIRE_INSIGNIA                        0xf1
#define ROWOW_UNT_WATER_INSIGNIA                       0xf2
#define ROWOW_UNT_WIND_INSIGNIA                        0xf3
#define ROWOW_UNT_EARTH_INSIGNIA                       0xf4
#define ROWOW_UNT_POISON_MIST                          0xf5
#define ROWOW_UNT_LAVA_SLIDE                           0xf6
#define ROWOW_UNT_VOLCANIC_ASH                         0xf7
#define ROWOW_UNT_ZENKAI_WATER                         0xf8
#define ROWOW_UNT_ZENKAI_LAND                          0xf9
#define ROWOW_UNT_ZENKAI_FIRE                          0xfa
#define ROWOW_UNT_ZENKAI_WIND                          0xfb
#define ROWOW_UNT_MAKIBISHI                            0xfc
#define ROWOW_UNT_VENOMFOG                             0xfd
#define ROWOW_UNT_ICEMINE                              0xfe
#define ROWOW_UNT_FLAMECROSS                           0xff
#define ROWOW_UNT_HELLBURNING                          0x100
#define ROWOW_UNT_MAGMA_ERUPTION                       0x101
#define ROWOW_UNT_KINGS_GRACE                          0x102
#define ROWOW_UNT_GLITTERING_GREED                     0x103
#define ROWOW_UNT_B_TRAP                               0x104
#define ROWOW_UNT_FIRE_RAIN                            0x105
#define ROWOW_UNT_CATNIPPOWDER                         0x106
#define ROWOW_UNT_NYANGGRASS                           0x107
#define ROWOW_UNT_GROUNDDRIFT_NEUTRAL                  0x108
#define ROWOW_UNT_CREATINGSTAR                         0x109
#define ROWOW_UNT_RAIN_OF_CRYSTAL                      0x10a
#define ROWOW_UNT_MYSTERY_ILLUSION                     0x10b
#define ROWOW_UNT_SEEDTRAP                             0x10c
#define ROWOW_UNT_STRANTUM_TREMOR                      0x10d
#define ROWOW_UNT_VIOLENT_QUAKE                        0x10e
#define ROWOW_UNT_ALL_BLOOM                            0x10f
#define ROWOW_UNT_TORNADO_STORM                        0x110
#define ROWOW_UNT_FLORAL_FLARE_ROAD                    0x111
#define ROWOW_UNT_ASTRAL_STRIKE                        0x112
#define ROWOW_UNT_CROSS_RAIN                           0x113
#define ROWOW_UNT_PNEUMATICUS_PROCELLA                 0x114
#define ROWOW_UNT_ABYSS_SQUARE                         0x115
#define ROWOW_UNT_ACIDIFIED_ZONE_WATER                 0x116
#define ROWOW_UNT_ACIDIFIED_ZONE_GROUND                0x117
#define ROWOW_UNT_ACIDIFIED_ZONE_WIND                  0x118
#define ROWOW_UNT_ACIDIFIED_ZONE_FIRE                  0x119
#define ROWOW_UNT_LIGHTNING_LAND                       0x11a
#define ROWOW_UNT_VENOM_SWAMP                          0x11b
#define ROWOW_UNT_CONFLAGRATION                        0x11c
#define ROWOW_UNT_CANE_OF_EVIL_EYE                     0x11d
#define ROWOW_UNT_TWINKLING_GALAXY                     0x11e
#define ROWOW_UNT_STAR_CANNON                          0x11f
#define ROWOW_UNT_GRENADES_DROPPING                    0x120
#define ROWOW_UNT_UNKNOWN_2                            0x121
#define ROWOW_UNT_FUUMASHOUAKU                         0x122
#define ROWOW_UNT_MISSION_BOMBARD                      0x123
#define ROWOW_UNT_TOTEM_OF_TUTELARY                    0x124
#define ROWOW_UNT_HYUN_ROKS_BREEZE                     0x125
#define ROWOW_UNT_SHINKIROU                            0x126
#define ROWOW_UNT_JACK_FROST_NOVA                      0x127
#define ROWOW_UNT_GROUND_GRAVITATION                   0x128
#define ROWOW_UNT_KUNAIKAITEN                          0x129
#define ROWOW_UNT_KUNAIWAIKYOKU                        0x12a
#define ROWOW_UNT_KUNAIKUSSETSU                        0x12b
#define ROWOW_UNT_SEKIENHOU                            0x12c
#define ROWOW_UNT_STAR_BURST                           0x969
#define ROWOW_UNT_DEEPBLINDTRAP                        20852
#define ROWOW_UNT_SOLIDTRAP                            20853
#define ROWOW_UNT_SWIFTTRAP                            20854
#define ROWOW_UNT_FLAMETRAP                            20855
#define ROWOW_UNT_GD_LEADERSHIP                        0xc1
#define ROWOW_UNT_GD_GLORYWOUNDS                       0xc2
#define ROWOW_UNT_GD_SOULCOLD                          0xc3
#define ROWOW_UNT_GD_HAWKEYES                          0xc4
#define ROWOW_UNT_MAX                                  0x190

// The units by name, as skill_db's Unit Id and Unit AlternateId give them (in any case)
inline constexpr std::pair<char const*, uint16> ROWOWSkillUnitNames[] =
{
    { "SAFETYWALL", 0x7e },
    { "FIREWALL", 0x7f },
    { "WARP_WAITING", 0x80 },
    { "WARP_ACTIVE", 0x81 },
    { "BENEDICTIO", 0x82 },
    { "SANCTUARY", 0x83 },
    { "MAGNUS", 0x84 },
    { "PNEUMA", 0x85 },
    { "DUMMYSKILL", 0x86 },
    { "FIREPILLAR_WAITING", 0x87 },
    { "FIREPILLAR_ACTIVE", 0x88 },
    { "HIDDEN_TRAP", 0x89 },
    { "TRAP", 0x8a },
    { "HIDDEN_WARP_NPC", 0x8b },
    { "USED_TRAPS", 0x8c },
    { "ICEWALL", 0x8d },
    { "QUAGMIRE", 0x8e },
    { "BLASTMINE", 0x8f },
    { "SKIDTRAP", 0x90 },
    { "ANKLESNARE", 0x91 },
    { "VENOMDUST", 0x92 },
    { "LANDMINE", 0x93 },
    { "SHOCKWAVE", 0x94 },
    { "SANDMAN", 0x95 },
    { "FLASHER", 0x96 },
    { "FREEZINGTRAP", 0x97 },
    { "CLAYMORETRAP", 0x98 },
    { "TALKIEBOX", 0x99 },
    { "VOLCANO", 0x9a },
    { "DELUGE", 0x9b },
    { "VIOLENTGALE", 0x9c },
    { "LANDPROTECTOR", 0x9d },
    { "LULLABY", 0x9e },
    { "RICHMANKIM", 0x9f },
    { "ETERNALCHAOS", 0xa0 },
    { "DRUMBATTLEFIELD", 0xa1 },
    { "RINGNIBELUNGEN", 0xa2 },
    { "ROKISWEIL", 0xa3 },
    { "INTOABYSS", 0xa4 },
    { "SIEGFRIED", 0xa5 },
    { "DISSONANCE", 0xa6 },
    { "WHISTLE", 0xa7 },
    { "ASSASSINCROSS", 0xa8 },
    { "POEMBRAGI", 0xa9 },
    { "APPLEIDUN", 0xaa },
    { "UGLYDANCE", 0xab },
    { "HUMMING", 0xac },
    { "DONTFORGETME", 0xad },
    { "FORTUNEKISS", 0xae },
    { "SERVICEFORYOU", 0xaf },
    { "GRAFFITI", 0xb0 },
    { "DEMONSTRATION", 0xb1 },
    { "CALLFAMILY", 0xb2 },
    { "GOSPEL", 0xb3 },
    { "BASILICA", 0xb4 },
    { "MOONLIT", 0xb5 },
    { "FOGWALL", 0xb6 },
    { "SPIDERWEB", 0xb7 },
    { "GRAVITATION", 0xb8 },
    { "HERMODE", 0xb9 },
    { "KAENSIN", 0xba },
    { "SUITON", 0xbb },
    { "TATAMIGAESHI", 0xbc },
    { "KAEN", 0xbd },
    { "GROUNDDRIFT_WIND", 0xbe },
    { "GROUNDDRIFT_DARK", 0xbf },
    { "GROUNDDRIFT_POISON", 0xc0 },
    { "GROUNDDRIFT_WATER", 0xc1 },
    { "GROUNDDRIFT_FIRE", 0xc2 },
    { "DEATHWAVE", 0xc3 },
    { "WATERATTACK", 0xc4 },
    { "WINDATTACK", 0xc5 },
    { "EARTHQUAKE", 0xc6 },
    { "EVILLAND", 0xc7 },
    { "DARK_RUNNER", 0xc8 },
    { "DARK_TRANSFER", 0xc9 },
    { "EPICLESIS", 0xca },
    { "EARTHSTRAIN", 0xcb },
    { "MANHOLE", 0xcc },
    { "DIMENSIONDOOR", 0xcd },
    { "CHAOSPANIC", 0xce },
    { "MAELSTROM", 0xcf },
    { "BLOODYLUST", 0xd0 },
    { "FEINTBOMB", 0xd1 },
    { "MAGENTATRAP", 0xd2 },
    { "COBALTTRAP", 0xd3 },
    { "MAIZETRAP", 0xd4 },
    { "VERDURETRAP", 0xd5 },
    { "FIRINGTRAP", 0xd6 },
    { "ICEBOUNDTRAP", 0xd7 },
    { "ELECTRICSHOCKER", 0xd8 },
    { "CLUSTERBOMB", 0xd9 },
    { "REVERBERATION", 0xda },
    { "SEVERE_RAINSTORM", 0xdb },
    { "FIREWALK", 0xdc },
    { "ELECTRICWALK", 0xdd },
    { "NETHERWORLD", 0xde },
    { "PSYCHIC_WAVE", 0xdf },
    { "CLOUD_KILL", 0xe0 },
    { "POISONSMOKE", 0xe1 },
    { "NEUTRALBARRIER", 0xe2 },
    { "STEALTHFIELD", 0xe3 },
    { "WARMER", 0xe4 },
    { "THORNS_TRAP", 0xe5 },
    { "WALLOFTHORN", 0xe6 },
    { "DEMONIC_FIRE", 0xe7 },
    { "FIRE_EXPANSION_SMOKE_POWDER", 0xe8 },
    { "FIRE_EXPANSION_TEAR_GAS", 0xe9 },
    { "HELLS_PLANT", 0xea },
    { "VACUUM_EXTREME", 0xeb },
    { "BANDING", 0xec },
    { "FIRE_MANTLE", 0xed },
    { "WATER_BARRIER", 0xee },
    { "ZEPHYR", 0xef },
    { "POWER_OF_GAIA", 0xf0 },
    { "FIRE_INSIGNIA", 0xf1 },
    { "WATER_INSIGNIA", 0xf2 },
    { "WIND_INSIGNIA", 0xf3 },
    { "EARTH_INSIGNIA", 0xf4 },
    { "POISON_MIST", 0xf5 },
    { "LAVA_SLIDE", 0xf6 },
    { "VOLCANIC_ASH", 0xf7 },
    { "ZENKAI_WATER", 0xf8 },
    { "ZENKAI_LAND", 0xf9 },
    { "ZENKAI_FIRE", 0xfa },
    { "ZENKAI_WIND", 0xfb },
    { "MAKIBISHI", 0xfc },
    { "VENOMFOG", 0xfd },
    { "ICEMINE", 0xfe },
    { "FLAMECROSS", 0xff },
    { "HELLBURNING", 0x100 },
    { "MAGMA_ERUPTION", 0x101 },
    { "KINGS_GRACE", 0x102 },
    { "GLITTERING_GREED", 0x103 },
    { "B_TRAP", 0x104 },
    { "FIRE_RAIN", 0x105 },
    { "CATNIPPOWDER", 0x106 },
    { "NYANGGRASS", 0x107 },
    { "GROUNDDRIFT_NEUTRAL", 0x108 },
    { "CREATINGSTAR", 0x109 },
    { "RAIN_OF_CRYSTAL", 0x10a },
    { "MYSTERY_ILLUSION", 0x10b },
    { "SEEDTRAP", 0x10c },
    { "STRANTUM_TREMOR", 0x10d },
    { "VIOLENT_QUAKE", 0x10e },
    { "ALL_BLOOM", 0x10f },
    { "TORNADO_STORM", 0x110 },
    { "FLORAL_FLARE_ROAD", 0x111 },
    { "ASTRAL_STRIKE", 0x112 },
    { "CROSS_RAIN", 0x113 },
    { "PNEUMATICUS_PROCELLA", 0x114 },
    { "ABYSS_SQUARE", 0x115 },
    { "ACIDIFIED_ZONE_WATER", 0x116 },
    { "ACIDIFIED_ZONE_GROUND", 0x117 },
    { "ACIDIFIED_ZONE_WIND", 0x118 },
    { "ACIDIFIED_ZONE_FIRE", 0x119 },
    { "LIGHTNING_LAND", 0x11a },
    { "VENOM_SWAMP", 0x11b },
    { "CONFLAGRATION", 0x11c },
    { "CANE_OF_EVIL_EYE", 0x11d },
    { "TWINKLING_GALAXY", 0x11e },
    { "STAR_CANNON", 0x11f },
    { "GRENADES_DROPPING", 0x120 },
    { "UNKNOWN_2", 0x121 },
    { "FUUMASHOUAKU", 0x122 },
    { "MISSION_BOMBARD", 0x123 },
    { "TOTEM_OF_TUTELARY", 0x124 },
    { "HYUN_ROKS_BREEZE", 0x125 },
    { "SHINKIROU", 0x126 },
    { "JACK_FROST_NOVA", 0x127 },
    { "GROUND_GRAVITATION", 0x128 },
    { "KUNAIKAITEN", 0x129 },
    { "KUNAIWAIKYOKU", 0x12a },
    { "KUNAIKUSSETSU", 0x12b },
    { "SEKIENHOU", 0x12c },
    { "STAR_BURST", 0x969 },
    { "DEEPBLINDTRAP", 20852 },
    { "SOLIDTRAP", 20853 },
    { "SWIFTTRAP", 20854 },
    { "FLAMETRAP", 20855 },
    { "GD_LEADERSHIP", 0xc1 },
    { "GD_GLORYWOUNDS", 0xc2 },
    { "GD_SOULCOLD", 0xc3 },
    { "GD_HAWKEYES", 0xc4 },
};

#endif
