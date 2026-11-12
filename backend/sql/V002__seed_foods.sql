-- V002__seed_foods.sql —— M5 食物库种子数据（B 提案，按 08 §2 由 Agent A 复核后落盘到 backend/migrations/）
-- 数据源：原始前端 app.js 的 foods 数组（6 条），逐字段一致。
--
-- 匹配键：**slug**（= app.js:foods.id），不是 food_id 也不是 name。
-- 每条 3 条语句，刻意不用 ON DUPLICATE KEY UPDATE —— 原因见下：
--   ON DUPLICATE KEY 对**任意**唯一键冲突都会触发更新。foods 同时有 uk_slug 与 uk_name，
--   若某个 name 已被"另一个 slug"占用，ON DUPLICATE 会去更新那条无关记录，
--   既不报错也不正确（B-12 指出的缺陷）。
--
-- 本文件的语义（三条硬规则）：
--   ① 以 slug 为业务匹配键；不存在才 INSERT，存在只 UPDATE 该 slug 那一行；
--   ② 若 INSERT 时撞上 uk_name（同名不同 slug），**让它以 1062 失败**，迁移中止，
--      由人工判断是"改名"还是"删旧行"——绝不自动更新另一条记录；
--   ③ 重复执行不产生重复记录（INSERT 被 @exists 守卫拦住）。
-- 不写 food_id：交给 AUTO_INCREMENT，避免"显式 id 与既有数据抢主键"。


-- ================================================================ 前置校验
-- V001 必须同时提供 uk_slug 与 uk_name；缺任一则本迁移的语义不成立，必须中止。
--
-- 实现方式说明（协调者改）：
-- 原方案用「TEMPORARY TABLE + CHECK 约束」制造硬失败。但 MySQL 对临时表上
-- CHECK 约束的支持是**版本相关**的（可能被忽略、也可能在建表时报错），
-- 两种结果都不可接受：被忽略=守卫形同不存在；报错=整个迁移无谓中止。
-- 改为**可移植的标量子查询守卫**：条件不满足时子查询返回 2 行，
-- MySQL 必报 ERROR 1242 (Subquery returns more than 1 row) → 迁移中止；
-- 条件满足时返回 0 行 → 赋 NULL，脚本继续。5.7/8.0 行为一致，不依赖临时表。
SET @uk_count := (SELECT COUNT(DISTINCT index_name)
                    FROM information_schema.statistics
                   WHERE table_schema = DATABASE()
                     AND table_name = 'foods'
                     AND index_name IN ('uk_slug', 'uk_name'));

-- @uk_count = 2 时无行；否则 2 行 → ERROR 1242，迁移在此中止
SET @uk_guard := (SELECT 1 FROM (SELECT 1 AS n UNION ALL SELECT 2) t
                   WHERE @uk_count <> 2);

SELECT IF(@uk_count = 2, 'uk_ok', CONCAT('uk_bad:', @uk_count)) AS uk_assert;


-- ---------------------------------------------------------------- lemon-chicken
SET @exists := (SELECT COUNT(*) FROM foods WHERE slug = 'lemon-chicken');

INSERT INTO foods (slug, name, category, description,
                   calories, protein_g, fiber_g, veggies_g, water_ml, image_url, active)
SELECT 'lemon-chicken', '柠檬鸡胸能量碗', '高蛋白 · 清爽', '烤鸡胸、牛油果、藜麦与时令蔬菜',
       460, 38, 9, 320, 250, 'https://images.unsplash.com/photo-1546069901-ba9599a7e63c?auto=format&fit=crop&w=600&q=82', 1
FROM DUAL
WHERE @exists = 0;   -- 同名不同 slug 会在这一步以 1062 失败（期望行为）

UPDATE foods
   SET name = '柠檬鸡胸能量碗', category = '高蛋白 · 清爽', description = '烤鸡胸、牛油果、藜麦与时令蔬菜',
       calories = 460, protein_g = 38, fiber_g = 9,
       veggies_g = 320, water_ml = 250, image_url = 'https://images.unsplash.com/photo-1546069901-ba9599a7e63c?auto=format&fit=crop&w=600&q=82', active = 1
 WHERE slug = 'lemon-chicken';   -- 只动这一行

