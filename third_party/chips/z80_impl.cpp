// Keep the single-header Z80 implementation in its own translation unit so
// every emulated board can use the chipset without owning duplicate symbols.
#define CHIPS_IMPL
#include "z80.h"
