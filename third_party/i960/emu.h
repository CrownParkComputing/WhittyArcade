// Minimal framework compatibility for the standalone V60 core.
//
// The instruction implementation in this directory comes from MAME's
// BSD-3-Clause V60 device. This file supplies only the memory/cycle plumbing
// that device expects; it is not a MAME runtime or device framework.
#pragma once

#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using offs_t = uint32_t;
using device_type = int;

template <typename T, typename U>
constexpr auto BIT(T value, U bit) noexcept {
    return (value >> bit) & T(1);
}

template <typename T, typename U, typename... V>
constexpr T bitswap_value(T value, U bit, V... remaining) noexcept {
    if constexpr (sizeof...(remaining) != 0)
        return static_cast<T>((BIT(value, bit) << sizeof...(remaining)) |
                              bitswap_value(value, remaining...));
    else
        return static_cast<T>(BIT(value, bit));
}

template <unsigned Bits, typename T, typename... U>
constexpr T bitswap(T value, U... bits) noexcept {
    static_assert(sizeof...(bits) == Bits, "wrong number of bit positions");
    return bitswap_value(value, bits...);
}

constexpr int64_t mul_32x32(int32_t left, int32_t right) {
    return int64_t(left) * int64_t(right);
}
constexpr uint64_t mulu_32x32(uint32_t left, uint32_t right) {
    return uint64_t(left) * uint64_t(right);
}
constexpr uint32_t rotl_32(uint32_t value, unsigned count) {
    count &= 31;
    return count ? (value << count) | (value >> (32 - count)) : value;
}

#define WORD_ALIGNED(address) (((address) & 1U) == 0)
#define DWORD_ALIGNED(address) (((address) & 3U) == 0)
inline int32_t div_64x32_rem(int64_t dividend, int32_t divisor,
                            int32_t& remainder) {
    const int32_t quotient = static_cast<int32_t>(dividend / divisor);
    remainder = static_cast<int32_t>(
        dividend - int64_t(divisor) * quotient);
    return quotient;
}
inline uint32_t divu_64x32_rem(uint64_t dividend, uint32_t divisor,
                              uint32_t& remainder) {
    const uint32_t quotient = static_cast<uint32_t>(dividend / divisor);
    remainder = static_cast<uint32_t>(
        dividend - uint64_t(divisor) * quotient);
    return quotient;
}
inline float u2f(uint32_t value) {
    union { uint32_t integer; float real; } bits{value};
    return bits.real;
}
inline uint32_t f2u(float value) {
    union { float real; uint32_t integer; } bits{value};
    return bits.integer;
}
constexpr uint32_t swapendian_int32(uint32_t value) {
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) |
           ((value & 0xff000000U) >> 24);
}

constexpr int AS_PROGRAM = 0;
constexpr int AS_IO = 1;
constexpr int ENDIANNESS_LITTLE = 0;
constexpr int INPUT_LINE_NMI = -1;
constexpr int CLEAR_LINE = 0;
constexpr int ASSERT_LINE = 1;
constexpr int STATE_GENPC = -1;
constexpr int STATE_GENPCBASE = -2;
constexpr int STATE_GENFLAGS = -3;

#define ATTR_COLD
#define NAME(value) value
#define save_item(...)
#define DECLARE_DEVICE_TYPE(name, type) extern const device_type name;
#define DEFINE_DEVICE_TYPE(name, type, short_name, full_name) \
    const device_type name = __COUNTER__ + 1;

struct machine_config {};
class device_t {};

class address_space_config {
public:
    address_space_config(const char*, int, int data_width, int, int)
        : m_data_width(data_width) {}
    int data_width() const { return m_data_width; }

private:
    int m_data_width;
};

class address_space {
public:
    using read_callback = std::function<u8(offs_t)>;
    using write_callback = std::function<void(offs_t, u8)>;
    using read32_callback = std::function<u32(offs_t)>;
    using write32_callback = std::function<void(offs_t, u32)>;
    using write16_callback = std::function<void(offs_t, u16)>;
    using flags_callback = std::function<u16(offs_t)>;

    explicit address_space(int data_width = 16) : m_data_width(data_width) {}

    void set_callbacks(read_callback read, write_callback write,
                       flags_callback flags = {},
                       read32_callback read32 = {},
                       write32_callback write32 = {},
                       write16_callback write16 = {}) {
        m_read = std::move(read);
        m_write = std::move(write);
        m_flags = std::move(flags);
        m_read32 = std::move(read32);
        m_write32 = std::move(write32);
        m_write16 = std::move(write16);
    }

