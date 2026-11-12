-- V001__init.sql —— 初始 schema（唯一权威：02-ARCH v2.3 §5.2 / 08 §6.2 迁移规约）
-- 规约：InnoDB / utf8mb4_0900_ai_ci / DATETIME(3) / 禁分区、禁外键级联删除、禁触发器、禁存储过程。
-- 种子数据（foods 6 道、themes 4 个）由模块迁移 V002+ 追加，本文件只建表。
-- rollback: 见文件末尾。

CREATE TABLE IF NOT EXISTS users (
  user_id       BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  username      VARCHAR(32)  NOT NULL,
  pass_hash     VARCHAR(255) NOT NULL,             -- Argon2id 编码串
  status        TINYINT      NOT NULL DEFAULT 1,   -- 1 正常 0 禁用
  fail_count    SMALLINT     NOT NULL DEFAULT 0,
  locked_until  DATETIME(3)  NULL,
  created_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (user_id),
  UNIQUE KEY uk_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS user_sessions (
  session_id  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  user_id     BIGINT UNSIGNED NOT NULL,
  token_hash  CHAR(64)     NOT NULL,               -- SHA-256(token)，不存明文
  device      VARCHAR(64)  NULL,
  expires_at  DATETIME(3)  NOT NULL,
  created_at  DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (session_id),
  UNIQUE KEY uk_token (token_hash),
  KEY idx_uid (user_id),
  KEY idx_expire (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS profiles (
  user_id      BIGINT UNSIGNED NOT NULL,
  name         VARCHAR(32)  NOT NULL DEFAULT '我',
  height_cm    SMALLINT     NOT NULL,
  weight_kg    DECIMAL(5,1) NOT NULL,
  age          TINYINT      NOT NULL,
  goal         VARCHAR(16)  NOT NULL,              -- 均衡饮食/控制体重/增加蛋白/减少添加糖
  activity     VARCHAR(16)  NOT NULL,              -- 久坐/适中/较高
  water_ml     SMALLINT     NOT NULL,
  sleep_hours  DECIMAL(3,1) NOT NULL,
  note         VARCHAR(255) NULL,
  updated_at   DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  PRIMARY KEY (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS foods (
  food_id     BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  slug        VARCHAR(32)  NOT NULL,               -- 对外标识（app.js:foods.id）
  name        VARCHAR(64)  NOT NULL,
  category    VARCHAR(64)  NOT NULL,
  description VARCHAR(255) NOT NULL,
  calories    SMALLINT     NOT NULL,
  protein_g   SMALLINT     NOT NULL,
  fiber_g     SMALLINT     NOT NULL,
  veggies_g   SMALLINT     NOT NULL,
  water_ml    SMALLINT     NOT NULL,
  image_url   VARCHAR(512) NOT NULL,
  active      TINYINT      NOT NULL DEFAULT 1,
  PRIMARY KEY (food_id),
  UNIQUE KEY uk_slug (slug),
  UNIQUE KEY uk_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS meals (
  meal_id       BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  user_id       BIGINT UNSIGNED NOT NULL,
  eaten_date    DATE      NOT NULL,
  meal_type     VARCHAR(8) NOT NULL,               -- 早餐/午餐/晚餐/加餐
  eaten_at      DATETIME(3) NOT NULL,
  source        TINYINT    NOT NULL,               -- 1自然语言 2卡片 3猜餐 4快捷
  note          VARCHAR(500) NOT NULL,
  protein_g     SMALLINT   NOT NULL DEFAULT 0,
  fiber_g       SMALLINT   NOT NULL DEFAULT 0,
  veggies_g     SMALLINT   NOT NULL DEFAULT 0,
  water_ml      SMALLINT   NOT NULL DEFAULT 0,
  penalty       TINYINT    NOT NULL DEFAULT 0,
  client_req_id CHAR(36)   NOT NULL,               -- 幂等键（仅 POST /meals）
  created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (meal_id),
  UNIQUE KEY uk_req (user_id, client_req_id),
  KEY idx_user_date (user_id, eaten_date, meal_id),
  CONSTRAINT chk_type CHECK (meal_type IN ('早餐','午餐','晚餐','加餐')),
  CONSTRAINT chk_req  CHECK (client_req_id <> '')
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS meal_items (
  item_id    BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  meal_id    BIGINT UNSIGNED NOT NULL,
  food_id    BIGINT UNSIGNED NULL,                 -- 自然语言记录可为空
  food_name  VARCHAR(64)  NOT NULL,                -- 快照，防食物库改动影响历史
  qty        DECIMAL(4,1) NOT NULL DEFAULT 1.0,
  protein_g  SMALLINT NOT NULL,
  fiber_g    SMALLINT NOT NULL,
  veggies_g  SMALLINT NOT NULL,
  water_ml   SMALLINT NOT NULL,
  PRIMARY KEY (item_id),
  KEY idx_meal (meal_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS daily_scores (
  user_id      BIGINT UNSIGNED NOT NULL,
  score_date   DATE      NOT NULL,
  score        TINYINT   NOT NULL,
  meals_count  TINYINT   NOT NULL,                 -- 含加餐
  status_text  VARCHAR(16) NOT NULL,
  rule_version SMALLINT  NOT NULL,
  recomputable TINYINT   NOT NULL DEFAULT 1,       -- 演示数据置 0
  updated_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  PRIMARY KEY (user_id, score_date),
  KEY idx_date (score_date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- append-only：应用层保证本表无 UPDATE / DELETE（CI 检查 check_ledger_append_only.sh）。
CREATE TABLE IF NOT EXISTS point_ledger (
  ledger_id  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  user_id    BIGINT UNSIGNED NOT NULL,
  week_key   DATE      NOT NULL,                   -- 该积分周周一
  biz_date   DATE      NOT NULL,                   -- 事件所属业务日
  event_type VARCHAR(16) NOT NULL,
  ref_key    VARCHAR(64) NOT NULL,
  delta      SMALLINT  NOT NULL,                   -- 兑换为负
  created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (ledger_id),
  UNIQUE KEY uk_event (user_id, event_type, ref_key),
  KEY idx_user_week (user_id, week_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS user_points (
  user_id    BIGINT UNSIGNED NOT NULL,
  week_key   DATE      NOT NULL,
  earned     INT       NOT NULL DEFAULT 0,
  spent      INT       NOT NULL DEFAULT 0,
  updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  PRIMARY KEY (user_id, week_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS point_rules (
  version         SMALLINT NOT NULL,
  checkin         SMALLINT NOT NULL DEFAULT 10,
  meal            SMALLINT NOT NULL DEFAULT 10,
  three_meals     SMALLINT NOT NULL DEFAULT 20,
  interaction     SMALLINT NOT NULL DEFAULT 2,
  interaction_cap SMALLINT NOT NULL DEFAULT 20,
  health_high     SMALLINT NOT NULL DEFAULT 20,    -- ≥90
  health_mid      SMALLINT NOT NULL DEFAULT 10,    -- 80–89
  health_low      SMALLINT NOT NULL DEFAULT 5,     -- <80
  rank_1          SMALLINT NOT NULL DEFAULT 50,
  rank_2          SMALLINT NOT NULL DEFAULT 30,
  rank_3          SMALLINT NOT NULL DEFAULT 20,
  rank_other      SMALLINT NOT NULL DEFAULT 10,
  effective_from  DATE     NOT NULL,
  PRIMARY KEY (version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS themes (
  theme_id    VARCHAR(16) NOT NULL,
  name        VARCHAR(32) NOT NULL,
  description VARCHAR(128) NOT NULL,
  price       SMALLINT NOT NULL,
  css_key     VARCHAR(32) NOT NULL,
  active      TINYINT NOT NULL DEFAULT 1,
  PRIMARY KEY (theme_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS theme_ownership (
  user_id     BIGINT UNSIGNED NOT NULL,
  theme_id    VARCHAR(16) NOT NULL,
  price_paid  SMALLINT    NOT NULL,
  acquired_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (user_id, theme_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS user_prefs (
  user_id        BIGINT UNSIGNED NOT NULL,
  theme_selected VARCHAR(16) NOT NULL DEFAULT 'aurora',
  motion_enabled TINYINT NOT NULL DEFAULT 1,
  last_view      VARCHAR(16) NULL,
  updated_at     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  PRIMARY KEY (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS weekly_settlements (
  user_id     BIGINT UNSIGNED NOT NULL,
  week_key    DATE     NOT NULL,
  avg_score   DECIMAL(4,1) NOT NULL,
  rank_no     SMALLINT NOT NULL,
  bonus       SMALLINT NOT NULL,
  settled_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (user_id, week_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS settlement_runs (
  week_key    DATE     NOT NULL,
  status      VARCHAR(12) NOT NULL,                -- running/done/failed
  total_users INT      NOT NULL DEFAULT 0,
  done_users  INT      NOT NULL DEFAULT 0,
  started_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  finished_at DATETIME(3) NULL,
  last_error  VARCHAR(255) NULL,
  PRIMARY KEY (week_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS task_checkpoint (
  task_id     VARCHAR(32) NOT NULL,
  last_run_at DATETIME(3) NOT NULL,
  PRIMARY KEY (task_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS schema_migrations (
  version    VARCHAR(16) NOT NULL,
  applied_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- rollback: 逆序 DROP（仅人工执行）
--   DROP TABLE IF EXISTS schema_migrations;
--   DROP TABLE IF EXISTS task_checkpoint;
--   DROP TABLE IF EXISTS settlement_runs;
--   DROP TABLE IF EXISTS weekly_settlements;
--   DROP TABLE IF EXISTS user_prefs;
--   DROP TABLE IF EXISTS theme_ownership;
--   DROP TABLE IF EXISTS themes;
--   DROP TABLE IF EXISTS point_rules;
--   DROP TABLE IF EXISTS user_points;
--   DROP TABLE IF EXISTS point_ledger;
--   DROP TABLE IF EXISTS daily_scores;
--   DROP TABLE IF EXISTS meal_items;
--   DROP TABLE IF EXISTS meals;
--   DROP TABLE IF EXISTS foods;
--   DROP TABLE IF EXISTS profiles;
--   DROP TABLE IF EXISTS user_sessions;
--   DROP TABLE IF EXISTS users;
