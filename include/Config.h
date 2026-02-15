#pragma once
// ============================================================================
// Config.h -- Simple INI-style configuration loader
//
// Reads key=value pairs from a config file. Lines starting with '#' or ';'
// are comments. Section headers [Section] are ignored (flat namespace).
// All tunable constants can be overridden via config.ini.
// ============================================================================

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace msbt {

class Config {
public:
    // Load from file. Returns false if file not found (defaults used).
    bool Load(const char* path) {
        FILE* f = fopen(path, "r");
        if (!f) return false;

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            // Strip newline
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
                line[--len] = '\0';
            }

            // Skip empty lines and comments
            if (len == 0 || line[0] == '#' || line[0] == ';') continue;

            // Skip section headers
            if (line[0] == '[') continue;

            // Split on '='
            char* eq = strchr(line, '=');
            if (!eq) continue;

            *eq = '\0';
            const char* key = line;
            const char* val = eq + 1;

            // Trim leading/trailing spaces from key
            while (*key == ' ' || *key == '\t') key++;
            char* keyEnd = eq - 1;
            while (keyEnd > key && (*keyEnd == ' ' || *keyEnd == '\t')) {
                *keyEnd-- = '\0';
            }

            // Trim leading spaces from value
            while (*val == ' ' || *val == '\t') val++;

            values_[key] = val;
        }

        fclose(f);
        loaded_ = true;
        return true;
    }

    bool IsLoaded() const { return loaded_; }

    // Get string value with default
    std::string GetString(const char* key, const char* defaultVal = "") const {
        auto it = values_.find(key);
        if (it != values_.end()) return it->second;
        return defaultVal;
    }

    // Get integer value with default
    int GetInt(const char* key, int defaultVal = 0) const {
        auto it = values_.find(key);
        if (it != values_.end()) return atoi(it->second.c_str());
        return defaultVal;
    }

    // Get double value with default
    double GetDouble(const char* key, double defaultVal = 0.0) const {
        auto it = values_.find(key);
        if (it != values_.end()) return atof(it->second.c_str());
        return defaultVal;
    }

    // Get bool value with default (accepts "true", "1", "yes")
    bool GetBool(const char* key, bool defaultVal = false) const {
        auto it = values_.find(key);
        if (it == values_.end()) return defaultVal;
        const auto& v = it->second;
        return v == "true" || v == "1" || v == "yes" || v == "True" || v == "Yes";
    }

private:
    std::unordered_map<std::string, std::string> values_;
    bool loaded_ = false;
};

} // namespace msbt
