#pragma once

#include <memory>
#include <string>
#include <vector>

namespace sc2::app::platform {

std::string config_path();
std::string get_config(const std::string& key);
bool set_config(const std::string& key, const std::string& value);

std::string pick_folder(const std::string& title);

void alert(const std::string& title, const std::string& message);

struct MapEntry {
    std::string path;
    std::string name;
    bool casc = false;
};
std::vector<MapEntry> list_maps(const std::string& root, int max_depth = 4);

class LoadingScreen {
public:
    explicit LoadingScreen(const std::string& title);
    ~LoadingScreen();
    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    void progress(float frac, const std::string& label);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
