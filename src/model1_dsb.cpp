#include "model1_dsb.h"

#include "z80.h"

#include <mpg123.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr std::size_t uart_capacity = 32;
constexpr std::size_t decode_frames = 1152;
constexpr int native_rate = 32000;
}

struct model1_dsb::implementation {
    ~implementation() {
        if (decoder) mpg123_delete(decoder);
        if (mpg123_ready) mpg123_exit();
    }

    bool initialize(const std::vector<uint8_t>& cpu_image,
                    const std::vector<uint8_t>& mpeg_image) {
        cpu_rom = cpu_image;
        mpeg_rom = mpeg_image;
        if (cpu_rom.size() != 0x20000 || mpeg_rom.empty()) return false;

        if (mpg123_init() != MPG123_OK) return false;
        mpg123_ready = true;
        int error = MPG123_OK;
        decoder = mpg123_new(nullptr, &error);
        if (!decoder) {
            std::fprintf(stderr, "Model 1 DSB: mpg123 initialization failed: %s\n",
                         mpg123_plain_strerror(error));
            return false;
        }
        mpg123_format_none(decoder);
        if (mpg123_format(decoder, native_rate, MPG123_STEREO,
                          MPG123_ENC_SIGNED_16) != MPG123_OK)
            return false;
        valid = true;
        reset();
        return true;
    }

    void reset() {
        ram.fill(0);
        uart.fill(0);
        uart_read = uart_write = uart_count = 0;
        uart_overrun = false;
        start_latch = end_latch = 0;
        play_start = play_end = loop_start = loop_end = 0;
        stream_start = stream_end = 0;
        state = 0;
        volume = 0x7f;
        pan = 0;
        decode_cursor = decode_available = 0;
        source_phase = 0;
        source_primed = false;
        total_clocks = total_bytes = triggers = 0;
        pins = z80_init(&cpu);
        close_stream();
    }

    void close_stream() {
        if (decoder_open && decoder) mpg123_close(decoder);
        decoder_open = false;
        decode_cursor = decode_available = 0;
    }

    void receive_uart(uint8_t data) {
        if (uart_count == uart_capacity) {
            uart_overrun = true;
            return;
        }
        uart[uart_write] = data;
        uart_write = (uart_write + 1) % uart_capacity;
        ++uart_count;
        ++total_bytes;
        if (std::getenv("MODEL1_DSB_TRACE"))
            std::fprintf(stderr, "Model 1 DSB UART: %02x\n", data);
    }

    uint8_t uart_data() {
        if (!uart_count) return 0;
        const uint8_t result = uart[uart_read];
        uart_read = (uart_read + 1) % uart_capacity;
        --uart_count;
        return result;
    }

    uint8_t uart_status() const {
        uint8_t result = 0x05; // TXRDY | TXEMPTY
        if (uart_count) result |= 0x02; // RXRDY
        if (uart_overrun) result |= 0x10;
        return result;
    }

    uint32_t current_position() const {
        if (!decoder_open || !decoder) return stream_start;
        const int64_t consumed = mpg123_tell_stream64(decoder);
        return consumed < 0 ? stream_start : static_cast<uint32_t>(
            std::min<uint64_t>((stream_sync_bit >> 3) + consumed,
                               mpeg_rom.size()));
    }

    unsigned bit_at(uint64_t position) const {
        if (position >= uint64_t{mpeg_rom.size()} * 8) return 0;
        return (mpeg_rom[position >> 3] >> (7 - (position & 7))) & 1;
    }