-- ---------------------------------------------------------------- tomato-beef
SET @exists := (SELECT COUNT(*) FROM foods WHERE slug = 'tomato-beef');

INSERT INTO foods (slug, name, category, description,
                   calories, protein_g, fiber_g, veggies_g, water_ml, image_url, active)
SELECT 'tomato-beef', '番茄牛腩盖饭', '热乎 · 饱腹', '番茄牛腩、杂粮米饭与焯拌时蔬',
       620, 32, 7, 180, 180, 'https://images.unsplash.com/photo-1512058564366-18510be2db19?auto=format&fit=crop&w=600&q=82', 1
FROM DUAL
WHERE @exists = 0;   -- 同名不同 slug 会在这一步以 1062 失败（期望行为）

UPDATE foods
   SET name = '番茄牛腩盖饭', category = '热乎 · 饱腹', description = '番茄牛腩、杂粮米饭与焯拌时蔬',
       calories = 620, protein_g = 32, fiber_g = 7,
       veggies_g = 180, water_ml = 180, image_url = 'https://images.unsplash.com/photo-1512058564366-18510be2db19?auto=format&fit=crop&w=600&q=82', active = 1
 WHERE slug = 'tomato-beef';   -- 只动这一行

-- ---------------------------------------------------------------- mushroom-soup
SET @exists := (SELECT COUNT(*) FROM foods WHERE slug = 'mushroom-soup');

INSERT INTO foods (slug, name, category, description,
                   calories, protein_g, fiber_g, veggies_g, water_ml, image_url, active)
SELECT 'mushroom-soup', '菌菇鸡汤面', '清淡 · 汤食', '清鸡汤、三种菌菇、细面与小青菜',
       430, 24, 6, 160, 450, 'https://images.unsplash.com/photo-1547592166-23ac45744acd?auto=format&fit=crop&w=600&q=82', 1
FROM DUAL
WHERE @exists = 0;   -- 同名不同 slug 会在这一步以 1062 失败（期望行为）

UPDATE foods
   SET name = '菌菇鸡汤面', category = '清淡 · 汤食', description = '清鸡汤、三种菌菇、细面与小青菜',
       calories = 430, protein_g = 24, fiber_g = 6,
       veggies_g = 160, water_ml = 450, image_url = 'https://images.unsplash.com/photo-1547592166-23ac45744acd?auto=format&fit=crop&w=600&q=82', active = 1
 WHERE slug = 'mushroom-soup';   -- 只动这一行

-- ---------------------------------------------------------------- shrimp-pasta
SET @exists := (SELECT COUNT(*) FROM foods WHERE slug = 'shrimp-pasta');

INSERT INTO foods (slug, name, category, description,
                   calories, protein_g, fiber_g, veggies_g, water_ml, image_url, active)
SELECT 'shrimp-pasta', '蒜香虾仁意面', '高蛋白 · 换口味', '虾仁、樱桃番茄与全麦意面',
       560, 35, 8, 140, 160, 'https://images.unsplash.com/photo-1563379926898-05f4575a45d8?auto=format&fit=crop&w=600&q=82', 1
FROM DUAL
WHERE @exists = 0;   -- 同名不同 slug 会在这一步以 1062 失败（期望行为）

UPDATE foods
   SET name = '蒜香虾仁意面', category = '高蛋白 · 换口味', description = '虾仁、樱桃番茄与全麦意面',
       calories = 560, protein_g = 35, fiber_g = 8,
       veggies_g = 140, water_ml = 160, image_url = 'https://images.unsplash.com/photo-1563379926898-05f4575a45d8?auto=format&fit=crop&w=600&q=82', active = 1
 WHERE slug = 'shrimp-pasta';   -- 只动这一行

-- ---------------------------------------------------------------- vegetable-rice
SET @exists := (SELECT COUNT(*) FROM foods WHERE slug = 'vegetable-rice');

