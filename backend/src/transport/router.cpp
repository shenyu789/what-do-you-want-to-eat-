// router.cpp —— HTTP 路由实现（段匹配，支持 {param} 路径参数）
#include "tnycl/http_router.h"

#include <sstream>
#include <vector>

#include "tnycl/status.h"

namespace tnycl {
namespace {

std::vector<std::string> split_segments(const std::string& path) {
  std::vector<std::string> out;
  std::stringstream ss(path);
  std::string seg;
  while (std::getline(ss, seg, '/')) {
    if (!seg.empty()) out.push_back(seg);
  }
  return out;
}

bool is_param(const std::string& s) {
  return s.size() >= 2 && s.front() == '{' && s.back() == '}';
}

std::string param_name(const std::string& s) {
  return s.substr(1, s.size() - 2);
}

}  // namespace

std::unordered_map<std::string, std::string> HttpRequest::query() const {
  std::unordered_map<std::string, std::string> out;
  std::stringstream ss(raw_query);
  std::string item;
  while (std::getline(ss, item, '&')) {
    if (item.empty()) continue;
    auto eq = item.find('=');
    if (eq == std::string::npos)
      out[item] = "";
    else
      out[item.substr(0, eq)] = item.substr(eq + 1);
  }
  return out;
}

namespace {

class Router : public IHttpRouter {
  struct Route {
    std::string method;
    std::string pattern;
    std::vector<std::string> segments;
    Handler handler;
  };
  std::vector<Route> routes_;

  void add(std::string_view method, std::string_view pattern, Handler h) {
    std::string m(method);
    std::string p(pattern);
    for (const auto& r : routes_)
      if (r.method == m && r.pattern == p)
        throw AppError(ErrorCode::kInternalError,
                       "重复注册路由: " + m + " " + p);
    Route r;
    r.method = m;
    r.pattern = p;
    r.segments = split_segments(p);
    r.handler = std::move(h);
    routes_.push_back(std::move(r));
  }

 public:
  void get(std::string_view pattern, Handler h) override { add("GET", pattern, std::move(h)); }
  void post(std::string_view pattern, Handler h) override { add("POST", pattern, std::move(h)); }
  void put(std::string_view pattern, Handler h) override { add("PUT", pattern, std::move(h)); }
  void del(std::string_view pattern, Handler h) override { add("DELETE", pattern, std::move(h)); }

  bool match(const std::string& method, const std::string& path,
             HttpRequest& req, Handler& out) const override {
    auto segs = split_segments(path);
    for (const auto& r : routes_) {
      if (r.method != method) continue;
      if (r.segments.size() != segs.size()) continue;
      std::unordered_map<std::string, std::string> params;
      bool ok = true;
      for (std::size_t i = 0; i < segs.size(); ++i) {
        if (is_param(r.segments[i])) {
          params[param_name(r.segments[i])] = segs[i];
        } else if (r.segments[i] != segs[i]) {
          ok = false;
          break;
        }
      }
      if (ok) {
        req.path_params = std::move(params);
        out = r.handler;
        return true;
      }
    }
    return false;
  }
};

}  // namespace

std::unique_ptr<IHttpRouter> make_http_router() {
  return std::make_unique<Router>();
}

}  // namespace tnycl
