#include "omc/app/install.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "omc/app/install_ids.hpp"
#include "omc/platform/file.hpp"

namespace fs = std::filesystem;

namespace omc::app {

static std::string build_config_field(const std::string& install_root,
                                      const std::string& key) {
    std::error_code ec;
    std::string cfg = install_root + "/" + install_ids::kConfigDir;
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
    info.data_dir = install_root + "/" + install_ids::kDataDir;

    if (!platform::file_exists(install_root)) {
        info.message = "folder does not exist";
        return info;
    }

    std::string bi = install_root + "/" + install_ids::kBuildInfoFile;
    if (!platform::file_exists(bi)) {
        info.message = "not a game installation (.build.info missing)";
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
        info.message = std::string(install_ids::kDataDir) + " folder missing";
        return info;
    }
    if (platform::list_dir(info.data_dir, ".idx").empty()) {
        info.message = std::string("no .idx files in ") + install_ids::kDataDir + " (install incomplete?)";
        return info;
    }
    if (!platform::file_exists(info.data_dir + "/data.000")) {
        info.message = std::string("no data.### blobs in ") + install_ids::kDataDir;
        return info;
    }

    info.product = build_config_field(install_root, "build-product");
    if (info.product != install_ids::kBuildProduct) {
        info.message =
            "not a game installation (build-product '" + info.product + "')";
        return info;
    }
    if (info.build.empty())
        if (std::string bn = build_config_field(install_root, "build-name");
            bn.size() > 1 && bn[0] == 'B')
            info.build = bn.substr(1);

    info.valid = true;
    info.compatible = (info.build == supported_build());
    if (info.compatible)
        info.message = "valid game installation, build " + info.build + " (verified)";
    else
        info.message = "valid game installation, build " + info.build +
                       " (engine verified against " + supported_build() +
                       "; proceeding, formats are build-stable)";
    return info;
}

}
