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

AuxSendNode::AuxSendNode (std::unique_ptr<Node> inputNode, int busIDToUse,
                          SampleRateAndBlockSize info,
                          AuxSendPlugin& sourceSendPlugin, tracktion::graph::PlayHeadState& phs,
                          const TrackMuteState* trackMuteState, bool processAuxSendsWhenTrackIsMuted)
    : SendNode (std::move (inputNode), busIDToUse,
                [&sourceSendPlugin, trackMuteState, processAuxSendsWhenTrackIsMuted]
                {
                    if (processAuxSendsWhenTrackIsMuted)
                    {
                        if (trackMuteState
                            && ! trackMuteState->shouldTrackBeAudible()
                            && ! trackMuteState->shouldTrackContentsBeProcessed())
                           return 0.0f;
                    }
                    else
                    {
                        if (trackMuteState
                            && (! trackMuteState->shouldTrackBeAudible()
                                || ! trackMuteState->shouldTrackContentsBeProcessed()))
                           return 0.0f;
                    }

                    auto gain = volumeFaderPositionToGain (sourceSendPlugin.gain->getCurrentValue());
                    if (sourceSendPlugin.invertPhase)
                        gain *= -1.0f;

                    return gain;
               }),
      playHeadState (phs),
      pluginPtr (sourceSendPlugin),
      sendPlugin (sourceSendPlugin),
      r8MuteState (trackMuteState),           // R8: per-sample send gains
      r8SendsWhenMuted (processAuxSendsWhenTrackIsMuted)
{
    jassert (pluginPtr != nullptr);

    sendPlugin.baseClassInitialise ({ TimePosition(), info.sampleRate, info.blockSize });
    isInitialised = true;
}

AuxSendNode::~AuxSendNode()
{
    if (isInitialised && ! sendPlugin.baseClassNeedsInitialising())
        sendPlugin.baseClassDeinitialise();
}

//==============================================================================
NodeProperties AuxSendNode::getNodeProperties()
{
    if (cachedNodeProperties)
        return *cachedNodeProperties;

    auto props = SendNode::getNodeProperties();

    if (isPrepared)
        cachedNodeProperties = props;

    return props;
}

void AuxSendNode::prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo& info)
{
    sampleRate = info.sampleRate;

    // R8: per-sample send gains
    r8Gains.assign ((size_t) std::max (info.blockSize, 1) * 2, 0.0f);
    r8RampLen = std::max (16, (int) std::lround (info.sampleRate * 0.010));
    r8Started = false;
    r8GridValid = false;
    r8Left = 0;

    if (auto props = getNodeProperties(); props.latencyNumSamples > 0)
        automationAdjustmentTime = TimeDuration::fromSamples (-props.latencyNumSamples, sampleRate);

    isPrepared = true;
}

void AuxSendNode::process (ProcessContext& pc)
{
    if (sendPlugin.isAutomationNeeded()
        && sendPlugin.edit.getAutomationRecordManager().isReadingAutomation())
    {
        const auto editSamplePos = playHeadState.playHead.referenceSamplePositionToTimelinePosition (pc.referenceSampleRange.getStart());
        const auto editTime = TimePosition::fromSamples (editSamplePos, sampleRate) + automationAdjustmentTime;
        sendPlugin.updateParameterStreams (editTime);
    }

    SendNode::process (pc);
    r8WorkOutGains (pc);
}

// R8: per-sample send gains --------------------------------------------------------------------------------------
float AuxSendNode::r8Level (double seconds, AuxSendPlugin::LevelSource* src) noexcept
{
    return src != nullptr ? src->gainAt (seconds) : volumeFaderPositionToGain (sendPlugin.gain->getCurrentValue());
}

void AuxSendNode::r8WorkOutGains (ProcessContext& pc)
{
    const auto numFrames = pc.buffers.audio.getNumFrames();
    r8NumGains = 0;
    if (numFrames == 0 || numFrames > r8Gains.size())
        return;

    bool silent = false;
    if (r8MuteState != nullptr)
    {
        if (r8SendsWhenMuted)
            silent = ! r8MuteState->shouldTrackBeAudible() && ! r8MuteState->shouldTrackContentsBeProcessed();
        else
            silent = ! r8MuteState->shouldTrackBeAudible() || ! r8MuteState->shouldTrackContentsBeProcessed();
    }
    const float sign = sendPlugin.invertPhase ? -1.0f : 1.0f;
    auto* src = sendPlugin.levelSource.load (std::memory_order_acquire);
    const bool playing = playHeadState.playHead.isPlaying();
    const auto pos0 = playHeadState.playHead.referenceSamplePositionToTimelinePosition (pc.referenceSampleRange.getStart())
                        + (int64_t) std::llround (automationAdjustmentTime.inSeconds() * sampleRate);
    constexpr int64_t grid = 16;
    const float plain = src == nullptr ? r8Level (0.0, nullptr) : 0.0f;
    if (src == nullptr)
        r8GridValid = false;

    for (choc::buffer::FrameCount i = 0; i < numFrames; ++i)
    {
        float level = plain;
        if (src != nullptr)
        {
            const int64_t pos = playing ? pos0 + (int64_t) i : pos0;
            const int64_t g = pos >= 0 ? (pos / grid) * grid : ((pos - grid + 1) / grid) * grid;
            if (! r8GridValid || g != r8GridPos)
            {
                if (r8GridValid && g == r8GridPos + grid)
                    r8Grid0 = r8Grid1;
                else
                    r8Grid0 = r8Level ((double) g / sampleRate, src);
                r8Grid1 = r8Level ((double) (g + grid) / sampleRate, src);
                r8GridPos = g;
                r8GridValid = true;
            }
            level = r8Grid0 + (r8Grid1 - r8Grid0) * (float) (pos - g) * (1.0f / (float) grid);
        }
        const float target = silent ? 0.0f : level * sign;

        if (! r8Started)
        {
            r8Cur = target;
            r8Started = true;
        }
        else if (r8Left > 0)
        {
            r8Cur += (target - r8Cur) / (float) r8Left;
            if (--r8Left == 0)
                r8Cur = target;
        }
        else if (src != nullptr ? std::abs (target - r8Cur) > 0.01f : target != r8Cur)
        {
            r8Left = r8RampLen;
            r8Cur += (target - r8Cur) / (float) r8Left;
            --r8Left;
        }
        else
            r8Cur = target;

        r8Gains[(size_t) i] = r8Cur;
    }
    r8NumGains = numFrames;
}

const float* AuxSendNode::getPerSampleGains (choc::buffer::FrameCount numFrames)
{
    return numFrames == r8NumGains && r8NumGains > 0 ? r8Gains.data() : nullptr;
}

} // namespace tracktion::inline engine
