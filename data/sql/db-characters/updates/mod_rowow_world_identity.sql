-- A character's identity for each world (mod-rowow, Nathan 2026-11-09): which of its three forms it shows in Azeroth, in Norrath and in Midgard.
-- `world`: 0 Azeroth (every map that is neither of the others), 1 Norrath (the EverQuest maps), 2 Midgard (the Ragnarok Online maps).
-- `identity`: 0 Warcraft, 1 EverQuest, 2 Ragnarok.
-- A world with no row shows the world's own identity (Azeroth: Warcraft, Norrath: EverQuest, Midgard: Ragnarok), so only a choice that differs is kept.
CREATE TABLE IF NOT EXISTS `mod_rowow_world_identity` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The character''s GUID',
	`world` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0' COMMENT '0 Azeroth, 1 Norrath, 2 Midgard',
	`identity` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0' COMMENT '0 Warcraft, 1 EverQuest, 2 Ragnarok',
	PRIMARY KEY (`guid`, `world`) USING BTREE
);
