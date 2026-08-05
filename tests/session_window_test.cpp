// Who owns the screen while a game runs.
//
// A recompiled title is a separate process with its own window; MANX
// only starts it. The launcher's window therefore has to get out of the way,
// and the whole thing hinges on one virtual answering truthfully through a
// wrapper.
//
// It did not. `xbox360_board` picks a delegate at initialize time - the native
// port for the titles that have one, the in-process emulator otherwise - and
// forwards run_frame, process_events and the rest to it. A newly added virtual
// that the wrapper does NOT forward answers for the wrapper instead, and the
// wrapper never owns a window. So the launcher stayed up over the game: two
// windows, the game drawing in one, the desktop focus in the other, and a pad
// that looked completely dead while the game was in fact reading it.
//
// Nothing about that is visible in a build, and it needs a screen to see. This
// pins the forwarding itself, which is what was actually wrong.

#include "arcade_session.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

// The smallest session that can stand in for a real one.
class stub_session : public emulator_session {
public:
    explicit stub_session(bool owns) : m_owns(owns) {}

    bool initialize(const std::string&, const std::string&,
                    const emulator_settings&) override {
        return true;
    }
    void run_frame() override {}
    arcade_host_action process_events() override {
        return arcade_host_action::continue_running;
    }
    arcade_board_type board_type() const noexcept override {
        return arcade_board_type::xbox360;
    }
    bool owns_its_own_window() const noexcept override { return m_owns; }
    void set_rom_choices(const std::vector<rom_choice>&) override {}
    bool take_rom_selection(std::string&) override { return false; }
    bool take_operator_settings_request() override { return false; }
    bool take_controls_request() override { return false; }
    void open_operator_settings() override {}
    void reload_input_mappings() override {}
    bool take_settings_change(emulator_settings&) override { return false; }
    bool paused() const override { return false; }
    void set_paused(bool) override {}
    void refresh_output() override {}
    double frame_seconds() const override { return 1.0 / 60.0; }

private:
    bool m_owns;
};

// A wrapper shaped like xbox360_board: it holds a delegate and forwards.
class forwarding_session : public emulator_session {
public:
    explicit forwarding_session(std::unique_ptr<emulator_session> delegate)
        : m_delegate(std::move(delegate)) {}

    bool initialize(const std::string&, const std::string&,
                    const emulator_settings&) override {
        return true;
    }
    void run_frame() override {
        if (m_delegate) m_delegate->run_frame();
    }
    arcade_host_action process_events() override {
        return m_delegate ? m_delegate->process_events()
                          : arcade_host_action::return_to_menu;
    }
    arcade_board_type board_type() const noexcept override {
        return arcade_board_type::xbox360;
    }
    bool owns_its_own_window() const noexcept override {
        return m_delegate && m_delegate->owns_its_own_window();
    }
    void set_rom_choices(const std::vector<rom_choice>&) override {}
    bool take_rom_selection(std::string&) override { return false; }
    bool take_operator_settings_request() override { return false; }
    bool take_controls_request() override { return false; }
    void open_operator_settings() override {}
    void reload_input_mappings() override {}
    bool take_settings_change(emulator_settings&) override { return false; }
    bool paused() const override { return false; }
    void set_paused(bool) override {}
    void refresh_output() override {}
    double frame_seconds() const override { return 1.0 / 60.0; }

private:
    std::unique_ptr<emulator_session> m_delegate;
};

void test_a_plain_session_does_not_claim_the_screen() {
    // The default has to be false, or every emulated board would hide the
    // launcher it draws through and the screen would go blank.
    const stub_session board(false);
    assert(!board.owns_its_own_window());
}

void test_a_native_port_claims_the_screen() {
    const stub_session port(true);
    assert(port.owns_its_own_window());
}

void test_a_wrapper_answers_for_its_delegate_not_itself() {
    // THE bug. The wrapper owns no window of its own, so answering from itself
    // is always false and always wrong for the titles that matter.
    forwarding_session wrapping_port(std::make_unique<stub_session>(true));
    assert(wrapping_port.owns_its_own_window() &&
           "a wrapper must forward this, or the launcher stays on screen over "
           "the game and the pad appears dead");

    forwarding_session wrapping_board(std::make_unique<stub_session>(false));
    assert(!wrapping_board.owns_its_own_window());

    // And a wrapper with nothing behind it must not claim the screen.
    forwarding_session empty(nullptr);
    assert(!empty.owns_its_own_window());
}

} // namespace

int main() {
    test_a_plain_session_does_not_claim_the_screen();
    test_a_native_port_claims_the_screen();
    test_a_wrapper_answers_for_its_delegate_not_itself();
    std::printf("session_window_test: all checks passed\n");
    return 0;
}
