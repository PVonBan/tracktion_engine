/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion::inline engine {

class CompressorPlugin : public Plugin
{
public:
    CompressorPlugin (PluginCreationInfo);
    ~CompressorPlugin() override;

    //==============================================================================
    float getThreshold() const;
    void setThreshold (float);
    float getRatio() const;
    void setRatio (float);

    //==============================================================================
    static const char* getPluginName()      { return NEEDS_TRANS("Compressor/Limiter"); }
    static const char* xmlTypeName;

    juce::String getName() const override                               { return TRANS("Compressor"); }
    juce::String getPluginType() override                               { return xmlTypeName; }
    juce::String getShortName (int) override                            { return TRANS("Comp"); }
    int getNumOutputChannelsGivenInputs (int numInputChannels) override { return juce::jmin (numInputChannels, 2); }
    BusLayout getBusses() const override   { return BusLayout::singleStereoInOut(); }
    void getChannelNames (juce::StringArray*, juce::StringArray*) override;

    void initialise (const PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const PluginRenderContext&) override;

    juce::String getSelectableDescription() override                    { return TRANS("Compressor/Limiter Plugin"); }

    ParameterWithStateValue thresholdGain, ratio, attackMs,
                            releaseMs, outputDb, sidechainDb;
    juce::CachedValue<bool> useSidechainTrigger;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    static float getMinThreshold()      { return 0.01f; }
    static float getMaxThreshold()      { return 1.0f; }

    //==============================================================================
    /** R8: the compressor's running state (its level envelope and its detector's last value), for a host that checks
        whether processing silence would change it: it saves the state, runs applyToBuffer on a silent sample, compares
        and puts the state back (Recorder 8's bypass on silence). */
    struct R8State { double currentLevel = 0.0; float lastSamp = 0.0f; };
    R8State r8GetState() const noexcept             { return { currentLevel, lastSamp }; }
    void r8SetState (R8State s) noexcept            { currentLevel = s.currentLevel; lastSamp = s.lastSamp; }

    /** R8: what applyToBuffer does to the state over a block of digital silence once the detector has settled
        (lastSamp exactly zero, no sidechain trigger): the detector stays at zero and the level envelope releases, by
        the same arithmetic, sample by sample. The output of such a block is silence. Returns false and changes
        nothing when the detector hasn't settled. (A host checks it against applyToBuffer itself before relying on it.) */
    bool r8AdvanceOnSilence (int numSamples) noexcept;

private:
    double currentLevel = 0.0;
    float lastSamp = 0.0f;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompressorPlugin)
};

} // namespace tracktion::inline engine
