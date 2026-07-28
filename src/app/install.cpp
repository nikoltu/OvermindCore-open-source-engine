#include "sc2/app/install.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "sc2/platform/file.hpp"

namespace fs = std::filesystem;

namespace sc2::app {

static std::string build_config_field(const std::string& install_root,
                                      const std::string& key) {
    std::error_code ec;
    std::string cfg = install_root + "/SC2Data/config";
    for (auto it = fs::recursive_directory_iterator(cfg, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto sz = it->file_size(ec);
        if (ec || sz == 0 || sz > 16384) continue;
        std::ifstream f(it->path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), {});
        if (text.find("build-product") == std::string::npos) continue;
        auto pos = text.find(key);
        if (pos == std::string::npos) continue;
        pos += key.size();
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '=')) ++pos;
        std::string val;
        while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r')
            val += text[pos++];
        while (!val.empty() && val.back() == ' ') val.pop_back();
        return val;
    }
    return {};
}

static std::map<std::string, std::string> parse_build_info(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string header, values;
    if (!std::getline(in, header) || !std::getline(in, values)) return out;
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        std::string cur;
        for (char c : s) {
            if (c == '|' || c == '\r') {
                v.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        v.push_back(cur);
        return v;
    };
    auto names = split(header), vals = split(values);
    for (size_t i = 0; i < names.size() && i < vals.size(); ++i) {
        std::string key = names[i].substr(0, names[i].find('!'));
        out[key] = vals[i];
    }
    return out;
}

InstallInfo probe_install(const std::string& install_root) {
    InstallInfo info;
    info.install_root = install_root;
    info.data_dir = install_root + "/SC2Data/data";

    if (!platform::file_exists(install_root)) {
        info.message = "folder does not exist";
        return info;
    }

    std::string bi = install_root + "/.build.info";
    if (!platform::file_exists(bi)) {
        info.message = "not an SC2 install (.build.info missing)";
        return info;
    }
    try {
        auto bytes = platform::read_file(bi);
        auto fields = parse_build_info(std::string(bytes.begin(), bytes.end()));
        info.version = fields.count("Version") ? fields["Version"] : "";
        info.branch = fields.count("Branch") ? fields["Branch"] : "";
        auto dot = info.version.rfind('.');
        info.build = (dot != std::string::npos) ? info.version.substr(dot + 1)
                                                : info.version;
    } catch (const std::exception&) {
        info.message = ".build.info unreadable";
        return info;
    }

    if (!platform::file_exists(info.data_dir)) {
        info.message = "SC2Data/data folder missing";
        return info;
    }
    if (platform::list_dir(info.data_dir, ".idx").empty()) {
        info.message = "no .idx files in SC2Data/data (install incomplete?)";
        return info;
    }
    if (!platform::file_exists(info.data_dir + "/data.000")) {
        info.message = "no data.### blobs in SC2Data/data";
        return info;
    }

    info.product = build_config_field(install_root, "build-product");
    if (info.product != "SC2") {
        info.message =
            "not an SC2 install (build-product '" + info.product + "')";
        return info;
    }
    if (info.build.empty())
        if (std::string bn = build_config_field(install_root, "build-name");
            bn.size() > 1 && bn[0] == 'B')
            info.build = bn.substr(1);

    info.valid = true;
    info.compatible = (info.build == supported_build());
    if (info.compatible)
        info.message = "valid SC2 install, build " + info.build + " (verified)";
    else
        info.message = "valid SC2 install, build " + info.build +
                       " (engine verified against " + supported_build() +
                       "; proceeding, formats are build-stable)";
    return info;
}

}
