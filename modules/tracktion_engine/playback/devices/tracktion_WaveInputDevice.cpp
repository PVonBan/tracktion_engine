/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion::inline engine {

class DiskSpaceCheckTask  : public juce::ThreadPoolJob,
                            private juce::Timer,
                            private juce::AsyncUpdater
{
public:
    DiskSpaceCheckTask (Engine& e, const juce::File& f)
        : juce::ThreadPoolJob ("SpaceCheck"), engine (e), file (f)
    {
        startTimer (1000);
    }

    ~DiskSpaceCheckTask() override
    {
        stopTimer();
        engine.getBackgroundJobs().getPool().removeJob (this, true, 10000);
    }

    JobStatus runJob() override
    {
        auto bytesFree = file.getBytesFreeOnVolume();

        if (bytesFree > 0 && bytesFree < 1024 * 1024 * 50)
            triggerAsyncUpdate();

        return jobHasFinished;
    }

    void handleAsyncUpdate() override
    {
        // R8: disk space. Stopping the transports deletes this check (with the recording it belongs to), so nothing of
        // it may be used afterwards.
        auto& e = engine;
        TransportControl::stopAllTransports (e, false, false);
        e.getUIBehaviour().showWarningMessage (TRANS("Recording error - disk space is getting dangerously low!"));
    }

    void timerCallback() override
    {
        startTimer (31111);
        engine.getBackgroundJobs().getPool().addJob (this, false);
    }

    Engine& engine;
    juce::File file;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DiskSpaceCheckTask)
};

//==============================================================================
static const char* projDirPattern  = "%projectdir%";
static const char* editPattern     = "%edit%";
static const char* trackPattern    = "%track%";
static const char* datePattern     = "%date%";
static const char* timePattern     = "%time%";
static const char* takePattern     = "%take%";

static juce::String expandPatterns (Edit& ed, juce::String s, Track* track, int take)
{
    {
        auto editName = ed.getName();

        if (editName.isEmpty())
            s = s.replace (juce::String (editPattern) + "_", {}, true);

        s = s.replace (editPattern, editName, true);
    }

    {
        juce::String trackName;

        if (track != nullptr)
            trackName = track->getName();

        if (trackName.isEmpty())
            s = s.replace (juce::String (trackPattern) + "_", {}, true);

        s = s.replace (trackPattern, trackName, true);
    }

    {
        auto projDir = ed.engine.getEngineBehaviour().getDefaultFolderForAudioRecordings (ed);

        if (auto proj = getProjectForEdit (ed))
        {
            projDir = proj->getDirectoryForMedia (ProjectItem::Category::recorded);
        }
        else if (ed.editFileRetriever)
        {
            auto editFile = ed.editFileRetriever();

            if (editFile != juce::File() && editFile.getParentDirectory().isDirectory())
                projDir = editFile.getParentDirectory();

            if (! projDir.isDirectory())
                projDir = juce::File::getCurrentWorkingDirectory();
        }

        s = s.replace (projDirPattern, projDir.getFullPathName(), true);
    }

    auto now = juce::Time::getCurrentTime();

    {
        juce::String date;

        date << now.getDayOfMonth()
            << juce::Time::getMonthName (now.getMonth(), true)
            << now.getYear();

        s = s.replace (datePattern, date, true);
    }

    s = s.replace (timePattern, juce::String::formatted ("%d%02d%02d",
                                                         now.getHours(),
                                                         now.getMinutes(),
                                                         now.getSeconds()), true);

    if (! s.contains (takePattern))
        s += "_" + juce::String (takePattern);

    s = s.replace (takePattern, juce::String (take), true);

    return juce::File::createLegalPathName (s.trim());
}


//==============================================================================
struct RetrospectiveRecordBuffer
{
    RetrospectiveRecordBuffer (Engine& e)
    {
        lengthInSeconds = e.getPropertyStorage().getProperty (SettingID::retrospectiveRecord, 30);
    }

    void updateSizeIfNeeded (int newNumChannels, double newSampleRate)
    {
        int newNumSamples = juce::roundToInt (lengthInSeconds * newSampleRate);

        if (newNumChannels != numChannels || newNumSamples != numSamples || newSampleRate != sampleRate)
        {
            numChannels = newNumChannels;
            numSamples  = newNumSamples;
            sampleRate  = newSampleRate;

            fifo.setSize (numChannels, std::max (1, numSamples));
            fifo.reset();
        }
    }

    void processBuffer (double streamTime, const juce::AudioBuffer<float>& inputBuffer, int numSamplesIn)
    {
        if (numSamplesIn < numSamples)
        {
            lastStreamTime = streamTime;

            fifo.ensureFreeSpace (numSamplesIn);
            fifo.write (inputBuffer);
        }
    }

    void syncToEdit (Edit& edit, EditPlaybackContext& context, double streamTime, int numSamplesIn)
    {
        const juce::SpinLock::ScopedLockType sl (editInfoLock);
        auto& pei = editInfo[edit.getProjectItemRef()];

        if (context.isPlaying())
        {
            pei.pausedTime = 0s;
            pei.lastEditTime = context.globalStreamTimeToEditTime (streamTime);
        }
        else
        {
            pei.pausedTime = pei.pausedTime + TimeDuration::fromSamples (numSamplesIn, sampleRate);
        }
    }

    bool wasRecentlyPlaying (Edit& edit)
    {
        const juce::SpinLock::ScopedLockType sl (editInfoLock);
        auto& pei = editInfo[edit.getProjectItemRef()];

        return (pei.lastEditTime >= 0s && pei.pausedTime < 20s);
    }

    void removeEditSync (Edit& edit)
    {
        const juce::SpinLock::ScopedLockType sl (editInfoLock);
        auto itr = editInfo.find (edit.getProjectItemRef());

        if (itr != editInfo.end())
            editInfo.erase (itr);
    }

    double lengthInSeconds = 30.0;

    AudioFifo fifo { 1, 1 };
    double lastStreamTime = 0;

    int numChannels = 0;
    int numSamples = 0;
    double sampleRate = 0;

    struct PerEditInfo
    {
        TimePosition lastEditTime = -1s;
        TimeDuration pausedTime;
    };

    std::map<ProjectItemRef, PerEditInfo> editInfo;
    juce::SpinLock editInfoLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RetrospectiveRecordBuffer)
};


//==============================================================================
//==============================================================================
class WaveInputDeviceInstance  : public InputDeviceInstance
{
public:
    WaveInputDeviceInstance (WaveInputDevice& dev, EditPlaybackContext& c)
        : InputDeviceInstance (dev, c),
          inputBuffer (1, 128)
    {
        getWaveInput().addInstance (this);
    }

    ~WaveInputDeviceInstance() override
    {
        StopRecordingParameters params;
        params.unloopedTimeToEndRecording = context.getUnloopedPosition();
        params.isLooping = context.transport.looping;
        params.markedRange = context.transport.getLoopRange();
        prepareToStopRecording (params.targetsToStop);
        stopRecording (params);

        auto& wi = getWaveInput();

        if (wi.retrospectiveBuffer)
            wi.retrospectiveBuffer->removeEditSync (edit);

        getWaveInput().removeInstance (this);
    }

    bool isRecordingActive() const override
    {
        return getWaveInput().mergeMode != 2 && InputDeviceInstance::isRecordingActive();
    }

    bool isRecordingActive (EditItemID targetID) const override
    {
        return getWaveInput().mergeMode != 2 && InputDeviceInstance::isRecordingActive (targetID);
    }

    bool isRecordingQueuedToStop (EditItemID targetID) override
    {
        return getRecordStopper().isQueued (targetID);
    }

    bool shouldTrackContentsBeMuted (const Track& t) override
    {
        bool isTrackRecordingWithPunch = false, muteTrackNow = false,
            muteTrackContentsWhilstRecording = false, isActivelyRecording = false;

        {
            const std::shared_lock sl (contextLock);

            if (recordingContexts.empty())
                return false;

            if (auto recContext = getContextForID (t.itemID))
            {
                isTrackRecordingWithPunch = recContext->recordingWithPunch;
                muteTrackNow = recContext->muteTargetNow;
                // R8: the behaviour is read live as well, so the host can lift the mute mid-take (a post-roll after STOP:
                // the take has ended for the host but the device is still recording until the real stop)
                muteTrackContentsWhilstRecording = recContext->muteTrackContentsWhilstRecording
                                                    && t.edit.engine.getEngineBehaviour().muteTrackContentsWhilstRecording();
                isActivelyRecording = recContext->hasHitThreshold;
            }
        }

        if (muteTrackContentsWhilstRecording && isActivelyRecording)
            return true;

        if (isTrackRecordingWithPunch
            && muteTrackNow
            && getWaveInput().mergeMode == 1)
           return true;

        return false;
    }

    juce::AudioFormat* getFormatToUse() const
    {
        return edit.engine.getAudioFileFormatManager().getNamedFormat (getWaveInput().outputFormat);
    }

    static juce::File getDestinationRecordingFileToTry (Edit& ed, EditItemID targetID,
                                                        const juce::AudioFormat& format, juce::String filenameMask)
    {
        auto track = findTrackForID (ed, targetID);

        if (! track)
            if (auto cs = findClipSlotForID (ed, targetID))
                track = &cs->track;

        auto fileExtension = format.getFileExtensions()[0];

        if (track)
        {
            auto file = ed.engine.getEngineBehaviour().getFileForNewAudioRecording (*track, fileExtension);

            if (file != juce::File())
                return file;
        }

        for (int take = 0;;)
        {
            auto file = juce::File (expandPatterns (ed, filenameMask, track, ++take) + fileExtension);

            if (! file.exists())
               return file;
        }
    }

