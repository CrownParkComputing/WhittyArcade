// Multi-instance Motorola 68000 CPU wrapper (Moira core).
//
// Unlike Musashi -- whose callbacks and register file are process-global,
// preventing two 68000s from existing in one emulator -- every m68000_cpu
// is a fully independent object with its own bus binding. Boards with
// dual 68000s (e.g. main + sound CPU) simply create two instances.
#pragma once

#include <cstdint>
#include <memory>

// 16-bit big-endian bus seen by one 68000 instance. 32-bit accesses are
// decomposed by the core into 16-bit bus cycles, as on real hardware.
class m68000_bus {
public:
    virtual ~m68000_bus() = default;
    virtual uint8_t read8(uint32_t address) = 0;
    virtual uint16_t read16(uint32_t address) = 0;
    virtual void write8(uint32_t address, uint8_t value) = 0;
    virtual void write16(uint32_t address, uint16_t value) = 0;
};

class m68000_cpu final {
public:
    explicit m68000_cpu(m68000_bus& bus);
    ~m68000_cpu();

    m68000_cpu(const m68000_cpu&) = delete;
    m68000_cpu& operator=(const m68000_cpu&) = delete;

    // Hard reset: fetches SSP and PC from bus addresses 0 and 4.
    void reset();
    // Runs for at least `cycles` clock cycles; returns the elapsed count.
    int execute(int cycles);
    // Level-triggered interrupt pins, 0 (no interrupt) .. 7 (NMI).
    // System 16 boards use autovectored interrupts, which is the default.
    void set_irq(unsigned level);
    unsigned irq_level() const;

    uint32_t program_counter() const;
    uint16_t status_register() const;
    uint32_t data_register(unsigned index) const;
    uint32_t address_register(unsigned index) const;

private:
    struct core;
    std::unique_ptr<core> m_core;
};