    bool align_mpeg_stream(uint32_t begin, uint32_t end) {
        const uint64_t first = uint64_t{begin} * 8;
        const uint64_t limit = uint64_t{end} * 8;
        if (limit - first < 15) return false;
        uint64_t sync = first;
        bool found = false;
        unsigned marker = 0;
        for (unsigned bit = 0; bit < 12; ++bit)
            marker = (marker << 1) | bit_at(sync + bit);
        for (; sync + 15 <= limit; ++sync) {
            const unsigned variant = (bit_at(sync + 12) << 2) |
                                     (bit_at(sync + 13) << 1) |
                                      bit_at(sync + 14);
            if (marker == 0xfff && (variant == 2 || variant == 6)) {
                found = true;
                break;
            }
            marker = ((marker << 1) | bit_at(sync + 12)) & 0xfff;
        }
        if (!found) return false;

        packed_stream.assign(static_cast<std::size_t>((limit - sync + 7) / 8),
                             0);
        const unsigned shift = sync & 7;
        const std::size_t source_byte = static_cast<std::size_t>(sync >> 3);
        for (std::size_t index = 0; index < packed_stream.size(); ++index) {
            const std::size_t source = source_byte + index;
            const unsigned upper = source < mpeg_rom.size() ?
                mpeg_rom[source] : 0;
            if (!shift) {
                packed_stream[index] = static_cast<uint8_t>(upper);
            } else {
                const unsigned lower = source + 1 < mpeg_rom.size() ?
                    mpeg_rom[source + 1] : 0;
                packed_stream[index] = static_cast<uint8_t>(
                    (upper << shift) | (lower >> (8 - shift)));
            }
        }
        stream_sync_bit = sync;
        return true;
    }

    void write_start(unsigned offset, uint8_t data) {
        const unsigned shift = (2 - offset) * 8;
        start_latch = (start_latch & ~(uint32_t{0xff} << shift)) |
                      (uint32_t{data} << shift);
        if (offset != 2) return;
        if (state == 0) play_start = start_latch;
        else loop_start = start_latch;
    }

    void write_end(unsigned offset, uint8_t data) {
        const unsigned shift = (2 - offset) * 8;
        end_latch = (end_latch & ~(uint32_t{0xff} << shift)) |
                    (uint32_t{data} << shift);
        if (offset != 2) return;
        if (state == 0) play_end = end_latch;
        else loop_end = end_latch;
    }

    bool open_stream(uint32_t begin, uint32_t end) {
        close_stream();
        begin = std::min<uint32_t>(begin, mpeg_rom.size());
        end = std::min<uint32_t>(end, mpeg_rom.size());
        if (end <= begin || !decoder) {
            if (std::getenv("MODEL1_DSB_TRACE"))
                std::fprintf(stderr,
                             "Model 1 DSB rejected stream %06x-%06x\n",
                             begin, end);
            return false;
        }
        if (!align_mpeg_stream(begin, end)) {
            if (std::getenv("MODEL1_DSB_TRACE"))
                std::fprintf(stderr,
                             "Model 1 DSB found no Layer-II sync in "
                             "%06x-%06x\n", begin, end);
            return false;
        }
        if (mpg123_open_feed(decoder) != MPG123_OK) return false;
        decoder_open = true;
        stream_start = begin;
        stream_end = end;
        const int fed = mpg123_feed(decoder, packed_stream.data(),
                                    packed_stream.size());
        if (fed != MPG123_OK) {
            close_stream();
            return false;
        }
        return true;
    }

    void trigger(uint8_t data) {
        ++triggers;
        state = data <= 2 ? data : 0;
        if (std::getenv("MODEL1_DSB_TRACE"))
            std::fprintf(stderr,
                         "Model 1 DSB trigger=%u start=%06x end=%06x "
                         "loop=%06x-%06x vol=%u pan=%u\n",
                         state, play_start, play_end, loop_start, loop_end,
                         volume, pan);
        if (state == 0) {
            close_stream();
            return;
        }
        loop_start = loop_end = 0;
        if (!open_stream(play_start, play_end)) state = 0;
    }

    uint8_t io_read(uint8_t port) {
        if (port >= 0xe2 && port <= 0xe4) {
            const unsigned shift = (0xe4 - port) * 8;
            return static_cast<uint8_t>(current_position() >> shift);
        }
        if (port == 0xf0) return uart_data();
        if (port == 0xf1) return uart_status();
        return 0xff;
    }

