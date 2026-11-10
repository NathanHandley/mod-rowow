-- A character's RO look, shared by its three save slots (mod-rowow, Nathan 2026-11-09).
-- One head: the hair style and color are the character's (mod_rowow_identity_character), not each slot's, so the hair is picked once, when the
-- character's first RO character is made, and a stylist's change is every slot's.  `hair_style` 0 is no head yet (RO's styles start at 1).  The slots'
-- own `hair_style` and `hair_color` (mod_rowow_identity) are still written, as copies of the shared ones.
-- The job sprite shown (Options > Interface > Ragnarok): -1 follows the job of the slot in use, else the ID of a job in the character's job history.
ALTER TABLE `mod_rowow_identity_character`
	ADD COLUMN `hair_style` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The head its slots share: 0 none yet',
	ADD COLUMN `hair_color` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
	ADD COLUMN `sprite_job` SMALLINT(6) NOT NULL DEFAULT '-1' COMMENT 'The job whose sprite is shown: -1 the job of the slot in use';

-- The jobs the character's RO characters are or have been, over all its save slots (the job sprites it may pick from).  Kept when a slot is reset.
-- The module adds a job when it's reached, and at each login whatever the slots' jobs and the jobs leading to them lack (so no rows are made here)
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_job_history` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The character''s GUID',
	`job` SMALLINT(5) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'rAthena''s job ID',
	PRIMARY KEY (`guid`, `job`) USING BTREE
);

-- A character whose identities are from before the slots' rows has its row now (as mod_rowow_identity_slot.sql made them)
INSERT IGNORE INTO `mod_rowow_identity_character` (`guid`, `slot`, `zeny`)
	SELECT `guid`, 1, `zeny` FROM `mod_rowow_identity` WHERE `guid` < 2147483648;

-- The head every slot keeps is slot 1's, else the lowest slot's that has an identity (slot 1's identity ID is the character's GUID)
UPDATE `mod_rowow_identity_character` `character`
	LEFT JOIN `mod_rowow_identity` `identity1` ON `identity1`.`guid` = `character`.`guid`
	LEFT JOIN `mod_rowow_identity_slot` `slot2` ON `slot2`.`guid` = `character`.`guid` AND `slot2`.`slot` = 2
	LEFT JOIN `mod_rowow_identity` `identity2` ON `identity2`.`guid` = `slot2`.`identity` AND `slot2`.`identity` >= 2147483648
	LEFT JOIN `mod_rowow_identity_slot` `slot3` ON `slot3`.`guid` = `character`.`guid` AND `slot3`.`slot` = 3
	LEFT JOIN `mod_rowow_identity` `identity3` ON `identity3`.`guid` = `slot3`.`identity` AND `slot3`.`identity` >= 2147483648
	SET `character`.`hair_style` = COALESCE(`identity1`.`hair_style`, `identity2`.`hair_style`, `identity3`.`hair_style`, 0),
		`character`.`hair_color` = COALESCE(`identity1`.`hair_color`, `identity2`.`hair_color`, `identity3`.`hair_color`, 0)
	WHERE `character`.`hair_style` = 0;

-- ... and the slots' own copies follow it
UPDATE `mod_rowow_identity` `identity`
	JOIN `mod_rowow_identity_slot` `slot` ON `slot`.`identity` = `identity`.`guid`
	JOIN `mod_rowow_identity_character` `character` ON `character`.`guid` = `slot`.`guid`
	SET `identity`.`hair_style` = `character`.`hair_style`, `identity`.`hair_color` = `character`.`hair_color`
	WHERE `character`.`hair_style` <> 0;
