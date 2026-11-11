-- The return tether's time is its save slot's play time in Midgard, not real time (mod-rowow, Nathan 2026-11-11; ROWOW_ReturnTether.cpp): remaining_ms is
-- what it has left, which counts down only while that identity (save slot, `guid`) is played in the RO world.  A row from before keeps the real time it
-- had left.  (This file's name sorts after mod_rowow_identity_tether.sql.)
ALTER TABLE `mod_rowow_identity_tether`
	ADD COLUMN `remaining_ms` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'Time the tether has left, counted only while its identity is played in Midgard' AFTER `o`;
UPDATE `mod_rowow_identity_tether` SET `remaining_ms` = LEAST(GREATEST(CAST(`expire_time` AS SIGNED) - UNIX_TIMESTAMP(), 0), 1800) * 1000;
DELETE FROM `mod_rowow_identity_tether` WHERE `remaining_ms` = 0;
ALTER TABLE `mod_rowow_identity_tether`
	DROP COLUMN `expire_time`;
