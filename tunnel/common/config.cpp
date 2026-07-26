#include "config.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include "logger.h"

namespace tunnel {

bool Config::LoadFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        LOG_ERROR("cannot open config file: %s (%s)", filepath.c_str(), strerror(errno));
        return false;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(file, line)) {
        ++lineno;
        // 去除首尾空白
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;  // 空行
        size_t end = line.find_last_not_of(" \t\r");
        line = line.substr(start, end - start + 1);
        if (line.empty() || line[0] == '#') continue;  // 注释

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            LOG_WARN("config line %d: no '=' found, skipping: %s", lineno, line.c_str());
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // 去除 key/val 尾部的空格
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(0, 1);
        kv_[key] = val;
    }
    LOG_INFO("loaded config: %s (%zu keys)", filepath.c_str(), kv_.size());
    return true;
}

std::string Config::Get(const std::string& key, const std::string& default_val) const {
    auto it = kv_.find(key);
    return (it != kv_.end()) ? it->second : default_val;
}

bool Config::HasKey(const std::string& key) const {
    return kv_.count(key) > 0;
}

}  // namespace tunnel
