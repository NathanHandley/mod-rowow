-- An RO identity's character variables (mod-rowow) for the NPC scripts (R7): rAthena's char_reg_num and char_reg_str by name and array
-- index (a string variable's name ends in $ and keeps its text)
ALTER TABLE `mod_rowow_identity_variable`
	ADD COLUMN `index` INT(10) UNSIGNED NOT NULL DEFAULT '0' AFTER `name`,
	ADD COLUMN `text` VARCHAR(255) NOT NULL DEFAULT '' AFTER `value`,
	DROP PRIMARY KEY,
	ADD PRIMARY KEY (`guid`, `name`, `index`) USING BTREE;