INSERT INTO foods (slug, name, category, description,
                   calories, protein_g, fiber_g, veggies_g, water_ml, image_url, active)
SELECT 'vegetable-rice', '时蔬豆腐拌饭', '植物蛋白 · 多蔬菜', '香煎豆腐、烤南瓜、西兰花与杂粮饭',
       410, 22, 12, 360, 180, 'https://images.unsplash.com/photo-1512621776951-a57141f2eefd?auto=format&fit=crop&w=600&q=82', 1
FROM DUAL
WHERE @exists = 0;   -- 同名不同 slug 会在这一步以 1062 失败（期望行为）

UPDATE foods
   SET name = '时蔬豆腐拌饭', category = '植物蛋白 · 多蔬菜', description = '香煎豆腐、烤南瓜、西兰花与杂粮饭',
       calories = 410, protein_g = 22, fiber_g = 12,
       veggies_g = 360, water_ml = 180, image_url = 'https://images.unsplash.com/photo-1512621776951-a57141f2eefd?auto=format&fit=crop&w=600&q=82', active = 1
 WHERE slug = 'vegetable-rice';   -- 只动这一行

-- ---------------------------------------------------------------- pepper-chicken
SET @exists := (SELECT COUNT(*) FROM foods WHERE slug = 'pepper-chicken');

INSERT INTO foods (slug, name, category, description,
                   calories, protein_g, fiber_g, veggies_g, water_ml, image_url, active)
SELECT 'pepper-chicken', '藤椒鸡丝拌面', '微辣 · 快手', '嫩鸡丝、黄瓜丝与荞麦面',
       520, 30, 7, 140, 180, 'https://images.unsplash.com/photo-1473093295043-cdd812d0e601?auto=format&fit=crop&w=600&q=82', 1
FROM DUAL
WHERE @exists = 0;   -- 同名不同 slug 会在这一步以 1062 失败（期望行为）

UPDATE foods
   SET name = '藤椒鸡丝拌面', category = '微辣 · 快手', description = '嫩鸡丝、黄瓜丝与荞麦面',
       calories = 520, protein_g = 30, fiber_g = 7,
       veggies_g = 140, water_ml = 180, image_url = 'https://images.unsplash.com/photo-1473093295043-cdd812d0e601?auto=format&fit=crop&w=600&q=82', active = 1
 WHERE slug = 'pepper-chicken';   -- 只动这一行

-- ---------------------------------------------------------------- 校验
-- 期望：6 个 slug 全部存在，且总数恰好 6（重复执行不新增）。
-- CI/迁移 runner 必须断言输出为 seed_ok；否则判迁移失败。
SELECT IF(COUNT(*) = 6, 'seed_ok', CONCAT('seed_bad:', COUNT(*))) AS seed_assert
  FROM foods WHERE slug IN ('lemon-chicken','tomato-beef','mushroom-soup','shrimp-pasta','vegetable-rice','pepper-chicken');

-- 期望：0 行（slug 未被重复插入，说明 uk_slug 生效且守卫有效）。
SELECT slug, COUNT(*) AS dup_count
  FROM foods
 WHERE slug IN ('lemon-chicken','tomato-beef','mushroom-soup','shrimp-pasta','vegetable-rice','pepper-chicken')
 GROUP BY slug
HAVING COUNT(*) > 1;

-- 期望：0 行（6 条种子的 name 互不重复，也未与任何其他 slug 同名）。
SELECT name, COUNT(DISTINCT slug) AS slug_count
  FROM foods
 WHERE name IN ('柠檬鸡胸能量碗','番茄牛腩盖饭','菌菇鸡汤面','蒜香虾仁意面','时蔬豆腐拌饭','藤椒鸡丝拌面')
 GROUP BY name
HAVING COUNT(DISTINCT slug) > 1;

-- rollback（仅在需要回滚种子时手动执行）：
-- DELETE FROM foods WHERE slug IN ('lemon-chicken','tomato-beef','mushroom-soup','shrimp-pasta','vegetable-rice','pepper-chicken');
