-- A character's three RO identity save slots (mod-rowow, Nathan 2026-11-09).  Each slot's identity has its own ID, which keys its rows in the identity
-- tables (their `guid`, the homunculus and mercenary tables' `CharacterGUID`, and mod_rowow_pet's `OwnerGUID` for the pet that's out): rAthena's char_id.
-- Slot 1's is the character's GUID, so the identity a character had before the slots is its slot 1; the others' are given from 2147483648 up, past any
-- character GUID.  The identities share only the zeny (mod_rowow_identity_character, beside the slot picked) and the account's Ragnarok bank
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_slot` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The character''s GUID',
	`slot` TINYINT(3) UNSIGNED NOT NULL DEFAULT '1' COMMENT '1 to 3',
	`identity` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The identity''s ID, its rows'' guid: the character''s GUID for slot 1',
	`name` VARCHAR(24) NOT NULL DEFAULT '' COMMENT 'The slot''s name (not the character''s)',
	PRIMARY KEY (`guid`, `slot`) USING BTREE,
	UNIQUE KEY `identity` (`identity`)
);

CREATE TABLE IF NOT EXISTS `mod_rowow_identity_character` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The character''s GUID',
	`slot` TINYINT(3) UNSIGNED NOT NULL DEFAULT '1' COMMENT 'The save slot picked',
	`zeny` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The zeny its slots share',
	PRIMARY KEY (`guid`) USING BTREE
);

-- The identities from before the slots are their characters' slot 1, named "Slot 1", and picked, their zeny the shared zeny.  Only rows keyed by a
-- character's GUID, and none already there, so it can run again
INSERT IGNORE INTO `mod_rowow_identity_slot` (`guid`, `slot`, `identity`, `name`)
	SELECT `guid`, 1, `guid`, 'Slot 1' FROM `mod_rowow_identity` WHERE `guid` < 2147483648;
INSERT IGNORE INTO `mod_rowow_identity_character` (`guid`, `slot`, `zeny`)
	SELECT `guid`, 1, `zeny` FROM `mod_rowow_identity` WHERE `guid` < 2147483648;
