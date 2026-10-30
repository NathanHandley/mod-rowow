-- The options an RO identity keeps (mod-rowow): rAthena's status.option, saved with the character (char.option): a Knight's or Crusader's Peco (OPTION_RIDING 0x20)
ALTER TABLE `mod_rowow_identity` ADD COLUMN `option` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'rAthena status.option (OPTION_RIDING)' AFTER `in_form`;
