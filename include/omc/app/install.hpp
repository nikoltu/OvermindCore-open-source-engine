#pragma once

#include <string>

namespace omc::app {

struct InstallInfo {
    bool valid = false;
    bool compatible = false;
    std::string install_root;
    std::string data_dir;
    std::string version;
    std::string build;
    std::string branch;
    std::string product;
    std::string message;
};

inline const char* supported_build() { return "98310"; }

InstallInfo probe_install(const std::string& install_root);

}
