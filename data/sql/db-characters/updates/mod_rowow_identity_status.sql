-- An RO identity's status changes kept for the next time in the RO world (mod-rowow), rAthena's sc_data: the tick left (-1 infinite) and values
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_status` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`type` SMALLINT(6) NOT NULL DEFAULT '0',
	`tick` BIGINT(20) NOT NULL DEFAULT '0',
	`val1` INT(11) NOT NULL DEFAULT '0',
	`val2` INT(11) NOT NULL DEFAULT '0',
	`val3` INT(11) NOT NULL DEFAULT '0',
	`val4` INT(11) NOT NULL DEFAULT '0',
	PRIMARY KEY (`guid`, `type`) USING BTREE
);
