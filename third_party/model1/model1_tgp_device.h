// Common host interface for Model 1 geometry processors.
#pragma once

#include <cstdint>

class model1_tgp_device {
public:
    virtual ~model1_tgp_device() = default;

    virtual void reset() = 0;
    virtual void push(uint32_t value, uint32_t source_pc) = 0;
    virtual bool output_ready() const = 0;
    virtual uint32_t pop_output() = 0;
    virtual uint16_t ram_address() const = 0;
    virtual void set_ram_address(uint16_t value) = 0;
    virtual uint16_t read_ram_half(unsigned half) = 0;
    virtual void write_ram_half(unsigned half, uint16_t value) = 0;

    // The original V60 driver briefly yields to the geometry CPU while a
    // command is waiting in its input FIFO.  Exposing that state lets the
    // host reproduce the handshake without knowing a device's internals.
    virtual bool input_pending() const { return false; }

    // HLE consumes work when push() is called. LLE uses this cycle budget.
    virtual int execute(int cycles) { return cycles; }
    virtual uint64_t unimplemented_count() const { return 0; }
    virtual uint32_t last_unimplemented_function() const { return 0; }
    virtual uint32_t last_unimplemented_pc() const { return 0; }
    virtual uint16_t geometry_pc() const { return 0; }
    virtual bool is_lle() const { return false; }
};
