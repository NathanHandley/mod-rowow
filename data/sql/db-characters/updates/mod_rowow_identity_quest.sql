-- An RO identity's quest log (mod-rowow, R8): rAthena's quest table, in order (the active and inactive quests first, then the completed ones).
-- state 0 inactive, 1 active, 2 complete; time is when it runs out (Unix time, 0 for no limit); count1-3 the kills made of its hunting targets
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_quest` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`slot` SMALLINT(5) UNSIGNED NOT NULL DEFAULT '0',
	`quest_id` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`state` TINYINT(3) UNSIGNED NOT NULL DEFAULT '1',
	`time` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`count1` INT(10) NOT NULL DEFAULT '0',
	`count2` INT(10) NOT NULL DEFAULT '0',
	`count3` INT(10) NOT NULL DEFAULT '0',
	PRIMARY KEY (`guid`, `slot`) USING BTREE
);
