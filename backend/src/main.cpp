#include <cstdio>
#include <exception>
#include <string>

#include "tnycl/config.h"

// T-1.01 工程骨架入口：加载并校验配置，打印摘要后退出。
// 本文件**不含业务逻辑**：网络接入（T-2.07）、账号（T-3.x）、记录（T-6.x）等
// 按 WBS 后续任务加入 backend/src/{infra,transport,service,domain,repository}。
//
// 用法：eat_server [config路径]   默认 config/server.example.toml
int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "config/server.example.toml";
    try {
        const tnycl::Config cfg = tnycl::load_config(path);
        tnycl::validate_config(cfg);
        std::printf("eat_backend 0.1.0 skeleton\n");
        std::printf("config=%s\n", path.c_str());
        std::printf("bind=%s port=%d io_threads=%d\n", cfg.server.bind.c_str(),
                    cfg.server.port, cfg.server.io_threads);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "启动失败：无法加载配置 %s：%s\n", path.c_str(), e.what());
        return 1;
    }
}
