#include "Player.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>

namespace fall
{

void Player::setSong (std::shared_ptr<const Song> s)
{
    song = std::move (s);
    markers.clear();
    loop = {};
    loopPass = 0;
    playing = false;
    groupsCompleted = groupsSkipped = groupsPassed = 0;
    groups = song ? buildGroups (*song, practiceHands) : std::vector<NoteGroup> {};
    time = song ? std::max (kMinTime, song->firstNoteTime() - 2.0) : 0.0;
    needsResync = true;
    resync();
}

void Player::setMode (Mode m)
{
    if (mode == m) return;
    mode = m;
    resync();
}

void Player::setPracticeHands (uint8_t mask)
{
    practiceHands = mask;
    if (song) groups = buildGroups (*song, practiceHands);
    resync();
}

void Player::setAudibleHands (uint8_t mask)
{
    audibleHands = mask;
    needsResync = true; // silence notes from a hand that was just muted
}

void Player::setSpeed (double s) { speed = juce::jlimit (0.05, 4.0, s); }

void Player::setPlaying (bool shouldPlay)
{
    if (playing && ! shouldPlay)
        needsResync = true; // release everything when pausing
    playing = shouldPlay && song != nullptr;
}

double Player::endTime() const noexcept { return song ? song->length + 1.0 : 0.0; }

void Player::seek (double t)
{
    time = juce::jlimit (kMinTime, std::max (kMinTime, endTime()), t);
    needsResync = true;
    resync();
}

void Player::setLoop (const LoopSettings& l)
{
    loop = l;
    if (loop.b < loop.a) std::swap (loop.a, loop.b);
    loop.enabled = loop.enabled && loop.b - loop.a > 0.05;
    loopPass = 0;

    if (loop.enabled)
    {
        if (loop.progressive)
            setSpeed (loop.startSpeed);
        if (time >= loop.b || time < loop.a)
            seek (loop.a);
    }
}

void Player::setMarkers (std::vector<Marker> m) { markers = std::move (m); }

PitchSet Player::jumpKeys() const noexcept
{
    PitchSet keys;
    for (const auto& m : markers)
        if (m.triggerPitch >= 0 && m.triggerPitch < 128)
            keys.set ((size_t) m.triggerPitch);
    return keys;
}

void Player::setLearnStyle (LearnStyle s)
{
    style = s;
    history = 0;
    lastHeard = kNever;
    lastPassed = clock;
}

void Player::heardSomething()
{
    lastHeard = clock;
    if (style == LearnStyle::WhilePlaying && isWaiting())
    {
        ++groupsPassed;
        decided (false);
    }
}

void Player::resync()
{
    satisfied.reset();
    history = 0;
    waited = 0.0;
    lastHeard = kNever;
    lastPassed = clock;
    if (! song) { nextNote = nextControl = nextGroup = 0; return; }

    nextNote = song->firstNoteAtOrAfter (time);
    nextControl = (size_t) std::distance (song->controls.begin(),
        std::lower_bound (song->controls.begin(), song->controls.end(), time,
                          [] (const ControlEvent& c, double v) { return c.time < v; }));
    nextGroup = (size_t) std::distance (groups.begin(),
        std::lower_bound (groups.begin(), groups.end(), time - 1e-9,
                          [] (const NoteGroup& g, double v) { return g.time < v; }));
}

bool Player::gateOpen() const noexcept
{
    return mode != Mode::Learn || nextGroup >= groups.size();
}

bool Player::isWaiting() const noexcept
{
    return playing && ! gateOpen() && time >= groups[nextGroup].time - 1e-9;
}

PitchSet Player::expectedPitches() const noexcept
{
    return gateOpen() ? PitchSet {} : groups[nextGroup].pitches;
}

double Player::nextGroupTime() const noexcept
{
    return gateOpen() ? -1.0 : groups[nextGroup].time;
}

int Player::notePlayed (const PitchSet& pitches)
{
    for (size_t i = 0; i < markers.size(); ++i)
    {
        const int p = markers[i].triggerPitch;
        if (p >= 0 && p < 128 && pitches.test ((size_t) p))
        {
            seek (markers[i].time - jumpLeadIn);
            return (int) i;
        }
    }

    if (gateOpen() || ! song)
        return -1;

    const auto& g = groups[nextGroup];
    if (time < g.time - earlyWindow * speed)
        return -1;

    satisfied |= (pitches & ~jumpKeys()) & g.pitches;
    if ((satisfied & g.pitches) == g.pitches)
    {
        ++groupsCompleted;
        decided (true);
    }
    return -1;
}

void Player::decided (bool played)
{
    history = ((history << 1) | (played ? 1u : 0u)) & 0x3ffu;
    ++nextGroup;
    satisfied.reset();
    waited = 0.0;
    lastPassed = clock;
}

bool Player::mayPassWhilePlaying() const noexcept
{
    return style == LearnStyle::WhilePlaying && lastHeard >= std::min (clock - 1.5, lastPassed - 0.3);
}

void Player::skipIfMostlyRight()
{
    const bool lastWasHit = (history & 1u) != 0;
    if (style == LearnStyle::MostlyRight && waited >= 0.5 - 1e-9 && lastWasHit && std::popcount (history) >= 8)
    {
        lastSkipped = groups[nextGroup].pitches;
        ++groupsSkipped;
        decided (false);
    }
}

// ---------------------------------------------------------------------------------------------

namespace
{
    int toOffset (double sample, int numSamples)
    {
        return juce::jlimit (0, std::max (0, numSamples - 1), (int) std::floor (sample + 1e-6));
    }
}

void Player::silenceAll (juce::MidiBuffer& out, int sampleOffset)
{
    if (! song) return;
    for (int idx : sounding)
    {
        const auto& n = song->notes[(size_t) idx];
        out.addEvent (juce::MidiMessage::noteOff (n.channel, n.pitch), sampleOffset);
    }
    sounding.clear();
}

void Player::sendControllerStateAt (juce::MidiBuffer& out, int sampleOffset, double t)
{
    if (! song) return;
    std::map<std::pair<int, int>, int> state; // (channel, cc) -> value
    for (const auto& c : song->controls)
    {
        if (c.time >= t) state.try_emplace ({ c.channel, c.controller }, 0);
        else state[{ c.channel, c.controller }] = c.value;
    }
    for (const auto& [key, value] : state)
        out.addEvent (juce::MidiMessage::controllerEvent (key.first, key.second, value), sampleOffset);
}

void Player::emitRange (juce::MidiBuffer& out, double from, double to, double startSample, double sps)
{
    const auto& notes = song->notes;
    auto offsetOf = [&] (double t) { return startSample + (t - from) * sps; };

    auto releaseEndedBefore = [&] (double limit) {
        for (auto it = sounding.begin(); it != sounding.end();)
        {
            const auto& n = notes[(size_t) *it];
            if (n.end < limit)
            {
                out.addEvent (juce::MidiMessage::noteOff (n.channel, n.pitch), toOffset (offsetOf (n.end), blockSamples));
                it = sounding.erase (it);
            }
            else ++it;
        }
    };

    releaseEndedBefore (to);

    while (nextControl < song->controls.size() && song->controls[nextControl].time < to)
    {
        const auto& c = song->controls[nextControl++];
        out.addEvent (juce::MidiMessage::controllerEvent (c.channel, c.controller, c.value),
                      toOffset (offsetOf (c.time), blockSamples));
    }

    while (nextNote < notes.size() && notes[nextNote].start < to)
    {
        const int idx = (int) nextNote++;
        const auto& n = notes[(size_t) idx];
        if (! handIn (n.hand, audibleHands))
            continue;

        const int offset = toOffset (offsetOf (n.start), blockSamples);

        // Re-striking a pitch that's still held: release the old one first so its
        // later note-off doesn't cut the new note short.
        for (auto it = sounding.begin(); it != sounding.end();)
        {
            const auto& o = notes[(size_t) *it];
            if (o.pitch == n.pitch && o.channel == n.channel)
            {
                out.addEvent (juce::MidiMessage::noteOff (o.channel, o.pitch), offset);
                it = sounding.erase (it);
            }
            else ++it;
        }

        out.addEvent (juce::MidiMessage::noteOn (n.channel, n.pitch, n.velocity), offset);
        sounding.push_back (idx);
    }

    releaseEndedBefore (to); // very short notes that start and end inside this range
}

void Player::render (juce::MidiBuffer& out, int numSamples, double sampleRate)
{
    if (! song || numSamples <= 0 || sampleRate <= 0)
        return;

    blockSamples = numSamples;
    clock += numSamples / sampleRate;

    if (needsResync)
    {
        silenceAll (out, 0);
        sendControllerStateAt (out, 0, time);
        needsResync = false;
    }

    if (! playing)
        return;

    if (isWaiting())
    {
        waited += numSamples / sampleRate;
        skipIfMostlyRight();
    }

    double sample = 0.0;
    for (int guard = 0; guard < 64 && sample < numSamples - 0.5; ++guard)
    {
        const double sps = sampleRate / speed; // samples per song-second
        double target = time + (numSamples - sample) / sps;

        enum class Stop { None, Gate, LoopEnd, SongEnd } stop = Stop::None;

        if (loop.enabled && time < loop.b && target >= loop.b) { target = loop.b; stop = Stop::LoopEnd; }

        if (! gateOpen())
        {
            const double g = groups[nextGroup].time;
            if (g <= target) { target = std::max (time, g); stop = Stop::Gate; }
        }

        if (target >= endTime()) { target = endTime(); stop = Stop::SongEnd; }

        emitRange (out, time, target, sample, sps);
        sample += (target - time) * sps;
        time = target;

        if (stop == Stop::Gate)
        {
            if (mayPassWhilePlaying())
            {
                ++groupsPassed;
                decided (false);
                continue; // on to the next chord
            }
            if (waited == 0.0)
                waited = (numSamples - sample) / sampleRate; // the rest of this block is already waiting
            break; // hold here until notePlayed() opens the gate
        }

        if (stop == Stop::SongEnd)
        {
            silenceAll (out, toOffset (sample, numSamples));
            playing = false;
            break;
        }

        if (stop == Stop::LoopEnd)
        {
            const int offset = toOffset (sample, numSamples);
            silenceAll (out, offset);
            time = loop.a;
            resync();
            sendControllerStateAt (out, offset, time);
            ++loopPass;
            if (loop.progressive)
                speed = std::min (loop.targetSpeed, speed + loop.step);
        }
    }
}

} // namespace fall
