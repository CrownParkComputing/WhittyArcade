#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

inline int test_process_id() {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

inline bool test_set_environment(const char* name, const char* value) {
#if defined(_WIN32)
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

inline bool test_set_environment(const char* name, const std::string& value) {
    return test_set_environment(name, value.c_str());
}

inline bool test_set_environment(const char* name,
                                 const std::filesystem::path& value) {
    return test_set_environment(name, value.string());
}

inline bool test_unset_environment(const char* name) {
#if defined(_WIN32)
    return _putenv_s(name, "") == 0;
#else
    return unsetenv(name) == 0;
#endif
}
