// tnycl/http_router.h —— HTTP 路由与请求/响应封装（W2 冻结契约）
//
// 语义约定：
//  - 传输层工作在「字节」层面：body 为原始字符串，JSON 编解码由 Service 层负责（nlohmann）。
//  - 路由为自研前缀树（< 40 条路由）；支持路径参数 `{name}`（如 /v1/foods/{slug}）。
//  - 匹配失败由 transport 返回 404/1003；方法不匹配返回 405/1004。
//  - GET 一律无副作用；写接口的副作用见 07 §4。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tnycl {

struct HttpRequest {
  std::string method;              // 大写：GET/POST/PUT/DELETE
  std::string path;                // 不含 query，如 /v1/meals
  std::string raw_query;           // 如 date=2026-11-12&limit=20
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  // 路由匹配后填充的路径参数（如 {slug} → "lemon-chicken"）。
  std::unordered_map<std::string, std::string> path_params;

  // 便捷：query 解析为键值对（重复键取最后一次）。
  std::unordered_map<std::string, std::string> query() const;
};

struct HttpResponse {
  int status = 200;  // HTTP 状态码
  std::unordered_map<std::string, std::string> headers;
  std::string body;

  void set_content_type_json() { headers["Content-Type"] = "application/json; charset=utf-8"; }
  void set_connection_close() { headers["Connection"] = "close"; }
};

using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

class IHttpRouter {
 public:
  virtual ~IHttpRouter() = default;

  // 注册路由。pattern 支持 `{param}` 路径参数。重复注册同一 (method, pattern) 抛 AppError。
  virtual void get(std::string_view pattern, Handler h) = 0;
  virtual void post(std::string_view pattern, Handler h) = 0;
  virtual void put(std::string_view pattern, Handler h) = 0;
  virtual void del(std::string_view pattern, Handler h) = 0;

  // 路由分发。命中返回 true 并填充 req.path_params + 设置 out handler；
  // 未命中返回 false（transport 据此返回 404/1003）。
  virtual bool match(const std::string& method, const std::string& path,
                     HttpRequest& req, Handler& out) const = 0;
};

// 工厂：构造默认路由实现（段匹配，等价于前缀树语义；路由数 < 40，线性扫描可接受）。
std::unique_ptr<IHttpRouter> make_http_router();

}  // namespace tnycl