    static tl::expected<juce::File, juce::String> getDestinationRecordingFile (Edit& ed, EditItemID targetID,
                                                                               const juce::AudioFormat& format, juce::String filenameMask)
    {
        auto targetFile = getDestinationRecordingFileToTry (ed, targetID, format, filenameMask);

        if (! targetFile.getParentDirectory().createDirectory())
        {
            TRACKTION_LOG_ERROR ("Record fail: can't create parent directory: " + targetFile.getFullPathName());

            return TRANS("The directory\nXZZX\ndoesn't exist")
                .replace ("XZZX", targetFile.getParentDirectory().getFullPathName());
        }

        if (! targetFile.getParentDirectory().hasWriteAccess())
        {
            TRACKTION_LOG_ERROR ("Record fail: directory is read-only: " + targetFile.getFullPathName());

            return TRANS("The directory\nXZZX\n doesn't have write-access")
                .replace ("XZZX", targetFile.getParentDirectory().getFullPathName());
        }

        if (! targetFile.deleteFile())
        {
            TRACKTION_LOG_ERROR ("Record fail: can't overwrite file: " + targetFile.getFullPathName());

            return TRANS("Can't overwrite the existing file:") + "\n" + targetFile.getFullPathName();
        }

        return targetFile;
    }

    tl::expected<std::unique_ptr<RecordingContext>, juce::String> prepareToRecordTarget (EditItemID targetID, TimeRange punchRange)
    {
        CRASH_TRACER
        TRACKTION_ASSERT_MESSAGE_THREAD

        JUCE_TRY
        {
            if (getContextForID (targetID))
                return tl::unexpected (TRANS("Recording already in progress"));

            if (auto proj = getProjectForEdit (edit))
                if (proj->isReadOnly())
                    return tl::unexpected (TRANS("The current project is read-only, so new clips can't be recorded into it!"));

            auto format = getFormatToUse();
            const auto res = getDestinationRecordingFile (edit, targetID, *format, getWaveInput().getFilenameMask());

            if (! res)
                return tl::unexpected (res.error());

            auto recordedFile = res.value();
            auto rc = std::make_unique<WaveRecordingContext> (context, targetID, recordedFile);
            rc->sampleRate = edit.engine.getDeviceManager().getSampleRate();

            juce::StringPairArray metadata;
            AudioFileUtils::addBWAVStartToMetadata (metadata, (SampleCount) tracktion::toSamples (punchRange.getStart(), rc->sampleRate));
            auto& wi = getWaveInput();

            if (wi.isTrackDevice())
                for (auto t : getAudioTracks (edit))
                    if (&t->getWaveInputDevice() == &wi)
                        { wi.setChannelConfiguration (t->getChannelConfiguration()); break; }

            rc->fileWriter = std::make_unique<AudioFileWriter> (AudioFile (edit.engine, recordedFile), format,
                                                                (int) wi.getChannels().getNumChannels(),
                                                                rc->sampleRate, wi.bitDepth, metadata, 0,
                                                                wi.getChannelSet());

            if (rc->fileWriter->isOpen())
            {
                CRASH_TRACER
                rc->firstRecCallback = true;

                {
                    // R8: real-time recording: everything the audio thread will need, sized here
                    auto& dm = edit.engine.getDeviceManager();
                    const int chans = std::max (1, wi.getChannelSet().size());
                    const int blockSize = std::max (16, dm.getBlockSize());
                    edit.engine.getWaveInputRecordingThread().prepareForRealtime (chans, blockSize, rc->sampleRate);

                    {
                        const std::unique_lock sl (contextLock);
                        recordingContexts.reserve (recordingContexts.size() + 8);
                    }

                    const int silenceSize = std::max (blockSize, 128);
                    if (r8Silence.getNumChannels() < chans || r8Silence.getNumSamples() < silenceSize)
                    {
                        r8Silence.setSize (std::max (chans, r8Silence.getNumChannels()), std::max (silenceSize, r8Silence.getNumSamples()));
                        r8Silence.clear();
                    }
                    inputBuffer.setSize (std::max (chans, inputBuffer.getNumChannels()),
                                         std::max (blockSize, inputBuffer.getNumSamples()), false, false, true);
                }

                const auto adjustSeconds = wi.getAdjustmentSeconds();
                rc->adjustSamples = (int) tracktion::toSamples (adjustSeconds, rc->sampleRate);

                if (! owner.isTrackDevice())
                    rc->adjustSamples += context.getLatencySamples();

                rc->adjustDurationAtStart = TimeDuration::fromSamples (rc->adjustSamples, rc->sampleRate);

                if (edit.recordingPunchInOut)
                {
                    rc->recordingWithPunch = true;

                    auto loopRange = context.transport.getLoopRange();
                    auto muteStart = std::max (punchRange.getStart(), loopRange.getStart());
                    auto muteEnd   = punchRange.getEnd();

                    if (punchRange.getStart() < loopRange.getEnd() - 0.5s)
                    {
                        punchRange = punchRange.withEnd (punchRange.getEnd() + 0.8s);
                        muteEnd = loopRange.getEnd();
                    }

                    rc->muteTimes = { muteStart, muteEnd };
                }

                rc->punchTimes = punchRange;
                rc->recordingBlockRange = rc->punchTimes.withEnd (rc->punchTimes.getEnd() + adjustSeconds);
                rc->hasHitThreshold = (wi.recordTriggerDb <= -50.0f);

                if (edit.engine.getUIBehaviour().shouldGenerateLiveWaveformsWhenRecording())
                {
                    if ((rc->thumbnail = edit.engine.getRecordingThumbnailManager().getThumbnailFor (recordedFile)))
                    {
                        rc->thumbnail->reset ((int) wi.getChannels().getNumChannels(), rc->sampleRate);
                        rc->thumbnail->punchInTime = punchRange.getStart();
                    }
                }

                return rc;
            }
            else
            {
                TRACKTION_LOG_ERROR ("Record fail: couldn't write to file: " + recordedFile.getFullPathName());

                return tl::unexpected (TRANS("Couldn't record!") + "\n\n"
                        + TRANS("Couldn't create the file: XZZX").replace ("XZZX", recordedFile.getFullPathName()));
            }
        }
        JUCE_CATCH_EXCEPTION

        return tl::unexpected (TRANS("Unable to start recording"));
    }

    std::vector<tl::expected<std::unique_ptr<RecordingContext>, juce::String>> prepareToRecord (RecordingParameters params) override
    {
        CRASH_TRACER
        TRACKTION_ASSERT_MESSAGE_THREAD
        assert (params.punchRange.getEnd() < TimePosition::fromSeconds (std::numeric_limits<double>::max()));

        std::vector<tl::expected<std::unique_ptr<RecordingContext>, juce::String>> results;

        if (params.targets.empty())
            for (auto dest : destinations)
                if (dest->recordEnabled)
                    params.targets.push_back (dest->targetID);

        for (auto target : params.targets)
            results.emplace_back (prepareToRecordTarget (target, params.punchRange));

        return results;
    }

    std::vector<std::unique_ptr<RecordingContext>> startRecording (std::vector<std::unique_ptr<RecordingContext>> newContexts) override
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        bool hasAddedContexts = false;

        for (auto& recContext : newContexts)
        {
            if (auto midiContext = dynamic_cast<WaveRecordingContext*> (recContext.get()))
            {
                const auto targetID = midiContext->targetID;

                {
                    const std::unique_lock sl (contextLock);
                    recordingContexts.push_back (std::unique_ptr<WaveRecordingContext> (midiContext));
                    r8NumContexts.store ((int) recordingContexts.size(), std::memory_order_release);    // R8: real-time recording
                }

                hasAddedContexts = true;
                recContext.release();
                context.transport.callRecordingAboutToStartListeners (*this, targetID);
            }
        }

        if (hasAddedContexts)
        {
            // Close the meter gate so the user gets the same "waiting for trigger"
            // visual feedback during recording as they do when auditioning the
            // trigger level on the audio devices settings page.
            if (getWaveInput().recordTriggerDb > -50.0f)
                getWaveInput().setMeterGateClosed (true);

            if (! edit.getTransport().isPlaying())
                edit.getTransport().play (false);
        }

