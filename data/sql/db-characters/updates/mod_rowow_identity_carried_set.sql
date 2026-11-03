-- Which carried set an RO identity's character has on it (mod-rowow): 0 none set apart yet (an identity from before, its RO items mixed in with the
-- character's), 1 the character's own, 2 the RO form's.  The other one is in mod_rowow_identity_stowed_item
ALTER TABLE `mod_rowow_identity` ADD COLUMN `carried_set` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0' COMMENT '0 none apart yet, 1 the character''s, 2 the form''s';
