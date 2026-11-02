/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

#pragma once
#include "tracktion_R8SendGains.h"

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
    R8SendGains r8Gains;

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