        // Remove now empty contents and return the rest
        return std::move (erase_if_null (newContexts));
    }

    TimePosition getPunchInTime (EditItemID targetID) override
    {
        {
            const std::shared_lock sl (contextLock);

            for (auto& recContext : recordingContexts)
                if (recContext->targetID == targetID)
                    return recContext->punchTimes.getStart();
        }

        return edit.getTransport().getTimeWhenStarted();
    }

    bool isRecording (EditItemID targetID) override
    {
        return getContextForID (targetID) != nullptr;
    }

    bool isRecording() override
    {
        const std::shared_lock sl (contextLock);
        return ! recordingContexts.empty();
    }

    void prepareToStopRecording (std::vector<EditItemID> targetsToStop) override
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        const std::shared_lock sl (contextLock);

        for (auto& recContext : recordingContexts)
        {
            if (! targetsToStop.empty())
                if (! contains_v (targetsToStop, recContext->targetID))
                    continue;

            recContext->stopRecording();
        }
    }

    tl::expected<Clip::Array, juce::String> stopRecording (StopRecordingParameters params) override
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        // Reserve to avoid allocating whilst
        const auto numContextsRecording = [this]
                                          {
                                              const std::shared_lock sl (contextLock);
                                              return recordingContexts.size();
                                          }();

        std::vector<std::unique_ptr<WaveRecordingContext>> contextsToStop;
        contextsToStop.reserve (numContextsRecording);

        // Stop the relevant contexts
        {
            const std::unique_lock sl (contextLock);

            for (auto& recContext : recordingContexts)
            {
                if (! params.targetsToStop.empty())
                    if (! contains_v (params.targetsToStop, recContext->targetID))
                        continue;

                recContext->unloopedStopTime = params.unloopedTimeToEndRecording;
                contextsToStop.push_back (std::move (recContext));
            }

            // Erase any now-empty contexts
            erase_if_null (recordingContexts);
            r8NumContexts.store ((int) recordingContexts.size(), std::memory_order_release);    // R8: real-time recording
            assert ((recordingContexts.size() + contextsToStop.size()) == numContextsRecording);
        }

        // Now apply those stop contexts whilst not holding the lock
        Clip::Array clips;
        juce::String error;

        for (auto& recContext : contextsToStop)
        {
            const auto targetID = recContext->targetID;
            auto stopCallback = std::move (recContext->stopCallback);
            context.transport.callRecordingAboutToStopListeners (*this, targetID);
            auto res = applyRecording (std::move (recContext),
                                       params.unloopedTimeToEndRecording,
                                       params.isLooping, params.markedRange,
                                       params.discardRecordings);
            context.transport.callRecordingFinishedListeners (*this, targetID,
                                                              res.value_or (Clip::Array()));

            if (stopCallback)
                stopCallback (res);

            res.map ([&] (auto c) { clips.addArray (std::move (c)); })
               .map_error ([&] (auto err) { error = err; });
        }

        if (! error.isEmpty())
            return tl::unexpected (error);

        return clips;
    }

    void stopRecording (StopRecordingParameters params,
                        std::function<void (tl::expected<Clip::Array, juce::String>)> callback) override
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        // Reserve to avoid allocating whilst
        const auto getNumContextsRecording = [this]
                                             {
                                                const std::shared_lock sl (contextLock);
                                                return recordingContexts.size();
                                             };

        if (params.targetsToStop.empty())
        {
            params.targetsToStop.reserve (getNumContextsRecording());

            const std::shared_lock sl (contextLock);

            for (auto& recContext : recordingContexts)
                params.targetsToStop.push_back (recContext->targetID);
        }

        // Set the punch out time for the contexts
        {
            const std::unique_lock sl (contextLock);

            for (auto& recContext : recordingContexts)
            {
                if (! contains_v (params.targetsToStop, recContext->targetID))
                    continue;

                recContext->unloopedStopTime = params.unloopedTimeToEndRecording;

                // Set the recordingBlockRange to the stop time plus the adjust time when recording started as this number
                // of samples will have been dropped. To ensure the final file has the correct number of samples, we need
                // to record this many samples past the "real" end
                recContext->recordingBlockRange = recContext->recordingBlockRange.withEnd (recContext->unloopedStopTime
                                                                                           + recContext->adjustDurationAtStart);

                // Unlock whilst doing potentially allocating ops to avoid priority inversion
                {
                    contextLock.unlock();
                    recContext->stopCallback = callback;
                    recContext->stopParams = params;
                    recContext->stopParams.targetsToStop = { recContext->targetID };
                    contextLock.lock();
                }
            }
        }

        // Add the rec context to a timer list to poll if the recording can be stopped
        for (auto targetID : params.targetsToStop)
            getRecordStopper().addTargetToStop (targetID);
    }

    juce::File getRecordingFile (EditItemID targetID) const override
    {
        const std::shared_lock sl (contextLock);

        if (auto rc = getContextForID (targetID))
            return rc->file;

        return {};
    }

    struct WaveRecordingContext : public RecordingContext
    {
        WaveRecordingContext (EditPlaybackContext& epc, EditItemID targetID_, const juce::File& f)
            : RecordingContext (targetID_),
              editPlaybackContext (epc), file (f)
        {}

        EditPlaybackContext& editPlaybackContext;
        Engine& engine { editPlaybackContext.edit.engine };
        juce::File file;
        double sampleRate = 44100.0;
        TimeRange punchTimes;           /**< The Edit time range that the recorded clip should start/stop. */
        TimeRange muteTimes;            /**< The Edit time range that the destination track should be muted for. */
        TimeRange recordingBlockRange;  /**< The Edit time range that blocks should be recorded for.
                                             This might be different to the punch range as it accounts for device and graph latency. */
        TimePosition unloopedStopTime;  /**< When the reecording is stopped, this should be the end time. */
        TimeDuration adjustDurationAtStart;
        std::atomic<bool> hasHitThreshold { false }, isWaitingToClose { false };
        bool firstRecCallback = false, recordingWithPunch = false;
        int adjustSamples = 0;
        std::atomic<bool> muteTargetNow { false };
        const bool muteTrackContentsWhilstRecording = engine.getEngineBehaviour().muteTrackContentsWhilstRecording();

        std::mutex fileWriterLock;
        std::unique_ptr<AudioFileWriter> fileWriter;

        DiskSpaceCheckTask diskSpaceChecker { engine, file };
        RecordingThumbnailManager::Thumbnail::Ptr thumbnail;
        WaveInputRecordingThread::ScopedInitialiser threadInitialiser { engine.getWaveInputRecordingThread() };
        const detail::ScopedActiveRecordingDevice scopedActiveRecordingDevice { editPlaybackContext };

        std::function<void (tl::expected<Clip::Array, juce::String>)> stopCallback;
        StopRecordingParameters stopParams;

        void addBlockToRecord (const juce::AudioBuffer<float>& buffer, int start, int numSamples)
        {
            if (isWaitingToClose.load (std::memory_order_acquire))
                return;

            // R8: real-time recording: never wait here. The lock is only ever held by the close, which comes
            // after isWaitingToClose, so a failed try is a block that would have been thrown away anyway.
            if (! fileWriterLock.try_lock())
            {
                ++engine.getWaveInputRecordingThread().numBlocksNotQueued;
                return;
            }

            std::scoped_lock sl (std::adopt_lock, fileWriterLock);

            if (fileWriter != nullptr)
                engine.getWaveInputRecordingThread().addBlockToRecord (*fileWriter, buffer,
                                                                       start, numSamples, thumbnail);
        }

        void stopRecording()
        {
            assert (! isWaitingToClose.load (std::memory_order_acquire));
            isWaitingToClose.store (true, std::memory_order_release);
        }

        /** Blocks until there are no more pending samples to be written to this context.
            After the call, fileWriter will be nullptr and no more blocks should be added to this.
        */
        void closeFileWriter()
        {
            assert (isWaitingToClose);

            CRASH_TRACER
            auto extractedFileWriter = [this]
            {
                std::scoped_lock sl (fileWriterLock);
                return std::move (fileWriter);
            }();

            if (extractedFileWriter)
                engine.getWaveInputRecordingThread().waitForWriterToFinish (*extractedFileWriter);
        }
    };

    tl::expected<Clip::Array, juce::String> applyRecording (std::unique_ptr<WaveRecordingContext> rc,
                                                            TimePosition unloopedEndTime,
                                                            bool isLooping, TimeRange loopRange,
                                                            bool discardRecordings)
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        CRASH_TRACER

        if (! rc || discardRecordings)
            for (auto c : consumers)
                c->discardRecordings (rc ? rc->targetID : EditItemID());

        if (! rc)
            return {};

        rc->closeFileWriter();

        // If we didn't get as far as adding any samples, delete the header of the file that will have been written
        if (rc->punchTimes.getStart() >= context.getUnloopedPosition())
        {
            rc->file.deleteFile();
            return {};
        }

        if (! rc->file.existsAsFile() || rc->file.getSize() == 0)
            return {};

        const AudioFile recordedFile (edit.engine, rc->file);
        auto clipOwner = findClipOwnerForID (edit, rc->targetID);

        if (discardRecordings || ! clipOwner)
        {
            recordedFile.deleteFile();
            return {};
        }

        // Never loop or punch record to slots
        const bool isClipSlot = dynamic_cast<ClipSlot*> (clipOwner) != nullptr;
        const bool wasPunchRecording = isClipSlot ? false : edit.recordingPunchInOut;
        const bool wasLoopRecording = isClipSlot ? false : isLooping;

        return applyLastRecording (*rc, recordedFile, *clipOwner,
                                   { rc->punchTimes.getStart(), unloopedEndTime },
                                   wasLoopRecording, wasPunchRecording, loopRange.getEnd());
    }

    tl::expected<Clip::Array, juce::String> applyLastRecording (const WaveRecordingContext& rc,
                                                                const AudioFile& recordedFile, ClipOwner& destClipOwner,
                                                                TimeRange recordedRange,
                                                                bool isLooping, bool isPunching, TimePosition loopEnd)
    {
        CRASH_TRACER
        auto& engine = edit.engine;
        auto& afm = engine.getAudioFileManager();
        afm.forceFileUpdate (recordedFile);

        auto recordedFileLength = TimeDuration::fromSeconds (recordedFile.getLength());

        if (recordedFileLength <= 1ms)
            return {};

        auto newClipLen = std::min (recordedFileLength,
                                    recordedRange.getLength());

        if (newClipLen <= 0.01s)
        {
            CRASH_TRACER
            juce::String s;

            if (! rc.hasHitThreshold)
                s = TRANS("The device \"XZZX\" \nnever reached the trigger threshold set for recording (THRX).")
                      .replace ("XZZX", getWaveInput().getName())
                      .replace ("THRX", gainToDbString (dbToGain (getWaveInput().recordTriggerDb)));
            else if (isPunching && rc.punchTimes.getStart() >= recordedRange.getEnd())
                s = TRANS("The device \"XZZX\" \nnever got as far as the punch-in marker, so didn't start recording!")
                      .replace ("XZZX", getWaveInput().getName());
            else
                s = TRANS("The device \"XZZX\" \nrecorded a zero-length file which won't be added to the edit")
                      .replace ("XZZX", getWaveInput().getName());

            recordedFile.deleteFile();

            return tl::unexpected (s);
        }

        if (auto proj = getProjectForEdit (edit))
        {
            if (auto projectItem = proj->createNewItem (recordedFile.getFile(),
                                                        ProjectItem::waveItemType(),
                                                        recordedFile.getFile().getFileNameWithoutExtension(),
                                                        {}, ProjectItem::Category::recorded, true))
            {
                return applyLastRecording (rc, projectItem, recordedFile, destClipOwner,
                                           recordedFileLength, newClipLen, isLooping, isPunching, loopEnd);
            }

            return tl::unexpected (proj->isReadOnly() ? TRANS("Couldn't add the new recording to the project, because the project is read-only")
                                                      : TRANS("Couldn't add the new recording to the project!"));
        }

        return applyLastRecording (rc, nullptr, recordedFile, destClipOwner,
                                    recordedFileLength, newClipLen, isLooping, isPunching, loopEnd);
    }

    tl::expected<Clip::Array, juce::String> applyLastRecording (const WaveRecordingContext& rc, const ProjectItem::Ptr projectItem,
                                                                const AudioFile& recordedFile, ClipOwner& destClipOwner,
                                                                TimeDuration recordedFileLength, TimeDuration newClipLen,
                                                                bool isLooping, bool isPunching, TimePosition loopEnd)
    {
        CRASH_TRACER
        jassert (projectItem == nullptr || projectItem->getProjectItemRef().isValid());
        auto& engine = edit.engine;
        auto& afm = engine.getAudioFileManager();

        juce::ReferenceCountedArray<ProjectItem> extraTakes;
        juce::Array<juce::File> filesCreated;
        filesCreated.add (recordedFile.getFile());

        if (isLooping)
            if (! splitRecordingIntoMultipleTakes (context, recordedFile, projectItem, recordedFileLength, extraTakes, filesCreated))
                return tl::unexpected (TRANS("Couldn't create audio files for multiple takes"));

        auto endPos = rc.punchTimes.getStart() + newClipLen;

        if (isPunching || isLooping)
            endPos = juce::jlimit (rc.punchTimes.getStart() + 0.5s, loopEnd, endPos);

        Clip::Ptr newClip;
        bool replaceOldClips = getWaveInput().mergeMode == 1;
        const auto loopRange = edit.getTransport().getLoopRange();

        if (replaceOldClips && isPunching)
        {
            if (projectItem != nullptr)
                newClip = insertWaveClip (destClipOwner, getNameForNewClip (destClipOwner), projectItem->getProjectItemRef(),
                                          { { loopRange.getStart(), endPos }, {} }, DeleteExistingClips::yes);
            else
                newClip = insertWaveClip (destClipOwner, getNameForNewClip (destClipOwner), recordedFile.getFile(),
                                          { { loopRange.getStart(), endPos }, {} }, DeleteExistingClips::yes);

            if (newClip != nullptr)
                newClip->setStart (rc.punchTimes.getStart(), false, false);
        }
        else
        {
            if (projectItem != nullptr)
                newClip = insertWaveClip (destClipOwner, getNameForNewClip (destClipOwner), projectItem->getProjectItemRef(),
                                          { { rc.punchTimes.getStart(), endPos }, {} },
                                          replaceOldClips ? DeleteExistingClips::yes : DeleteExistingClips::no);
            else
                newClip = insertWaveClip (destClipOwner, getNameForNewClip (destClipOwner), recordedFile.getFile(),
                                         { { rc.punchTimes.getStart(), endPos }, {} },
                                         replaceOldClips ? DeleteExistingClips::yes : DeleteExistingClips::no);
        }

        if (newClip == nullptr)
        {
            juce::String s ("Couldn't insert new clip after recording: ");
            s << rc.punchTimes.getStart() << " " << rc.punchTimes.getStart()
              << " " << endPos << " " << getWaveInput().getAdjustmentSeconds()
              << " " << recordedFileLength;

            TRACKTION_LOG_ERROR (s);

            engine.getUIBehaviour().showWarningMessage (TRANS("Couldn't add the new recording to the project!"));
            return {};
        }

        CRASH_TRACER

        if (isPunching)
        {
            if (newClip->getPosition().getStart() < loopRange.getStart())
                newClip->setStart (loopRange.getStart(), true, false);

            if (newClip->getPosition().getEnd() > loopRange.getEnd()
                 && newClip->getPosition().getStart() < loopRange.getEnd() - 0.1s)
                newClip->setEnd (loopRange.getEnd(), true);
        }

        for (auto& f : filesCreated)
        {
            AudioFileUtils::applyBWAVStartTime (f, (SampleCount) tracktion::toSamples (newClip->getPosition().getStartOfSource(), rc.sampleRate));
            afm.forceFileUpdate (AudioFile (edit.engine, f));
        }

        if (auto wc = dynamic_cast<WaveAudioClip*> (newClip.get()))
        {
            if (extraTakes.size() > 0)
            {
                for (auto& take : extraTakes)
                    wc->addTake (take->getProjectItemRef());
            }
            else if (filesCreated.size() > 1)
            {
                for (auto& f : filesCreated)
                    wc->addTake (f);
            }
        }

        Clip::Array clips;
        clips.add (newClip.get());
        return clips;
    }

    static bool splitRecordingIntoMultipleTakes (EditPlaybackContext& epc,
                                                 const AudioFile& recordedFile,
                                                 const ProjectItem::Ptr& projectItem,
                                                 TimeDuration recordedFileLength,
                                                 juce::ReferenceCountedArray<ProjectItem>& extraTakes,
                                                 juce::Array<juce::File>& filesCreated)
    {
        auto& edit = epc.edit;
        auto& afm = edit.engine.getAudioFileManager();

        // break the wave into separate takes..
        if (projectItem != nullptr)
            extraTakes.add (projectItem);

        int take = 1;
        auto loopLength = epc.transport.getLoopRange().getLength();

        for (;;)
        {
            const auto takeStart    = toPosition (loopLength * take);
            const auto takeEnd      = toPosition (std::min (loopLength * (take + 1),
                                                            recordedFileLength));

            if ((takeEnd - takeStart) < 0.1s)
                break;

            const auto takeRange = TimeRange (takeStart, takeEnd);
            auto takeFile = recordedFile.getFile()
                             .getSiblingFile (recordedFile.getFile().getFileNameWithoutExtension()
                                                + "_take_" + juce::String (take + 1))
                             .withFileExtension (recordedFile.getFile().getFileExtension())
                             .getNonexistentSibling (false);

            afm.releaseFile (recordedFile);

            if (AudioFileUtils::copySectionToNewFile (edit.engine, recordedFile.getFile(), takeFile, takeRange) < 0)
                return false;

            if (projectItem != nullptr)
            {
                if (auto takeObject = projectItem->getProject()->createNewItem (takeFile, ProjectItem::waveItemType(),
                                                                                recordedFile.getFile().getFileNameWithoutExtension()
                                                                                  + " #" + juce::String (take + 1),
                                                                                {},
                                                                                ProjectItem::Category::recorded, true))
                {
                    extraTakes.add (takeObject);
                    filesCreated.add (takeFile);
                }
            }
            else
            {
                filesCreated.add (takeFile);
            }

            ++take;
        }

        // chop down the original wave file..
        auto tempFile = recordedFile.getFile().getNonexistentSibling (false);

        if (AudioFileUtils::copySectionToNewFile (edit.engine, recordedFile.getFile(), tempFile, TimeRange (0.0s, loopLength)) > 0)
        {
            afm.releaseFile (recordedFile);
            tempFile.moveFileTo (recordedFile.getFile());
            filesCreated.add (recordedFile.getFile());
            afm.forceFileUpdate (recordedFile);

            if (projectItem != nullptr)
                projectItem->verifyLength();

            return true;
        }

        return false;
    }

    juce::Array<Clip*> applyRetrospectiveRecord (bool armedOnly) override
    {
        juce::Array<Clip*> clips;

        for (auto dstTrack : getTargetTracks (*this))
        {
            if (armedOnly && ! isRecordingActive (dstTrack->itemID))
                continue;

            auto& wi = getWaveInput();

            auto recordBuffer = wi.getRetrospectiveRecordBuffer();

            if (recordBuffer == nullptr)
                return nullptr;

            auto format = getFormatToUse();
            const auto res = getDestinationRecordingFile (edit, dstTrack->itemID, *format, getWaveInput().getFilenameMask());

            if (! res)
                return {};

            const auto recordedFile = res.value();
            juce::StringPairArray metadata;

            {
                AudioFileWriter writer (AudioFile (dstTrack->edit.engine, recordedFile), format,
                                        recordBuffer->numChannels,
                                        recordBuffer->sampleRate,
                                        wi.bitDepth, metadata, 0);

                if (writer.isOpen())
                {
                    int numReady;
                    juce::AudioBuffer<float> scratchBuffer (recordBuffer->numChannels, 1000);

                    while ((numReady = recordBuffer->fifo.getNumReady()) > 0)
                    {
                        auto toRead = std::min (numReady, scratchBuffer.getNumSamples());

                        if (! recordBuffer->fifo.read (scratchBuffer, 0, toRead)
                             || ! writer.appendBuffer (scratchBuffer, toRead))
                            return nullptr;
                    }
                }
            }

            auto proj = getProjectForEdit (edit);

            if (proj == nullptr)
            {
                jassertfalse; // TODO
                return nullptr;
            }

            auto projectItem = proj->createNewItem (recordedFile, ProjectItem::waveItemType(),
                                                    recordedFile.getFileNameWithoutExtension(),
                                                    {}, ProjectItem::Category::recorded, true);

            if (projectItem == nullptr)
                continue;

            jassert (projectItem->getProjectItemRef().isValid());

            auto clipName = getNameForNewClip (*dstTrack);
            TimePosition start;
            const auto recordedLength = TimeDuration::fromSeconds (AudioFile (dstTrack->edit.engine, recordedFile).getLength());

            if (context.isPlaying() || recordBuffer->wasRecentlyPlaying (edit))
            {
                const auto blockSizeSeconds = edit.engine.getDeviceManager().getBlockLength();
                auto adjust = -wi.getAdjustmentSeconds() + blockSizeSeconds;

                adjust = adjust - TimeDuration::fromSamples (context.getLatencySamples(), edit.engine.getDeviceManager().getSampleRate());

                // TODO: Still not quite sure why the adjustment needs to be a block more with
                // the tracktion_graph engine, this may need correcting in the future
                if (context.getNodePlayHead() != nullptr)
                    adjust = adjust + blockSizeSeconds;

                if (context.isPlaying())
                {
                    start = context.globalStreamTimeToEditTime (recordBuffer->lastStreamTime) - recordedLength + adjust;
                }
                else
                {
                    auto& pei = recordBuffer->editInfo[edit.getProjectItemRef()];
                    start = pei.lastEditTime + pei.pausedTime - recordedLength + adjust;
                    pei.lastEditTime = -1s;
                }
            }
            else
            {
                auto position = context.getPosition();

                if (position >= 5s)
                    start = position - recordedLength;
                else
                    start = std::max<TimePosition> (0.0s, position);
            }

            ClipPosition clipPos = { { start, start + recordedLength }, {} };

            if (start < 0s)
            {
                clipPos.offset = toDuration (-start);
                clipPos.time = clipPos.time.withStart (0s);
            }

            auto newClip = dstTrack->insertWaveClip (clipName, projectItem->getProjectItemRef(), clipPos, false);

            if (newClip == nullptr)
                continue;

            CRASH_TRACER

            AudioFileUtils::applyBWAVStartTime (recordedFile, (SampleCount) tracktion::toSamples (newClip->getPosition().getStartOfSource(), recordBuffer->sampleRate));

            edit.engine.getAudioFileManager().forceFileUpdate (AudioFile (dstTrack->edit.engine, recordedFile));

            if (dstTrack->playSlotClips.get())
            {
                if (auto slot = getFreeSlot (*dstTrack))
                {
                    prepareClipForLauncher (*newClip);

                    newClip->removeFromParent();
                    slot->setClip (newClip.get());
                }
            }

            clips.add (newClip.get());
        }

        return clips;
    }

    void copyIncomingDataIntoBuffer (const float* const* allChannels, int numChannels, int numSamples)
    {
        auto& wi = getWaveInput();
        auto& channelSet = wi.getChannelSet();
        inputBuffer.setSize (channelSet.size(), numSamples, false, false, true);   // R8: real-time recording: sized when the recording is prepared

        if (numChannels == 0)
        {
            inputBuffer.clear();
            return;
        }

        for (const auto& ci : wi.getChannels())
        {
            if (juce::isPositiveAndBelow (ci.indexInDevice, numChannels))
            {
                auto inputIndex = channelSet.getChannelIndexForType (ci.channel);
                juce::FloatVectorOperations::copy (inputBuffer.getWritePointer (inputIndex),
                                                   allChannels[ci.indexInDevice], numSamples);
            }
            else
            {
                jassertfalse; // Is an input device getting created with more channels than the total number of device channels?
            }
        }
    }

    void acceptInputBuffer (const float* const* allChannels, int numChannels, int numSamples,
                            double streamTime, LevelMeasurer* measurerToUpdate,
                            RetrospectiveRecordBuffer* retrospectiveBuffer, bool addToRetrospective)
    {
        CRASH_TRACER
        copyIncomingDataIntoBuffer (allChannels, numChannels, numSamples);

        auto inputGainDb = getWaveInput().inputGainDb;

        if (inputGainDb > 0.01f || inputGainDb < -0.01f)
            inputBuffer.applyGain (0, numSamples, dbToGain (inputGainDb));

        if (measurerToUpdate != nullptr)
            getWaveInput().feedLevelMeasurerThroughGate (inputBuffer, 0, numSamples);

        if (retrospectiveBuffer != nullptr)
        {
            if (addToRetrospective)
            {
                retrospectiveBuffer->updateSizeIfNeeded (inputBuffer.getNumChannels(),
                                                         edit.engine.getDeviceManager().getSampleRate());
                retrospectiveBuffer->processBuffer (streamTime, inputBuffer, numSamples);
            }

            retrospectiveBuffer->syncToEdit (edit, context, streamTime, numSamples);
        }

        {
            const juce::ScopedLock sl (consumerLock);

            for (auto n : consumers)
                n->acceptInputBuffer (choc::buffer::createChannelArrayView (inputBuffer.getArrayOfWritePointers(),
                                                                            (choc::buffer::ChannelCount) inputBuffer.getNumChannels(),
                                                                            (choc::buffer::FrameCount) numSamples));
        }

        // If we haven't actually started playing yet, don't record the block as we
        // might get more than one block for the same position
        if (! context.isPlaying())
            return;

        {
            // R8: a position posted but not applied yet is applied later in this callback (fillNextNodeBlock), so this
            // block belongs there, not at the play head's old place. A take started behind where the play head stood
            // (or a count-in's pre-roll) got the gap as silence at its start when the position change missed the
            // start of play (performRecord's wait for it gives up after 100 ms); a take ahead of it lost a block.
            // R8: and a block reached while already playing (a count-in's pre-roll, a punch-in) belongs where this
            // callback plays it. globalStreamTimeToEditTimeUnlooped (streamTime) is one block behind that while
            // playing (the play head's reference range runs one block ahead of the stream time), so a take that
            // started while playing landed one block earlier than the same take started from a stop.
            auto blockStart = context.getNextBlockStartUnlooped();

            if (const auto pending = context.getPendingImmediatePosition())
                blockStart = *pending;

            // R8: real-time recording: never wait for the message thread (it holds the lock while a take starts or
            // stops); a block skipped while a take is running is counted, never lost silently
            if (! contextLock.try_lock_shared())
            {
                if (r8NumContexts.load (std::memory_order_acquire) > 0)
                    ++edit.engine.getWaveInputRecordingThread().numBlocksNotQueued;

                return;
            }

            const std::shared_lock sl (contextLock, std::adopt_lock);

            for (auto& recordingContext : recordingContexts)
            {
                const TimeRange blockRange (blockStart, TimeDuration::fromSamples (numSamples, recordingContext->sampleRate));

                recordingContext->muteTargetNow = recordingContext->muteTimes.overlaps (blockRange);

                if (recordingContext->recordingBlockRange.overlaps (blockRange))
                {
                    if (! recordingContext->hasHitThreshold)
                    {
                        auto bufferLevelDb = gainToDb (inputBuffer.getMagnitude (0, numSamples));
                        recordingContext->hasHitThreshold = bufferLevelDb > getWaveInput().recordTriggerDb;

                        if (! recordingContext->hasHitThreshold)
                            return;

                        recordingContext->punchTimes = recordingContext->punchTimes.withStart (blockRange.getStart());

                        if (recordingContext->thumbnail != nullptr)
                            recordingContext->thumbnail->punchInTime = blockRange.getStart();
                    }

                    if (recordingContext->firstRecCallback)
                    {
                        recordingContext->firstRecCallback = false;

                        auto timeDiff = blockRange.getStart() - recordingContext->recordingBlockRange.getStart();
                        recordingContext->adjustSamples -= (int) tracktion::toSamples (timeDiff, recordingContext->sampleRate);
                    }

                    const int adjustSamples = recordingContext->adjustSamples;

                    if (adjustSamples < 0)
                    {
                        // add silence (R8: real-time recording: from a buffer sized beforehand, a block at a time)
                        if (r8Silence.getNumChannels() < inputBuffer.getNumChannels() || r8Silence.getNumSamples() < 1)
                        {
                            ++edit.engine.getWaveInputRecordingThread().numAudioThreadAllocations;
                            r8Silence.setSize (inputBuffer.getNumChannels(), std::max (r8Silence.getNumSamples(), 512));
                            r8Silence.clear();
                        }

                        for (int left = -adjustSamples; left > 0;)
                        {
                            const int n = std::min (left, r8Silence.getNumSamples());
                            recordingContext->addBlockToRecord (r8Silence, 0, n);
                            left -= n;
                        }

                        recordingContext->adjustSamples = 0;
                    }
                    else if (adjustSamples > 0)
                    {
                        // drop samples
                        if (adjustSamples >= numSamples)
                        {
                            recordingContext->adjustSamples -= numSamples;
                        }
                        else
                        {
                            recordingContext->addBlockToRecord (inputBuffer, adjustSamples, numSamples - adjustSamples);
                            recordingContext->adjustSamples = 0;
                        }
                    }
                    else
                    {
                        recordingContext->addBlockToRecord (inputBuffer, 0, numSamples);
                    }
                }
            }
        }
    }

