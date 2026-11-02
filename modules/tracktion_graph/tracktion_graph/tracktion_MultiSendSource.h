/*
    R8: the merged channel (Recorder 8's fork, 2026-11-02).
*/

#pragma once

namespace tracktion { inline namespace graph {

//==============================================================================
/**
    R8: the merged channel. A Node that offers already-gained send signals for aux buses beside its own output.
    A ReturnNode for one of those buses sums the send straight from the Node's send buffer (R8BusSumNode), so the
    send costs no SendNode, GainNode or copy of its own. The Node must have processed before a send output is read:
    the sum node lists the Node as a direct input.
*/
struct MultiSendSource
{
    virtual ~MultiSendSource() = default;

    virtual int getNumSends() const = 0;
    virtual int getSendBusID (int send) const = 0;
    virtual int getSendNumChannels (int send) const = 0;
    /** The send's latency in samples: that of the signal at the send point, plus whatever addSendLatency() added. */
    virtual int getSendLatency (int send) const = 0;
    /** The return's compensation: this send's output is to be delayed by this many more samples. Called while the
        graph is transformed, before prepareToPlay(). */
    virtual void addSendLatency (int send, int numSamples) = 0;
    /** The send's output for the block just processed, already gained and delayed; valid once the Node hasProcessed(). */
    virtual choc::buffer::ChannelArrayView<float> getSendOutput (int send) = 0;
};

}} // namespace tracktion { inline namespace graph
