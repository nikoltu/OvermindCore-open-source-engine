#include "sc2/audio/wasapi.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

namespace sc2::audio {

namespace {

const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

bool is_float_format(const WAVEFORMATEX* wfx) {
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return IsEqualGUID(ext->SubFormat, kSubtypeFloat);
    }
    return false;
}

bool is_pcm_format(const WAVEFORMATEX* wfx) {
    if (wfx->wFormatTag == WAVE_FORMAT_PCM) return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return IsEqualGUID(ext->SubFormat, kSubtypePcm);
    }
    return false;
}

}

struct WasapiSink::Impl {
    WasapiSink::FillFn fill;

    std::thread thread;
    std::atomic<bool> running{false};

    std::mutex init_mtx;
    std::condition_variable init_cv;
    bool init_done = false;
    bool init_ok = false;
    std::uint32_t rate = 0;

    ~Impl() { stop(); }

    void stop() {
        running.store(false);
        if (thread.joinable()) thread.join();
    }

    void run() {
        bool com_inited = false;
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render = nullptr;
        WAVEFORMATEX* mix = nullptr;

        auto signal_init = [&](bool ok, std::uint32_t r) {
            std::lock_guard<std::mutex> lk(init_mtx);
            init_done = true;
            init_ok = ok;
            rate = r;
            init_cv.notify_all();
        };
        auto cleanup = [&]() {
            if (mix) { CoTaskMemFree(mix); mix = nullptr; }
            if (render) { render->Release(); render = nullptr; }
            if (client) { client->Release(); client = nullptr; }
            if (device) { device->Release(); device = nullptr; }
            if (enumerator) { enumerator->Release(); enumerator = nullptr; }
            if (com_inited) { CoUninitialize(); com_inited = false; }
        };

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_inited = SUCCEEDED(hr);

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) { signal_init(false, 0); cleanup(); return; }

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || !device) { signal_init(false, 0); cleanup(); return; }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client));
        if (FAILED(hr) || !client) { signal_init(false, 0); cleanup(); return; }

        hr = client->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) { signal_init(false, 0); cleanup(); return; }

        const bool fmt_float = is_float_format(mix);
        const bool fmt_pcm = is_pcm_format(mix);
        const int dev_channels = mix->nChannels;
        const int dev_bits = mix->wBitsPerSample;
        const std::uint32_t dev_rate = mix->nSamplesPerSec;
        if (dev_channels < 1 || (!fmt_float && !fmt_pcm) ||
            (fmt_pcm && dev_bits != 16 && dev_bits != 32)) {
            signal_init(false, 0);
            cleanup();
            return;
        }

        const REFERENCE_TIME buf_dur = 2000000;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buf_dur, 0, mix, nullptr);
        if (FAILED(hr)) { signal_init(false, 0); cleanup(); return; }

        UINT32 buf_frames = 0;
        hr = client->GetBufferSize(&buf_frames);
        if (FAILED(hr) || buf_frames == 0) { signal_init(false, 0); cleanup(); return; }

        hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
        if (FAILED(hr) || !render) { signal_init(false, 0); cleanup(); return; }

        hr = client->Start();
        if (FAILED(hr)) { signal_init(false, 0); cleanup(); return; }

        signal_init(true, dev_rate);

        std::vector<std::int16_t> scratch;
        const DWORD sleep_ms = 10;

        while (running.load()) {
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            UINT32 avail = (buf_frames > padding) ? (buf_frames - padding) : 0;
            if (avail == 0) { Sleep(sleep_ms); continue; }

            BYTE* out = nullptr;
            if (FAILED(render->GetBuffer(avail, &out)) || !out) { Sleep(sleep_ms); continue; }

            scratch.resize(static_cast<std::size_t>(avail) * 2);
            if (fill)
                fill(scratch.data(), avail);
            else
                std::fill(scratch.begin(), scratch.end(), std::int16_t(0));

            for (UINT32 f = 0; f < avail; ++f) {
                const std::int16_t l = scratch[std::size_t(f) * 2 + 0];
                const std::int16_t r = scratch[std::size_t(f) * 2 + 1];
                for (int c = 0; c < dev_channels; ++c) {
                    double s;
                    if (dev_channels == 1)
                        s = (double(l) + double(r)) * 0.5;
                    else
                        s = (c == 0) ? double(l) : (c == 1 ? double(r) : 0.0);

                    const std::size_t idx = std::size_t(f) * dev_channels + c;
                    if (fmt_float) {
                        reinterpret_cast<float*>(out)[idx] = float(s / 32768.0);
                    } else if (dev_bits == 16) {
                        reinterpret_cast<std::int16_t*>(out)[idx] =
                            std::int16_t(s < -32768.0 ? -32768.0 : (s > 32767.0 ? 32767.0 : s));
                    } else {
                        double v = s * 65536.0;
                        if (v > 2147483647.0) v = 2147483647.0;
                        if (v < -2147483648.0) v = -2147483648.0;
                        reinterpret_cast<std::int32_t*>(out)[idx] = std::int32_t(v);
                    }
                }
            }
            render->ReleaseBuffer(avail, 0);
            Sleep(sleep_ms);
        }

        client->Stop();
        cleanup();
    }
};

WasapiSink::WasapiSink() : impl_(new Impl()) {}
WasapiSink::~WasapiSink() { stop(); }

bool WasapiSink::start(FillFn fill) {
    if (!impl_ || impl_->running.load()) return active();
    impl_->fill = std::move(fill);
    impl_->init_done = false;
    impl_->init_ok = false;
    impl_->rate = 0;
    impl_->running.store(true);
    impl_->thread = std::thread([this]() { impl_->run(); });

    std::unique_lock<std::mutex> lk(impl_->init_mtx);
    impl_->init_cv.wait_for(lk, std::chrono::seconds(2), [&] { return impl_->init_done; });
    const bool ok = impl_->init_ok;
    lk.unlock();
    if (!ok) {
        impl_->running.store(false);
        if (impl_->thread.joinable()) impl_->thread.join();
        return false;
    }
    return true;
}

void WasapiSink::stop() {
    if (impl_) impl_->stop();
}

bool WasapiSink::active() const {
    return impl_ && impl_->running.load() && impl_->init_ok;
}

std::uint32_t WasapiSink::rate() const {
    return impl_ ? impl_->rate : 0;
}

}

#else

namespace sc2::audio {

struct WasapiSink::Impl {};

WasapiSink::WasapiSink() : impl_(nullptr) {}
WasapiSink::~WasapiSink() {}
bool WasapiSink::start(FillFn) { return false; }
void WasapiSink::stop() {}
bool WasapiSink::active() const { return false; }
std::uint32_t WasapiSink::rate() const { return 0; }

}

#endif
