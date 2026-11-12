#include "service/food_types.h"

namespace tnycl::food {

bool is_valid_slug(std::string_view slug) {
  if (slug.empty() || slug.size() > 32) {
    return false;
  }
  if (slug.front() == '-' || slug.back() == '-') {
    return false;
  }
  for (const char ch : slug) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

FoodView to_view(const Food& food) {
  FoodView view;
  view.slug = food.slug;
  view.name = food.name;
  view.category = food.category;
  view.description = food.description;
  view.calories = food.calories;
  view.protein_g = food.protein_g;
  view.fiber_g = food.fiber_g;
  view.veggies_g = food.veggies_g;
  view.water_ml = food.water_ml;
  view.image_url = food.image_url;
  return view;
}

}  // namespace tnycl::food

