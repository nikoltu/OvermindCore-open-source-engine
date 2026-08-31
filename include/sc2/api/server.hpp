#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sc2/app/sandbox_game.hpp"

namespace sc2::app {
class MeleeGame;
}
namespace sc2::formats {
struct LoadedMap;
}
namespace sc2::data {
class Catalog;
}

namespace sc2::api {

class Host {
public:
    virtual ~Host() = default;

    virtual bool create_match(std::vector<std::uint8_t> map_bytes, app::Race p0, app::Race p1,
                              std::string& error) = 0;
    virtual void leave_match() = 0;
    virtual app::MeleeGame* match() = 0;
    virtual const formats::LoadedMap* map() = 0;
    virtual std::string map_name() const = 0;
    virtual const data::Catalog* catalog() const = 0;
    virtual bool read_map_file(const std::string& path, std::vector<std::uint8_t>& bytes,
                               std::string& error) = 0;

    virtual int step(int ticks) = 0;
    virtual void set_realtime(bool on) = 0;

    virtual std::string game_version() const = 0;
    virtual std::uint32_t data_build() const = 0;
    virtual std::vector<std::string> mod_names() const = 0;
    virtual std::string truth_data_dir() const = 0;
};

class Dispatcher {
public:
    bool handle(Host& host, const std::vector<std::uint8_t>& request,
                std::vector<std::uint8_t>& response);
    void reset();

private:
    int status_ = 0;
    int view_player_ = 1;
    bool view_revealed_ = false;
    bool realtime_ = false;
};

class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(int port, std::string& error);
    void stop();
    bool pump(Host& host);
    int port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

inline constexpr int kDefaultPort = 8267;

}
