#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace omc::audio {

class WasapiSink {
public:
    using FillFn = std::function<void(std::int16_t* out, std::size_t frames)>;

    WasapiSink();
    ~WasapiSink();
    WasapiSink(const WasapiSink&) = delete;
    WasapiSink& operator=(const WasapiSink&) = delete;

    bool start(FillFn fill);

    void stop();

    bool active() const;
    std::uint32_t rate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
