#pragma once

#include <string>

#include "sc2/api/server.hpp"

namespace SC2APIProtocol {
class ResponseGameInfo;
class ResponseObservation;
class ResponseData;
}

namespace sc2::api::bridge {

struct View {
    int player = 1;
    bool revealed = false;
    bool disable_fog = false;
};

void fill_game_info(Host& host, const View& view, SC2APIProtocol::ResponseGameInfo& out);

void fill_observation(Host& host, const View& view, SC2APIProtocol::ResponseObservation& out);

bool fill_unit_type_data(Host& host, SC2APIProtocol::ResponseData& out, std::string& error);

}
