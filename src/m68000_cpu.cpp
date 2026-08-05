#include "m68000_cpu.h"

#include "Moira.h"

// Moira's bus hooks are const-qualified; the board buses they forward to
// maintain internal state (latches, open-bus counters), hence the
// const_cast at the boundary.
struct m68000_cpu::core final : moira::Moira {
    explicit core(m68000_bus& b) : bus(b) {}

    m68000_bus& bus;
    // Level asserted in MAME HOLD_LINE fashion: the request stays pending
    // (even across interrupt-masked stretches) until the CPU services it,
    // then drops by itself. 0 = none held.
    moira::u8 hold_level{0};

    void willInterrupt(moira::u8 level) override {
        if (hold_level != 0 && level >= hold_level) {
            setIPL(0);
            hold_level = 0;
        }
    }

    moira::u8 read8(moira::u32 addr) const override {
        return const_cast<m68000_bus&>(bus).read8(addr);
    }
    moira::u16 read16(moira::u32 addr) const override {
        return const_cast<m68000_bus&>(bus).read16(addr);
    }
    void write8(moira::u32 addr, moira::u8 val) const override {
        const_cast<m68000_bus&>(bus).write8(addr, val);
    }
    void write16(moira::u32 addr, moira::u16 val) const override {
        const_cast<m68000_bus&>(bus).write16(addr, val);
    }
};

m68000_cpu::m68000_cpu(m68000_bus& bus)
    : m_core(std::make_unique<core>(bus)) {}

m68000_cpu::~m68000_cpu() = default;

void m68000_cpu::reset() {
    m_core->reset();
}

int m68000_cpu::execute(int cycles) {
    if (cycles <= 0) return 0;
    m_core->execute(static_cast<moira::i64>(cycles));
    return cycles;
}

void m68000_cpu::set_irq(unsigned level) {
    m_core->hold_level = 0;
    m_core->setIPL(static_cast<moira::u8>(level & 7));
}

void m68000_cpu::set_irq_hold(unsigned level) {
    m_core->hold_level = static_cast<moira::u8>(level & 7);
    m_core->setIPL(static_cast<moira::u8>(level & 7));
}

unsigned m68000_cpu::irq_level() const {
    return m_core->getIPL();
}

uint32_t m68000_cpu::program_counter() const {
    return m_core->getPC();
}

uint16_t m68000_cpu::status_register() const {
    return m_core->getSR();
}

uint32_t m68000_cpu::data_register(unsigned index) const {
    return m_core->getD(static_cast<int>(index & 7));
}

uint32_t m68000_cpu::address_register(unsigned index) const {
    return m_core->getA(static_cast<int>(index & 7));
}
