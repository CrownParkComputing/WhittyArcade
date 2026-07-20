// Process-wide serialization for SDL event/controller access.
//
// The video worker owns SDL's event pump. Board threads may read keyboard and
// controller state, but SDL does not permit the event pump to race those
// reads or controller teardown. Keeping this tiny guard in a header gives all
// frontend translation units the same function-local static mutex.
#pragma once

#include <mutex>

inline std::mutex& arcade_sdl_mutex() {
    static std::mutex mutex;
    return mutex;
}
