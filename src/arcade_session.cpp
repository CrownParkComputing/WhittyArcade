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
    case arcade_board_type::model1:
        return make_model1_session(std::move(video),
                                   std::move(cabinet_state));
    case arcade_board_type::model2:
        return make_model2_session(std::move(video),
                                   std::move(cabinet_state));
    case arcade_board_type::phoenix:
    case arcade_board_type::mooncrst:
        return make_galaxian_session(board, std::move(video),
                                     std::move(cabinet_state));
    case arcade_board_type::shinobi:
        return make_shinobi_session(std::move(video),
                                    std::move(cabinet_state));
    }
    throw std::invalid_argument("Unknown arcade board type");
}
