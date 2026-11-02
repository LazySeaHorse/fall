#include "DetectInternal.h"

namespace fall
{
using namespace detect;

namespace
{
    constexpr uint32_t kMagic   = 0x4c414346; // "FCAL"
    constexpr uint32_t kVersion = 1;
}

Calibration::Calibration() : impl (std::make_unique<Impl>()) {}
Calibration::~Calibration() = default;
Calibration::Calibration (const Calibration& o) : impl (std::make_unique<Impl> (*o.impl)) {}
Calibration& Calibration::operator= (const Calibration& o) { if (this != &o) *impl = *o.impl; return *this; }

bool Calibration::hasKey (int pitch) const
{
    return isPianoKey (pitch) && impl->keys[(size_t) keyIndex (pitch)].valid;
}

int Calibration::numKeys() const
{
    int n = 0;
    for (auto& k : impl->keys)
        n += k.valid ? 1 : 0;
    return n;
}

void Calibration::clear() { *impl = Impl(); }

juce::MemoryBlock Calibration::toBinary() const
{
    juce::MemoryOutputStream out;
    out.writeInt ((int) kMagic);
    out.writeInt ((int) kVersion);
    out.writeInt (numKeys());
    for (int i = 0; i < kNumKeys; ++i)
    {
        auto& k = impl->keys[(size_t) i];
        if (! k.valid)
            continue;
        out.writeByte ((char) (kLowestPianoKey + i));
        out.writeByte ((char) k.n);
        out.writeFloat (k.tuneCents);
        out.writeFloat (k.B);
        for (int j = 0; j < k.n; ++j)
        {
            out.writeByte ((char) k.h[j]);
            out.writeFloat (k.hz[j]);
            out.writeFloat (k.w[j]);
        }
    }
    return out.getMemoryBlock();
}

bool Calibration::fromBinary (const juce::MemoryBlock& block)
{
    juce::MemoryInputStream in (block, false);
    if ((uint32_t) in.readInt() != kMagic)
        return false;
    if ((uint32_t) in.readInt() != kVersion)
        return false;
    const int count = in.readInt();
    if (count < 0 || count > kNumKeys)
        return false;

    Impl tmp; // parse into a scratch copy so a corrupt file leaves us untouched
    for (int c = 0; c < count; ++c)
    {
        if (in.isExhausted())
            return false;
        const int pitch = (uint8_t) in.readByte();
        const int n     = (uint8_t) in.readByte();
        if (! isPianoKey (pitch) || n < 1 || n > kMaxPartials)
            return false;
        auto& k = tmp.keys[(size_t) keyIndex (pitch)];
        k.valid     = true;
        k.n         = n;
        k.tuneCents = in.readFloat();
        k.B         = in.readFloat();
        for (int j = 0; j < n; ++j)
        {
            k.h[j]  = (uint8_t) in.readByte();
            k.hz[j] = in.readFloat();
            k.w[j]  = in.readFloat();
            if (! (k.hz[j] > 1.0f && k.hz[j] < 50000.0f) || ! (k.w[j] >= 0.0f && k.w[j] <= 1.5f) || k.h[j] < 1)
                return false;
        }
    }
    if (! in.isExhausted())
        return false;
    *impl = tmp;
    return true;
}

} // namespace fall