protected:
    mutable std::shared_mutex contextLock;
    std::vector<std::unique_ptr<WaveRecordingContext>> recordingContexts;
    std::atomic<int> r8NumContexts { 0 };    // R8: real-time recording: how many takes are running (read without the lock)
    std::unique_ptr<RecordStopper> recordStopper;

    juce::AudioBuffer<float> inputBuffer;
    juce::AudioBuffer<float> r8Silence;    // R8: real-time recording: punch-in silence, sized when a recording is prepared

    WaveRecordingContext* getContextForID (EditItemID targetID) const
    {
        const std::shared_lock sl (contextLock);

        for (auto& recContext : recordingContexts)
            if (recContext->targetID == targetID)
                return recContext.get();

        return nullptr;
    }

    RecordStopper& getRecordStopper()
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        if (! recordStopper)
            recordStopper = std::make_unique<RecordStopper> ([this] (auto targetID)
                                                             {
                                                                 const auto unloopedTimeNow = context.getUnloopedPosition();

                                                                 const std::shared_lock sl (contextLock);

                                                                 if (auto recContext = getContextForID (targetID))
                                                                 {
                                                                     if (unloopedTimeNow >= recContext->recordingBlockRange.getEnd())
                                                                     {
                                                                         auto stopParams = recContext->stopParams;
                                                                         recContext->stopRecording();

                                                                         // Temp unlock as stopRecording takes a unique lock
                                                                         contextLock.unlock_shared();
                                                                         auto res = stopRecording (stopParams);
                                                                         contextLock.lock_shared();

                                                                         return RecordStopper::HasFinished::yes;
                                                                     }
                                                                 }

                                                                  return RecordStopper::HasFinished::no;
                                                             });

        return *recordStopper;
    }

    WaveInputDevice& getWaveInput() const noexcept    { return static_cast<WaveInputDevice&> (owner); }

    //==============================================================================
    juce::Array<Consumer*> consumers;
    juce::CriticalSection consumerLock;

    void addConsumer (Consumer* consumer) override
    {
        const juce::ScopedLock sl (consumerLock);
        consumers.addIfNotAlreadyThere (consumer);
    }

    void removeConsumer (Consumer* consumer) override
    {
        const juce::ScopedLock sl (consumerLock);
        consumers.removeAllInstancesOf (consumer);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveInputDeviceInstance)
};

