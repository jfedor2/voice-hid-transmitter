// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "audio.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <initguid.h>  // defines PKEY_Device_FriendlyName in this file
#include <functiondiscoverykeys_devpkey.h>

#include "features.h"
#include "util.h"

using Microsoft::WRL::ComPtr;

static constexpr REFERENCE_TIME BUFFER_DURATION = 2000000;  // 200 ms, in 100 ns units
static constexpr DWORD DEFAULT_CHECK_MS = 1000;
static constexpr DWORD RETRY_MS = 1000;

static std::string friendly_name(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
        return std::string();
    }
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
        name = to_utf8(v.pwszVal);
    }
    PropVariantClear(&v);
    return name;
}

static std::string device_id(IMMDevice* device) {
    LPWSTR id = nullptr;
    std::string out;
    if (SUCCEEDED(device->GetId(&id))) {
        out = to_utf8(id);
        CoTaskMemFree(id);
    }
    return out;
}

static std::string default_capture_id(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) {
        return std::string();
    }
    return device_id(device.Get());
}

std::vector<AudioDevice> list_capture_devices() {
    std::vector<AudioDevice> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return out;
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) {
        return out;
    }
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(devices->Item(i, &device))) {
            out.push_back({ device_id(device.Get()), friendly_name(device.Get()) });
        }
    }
    return out;
}

void AudioCapture::start(Sink sink, ResetFn on_reset) {
    sink_ = std::move(sink);
    on_reset_ = std::move(on_reset);
    quit_ = false;
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&AudioCapture::thread_main, this);
}

void AudioCapture::stop() {
    if (!thread_.joinable()) {
        return;
    }
    quit_ = true;
    SetEvent((HANDLE) wake_event_);
    thread_.join();
    CloseHandle((HANDLE) wake_event_);
    wake_event_ = nullptr;
}

void AudioCapture::set_device(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == requested_id_) {
            return;
        }
        requested_id_ = id;
    }
    device_changed_ = true;
    if (wake_event_ != nullptr) {
        SetEvent((HANDLE) wake_event_);
    }
}

std::string AudioCapture::device_name() {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_name_;
}

std::string AudioCapture::error() {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void AudioCapture::thread_main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (!quit_) {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = requested_id_;
        }
        device_changed_ = false;
        if (!run_stream(id) && !quit_ && !device_changed_) {
            WaitForSingleObject((HANDLE) wake_event_, RETRY_MS);
        }
    }
    CoUninitialize();
}

// Captures until asked to stop, the device changes or an error happens.
// Returns false on error.
bool AudioCapture::run_stream(const std::string& id) {
    auto set_error = [this](const std::string& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = e;
        device_name_.clear();
        return false;
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return set_error("Audio system not available.");
    }
    ComPtr<IMMDevice> device;
    HRESULT hr = id.empty() ? enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)
                            : enumerator->GetDevice(from_utf8(id).c_str(), &device);
    if (FAILED(hr)) {
        return set_error(id.empty() ? "No microphone found." : "The selected microphone is not available.");
    }
    const std::string opened_id = device_id(device.Get());

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**) client.GetAddressOf()))) {
        return set_error("Could not open the microphone.");
    }
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = SAMPLE_RATE;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = SAMPLE_RATE * 4;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, BUFFER_DURATION, 0, &fmt, nullptr);
    if (FAILED(hr)) {
        if (hr == E_ACCESSDENIED) {
            return set_error("Microphone access is denied. Check Windows privacy settings.");
        }
        char buf[96];
        snprintf(buf, sizeof(buf), "Could not start the microphone (error 0x%08lx).", (unsigned long) hr);
        return set_error(buf);
    }
    HANDLE audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->SetEventHandle(audio_event)) ||
        FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) {
        CloseHandle(audio_event);
        return set_error("Could not start the microphone.");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        device_name_ = friendly_name(device.Get());
        error_.clear();
    }
    if (on_reset_) {
        on_reset_();
    }

    bool ok = true;
    ULONGLONG last_default_check = GetTickCount64();
    HANDLE handles[2] = { audio_event, (HANDLE) wake_event_ };
    while (!quit_ && !device_changed_) {
        WaitForMultipleObjects(2, handles, FALSE, 200);
        while (true) {
            UINT32 packet = 0;
            hr = capture->GetNextPacketSize(&packet);
            if (FAILED(hr)) {
                ok = false;
                break;
            }
            if (packet == 0) {
                break;
            }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD buffer_flags = 0;
            hr = capture->GetBuffer(&data, &frames, &buffer_flags, nullptr, nullptr);
            if (FAILED(hr)) {
                ok = false;
                break;
            }
            if (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::vector<float> silence(frames, 0.0f);
                sink_(silence.data(), frames);
            } else {
                sink_((const float*) data, frames);
            }
            capture->ReleaseBuffer(frames);
        }
        if (!ok) {
            set_error("The microphone was disconnected.");
            break;
        }
        // Follow changes of the default device.
        const ULONGLONG now = GetTickCount64();
        if (id.empty() && now - last_default_check >= DEFAULT_CHECK_MS) {
            last_default_check = now;
            if (default_capture_id(enumerator.Get()) != opened_id) {
                break;
            }
        }
    }
    client->Stop();
    CloseHandle(audio_event);
    return ok;
}
