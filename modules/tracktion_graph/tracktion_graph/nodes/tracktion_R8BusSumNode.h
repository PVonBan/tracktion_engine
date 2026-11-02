/*
    R8: the merged channel (Recorder 8's fork, 2026-11-02).
*/

#pragma once

namespace tracktion::inline graph {

//==============================================================================
//==============================================================================
/**
    R8: the merged channel. The sum at an aux return when a MultiSendSource feeds the bus: SummingNode's behaviour
    (inputs added in order into the cleared output, child SummingNodes flattened to the end, inputs with less latency
    than the most delayed and moved to the end) for a list of entries that are either a Node's output or one send
    of a MultiSendSource Node. A send entry is added straight from the source's send buffer: no SendNode, GainNode or
    LatencyNode of its own; its delay, when the bus needs one, is added inside the source (addSendLatency).
    Single precision only (the device sum, which may use double, never holds sends).
*/
class R8BusSumNode final : public Node
{
public:
    struct Entry
    {
        Node* node = nullptr;
        int send = -1;              // -1: the node's own output; otherwise that send of the node (a MultiSendSource)
        MultiSendSource* source = nullptr;      // the node as a MultiSendSource, for a send entry (cast once, not per block)
    };

    /** `entries` in summing order; `owned` holds the nodes this one owns (any order). */
    R8BusSumNode (std::vector<Entry> entriesToUse, std::vector<std::unique_ptr<Node>> owned)
        : entries (std::move (entriesToUse)), ownedNodes (std::move (owned))
    {
        for (auto& e : entries)
        {
            assert (e.node != nullptr);
            if (e.send >= 0 && e.source == nullptr)
                e.source = dynamic_cast<MultiSendSource*> (e.node);
            assert (e.send < 0 || e.source != nullptr);
        }
    }

    //==============================================================================
    NodeProperties getNodeProperties() override
    {
        if (cachedNodeProperties)
            return *cachedNodeProperties;

        NodeProperties props;
        props.hasAudio = false;
        props.hasMidi = false;
        props.numberOfChannels = 0;
        props.latencyNumSamples = entries.empty() ? 0 : std::numeric_limits<int>::min();

        for (auto& e : entries)
        {
            const auto p = propsOf (e);
            props.hasAudio = props.hasAudio || p.hasAudio;
            props.hasMidi = props.hasMidi || p.hasMidi;
            props.numberOfChannels = std::max (props.numberOfChannels, p.numberOfChannels);
            props.latencyNumSamples = std::max (props.latencyNumSamples, p.latencyNumSamples);

            if (props.nodeID != 0 || p.nodeID != 0)
                hash_combine (props.nodeID, p.nodeID);
        }

        cachedNodeProperties = props;
        return props;
    }

    std::vector<Node*> getDirectInputNodes() override
    {
        std::vector<Node*> nodes;
        for (auto& e : entries)
            if (std::find (nodes.begin(), nodes.end(), e.node) == nodes.end())
                nodes.push_back (e.node);
        return nodes;
    }

    TransformResult transform (TransformOptions& options) override
    {
        cachedNodeProperties = std::nullopt;
        const bool hasFlattened = flattenSummingNodes();
        const bool hasCreatedLatency = ! options.disableLatencyCompensation && createLatencyNodes();

        if (hasFlattened)
            return TransformResult::nodesDeleted;

        if (hasCreatedLatency)
            return TransformResult::connectionsMade;

        return TransformResult::none;
    }

    bool isReadyToProcess() override
    {
        for (auto& e : entries)
            if (! e.node->hasProcessed())
                return false;

        return true;
    }

    void process (ProcessContext& pc) override
    {
        const auto numChannels = pc.buffers.audio.getNumChannels();
        int nodesWithMidi = pc.buffers.midi.isEmpty() ? 0 : 1;

        for (auto& e : entries)
        {
            if (e.send >= 0)
            {
                auto sendAudio = e.source->getSendOutput (e.send);

                if (auto numChannelsToAdd = std::min (sendAudio.getNumChannels(), numChannels))
                    add (pc.buffers.audio.getFirstChannels (numChannelsToAdd),
                         sendAudio.getFirstChannels (numChannelsToAdd));

                continue;
            }

            auto inputFromNode = e.node->getProcessedOutput();

            if (auto numChannelsToAdd = std::min (inputFromNode.audio.getNumChannels(), numChannels))
                add (pc.buffers.audio.getFirstChannels (numChannelsToAdd),
                     inputFromNode.audio.getFirstChannels (numChannelsToAdd));

            if (inputFromNode.midi.isNotEmpty())
                nodesWithMidi++;

            pc.buffers.midi.mergeFrom (inputFromNode.midi);
        }

        if (nodesWithMidi > 1)
            sortByTimestampUnstable (pc.buffers.midi);
    }

private:
    //==============================================================================
    std::vector<Entry> entries;
    std::vector<std::unique_ptr<Node>> ownedNodes;
    std::optional<NodeProperties> cachedNodeProperties;