//==============================================================================
WaveInputDevice::WaveInputDevice (Engine& e, const WaveDeviceDescription& desc, DeviceType t)
    : InputDevice (e, desc.name, "wavein_" + juce::String::toHexString (desc.name.hashCode())),
      deviceDescription (desc),
      deviceType (t),
      channelSet (desc.channels.toChannelSet())
{
    enabled = desc.enabled;
    loadProps();
    r8IdleBuffer.setSize (std::max (1, channelSet.size()), std::max (512, e.getDeviceManager().getBlockSize()));   // R8: real-time recording
}

WaveInputDevice::~WaveInputDevice()
{
    notifyListenersOfDeletion();
    closeDevice();
}

void WaveInputDevice::setChannelConfiguration (const ChannelConfiguration& newConfig)
{
    deviceDescription.channels = newConfig;
    channelSet = newConfig.toChannelSet();
}

juce::String WaveInputDevice::getDeviceTypeDescription() const
{
    if (deviceType == trackWaveDevice)
        return TRANS("Track Wave Input");

    return TRANS("Wave Audio Input");
}

juce::StringArray WaveInputDevice::getMergeModes()
{
    juce::StringArray s;
    s.add (TRANS("Overlay newly recorded clips onto edit"));
    s.add (TRANS("Replace old clips in edit with new ones"));
    s.add (TRANS("Don't make recordings from this device"));

    return s;
}

