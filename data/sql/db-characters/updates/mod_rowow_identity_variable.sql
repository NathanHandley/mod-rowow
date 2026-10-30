-- An RO identity's character variables (mod-rowow), rAthena's char_reg_num (like jobchange_level, the job level of the change to a second job)
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_variable` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`name` VARCHAR(32) NOT NULL DEFAULT '',
	`value` BIGINT(20) NOT NULL DEFAULT '0',
	PRIMARY KEY (`guid`, `name`) USING BTREE
);
