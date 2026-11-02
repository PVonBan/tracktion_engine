/*
    R8: the merged channel (Recorder 8's fork, 2026-11-02). Shared by AuxSendNode and the host's channel node, so this
    header needs only the engine's public headers (the host includes it without tracktion_TestNodes.h).
*/

#pragma once

namespace tracktion::inline engine {

class TrackMuteState;

/** R8: per-sample send gains, as one struct so the merged channel's inline sends (R8 ChannelNode) and the AuxSendNode
    work them out with the same code: the send's level (its lane, read on a 16-sample grid, or its gain parameter),
    muted to 0, phase-inverted, every change glided over 10 ms. `gains` holds one gain per frame after workOut(). */
struct R8SendGains
{
    std::vector<float> gains;
    choc::buffer::FrameCount numGains = 0;
    float cur = 0.0f;
    int left = 0, rampLen = 480;
    bool started = false;
    int64_t gridPos = 0;
    bool gridValid = false;
    float grid0 = 0.0f, grid1 = 0.0f;

    void prepare (double sampleRate, int blockSize);
    /** The gains for a block: `numFrames` frames from reference sample `referenceStart` (the play head maps it to the
        timeline; `automationAdjustment` is the latency up to the send, taken off). */
    void workOut (AuxSendPlugin&, tracktion::graph::PlayHeadState&, const TrackMuteState*, bool sendsWhenMuted,
                  double sampleRate, TimeDuration automationAdjustment, choc::buffer::FrameCount numFrames, int64_t referenceStart);
    const float* get (choc::buffer::FrameCount numFrames) const      { return numFrames == numGains && numGains > 0 ? gains.data() : nullptr; }

private:
    float level (double seconds, AuxSendPlugin&, AuxSendPlugin::LevelSource*) noexcept;
};

} // namespace tracktion::inline engine