juce::StringArray WaveInputDevice::getRecordFormatNames()
{
    juce::StringArray s;

    auto& afm = engine.getAudioFileFormatManager();
    s.add (afm.getWavFormat()->getFormatName());
    s.add (afm.getAiffFormat()->getFormatName());
    s.add (afm.getFlacFormat()->getFormatName());

    return s;
}

InputDeviceInstance* WaveInputDevice::createInstance (EditPlaybackContext& ed)
{
    // R8: retrospective buffer. Sized here, on the main thread, not on the audio thread at the first play (with many
    // inputs that is a large memory clear there, and a click); again when no session is using it (a new sample rate)
    if (! isTrackDevice())
    {
        const auto rate = ed.edit.engine.getDeviceManager().getSampleRate();
        const int chans = getChannelSet().size();
        if (retrospectiveBuffer == nullptr)
        {
            auto b = std::make_unique<RetrospectiveRecordBuffer> (ed.edit.engine);
            if (rate > 0 && chans > 0)
                b->updateSizeIfNeeded (chans, rate);
            retrospectiveBuffer = std::move (b);
        }
        else if (instances.isEmpty() && rate > 0 && chans > 0)
        {
            retrospectiveBuffer->updateSizeIfNeeded (chans, rate);
        }
    }

    return new WaveInputDeviceInstance (*this, ed);
}

void WaveInputDevice::resetToDefault()
{
    juce::String propName = isTrackDevice() ? "TRACKTION_TRACK_DEVICE" : getName();
    engine.getPropertyStorage().removePropertyItem (SettingID::wavein, propName);
    loadProps();
}

void WaveInputDevice::setEnabled (bool b)
{
    if (enabled != b)
    {
        enabled = b;
        changed();

        if (! isTrackDevice())
        {
            engine.getDeviceManager().setDeviceEnabled (*this, b);
        }
        else
        {
            // reload our properties in case another track device has changed them
            loadProps();
        }
        // do nothing now! this object is probably deleted..
    }
}

juce::String WaveInputDevice::openDevice()
{
    return {};
}

void WaveInputDevice::closeDevice()
{
    saveProps();
}

void WaveInputDevice::loadProps()
{
    filenameMask = {};
    inputGainDb = 0.0f;
    monitorMode = MonitorMode::automatic;
    outputFormat = engine.getAudioFileFormatManager().getDefaultFormat()->getFormatName();

    recordTriggerDb = -50.0f;
    mergeMode = 0;
    bitDepth = 24;
    recordAdjustMs = 0;

    juce::String propName = isTrackDevice() ? "TRACKTION_TRACK_DEVICE" : getName();

    if (auto n = engine.getPropertyStorage().getXmlPropertyItem (SettingID::wavein, propName))
    {
        filenameMask = n->getStringAttribute ("filename", filenameMask);

        if (filenameMask == engine.getEngineBehaviour().getDefaultAudioRecordingFilePattern())
            filenameMask = {};

        inputGainDb = (float) n->getDoubleAttribute ("gainDb", inputGainDb);
        monitorMode = magic_enum::enum_cast<MonitorMode> (n->getStringAttribute ("monitorMode").toStdString()).value_or (MonitorMode::automatic);

        outputFormat = n->getStringAttribute ("format", outputFormat);
        bitDepth = n->getIntAttribute ("bits", bitDepth);

        if (! getRecordFormatNames().contains (outputFormat))
            outputFormat = engine.getAudioFileFormatManager().getDefaultFormat()->getFormatName();

        recordTriggerDb = (float) n->getDoubleAttribute ("triggerDb", recordTriggerDb);
        mergeMode = n->getIntAttribute ("mode", mergeMode);
        recordAdjustMs = n->getDoubleAttribute ("adjustMs", recordAdjustMs);

        if (recordAdjustMs != 0)
            TRACKTION_LOG ("Manual record adjustment: " + juce::String (recordAdjustMs) + "ms");
    }

    checkBitDepth();
}

void WaveInputDevice::saveProps()
{
    juce::XmlElement n ("SETTINGS");

    if (filenameMask.isNotEmpty())
        n.setAttribute ("filename", filenameMask);

    n.setAttribute ("gainDb", inputGainDb);
    n.setAttribute ("monitorMode", std::string (magic_enum::enum_name (monitorMode)));
    n.setAttribute ("format", outputFormat);
    n.setAttribute ("bits", bitDepth);
    n.setAttribute ("triggerDb", recordTriggerDb);
    n.setAttribute ("mode", mergeMode);
    n.setAttribute ("adjustMs", recordAdjustMs);

    juce::String propName = isTrackDevice() ? "TRACKTION_TRACK_DEVICE" : getName();

    engine.getPropertyStorage().setXmlPropertyItem (SettingID::wavein, propName, n);
}

//==============================================================================
bool WaveInputDevice::isStereoPair() const
{
    return deviceDescription.getNumChannels() == 2;
}

void WaveInputDevice::setStereoPair (bool stereo)
{
    if (isTrackDevice())
    {
        jassertfalse;
        return;
    }

    engine.getDeviceManager().setDeviceNumChannels (*this, stereo ? 2 : 1);
}

static bool shouldIncludeChannelCount (uint32_t num, uint32_t totalOptions)
{
    if (num <= 8)            return true;
    if (num == 32)           return true;
    if (num == 64)           return true;
    if (num == 128)          return true;
    if (num == totalOptions) return true;
    return false;
}

static juce::String getChannelCountLabel (uint32_t num)
{
    auto canonical = ChannelConfiguration::canonical ((int) num);

    for (const auto& preset : getChannelConfigurationPresets())
        if (preset.config == canonical)
            return preset.name;

    return juce::String (num) + " " + TRANS("channels");
}


juce::PopupMenu WaveInputDevice::createChannelGroupMenu (bool includeSetAllChannelsOptions)
{
    juce::PopupMenu m;
    auto& dm = engine.getDeviceManager();
    auto currentNumChannels = deviceDescription.getNumChannels();
    auto options = dm.getPossibleChannelGroupsForDevice (*this, DeviceManager::maxNumChannelsPerDevice);
    auto totalOptions = (uint32_t) options.size();

    for (uint32_t num = 1; num <= totalOptions; ++num)
    {
        if (shouldIncludeChannelCount (num, totalOptions))
            m.addItem (getChannelCountLabel (num), true, num == currentNumChannels,
                       [this, &dm, num] { dm.setDeviceNumChannels (*this, num); });
    }

    juce::PopupMenu otherMenu;

    for (uint32_t num = 1; num <= totalOptions; ++num)
        if (! shouldIncludeChannelCount (num, totalOptions))
            otherMenu.addItem (getChannelCountLabel (num), true, num == currentNumChannels,
                               [this, &dm, num] { dm.setDeviceNumChannels (*this, num); });

    if (otherMenu.getNumItems() > 0)
    {
        m.addSeparator();
        m.addSubMenu (TRANS("Other"), otherMenu);
    }

    if (includeSetAllChannelsOptions)
    {
        m.addSeparator();
        m.addItem ("Set all to mono channels", [&dm] { dm.setAllWaveInputsToNumChannels (1); });
        m.addItem ("Set all to stereo pairs",  [&dm] { dm.setAllWaveInputsToNumChannels (2); });
    }

    return m;
}

void WaveInputDevice::setRecordAdjustment (TimeDuration d)
{
    setRecordAdjustmentMs (d.inSeconds() * 1000.0);
}

void WaveInputDevice::setRecordAdjustmentMs (double ms)
{
    recordAdjustMs = juce::jlimit (-500.0, 500.0, ms);
    changed();
    saveProps();
}

