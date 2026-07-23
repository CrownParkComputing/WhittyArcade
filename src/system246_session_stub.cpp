#include "arcade_session_internal.h"

#include <cstdio>
#include <utility>

namespace {

class unavailable_system246_session final : public video_emulator_session {
public:
    unavailable_system246_session(
            std::shared_ptr<arcade_video_worker> video,
            std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::system246,
                                 std::move(video), std::move(cabinet)) {}

    bool initialize(const std::string&, const std::string&,
                    const emulator_settings&) override {
        std::fprintf(stderr,
                     "This build was configured with "
                     "WHITTY_ENABLE_SYSTEM246=OFF; rrvac cannot start.\n");
        return false;
    }
    void run_frame() override {}
    void reload_input_mappings() override {}
    double frame_seconds() const override { return 1.0 / 60.0; }
};

} // namespace

std::unique_ptr<emulator_session> make_system246_session(
        std::shared_ptr<arcade_video_worker> video,
        std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<unavailable_system246_session>(
        std::move(video), std::move(cabinet));
}
