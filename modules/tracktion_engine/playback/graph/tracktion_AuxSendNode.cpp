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

    r8Gains.prepare (info.sampleRate, info.blockSize);      // R8: per-sample send gains

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
    r8Gains.workOut (sendPlugin, playHeadState, r8MuteState, r8SendsWhenMuted, sampleRate, automationAdjustmentTime,
                     pc.buffers.audio.getNumFrames(), pc.referenceSampleRange.getStart());     // R8: per-sample send gains
}

// R8: per-sample send gains --------------------------------------------------------------------------------------
void R8SendGains::prepare (double sampleRate, int blockSize)
{
    gains.assign ((size_t) std::max (blockSize, 1) * 2, 0.0f);
    rampLen = std::max (16, (int) std::lround (sampleRate * 0.010));
    started = false;
    gridValid = false;
    left = 0;
}

float R8SendGains::level (double seconds, AuxSendPlugin& sendPlugin, AuxSendPlugin::LevelSource* src) noexcept
{
    return src != nullptr ? src->gainAt (seconds) : volumeFaderPositionToGain (sendPlugin.gain->getCurrentValue());
}

void R8SendGains::workOut (AuxSendPlugin& sendPlugin, tracktion::graph::PlayHeadState& playHeadState, const TrackMuteState* muteState,
                           bool sendsWhenMuted, double sampleRate, TimeDuration automationAdjustmentTime,
                           choc::buffer::FrameCount numFrames, int64_t referenceStart)
{
    numGains = 0;
    if (numFrames == 0 || numFrames > gains.size())
        return;

    bool silent = false;
    if (muteState != nullptr)
    {
        if (sendsWhenMuted)
            silent = ! muteState->shouldTrackBeAudible() && ! muteState->shouldTrackContentsBeProcessed();
        else
            silent = ! muteState->shouldTrackBeAudible() || ! muteState->shouldTrackContentsBeProcessed();
    }
    const float sign = sendPlugin.invertPhase ? -1.0f : 1.0f;
    auto* src = sendPlugin.levelSource.load (std::memory_order_acquire);
    const bool playing = playHeadState.playHead.isPlaying();
    const auto pos0 = playHeadState.playHead.referenceSamplePositionToTimelinePosition (referenceStart)
                        + (int64_t) std::llround (automationAdjustmentTime.inSeconds() * sampleRate);
    constexpr int64_t grid = 16;
    const float plain = src == nullptr ? level (0.0, sendPlugin, nullptr) : 0.0f;
    if (src == nullptr)
        gridValid = false;

    for (choc::buffer::FrameCount i = 0; i < numFrames; ++i)
    {
        float lvl = plain;
        if (src != nullptr)
        {
            const int64_t pos = playing ? pos0 + (int64_t) i : pos0;
            const int64_t g = pos >= 0 ? (pos / grid) * grid : ((pos - grid + 1) / grid) * grid;
            if (! gridValid || g != gridPos)
            {
                if (gridValid && g == gridPos + grid)
                    grid0 = grid1;
                else
                    grid0 = level ((double) g / sampleRate, sendPlugin, src);
                grid1 = level ((double) (g + grid) / sampleRate, sendPlugin, src);
                gridPos = g;
                gridValid = true;
            }
            lvl = grid0 + (grid1 - grid0) * (float) (pos - g) * (1.0f / (float) grid);
        }
        const float target = silent ? 0.0f : lvl * sign;

        if (! started)
        {
            cur = target;
            started = true;
        }
        else if (left > 0)
        {
            cur += (target - cur) / (float) left;
            if (--left == 0)
                cur = target;
        }
        else if (src != nullptr ? std::abs (target - cur) > 0.01f : target != cur)
        {
            left = rampLen;
            cur += (target - cur) / (float) left;
            --left;
        }
        else
            cur = target;

        gains[(size_t) i] = cur;
    }
    numGains = numFrames;
}

const float* AuxSendNode::getPerSampleGains (choc::buffer::FrameCount numFrames)
{
    return r8Gains.get (numFrames);
}

} // namespace tracktion::inline engine
