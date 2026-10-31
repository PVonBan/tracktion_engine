/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

#pragma once

namespace tracktion::inline engine {

class TrackMuteState;

/**
    Node for an aux send.
    This is a SendNode but needs to update automation with the correct latency.
*/
class AuxSendNode final : public tracktion::graph::SendNode
{
public:
    //==============================================================================
    /** Creates a AuxSendNode to process an aux send. */
    AuxSendNode (std::unique_ptr<Node> inputNode, int busIDToUse,
                 SampleRateAndBlockSize,
                 AuxSendPlugin&, tracktion::graph::PlayHeadState&, const TrackMuteState*,
                 bool processAuxSendsWhenTrackIsMuted);
    ~AuxSendNode() override;

    //==============================================================================
    NodeProperties getNodeProperties() override;
    void prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo&) override;
    void process (ProcessContext&) override;
    const float* getPerSampleGains (choc::buffer::FrameCount) override;

private:
    // R8: per-sample send gains
    const TrackMuteState* r8MuteState = nullptr;
    bool r8SendsWhenMuted = false;
    std::vector<float> r8Gains;
    choc::buffer::FrameCount r8NumGains = 0;
    float r8Cur = 0.0f;
    int r8Left = 0, r8RampLen = 480;
    bool r8Started = false;
    int64_t r8GridPos = 0;
    bool r8GridValid = false;
    float r8Grid0 = 0.0f, r8Grid1 = 0.0f;
    float r8Level (double seconds, AuxSendPlugin::LevelSource*) noexcept;
    void r8WorkOutGains (ProcessContext&);

    //==============================================================================
    tracktion::graph::PlayHeadState& playHeadState;

    Plugin::Ptr pluginPtr;
    AuxSendPlugin& sendPlugin;

    double sampleRate = 44100.0;
    TimeDuration automationAdjustmentTime;

    std::optional<NodeProperties> cachedNodeProperties;
    bool isPrepared = false;
    bool isInitialised = false;
};

} // namespace tracktion::inline engine
