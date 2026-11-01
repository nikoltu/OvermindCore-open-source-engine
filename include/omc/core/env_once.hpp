#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

namespace omc::core {

inline const char* env_keep(const char* value) {
    if (!value) return nullptr;
    return (new std::string(value))->c_str();
}

}

#define OMC_ENV_ONCE(name) \
    ([]() -> const char* { \
        static const char* const kept = ::omc::core::env_keep(std::getenv(name)); \
        return kept; \
    }())
