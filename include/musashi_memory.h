// Shared memory boundary for the single active Musashi CPU in a session.
#pragma once

#include <cstdint>

class musashi_memory {
public:
    virtual ~musashi_memory() = default;
    virtual uint8_t read8(uint32_t address) = 0;
    virtual uint16_t read16(uint32_t address) = 0;
    virtual uint32_t read32(uint32_t address) = 0;
    virtual void write8(uint32_t address, uint8_t value) = 0;
    virtual void write16(uint32_t address, uint16_t value) = 0;
    virtual void write32(uint32_t address, uint32_t value) = 0;
};

// Musashi's callback ABI is process-global. The active board worker selects
// its 68K-family bus before executing CPU cycles.
void set_active_musashi_memory(musashi_memory* memory);
