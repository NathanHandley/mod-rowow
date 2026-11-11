-- An RO identity's family (mod-rowow, Nathan 2026-11-11; ROWOW_Family.cpp): rAthena's status.partner_id, father, mother and child, by the other identities' IDs
-- (an identity is a save slot: its ID is rAthena's char_id, see mod_rowow_identity_slot), 0 for none.  Written at each marriage, divorce and adoption, for
-- both or all three identities; a reset or deleted identity's row goes, and the others' links to it.  ring_removal_pending: divorced while that save slot
-- wasn't played in Midgard, so its wedding ring is taken when it next is (rAthena's char server deletes an offline partner's ring at once)
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_family` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The identity''s ID',
	`partner_id` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`father_id` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`mother_id` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`child_id` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`ring_removal_pending` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
	PRIMARY KEY (`guid`) USING BTREE
);