    static NodeProperties propsOf (const Entry& e)
    {
        if (e.send < 0)
            return e.node->getNodeProperties();

        auto* src = e.source;
        NodeProperties p;
        p.hasAudio = true;
        p.hasMidi = false;
        p.numberOfChannels = src->getSendNumChannels (e.send);
        p.latencyNumSamples = src->getSendLatency (e.send);
        p.nodeID = e.node->getNodeProperties().nodeID;

        if (p.nodeID != 0)
        {
            hash_combine (p.nodeID, (size_t) e.send);
            hash_combine (p.nodeID, size_t (0x52387365'6e64));     // "R8send"
        }

        return p;
    }

    static void sortByTimestampUnstable (tracktion_engine::MidiMessageArray& messages) noexcept
    {
        std::sort (messages.begin(), messages.end(), [] (const juce::MidiMessage& a, const juce::MidiMessage& b)
        {
            auto t1 = a.getTimeStamp();
            auto t2 = b.getTimeStamp();
            if (t1 < t2) return true;
            if (t2 < t1) return false;
            return a.isNoteOff() && ! b.isNoteOff();
        });
    }

    std::unique_ptr<Node> takeOwned (Node* n)
    {
        for (auto& o : ownedNodes)
            if (o.get() == n)
            {
                auto r = std::move (o);
                o.reset();
                return r;
            }

        return {};
    }

    // (as SummingNode::flattenSummingNodes: an owned SummingNode entry's inputs take its place, at the end)
    bool flattenSummingNodes()
    {
        bool hasChanged = false;
        std::vector<Entry> toAdd;

        for (auto& e : entries)
        {
            if (e.send >= 0)
                continue;

            auto* summingNode = dynamic_cast<SummingNode*> (e.node);

            if (summingNode == nullptr || takeOwnedFromSumming (*summingNode, toAdd) == false)
                continue;

            e.node = nullptr;
            hasChanged = true;
        }

        if (! hasChanged)
            return false;

        entries.erase (std::remove_if (entries.begin(), entries.end(), [] (auto& e) { return e.node == nullptr; }), entries.end());
        for (auto& e : toAdd)
            entries.push_back (e);
        ownedNodes.erase (std::remove_if (ownedNodes.begin(), ownedNodes.end(), [] (auto& n) { return n == nullptr; }), ownedNodes.end());
        return true;
    }

    // Takes a SummingNode's inputs over (owned ones with their ownership); false when this node doesn't own it
    bool takeOwnedFromSumming (SummingNode& summingNode, std::vector<Entry>& toAdd)
    {
        auto owned = takeOwned (&summingNode);

        if (owned == nullptr)
            return false;

        for (auto& n : summingNode.r8TakeOwnedNodes())
            ownedNodes.push_back (std::move (n));

        for (auto* n : summingNode.getDirectInputNodes())
            toAdd.push_back ({ n, -1 });

        return true;           // (`owned`, the SummingNode itself, goes out of scope here: deleted)
    }

    // (as SummingNode::createLatencyNodes: an entry with less latency than the most delayed one is delayed to match,
    // a Node by a LatencyNode, a send inside its source, and moved to the end)
    bool createLatencyNodes()
    {
        bool topologyChanged = false;
        const int maxLatency = getNodeProperties().latencyNumSamples;
        std::vector<Entry> moved;

        for (auto& e : entries)
        {
            const int latencyToAdd = subtractNoWrap (maxLatency, propsOf (e).latencyNumSamples);

            if (latencyToAdd <= 0)
                continue;

            if (e.send >= 0)
            {
                e.source->addSendLatency (e.send, latencyToAdd);
                moved.push_back (e);
            }
            else
            {
                auto ownedNode = takeOwned (e.node);
                auto latencyNode = ownedNode != nullptr ? makeNode<LatencyNode> (std::move (ownedNode), latencyToAdd)
                                                        : makeNode<LatencyNode> (e.node, latencyToAdd);
                moved.push_back ({ latencyNode.get(), -1 });
                ownedNodes.push_back (std::move (latencyNode));
            }

            e.node = nullptr;
            topologyChanged = true;
            cachedNodeProperties = std::nullopt;
        }

        if (! topologyChanged)
            return false;

        entries.erase (std::remove_if (entries.begin(), entries.end(), [] (auto& e) { return e.node == nullptr; }), entries.end());
        for (auto& e : moved)
            entries.push_back (e);
        ownedNodes.erase (std::remove_if (ownedNodes.begin(), ownedNodes.end(), [] (auto& n) { return n == nullptr; }), ownedNodes.end());
        return true;
    }
};

} // namespace tracktion::inline graph
