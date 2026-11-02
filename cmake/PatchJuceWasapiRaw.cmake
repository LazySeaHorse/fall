# Makes JUCE's WASAPI input ask Windows for the RAW microphone stream (AUDCLNT_STREAMOPTIONS_RAW).
#
# Without it, Windows runs the laptop microphone through its voice-call processing (noise
# suppression, "Voice Clarity", automatic gain). That processing treats a sustained piano tone as
# noise to be removed and squashes note onsets, which is poison for note detection. JUCE declares
# IAudioClient2::SetClientProperties but never calls it, so we add the call here.
#
# The switch is the global `juce::WasapiClasses::fallWasapiRawInput` (default true), read whenever an input device
# is opened; Fall's settings can turn it off for drivers that misbehave in raw mode.
#
# Usage: include() after FetchContent_MakeAvailable(JUCE). Idempotent; fails loudly if JUCE's code
# changed so the anchors no longer match (update the strings below when bumping JUCE).

set(_fall_wasapi "${juce_SOURCE_DIR}/modules/juce_audio_devices/native/juce_WASAPI_windows.cpp")
file(READ "${_fall_wasapi}" _src)

if(NOT _src MATCHES "fallWasapiRawInput")
    set(_anchor_create [=[    ComSmartPtr<IAudioClient> createClient()
    {
        ComSmartPtr<IAudioClient> newClient;

        if (device != nullptr)
            logFailure (device->Activate (__uuidof (IAudioClient), CLSCTX_INPROC_SERVER,
                                          nullptr, (void**) newClient.resetAndGetPointerAddress()));

        return newClient;
    }]=])
    set(_patched_create [=[    // [Fall patch] input devices may request the raw stream (no Windows voice processing)
    virtual bool wantsRawStream() const { return false; }

    ComSmartPtr<IAudioClient> createClient()
    {
        ComSmartPtr<IAudioClient> newClient;

        if (device != nullptr)
            logFailure (device->Activate (__uuidof (IAudioClient), CLSCTX_INPROC_SERVER,
                                          nullptr, (void**) newClient.resetAndGetPointerAddress()));

        if (newClient != nullptr && wantsRawStream())
        {
            if (auto client2 = newClient.getInterface<IAudioClient2>())
            {
                struct { UINT32 cbSize; BOOL bIsOffload; AUDIO_STREAM_CATEGORY eCategory; DWORD Options; } props {};
                props.cbSize = sizeof (props);
                props.eCategory = AudioCategory_Other;
                props.Options = 0x1; // AUDCLNT_STREAMOPTIONS_RAW (ignored by devices without raw support)
                client2->SetClientProperties ((const AudioClientProperties*) &props);
            }
        }

        return newClient;
    }]=])

    set(_anchor_input [=[class WASAPIInputDevice final : public WASAPIDeviceBase
{
public:
    using WASAPIDeviceBase::WASAPIDeviceBase;
]=])
    set(_patched_input [=[bool fallWasapiRawInput = true; // [Fall patch] see cmake/PatchJuceWasapiRaw.cmake

class WASAPIInputDevice final : public WASAPIDeviceBase
{
public:
    using WASAPIDeviceBase::WASAPIDeviceBase;

    bool wantsRawStream() const override { return fallWasapiRawInput; }
]=])

    foreach(_pair "create" "input")
        string(FIND "${_src}" "${_anchor_${_pair}}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "PatchJuceWasapiRaw: JUCE's WASAPI code changed (anchor '${_pair}' not found in ${_fall_wasapi}). Update cmake/PatchJuceWasapiRaw.cmake.")
        endif()
        string(REPLACE "${_anchor_${_pair}}" "${_patched_${_pair}}" _src "${_src}")
    endforeach()

    file(WRITE "${_fall_wasapi}" "${_src}")
    message(STATUS "Patched JUCE WASAPI input for raw microphone capture")
endif()