    int data_width() const { return m_data_width; }
    u8 read_byte(offs_t address) const {
        return m_read ? m_read(address) : 0xff;
    }
    u16 read_word_unaligned(offs_t address) const {
        return static_cast<u16>(read_byte(address)) |
               (static_cast<u16>(read_byte(address + 1)) << 8);
    }
    u16 read_word(offs_t address) const {
        return read_word_unaligned(address);
    }
    u32 read_dword_unaligned(offs_t address) const {
        if (m_read32) return m_read32(address);
        return static_cast<u32>(read_word_unaligned(address)) |
               (static_cast<u32>(read_word_unaligned(address + 2)) << 16);
    }
    u64 read_qword_unaligned(offs_t address) const {
        return static_cast<u64>(read_dword_unaligned(address)) |
               (static_cast<u64>(read_dword_unaligned(address + 4)) << 32);
    }
    u32 read_dword(offs_t address) const {
        return read_dword_unaligned(address);
    }
    std::pair<u8, u16> read_byte_flags(offs_t address) const {
        return {read_byte(address), flags(address)};
    }
    std::pair<u32, u16> read_dword_flags(offs_t address) const {
        return {read_dword(address), flags(address)};
    }

    void write_byte(offs_t address, u8 value) {
        if (m_write) m_write(address, value);
    }
    void write_word_unaligned(offs_t address, u16 value) {
        // A halfword store is a single bus transaction on the real machine.
        // Devices that consume one word per transaction (the Model 2 TGP
        // FIFO) lose data if it is split into byte lanes, so a bus that
        // installs the 16-bit callback sees the store whole.
        if (m_write16) {
            m_write16(address, value);
            return;
        }
        write_byte(address, static_cast<u8>(value));
        write_byte(address + 1, static_cast<u8>(value >> 8));
    }
    void write_word(offs_t address, u16 value) {
        write_word_unaligned(address, value);
    }
    void write_dword_unaligned(offs_t address, u32 value) {
        if (m_write32) {
            m_write32(address, value);
            return;
        }
        write_word_unaligned(address, static_cast<u16>(value));
        write_word_unaligned(address + 2, static_cast<u16>(value >> 16));
    }
    void write_dword(offs_t address, u32 value) {
        write_dword_unaligned(address, value);
    }
    u16 write_byte_flags(offs_t address, u8 value) {
        write_byte(address, value);
        return flags(address);
    }
    u16 write_dword_flags(offs_t address, u32 value) {
        write_dword(address, value);
        return flags(address);
    }
    void write_qword_unaligned(offs_t address, u64 value) {
        write_dword_unaligned(address, static_cast<u32>(value));
        write_dword_unaligned(address + 4, static_cast<u32>(value >> 32));
    }

    template <typename Cache>
    void cache(Cache& result) { result.set_space(this); }
    template <typename Specific>
    void specific(Specific& result) { result.set_space(this); }

private:
    int m_data_width;
    read_callback m_read;
    write_callback m_write;
    read32_callback m_read32;
    write32_callback m_write32;
    write16_callback m_write16;
    flags_callback m_flags;

    u16 flags(offs_t address) const {
        return m_flags ? m_flags(address) : 0x0001;
    }
};

template <int AddressWidth, int DataShift, int Alignment, int Endianness>
struct memory_access {
    class cache {
    public:
        void set_space(address_space* space) { m_space = space; }
        u8 read_byte(offs_t address) const { return m_space->read_byte(address); }
        u16 read_word_unaligned(offs_t address) const {
            return m_space->read_word_unaligned(address);
        }
        u32 read_dword_unaligned(offs_t address) const {
            return m_space->read_dword_unaligned(address);
        }
        u32 read_dword(offs_t address) const {
            return m_space->read_dword(address);
        }

    private:
        address_space* m_space{nullptr};
    };

    class specific {
    public:
        void set_space(address_space* space) { m_space = space; }
        u8 read_byte(offs_t address) const { return m_space->read_byte(address); }
        u16 read_word(offs_t address) const { return m_space->read_word(address); }
        u32 read_dword(offs_t address) const { return m_space->read_dword(address); }
        std::pair<u8, u16> read_byte_flags(offs_t address) const {
            return m_space->read_byte_flags(address);
        }
        std::pair<u32, u16> read_dword_flags(offs_t address) const {
            return m_space->read_dword_flags(address);
        }
        void write_byte(offs_t address, u8 value) {
            m_space->write_byte(address, value);
        }
        void write_word(offs_t address, u16 value) {
            m_space->write_word(address, value);
        }
        void write_dword(offs_t address, u32 value) {
            m_space->write_dword(address, value);
        }
        u16 write_byte_flags(offs_t address, u8 value) {
            return m_space->write_byte_flags(address, value);
        }
        u16 write_dword_flags(offs_t address, u32 value) {
            return m_space->write_dword_flags(address, value);
        }