void WaveInputDevice::setInputGainDb (float newGain)
{
    if (newGain != inputGainDb)
    {
        inputGainDb = newGain;
        changed();
        saveProps();
    }
}

void WaveInputDevice::setRecordTriggerDb (float newDB)
{
    if (recordTriggerDb != newDB)
    {
        recordTriggerDb = newDB;
        changed();
        saveProps();
    }
}

void WaveInputDevice::setMeterGateClosed (bool shouldBeClosed)
{
    if (meterGateClosed.exchange (shouldBeClosed, std::memory_order_relaxed) != shouldBeClosed)
        changed();
}

bool WaveInputDevice::isAnyRecordingActive() const
{
    const juce::ScopedLock sl (instanceLock);

    for (auto i : instances)
        if (i->isRecording())
            return true;

    return false;
}

void WaveInputDevice::feedLevelMeasurerThroughGate (juce::AudioBuffer<float>& buf, int start, int numSamples)
{
    if (meterGateClosed.load (std::memory_order_relaxed))
    {
        // Use the same convention as WaveRecordingContext: a trigger of -50dB or below means "no gate".
        if (recordTriggerDb <= -50.0f
            || gainToDb (buf.getMagnitude (start, numSamples)) > recordTriggerDb)
            meterGateClosed.store (false, std::memory_order_relaxed);
        else
            return;
    }

    levelMeasurer.processBuffer (buf, start, numSamples);
}

juce::String WaveInputDevice::getFilenameMask() const
{
    if (filenameMask.isNotEmpty())
        return filenameMask;

    return engine.getEngineBehaviour().getDefaultAudioRecordingFilePattern();
}

void WaveInputDevice::setFilenameMask (const juce::String& newMask)
{
    if (filenameMask != newMask)
    {
        filenameMask = newMask;

        if (filenameMask == engine.getEngineBehaviour().getDefaultAudioRecordingFilePattern())
            filenameMask = {};

        changed();
        saveProps();
    }
}

void WaveInputDevice::setFilenameMaskToDefault()
{
    setFilenameMask ({});
}

void WaveInputDevice::setBitDepth (int newDepth)
{
    if (bitDepth != newDepth)
    {
        bitDepth = newDepth;
        changed();
        saveProps();
    }
}

void WaveInputDevice::checkBitDepth()
{
    if (! getAvailableBitDepths().contains (bitDepth))
        bitDepth = getAvailableBitDepths().getLast();
}

juce::Array<int> WaveInputDevice::getAvailableBitDepths() const
{
    return getFormatToUse()->getPossibleBitDepths();
}

juce::AudioFormat* WaveInputDevice::getFormatToUse() const
{
    return engine.getAudioFileFormatManager().getNamedFormat (outputFormat);
}

void WaveInputDevice::setOutputFormat (const juce::String& newFormat)
{
    if (outputFormat != newFormat)
    {
        outputFormat = newFormat;
        checkBitDepth();
        changed();
        saveProps();
    }
}

juce::String WaveInputDevice::getMergeMode() const
{
    return getMergeModes() [mergeMode];
}

void WaveInputDevice::setMergeMode (const juce::String& newMode)
{
    int newIndex = getMergeModes().indexOf (newMode);

    if (mergeMode != newIndex)
    {
        mergeMode = newIndex;
        changed();
        saveProps();
    }
}

TimeDuration WaveInputDevice::getAdjustmentSeconds()
{
    auto& dm = engine.getDeviceManager();
    const double autoAdjustMs = isTrackDevice() ? 0.0 : dm.getRecordAdjustmentMs();

    return TimeDuration::fromSeconds (juce::jlimit (-3.0, 3.0, 0.001 * (recordAdjustMs + autoAdjustMs)));
}

//==============================================================================
void WaveInputDevice::addInstance (WaveInputDeviceInstance* i)
{
    const juce::ScopedLock sl (instanceLock);
    instances.addIfNotAlreadyThere (i);
    r8PublishInstances();
}

void WaveInputDevice::removeInstance (WaveInputDeviceInstance* i)
{
    const juce::ScopedLock sl (instanceLock);
    instances.removeAllInstancesOf (i);
    r8PublishInstances();

    // a callback that already saw this instance finishes before it is destroyed
    while (r8CallbackBusy.load (std::memory_order_acquire) != 0)
        std::this_thread::yield();
}

// R8: real-time recording. Called with instanceLock held (message thread). Writes the list the callback reads.
void WaveInputDevice::r8PublishInstances()
{
    const int n = std::min (instances.size(), r8MaxInstances);
    jassert (instances.size() <= r8MaxInstances);
    r8NumInstances.store (0, std::memory_order_release);

    for (int k = 0; k < n; ++k)
        r8Instances[k].store (instances[k], std::memory_order_relaxed);

    r8NumInstances.store (n, std::memory_order_release);
}

//==============================================================================
void WaveInputDevice::consumeNextAudioBlock (const float* const* allChannels, int numChannels, int numSamples, double streamTime)
{
    if (enabled)
    {
        // R8: real-time recording: no lock here; the instance list is published by the message thread
        r8CallbackBusy.fetch_add (1, std::memory_order_acq_rel);
        const int n = r8NumInstances.load (std::memory_order_acquire);

        if (n == 0)
        {
            auto& groupChannelSet = getChannelSet();
            auto& buf = r8IdleBuffer;
            buf.setSize (groupChannelSet.size(), numSamples, false, false, true);   // sized in the constructor; allocates only if bigger
            buf.clear();

            for (const auto& ci : getChannels())
                if (juce::isPositiveAndBelow (ci.indexInDevice, numChannels))
                    juce::FloatVectorOperations::copy (buf.getWritePointer (groupChannelSet.getChannelIndexForType (ci.channel)),
                                                       allChannels[ci.indexInDevice], numSamples);

            if (inputGainDb > 0.01f || inputGainDb < -0.01f)
                buf.applyGain (0, numSamples, dbToGain (inputGainDb));

            feedLevelMeasurerThroughGate (buf, 0, numSamples);
        }
        else
        {
            bool isFirst = true;

            for (int k = 0; k < n; ++k)
            {
                auto* i = r8Instances[k].load (std::memory_order_relaxed);

                if (i == nullptr)
                    continue;

                i->acceptInputBuffer (allChannels, numChannels, numSamples, streamTime,
                                     isFirst ? &levelMeasurer : nullptr,
                                     (! retrospectiveRecordLock) ? retrospectiveBuffer.get() : nullptr,
                                     isFirst);
                isFirst = false;
            }
        }

        r8CallbackBusy.fetch_sub (1, std::memory_order_acq_rel);
    }
}

//==============================================================================
void WaveInputDevice::updateRetrospectiveBufferLength (double length)
{
    if (retrospectiveBuffer != nullptr)
        retrospectiveBuffer->lengthInSeconds = length;
}

//==============================================================================
// R8: real-time recording. The pool of blocks and the queue to the writer, without locks on the audio thread.
//  - Every block is owned by `slots` for the life of the queue (never freed while in use), and has an `inUse` flag:
//    the audio thread claims a free block with one compare-exchange (starting at the block after the last one it
//    took, so it usually finds one at once), the recording thread clears the flag when the block is written.
//  - Blocks wait for the writer in an intrusive multi-producer single-consumer queue (the audio threads push, the
//    recording thread pops; a push is two atomic stores; a pop that meets a push half done just returns nothing
//    and tries again later).
//  - The pool is sized by prepareForRealtime (message thread): new blocks are added, and free blocks that are too
//    small are grown there; a block in flight is grown by the recording thread when it comes back.
struct WaveInputRecordingThread::BlockQueue
{
    static constexpr int maxBlocks = 16384;

    struct Node
    {
        std::atomic<Node*> next { nullptr };
    };

    struct QueuedBlock : public Node
    {
        QueuedBlock (int chans, int samples)
            : buffer (std::max (1, chans), std::max (1, samples)), capChannels (std::max (1, chans)), capSamples (std::max (1, samples)) {}

        /** Fills the block. Allocates only if the block is too small for what arrives (counted by the caller). */
        bool load (AudioFileWriter& w, const juce::AudioBuffer<float>& newBuffer,
                   int start, int numSamples, const RecordingThumbnailManager::Thumbnail::Ptr& thumb)
        {
            const int chans = newBuffer.getNumChannels();
            bool allocated = false;

            if (chans > capChannels.load (std::memory_order_relaxed) || numSamples > capSamples.load (std::memory_order_relaxed))
            {
                grow (chans, numSamples);
                allocated = true;
            }

            buffer.setSize (chans, numSamples, false, false, true);

            for (int i = buffer.getNumChannels(); --i >= 0;)
                buffer.copyFrom (i, 0, newBuffer, i, start, numSamples);

            writer = &w;
            thumbnail = thumb;
            return allocated;
        }

        /** Grows the block (not on the audio thread; the block must not be in use). */
        void ensureCapacity (int chans, int samples)
        {
            if (chans <= capChannels.load (std::memory_order_relaxed) && samples <= capSamples.load (std::memory_order_relaxed))
                return;

            grow (chans, samples);
        }

        void grow (int chans, int samples)
        {
            const int c = std::max (chans, capChannels.load (std::memory_order_relaxed));
            const int n = std::max (samples, capSamples.load (std::memory_order_relaxed));
            buffer.setSize (c, n);
            capChannels.store (c, std::memory_order_relaxed);
            capSamples.store (n, std::memory_order_relaxed);
        }

