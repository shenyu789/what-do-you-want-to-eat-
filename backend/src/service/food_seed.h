// M5 种子数据（T-5.01）：6 道菜与 app.js:foods 逐字段一致。
// 迁移 SQL 与本表同源，任何改动必须同时更新 SQL 与测试。
#pragma once

#include <vector>

#include "service/food_types.h"

namespace tnycl::food {

const std::vector<Food>& builtin_foods();

}  // namespace tnycl::food