    void io_write(uint8_t port, uint8_t data) {
        if (port == 0xe0) trigger(data);
        else if (port >= 0xe2 && port <= 0xe4)
            write_start(port - 0xe2, data);
        else if (port >= 0xe5 && port <= 0xe7)
            write_end(port - 0xe5, data);
        else if (port == 0xe8) volume = 0x7f - (data & 0x7f);
        else if (port == 0xe9) pan = data & 3;
        else if (port == 0xf1 && (data & 0x10)) uart_overrun = false;
        // Port F0 transmits toward the main audio board; Star Wars does not
        // use that return direction.
    }

    uint8_t memory_read(uint16_t address) const {
        if (address < 0x8000) return cpu_rom[address];
        return ram[address - 0x8000];
    }

    void memory_write(uint16_t address, uint8_t data) {
        if (address >= 0x8000) ram[address - 0x8000] = data;
    }

    void execute(int clocks) {
        if (!valid || clocks <= 0) return;
        for (int clock = 0; clock < clocks; ++clock) {
            if (uart_count) pins |= Z80_INT;
            else pins &= ~Z80_INT;
            pins = z80_tick(&cpu, pins);
            if (pins & Z80_MREQ) {
                const uint16_t address = Z80_GET_ADDR(pins);
                if (pins & Z80_RD) {
                    const uint8_t data = memory_read(address);
                    Z80_SET_DATA(pins, data);
                } else if (pins & Z80_WR) {
                    memory_write(address, Z80_GET_DATA(pins));
                }
            } else if ((pins & (Z80_IORQ | Z80_M1)) ==
                       (Z80_IORQ | Z80_M1)) {
                Z80_SET_DATA(pins, 0xff);
            } else if (pins & Z80_IORQ) {
                const uint8_t port = static_cast<uint8_t>(Z80_GET_ADDR(pins));
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, io_read(port));
                } else if (pins & Z80_WR) {
                    io_write(port, Z80_GET_DATA(pins));
                }
            }
        }
        total_clocks += clocks;
    }

    bool advance_segment() {
        if (state != 2) {
            state = 0;
            close_stream();
            return false;
        }
        const uint32_t begin = loop_start ? loop_start : play_start;
        const uint32_t end = loop_end ? loop_end : play_end;
        if (begin == stream_start && end == stream_end && end <= begin) {
            state = 0;
            close_stream();
            return false;
        }
        if (!open_stream(begin, end)) {
            state = 0;
            return false;
        }
        return true;
    }

    bool refill_decode_buffer() {
        decode_cursor = decode_available = 0;
        // A corrupt loop marker must not spin forever inside the audio worker.
        // Four attempts cover an initial segment plus a newly latched loop.
        for (unsigned attempt = 0; attempt < 4 && state && decoder_open;
             ++attempt) {
            std::size_t bytes = 0;
            const int result = mpg123_read(
                decoder, reinterpret_cast<unsigned char*>(decode_buffer.data()),
                decode_buffer.size() * sizeof(int16_t), &bytes);
            if (bytes) {
                decode_available = bytes / (sizeof(int16_t) * 2);
                return decode_available != 0;
            }
            if (result == MPG123_NEW_FORMAT) {
                long rate = 0;
                int channels = 0;
                int encoding = 0;
                mpg123_getformat(decoder, &rate, &channels, &encoding);
                continue;
            }
            if (result == MPG123_NEED_MORE || result == MPG123_DONE ||
                result != MPG123_OK) {
                if (!advance_segment()) return false;
                continue;
            }
        }
        state = 0;
        close_stream();
        return false;
    }

    std::array<int32_t, 2> next_source_frame() {
        if (decode_cursor >= decode_available && !refill_decode_buffer())
            return {0, 0};
        const int16_t left = decode_buffer[decode_cursor * 2];
        const int16_t right = decode_buffer[decode_cursor * 2 + 1];
        ++decode_cursor;
        const int gain = std::clamp<int>(volume, 0, 0x7f);
        if (pan == 1) return {left * gain / 0x7f, left * gain / 0x7f};
        if (pan == 2) return {right * gain / 0x7f, right * gain / 0x7f};
        if (pan == 3) return {0, 0};
        return {left * gain / 0x7f, right * gain / 0x7f};
    }

    void render(int32_t* output, std::size_t frames, int output_rate,
                int gain_percent) {
        if (!valid || !output || !frames || output_rate <= 0) return;
        const uint64_t step = (uint64_t{native_rate} << 32) / output_rate;
        if (!source_primed) {
            source_last = next_source_frame();
            source_next = next_source_frame();
            source_primed = true;
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            while (source_phase >= (uint64_t{1} << 32)) {
                source_phase -= uint64_t{1} << 32;
                source_last = source_next;
                source_next = next_source_frame();
            }
            const uint32_t fraction = static_cast<uint32_t>(source_phase >> 16);
            for (unsigned channel = 0; channel < 2; ++channel) {
                const int32_t sample = source_last[channel] +
                    static_cast<int32_t>((int64_t{source_next[channel] -
                        source_last[channel]} * fraction) >> 16);
                output[frame * 2 + channel] += sample * gain_percent / 100;
            }
            source_phase += step;
        }
    }

    bool valid{};
    bool mpg123_ready{};
    bool decoder_open{};
    mpg123_handle* decoder{};
    std::vector<uint8_t> cpu_rom;
    std::vector<uint8_t> mpeg_rom;
    std::vector<uint8_t> packed_stream;
    uint64_t stream_sync_bit{};
    std::array<uint8_t, 0x8000> ram{};
    z80_t cpu{};
    uint64_t pins{};
    std::array<uint8_t, uart_capacity> uart{};
    std::size_t uart_read{};
    std::size_t uart_write{};
    std::size_t uart_count{};
    bool uart_overrun{};
    uint32_t start_latch{};
    uint32_t end_latch{};
    uint32_t play_start{};
    uint32_t play_end{};
    uint32_t loop_start{};
    uint32_t loop_end{};
    uint32_t stream_start{};
    uint32_t stream_end{};
    uint8_t state{};
    uint8_t volume{0x7f};
    uint8_t pan{};
    std::array<int16_t, decode_frames * 2> decode_buffer{};
    std::size_t decode_cursor{};
    std::size_t decode_available{};
    uint64_t source_phase{};
    bool source_primed{};
    std::array<int32_t, 2> source_last{};
    std::array<int32_t, 2> source_next{};
    uint64_t total_clocks{};
    uint64_t total_bytes{};
    uint64_t triggers{};
};

model1_dsb::model1_dsb() : m_impl(std::make_unique<implementation>()) {}
model1_dsb::~model1_dsb() = default;

bool model1_dsb::initialize(const std::vector<uint8_t>& cpu_rom,
                            const std::vector<uint8_t>& mpeg_rom) {
    return m_impl->initialize(cpu_rom, mpeg_rom);
}
void model1_dsb::reset() { m_impl->reset(); }
void model1_dsb::receive_uart(uint8_t data) { m_impl->receive_uart(data); }
void model1_dsb::execute(int clocks) { m_impl->execute(clocks); }
void model1_dsb::render(int32_t* stereo, std::size_t frames, int output_rate,
                        int gain_percent) {
    m_impl->render(stereo, frames, output_rate, gain_percent);
}
bool model1_dsb::active() const { return m_impl->valid; }
uint16_t model1_dsb::program_counter() const { return m_impl->cpu.pc; }
uint64_t model1_dsb::executed_clocks() const { return m_impl->total_clocks; }
uint64_t model1_dsb::received_bytes() const { return m_impl->total_bytes; }
uint64_t model1_dsb::trigger_count() const { return m_impl->triggers; }
bool model1_dsb::playing() const { return m_impl->state != 0; }