        std::atomic<AudioFileWriter*> writer { nullptr };
        std::atomic<bool> inUse { false };
        juce::AudioBuffer<float> buffer;
        std::atomic<int> capChannels { 0 }, capSamples { 0 };
        RecordingThumbnailManager::Thumbnail::Ptr thumbnail;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QueuedBlock)
    };

    BlockQueue()
        : slots (new std::atomic<QueuedBlock*>[maxBlocks])
    {
        for (int i = 0; i < maxBlocks; ++i)
            slots[i].store (nullptr, std::memory_order_relaxed);

        pendingHead.store (&stub, std::memory_order_relaxed);
        pendingTail = &stub;
        ensurePool (64, 2, 512);
    }

    ~BlockQueue()
    {
        const int n = numBlocks.load();

        for (int i = 0; i < n; ++i)
            delete slots[i].load();
    }

    //==============================================================================
    // the pool
    std::unique_ptr<std::atomic<QueuedBlock*>[]> slots;
    std::atomic<int> numBlocks { 0 };
    std::atomic<int> takeHint { 0 };
    std::atomic<int> wantedChannels { 2 }, wantedSamples { 512 };
    std::mutex growLock;    // message thread only

    /** Message thread. Adds blocks until there are `count` of them, and grows the free ones that are too small. */
    void ensurePool (int count, int chans, int samples)
    {
        const std::scoped_lock sl (growLock);
        chans = std::max (chans, wantedChannels.load());
        samples = std::max (samples, wantedSamples.load());
        wantedChannels.store (chans);
        wantedSamples.store (samples);

        const int have = numBlocks.load (std::memory_order_acquire);

        for (int i = 0; i < have; ++i)
        {
            auto* b = slots[i].load (std::memory_order_relaxed);

            if (b->capChannels.load (std::memory_order_relaxed) >= chans && b->capSamples.load (std::memory_order_relaxed) >= samples)
                continue;

            bool wasFree = false;

            if (b->inUse.compare_exchange_strong (wasFree, true, std::memory_order_acq_rel))
            {
                b->ensureCapacity (chans, samples);
                b->inUse.store (false, std::memory_order_release);
            }
            // else: in flight; the recording thread grows it when it comes back
        }

        for (int n = have; n < std::min (count, maxBlocks); ++n)
        {
            slots[n].store (new QueuedBlock (chans, samples), std::memory_order_relaxed);
            numBlocks.store (n + 1, std::memory_order_release);
        }
    }

    /** Audio thread. A free block, or nullptr when every block is in flight. */
    QueuedBlock* takeFreeBlock() noexcept
    {
        const int n = numBlocks.load (std::memory_order_acquire);

        if (n <= 0)
            return nullptr;

        int i = takeHint.load (std::memory_order_relaxed);

        if (i < 0 || i >= n)
            i = 0;

        for (int k = 0; k < n; ++k)
        {
            auto* b = slots[i].load (std::memory_order_relaxed);
            bool wasFree = false;

            if (b->inUse.compare_exchange_strong (wasFree, true, std::memory_order_acq_rel))
            {
                takeHint.store (i + 1 < n ? i + 1 : 0, std::memory_order_relaxed);
                return b;
            }

            if (++i >= n)
                i = 0;
        }

        return nullptr;
    }

    /** Recording or message thread. Puts a block back, grown first if the pool's size has gone up. */
    void addToFreeQueue (QueuedBlock* b) noexcept
    {
        jassert (b != nullptr);
        b->writer = nullptr;
        b->thumbnail = nullptr;
        b->ensureCapacity (wantedChannels.load(), wantedSamples.load());
        b->inUse.store (false, std::memory_order_release);
    }

    //==============================================================================
    // the queue to the writer (multi-producer, single-consumer)
    Node stub;
    std::atomic<Node*> pendingHead;     // the most recently pushed
    Node* pendingTail = nullptr;        // consumer's end
    std::atomic<int> numPending { 0 };
    std::mutex processingMutex;

    void pushNode (Node* n) noexcept
    {
        n->next.store (nullptr, std::memory_order_relaxed);
        auto* prev = pendingHead.exchange (n, std::memory_order_acq_rel);
        prev->next.store (n, std::memory_order_release);
    }

    /** Audio thread. */
    void addToPendingQueue (QueuedBlock* b) noexcept
    {
        jassert (b != nullptr);
        ++numPending;
        pushNode (b);
    }

    /** Consumer only (the recording thread, or the message thread while it is stopped). */
    QueuedBlock* removeFirstPending() noexcept
    {
        auto* tail = pendingTail;
        auto* next = tail->next.load (std::memory_order_acquire);

        if (tail == &stub)
        {
            if (next == nullptr)
                return nullptr;

            pendingTail = next;
            tail = next;
            next = next->next.load (std::memory_order_acquire);
        }

        if (next != nullptr)
        {
            pendingTail = next;
            --numPending;
            return static_cast<QueuedBlock*> (tail);
        }

        if (tail != pendingHead.load (std::memory_order_acquire))
            return nullptr;    // a push is half done; it will be complete next time

        pushNode (&stub);
        next = tail->next.load (std::memory_order_acquire);

        if (next != nullptr)
        {
            pendingTail = next;
            --numPending;
            return static_cast<QueuedBlock*> (tail);
        }

        return nullptr;
    }

    void moveAnyPendingBlocksToFree() noexcept
    {
        for (int tries = 0; tries < 1000; ++tries)
        {
            while (auto b = removeFirstPending())
                addToFreeQueue (b);

            if (numPending.load() <= 0)
                break;

            std::this_thread::yield();    // a push half done
        }

        jassert (numPending == 0);
        numPending = 0;
    }

    /** Message thread, with processingMutex held (so the consumer's end is still). Says yes when unsure. */
    bool isWriterInQueue (AudioFileWriter& writer) const
    {
        for (const Node* n = pendingTail;;)
        {
            if (n != &stub && static_cast<const QueuedBlock*> (n)->writer == &writer)
                return true;

            if (n == pendingHead.load (std::memory_order_acquire))
                return false;

            const Node* nx = n->next.load (std::memory_order_acquire);

            if (nx == nullptr)
                return true;    // a push half done: can't tell yet

            n = nx;
        }
    }
};

//==============================================================================
WaveInputRecordingThread::WaveInputRecordingThread (Engine& e)
    : Thread ("WaveInputRecordingThread"),
      engine (e),
      queue (new BlockQueue())
{
}

WaveInputRecordingThread::~WaveInputRecordingThread()
{
    flushAndStop();
    queue.reset();
}

// R8: real-time recording
void WaveInputRecordingThread::prepareForRealtime (int numChannels, int blockSize, double sampleRate)
{
    blockSize = std::max (1, blockSize);
    const double blocksPerTwoSeconds = 2.0 * std::max (8000.0, sampleRate) / blockSize;
    const int count = juce::jlimit (64, BlockQueue::maxBlocks, (int) std::ceil (blocksPerTwoSeconds) + 64);
    queue->ensurePool (count, std::max (1, numChannels), blockSize);
}

void WaveInputRecordingThread::addUser()
{
    if (activeUsers++ == 0)
        prepareToStart();
}

void WaveInputRecordingThread::removeUser()
{
    if (--activeUsers == 0)
        flushAndStop();
}

//==============================================================================
void WaveInputRecordingThread::addBlockToRecord (AudioFileWriter& writer, const juce::AudioBuffer<float>& buffer,
                                                 int start, int numSamples, const RecordingThumbnailManager::Thumbnail::Ptr& thumbnail)
{
    if (! threadShouldExit())
    {
        // R8: real-time recording: no lock, no allocation; an empty pool is counted, not waited for
        auto block = queue->takeFreeBlock();

        if (block == nullptr)
        {
            ++numBlocksNotQueued;
            return;
        }

        if (block->load (writer, buffer, start, numSamples, thumbnail))
            ++numAudioThreadAllocations;

        queue->addToPendingQueue (block);
        notify();
    }
}

void WaveInputRecordingThread::waitForWriterToFinish (AudioFileWriter& writer)
{
    while (isThreadRunning())
    {
        {
            std::scoped_lock lock (queue->processingMutex);

            if (! queue->isWriterInQueue (writer))
                return;
        }

        Thread::sleep (2);
    }
}

void WaveInputRecordingThread::run()
{
    CRASH_TRACER
    juce::FloatVectorOperations::disableDenormalisedNumberSupport();

    for (;;)
    {
        if (queue->numPending > 500 && ! hasWarned)
        {
            hasWarned = true;
            TRACKTION_LOG_ERROR ("Audio recording can't keep up!");
        }

        auto processNextPendingBlock = [&]() -> BlockQueue::QueuedBlock*
        {
            std::scoped_lock lock (queue->processingMutex);

            if (auto b = queue->removeFirstPending())
            {
                auto w = b->writer.load();

                if (! w->appendBuffer (b->buffer, b->buffer.getNumSamples()))
                {
                    if (! hasSentStop)
                    {
                        hasSentStop = true;
                        TRACKTION_LOG_ERROR ("Audio recording failed to write to disk!");
                        startTimer (1);
                    }
                }

                if (b->thumbnail != nullptr)
                {
                    b->thumbnail->addBlock (b->buffer, 0, b->buffer.getNumSamples());
                    b->thumbnail = nullptr;
                }

                return b;
            }

            return nullptr;
        };

        if (auto block = processNextPendingBlock())
        {
            queue->addToFreeQueue (block);
        }
        else
        {
            if (threadShouldExit())
                break;

            wait (401);
        }
    }
}

void WaveInputRecordingThread::timerCallback()
{
    stopTimer();
    TransportControl::stopAllTransports (engine, false, false);
}

void WaveInputRecordingThread::prepareToStart()
{
    flushAndStop();
    sleep (2);
    jassert (! isThreadRunning());
    startThread (juce::Thread::Priority::normal);
}

void WaveInputRecordingThread::flushAndStop()
{
    signalThreadShouldExit();
    notify();
    stopThread (30000);
    queue->moveAnyPendingBlocksToFree();
    hasSentStop = false;
    hasWarned = false;
}

} // namespace tracktion::inline engine
