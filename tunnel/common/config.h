#ifndef TUNNEL_COMMON_CONFIG_H_
#define TUNNEL_COMMON_CONFIG_H_

#include <string>
#include <unordered_map>

namespace tunnel {

// 简单 key=value 配置文件解析器。
// 支持注释 (# 开头) 和空行。
class Config {
public:
    // 从文件加载。成功返回 true。
    bool LoadFromFile(const std::string& filepath);

    // 获取值, 不存在返回空字符串。
    std::string Get(const std::string& key, const std::string& default_val = "") const;
    bool HasKey(const std::string& key) const;

private:
    std::unordered_map<std::string, std::string> kv_;
};

}  // namespace tunnel

#endif  // TUNNEL_COMMON_CONFIG_H_
