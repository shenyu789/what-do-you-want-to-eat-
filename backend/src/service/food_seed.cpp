#include "service/food_seed.h"

namespace tnycl::food {

const std::vector<Food>& builtin_foods() {
  static const std::vector<Food> foods = {
      Food{1, "lemon-chicken", "柠檬鸡胸能量碗", "高蛋白 · 清爽",
           "烤鸡胸、牛油果、藜麦与时令蔬菜", 460, 38, 9, 320, 250,
           "https://images.unsplash.com/photo-1546069901-ba9599a7e63c?auto=format&fit=crop&w=600&q=82",
           true},
      Food{2, "tomato-beef", "番茄牛腩盖饭", "热乎 · 饱腹", "番茄牛腩、杂粮米饭与焯拌时蔬", 620, 32, 7,
           180, 180,
           "https://images.unsplash.com/photo-1512058564366-18510be2db19?auto=format&fit=crop&w=600&q=82",
           true},
      Food{3, "mushroom-soup", "菌菇鸡汤面", "清淡 · 汤食", "清鸡汤、三种菌菇、细面与小青菜", 430, 24, 6,
           160, 450,
           "https://images.unsplash.com/photo-1547592166-23ac45744acd?auto=format&fit=crop&w=600&q=82",
           true},
      Food{4, "shrimp-pasta", "蒜香虾仁意面", "高蛋白 · 换口味", "虾仁、樱桃番茄与全麦意面", 560, 35, 8,
           140, 160,
           "https://images.unsplash.com/photo-1563379926898-05f4575a45d8?auto=format&fit=crop&w=600&q=82",
           true},
      Food{5, "vegetable-rice", "时蔬豆腐拌饭", "植物蛋白 · 多蔬菜", "香煎豆腐、烤南瓜、西兰花与杂粮饭",
           410, 22, 12, 360, 180,
           "https://images.unsplash.com/photo-1512621776951-a57141f2eefd?auto=format&fit=crop&w=600&q=82",
           true},
      Food{6, "pepper-chicken", "藤椒鸡丝拌面", "微辣 · 快手", "嫩鸡丝、黄瓜丝与荞麦面", 520, 30, 7, 140,
           180,
           "https://images.unsplash.com/photo-1473093295043-cdd812d0e601?auto=format&fit=crop&w=600&q=82",
           true},
  };
  return foods;
}

}  // namespace tnycl::food
