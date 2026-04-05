-- ═══════════════════════════════════════════════════════════
-- LUMIN Traffic Controller — MySQL Database Schema
-- Run this in your MySQL server (via phpMyAdmin or mysql CLI)
-- ═══════════════════════════════════════════════════════════

CREATE DATABASE IF NOT EXISTS traffic_controller
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE traffic_controller;

-- ═══════════════════════════════════════════════════════════
-- TABLE: device_config
-- Stores the current state and configuration
-- ═══════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS device_config (
  id INT PRIMARY KEY DEFAULT 1,
  enabled BOOLEAN DEFAULT FALSE,
  mode VARCHAR(10) DEFAULT 'auto',       -- 'auto' | 'manual'
  manual_light VARCHAR(10) DEFAULT 'red', -- 'red' | 'yellow' | 'green'
  current_light VARCHAR(10) DEFAULT 'off', -- 'red' | 'yellow' | 'green' | 'off'
  remaining_time INT DEFAULT 0,
  updated_at BIGINT DEFAULT 0,          -- milliseconds since boot (or unix timestamp)
  updated_by VARCHAR(20) DEFAULT 'device' -- 'device' | 'dashboard'
);

-- Initialize with default row
INSERT INTO device_config (id, enabled, mode, manual_light, current_light, remaining_time, updated_at)
VALUES (1, FALSE, 'auto', 'red', 'off', 0, 0)
ON DUPLICATE KEY UPDATE id=id;

-- ═══════════════════════════════════════════════════════════
-- TABLE: device_status
-- Heartbeat / online status
-- ═══════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS device_status (
  id INT PRIMARY KEY DEFAULT 1,
  online BOOLEAN DEFAULT FALSE,
  last_seen BIGINT DEFAULT 0,
  ip_address VARCHAR(20) DEFAULT NULL
);

INSERT INTO device_status (id, online, last_seen)
VALUES (1, FALSE, 0)
ON DUPLICATE KEY UPDATE id=id;

-- ═══════════════════════════════════════════════════════════
-- TABLE: traffic_log
-- History of light changes
-- ═══════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS traffic_log (
  id INT AUTO_INCREMENT PRIMARY KEY,
  light VARCHAR(10) NOT NULL,           -- 'red' | 'yellow' | 'green' | 'off'
  mode VARCHAR(10) DEFAULT 'auto',        -- 'auto' | 'manual'
  source VARCHAR(20) DEFAULT 'device',  -- 'device' | 'dashboard'
  created_at BIGINT DEFAULT 0
);

-- Add indexes for performance
CREATE INDEX idx_traffic_log_created ON traffic_log(created_at DESC);
CREATE INDEX idx_traffic_log_light ON traffic_log(light);