    private:
        address_space* m_space{nullptr};
    };
};

class device_state_entry {
public:
    explicit device_state_entry(int value = 0) : m_index(value) {}
    int index() const { return m_index; }

private:
    int m_index;
};

class state_registration {
public:
    state_registration& formatstr(const char*) { return *this; }
    state_registration& callimport() { return *this; }
    state_registration& callexport() { return *this; }
    state_registration& noshow() { return *this; }
};

namespace util {
template <typename T>
constexpr T sext(T value, unsigned bits) {
    const T sign = T{1} << (bits - 1);
    const T mask = (sign << 1) - 1;
    value &= mask;
    return static_cast<T>((value ^ sign) - sign);
}

class disasm_interface {
public:
    class data_buffer {
    public:
        using reader = std::function<u8(offs_t)>;
        explicit data_buffer(reader read) : m_read(std::move(read)) {}
        u8 r8(offs_t address) const { return m_read(address); }
        u16 r16(offs_t address) const {
            return static_cast<u16>(r8(address)) |
                   (static_cast<u16>(r8(address + 1)) << 8);
        }
        u32 r32(offs_t address) const {
            return static_cast<u32>(r16(address)) |
                   (static_cast<u32>(r16(address + 2)) << 16);
        }

    private:
        reader m_read;
    };

    static constexpr offs_t SUPPORTED = 0x80000000U;
    static constexpr offs_t STEP_OUT = 0x40000000U;
    static constexpr offs_t STEP_OVER = 0x20000000U;
    static constexpr offs_t STEP_COND = 0x10000000U;
    virtual ~disasm_interface() = default;
    virtual u32 opcode_alignment() const = 0;
    virtual offs_t disassemble(std::ostream&, offs_t, const data_buffer&,
                               const data_buffer&) = 0;
};

inline const char* printf_argument(const std::string& value) {
    return value.c_str();
}

template <typename T>
inline T printf_argument(T value) {
    return value;
}

template <typename... Args>
inline void stream_format(std::ostream& output, const char* format,
                          Args... args) {
    char text[512];
    std::snprintf(text, sizeof(text), format, printf_argument(args)...);
    output << text;
}

template <typename... Args>
inline std::string string_format(const char* format, Args... args) {
    char text[512];
    std::snprintf(text, sizeof(text), format, printf_argument(args)...);
    return text;
}
} // namespace util

class device_memory_interface {
public:
    using space_config_vector =
        std::vector<std::pair<int, const address_space_config*>>;
    virtual ~device_memory_interface() = default;
    virtual space_config_vector memory_space_config() const { return {}; }
};

class cpu_device : public device_memory_interface {
public:
    cpu_device(const machine_config&, device_type, const char*, device_t*, u32)
        : m_program_space(16), m_io_space(16) {}
    virtual ~cpu_device() = default;

    virtual void device_start() {}
    virtual void device_reset() {}
    virtual u32 execute_min_cycles() const noexcept { return 1; }
    virtual u32 execute_max_cycles() const noexcept { return 1; }
    virtual bool execute_input_edge_triggered(int) const noexcept { return false; }
    virtual void execute_run() {}
    virtual void execute_set_input(int, int) {}
    virtual void state_import(const device_state_entry&) {}
    virtual void state_export(const device_state_entry&) {}
    virtual void state_string_export(const device_state_entry&, std::string&) const {}
    virtual std::unique_ptr<util::disasm_interface> create_disassembler() {
        return {};
    }

    address_space& space(int which) {
        return which == AS_PROGRAM ? m_program_space : m_io_space;
    }
    const address_space& space(int which) const {
        return which == AS_PROGRAM ? m_program_space : m_io_space;
    }
    template <typename T>
    state_registration state_add(int, const char*, T&) { return {}; }
    void set_icountptr(int&) {}
    void debugger_exception_hook(int) {}
    void debugger_instruction_hook(offs_t) {}
    int standard_irq_callback(int, offs_t) {
        return m_irq_callback ? m_irq_callback() : 0;
    }
    void set_standalone_irq_callback(std::function<int()> callback) {
        m_irq_callback = std::move(callback);
    }

private:
    address_space m_program_space;
    address_space m_io_space;
    std::function<int()> m_irq_callback;
};

template <typename... Args>
[[noreturn]] inline void fatalerror(const char* format, Args... args) {
    char message[512];
    std::snprintf(message, sizeof(message), format, args...);
    throw std::runtime_error(message);
}

template <typename... Args>
inline void logerror(const char* format, Args... args) {
    std::fprintf(stderr, format, args...);
}

template <typename... Args>
inline std::string string_format(const char* format, Args... args) {
    char value[128];
    std::snprintf(value, sizeof(value), format, args...);
    return value;
}
