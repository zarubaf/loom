// SPDX-License-Identifier: Apache-2.0
// Minimal TOML read/write utilities for Loom
//
// Provides simple TOML file I/O for loom_manifest.toml. Supports only
// the subset needed: [section] headers and key = "value" / key = number pairs.

#pragma once

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace loom {

// Nested map: section -> key -> value (all strings)
using TomlData = std::map<std::string, std::map<std::string, std::string>>;

// Write TOML data to a file. Overwrites existing content.
inline bool toml_write(const std::string& path, const TomlData& data) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;

    for (const auto& [section, kvs] : data) {
        std::fprintf(f, "[%s]\n", section.c_str());
        for (const auto& [key, value] : kvs) {
            // Numbers (hex or decimal) are written bare, strings are quoted
            bool is_number = false;
            if (!value.empty()) {
                if (value[0] == '0' && value.size() > 1 && value[1] == 'x') {
                    is_number = true;  // hex like 0x000100
                } else if (value[0] == '-' || (value[0] >= '0' && value[0] <= '9')) {
                    is_number = true;
                    for (size_t i = 1; i < value.size(); i++) {
                        if (value[i] != '.' && (value[i] < '0' || value[i] > '9')) {
                            // Check if it looks like a timestamp with T and :
                            if (value[i] == 'T' || value[i] == ':' || value[i] == '-' || value[i] == 'Z') {
                                is_number = false;
                                break;
                            }
                        }
                    }
                }
            }

            if (is_number) {
                std::fprintf(f, "%s = %s\n", key.c_str(), value.c_str());
            } else {
                std::fprintf(f, "%s = \"%s\"\n", key.c_str(), value.c_str());
            }
        }
        std::fprintf(f, "\n");
    }

    std::fclose(f);
    return true;
}

// Append sections to an existing TOML file
inline bool toml_append(const std::string& path, const TomlData& data) {
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return false;

    for (const auto& [section, kvs] : data) {
        std::fprintf(f, "[%s]\n", section.c_str());
        for (const auto& [key, value] : kvs) {
            bool is_number = false;
            if (!value.empty()) {
                if (value[0] == '0' && value.size() > 1 && value[1] == 'x') {
                    is_number = true;
                } else if (value[0] == '-' || (value[0] >= '0' && value[0] <= '9')) {
                    is_number = true;
                    for (size_t i = 1; i < value.size(); i++) {
                        if (value[i] != '.' && (value[i] < '0' || value[i] > '9')) {
                            if (value[i] == 'T' || value[i] == ':' || value[i] == '-' || value[i] == 'Z') {
                                is_number = false;
                                break;
                            }
                        }
                    }
                }
            }

            if (is_number) {
                std::fprintf(f, "%s = %s\n", key.c_str(), value.c_str());
            } else {
                std::fprintf(f, "%s = \"%s\"\n", key.c_str(), value.c_str());
            }
        }
        std::fprintf(f, "\n");
    }

    std::fclose(f);
    return true;
}

// Read a TOML file into nested map
inline TomlData toml_read(const std::string& path) {
    TomlData data;
    std::ifstream f(path);
    if (!f.is_open()) return data;

    std::string current_section;
    std::string line;
    while (std::getline(f, line)) {
        // Strip comments
        auto hash_pos = line.find('#');
        if (hash_pos != std::string::npos)
            line = line.substr(0, hash_pos);

        // Trim whitespace
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        auto end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);

        if (line.empty()) continue;

        // Section header
        if (line.front() == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            continue;
        }

        // Key = value
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Trim key
        auto ks = key.find_first_not_of(" \t");
        auto ke = key.find_last_not_of(" \t");
        if (ks != std::string::npos)
            key = key.substr(ks, ke - ks + 1);

        // Trim value
        auto vs = value.find_first_not_of(" \t");
        auto ve = value.find_last_not_of(" \t");
        if (vs != std::string::npos)
            value = value.substr(vs, ve - vs + 1);

        // Strip quotes
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        data[current_section][key] = value;
    }

    return data;
}

} // namespace loom
