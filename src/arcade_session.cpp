// Central board-session registration.

#include "arcade_session_internal.h"

#include <stdexcept>
#include <utility>

std::unique_ptr<emulator_session> create_emulator_session(
    arcade_board_type board,
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet_state) {
    if (!video)
        throw std::invalid_argument("An arcade session requires video output");
    if (!cabinet_state)
        cabinet_state = std::make_shared<arcade_cabinet_state>();

    switch (board) {
    case arcade_board_type::system22:
        return make_system22_session(std::move(video),
                                     std::move(cabinet_state));
    case arcade_board_type::system246:
        return make_system246_session(std::move(video),
                                      std::move(cabinet_state));
    case arcade_board_type::model1:
        return make_model1_session(std::move(video),
                                   std::move(cabinet_state));
    case arcade_board_type::model2:
        return make_model2_session(std::move(video),
                                   std::move(cabinet_state));
    case arcade_board_type::phoenix:
    case arcade_board_type::galaxian:
        return make_galaxian_session(board, std::move(video),
                                     std::move(cabinet_state));
    case arcade_board_type::system16b:
        return make_system16b_session(std::move(video),
                                    std::move(cabinet_state));
    case arcade_board_type::capcom_gng:
        return make_gng_session(std::move(video),
                                std::move(cabinet_state));
    case arcade_board_type::namco_galaga:
        return make_namco_galaga_session(std::move(video),
                                         std::move(cabinet_state));
    case arcade_board_type::namco_system1:
        return make_namco_system1_session(std::move(video),
                                          std::move(cabinet_state));
    case arcade_board_type::taito_z:
        return make_taitoz_session(std::move(video),
                                   std::move(cabinet_state));
    case arcade_board_type::midway:
        return make_midway_session(std::move(video),
                                   std::move(cabinet_state));
    case arcade_board_type::game_plugin:
        return make_plugin_session(std::move(video),
                                   std::move(cabinet_state));
    }
    throw std::invalid_argument("Unknown arcade board type");
}
