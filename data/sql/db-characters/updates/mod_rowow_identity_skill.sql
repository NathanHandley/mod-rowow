-- The skills an RO identity learned (mod-rowow), by rAthena skill ID, and their levels
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_skill` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`skill_id` SMALLINT(5) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'rAthena skill ID',
	`level` SMALLINT(5) UNSIGNED NOT NULL DEFAULT '0',
	PRIMARY KEY (`guid`, `skill_id`) USING BTREE
);
