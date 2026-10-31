-- The RO NPC scripts' account variables (mod-rowow): rAthena's acc_reg_num/acc_reg_str (#name) and global_acc_reg (##name), by name (with
-- its prefix) and array index (a string variable's name ends in $ and keeps its text)
CREATE TABLE IF NOT EXISTS `mod_rowow_account_variable` (
	`account` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`name` VARCHAR(32) NOT NULL DEFAULT '',
	`index` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`value` BIGINT(20) NOT NULL DEFAULT '0',
	`text` VARCHAR(255) NOT NULL DEFAULT '',
	PRIMARY KEY (`account`, `name`, `index`) USING BTREE
);
