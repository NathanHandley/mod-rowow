-- The RO NPC scripts' global variables (mod-rowow): rAthena's mapreg ($name), by name and array index (a string variable's name ends in $
-- and keeps its text)
CREATE TABLE IF NOT EXISTS `mod_rowow_global_variable` (
	`name` VARCHAR(32) NOT NULL DEFAULT '',
	`index` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`value` BIGINT(20) NOT NULL DEFAULT '0',
	`text` VARCHAR(255) NOT NULL DEFAULT '',
	PRIMARY KEY (`name`, `index`) USING BTREE
);
