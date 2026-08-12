// Test-double for the Saleae Analyzer SDK runtime.
//
// The real implementations of the SDK's PIMPL classes live inside the Logic 2
// application binary; this mock provides in-process implementations so the
// analyzer's decode and simulation code can run inside a plain test executable.
//
// Channel data is backed by explicit transition lists, results are recorded
// instead of rendered, and the simulation descriptors record the waveforms the
// simulation generator produces so they can be fed back into the decoder.

#ifndef QSPI_TESTS_MOCK_SDK_H
#define QSPI_TESTS_MOCK_SDK_H

#include <AnalyzerChannelData.h>
#include <AnalyzerResults.h>
#include <SimulationChannelDescriptor.h>

#include <map>
#include <string>
#include <vector>

// thrown by AnalyzerChannelData when a forward move runs past the last recorded
// transition -- the in-process equivalent of Logic killing the worker thread at
// the end of a capture
struct MockEndOfData
{
};

// thrown when the channel-data operation budget is exhausted; guards tests
// against a decode bug hanging the suite in an infinite loop
struct MockOpLimitExceeded
{
};

struct RecordedFrameV2
{
    std::string mType;
    U64 mStartingSample;
    U64 mEndingSample;
    std::map<std::string, std::string> mFields;
};

struct RecordedMarker
{
    U64 mSample;
    AnalyzerResults::MarkerType mMarkerType;
    Channel mChannel;
};

namespace MockSdk
{
    // channel data registry: the Analyzer base class resolves GetAnalyzerChannelData() through this
    void ResetChannelData(); // also frees everything created by MakeChannelData
    AnalyzerChannelData* MakeChannelData( BitState initial_state, const std::vector<U64>& transitions );
    void SetChannelData( const Channel& channel, AnalyzerChannelData* data );

    void SetSampleRate( U32 sample_rate_hz );
    void SetOpLimit( U64 max_channel_data_operations );

    // recorded analyzer output
    const std::vector<Frame>& GetFrames( AnalyzerResults* results );
    const std::vector<RecordedFrameV2>& GetFramesV2( AnalyzerResults* results );
    const std::vector<RecordedMarker>& GetMarkers( AnalyzerResults* results );

    // recorded simulation waveforms
    Channel GetSimChannel( SimulationChannelDescriptor* descriptor );
    BitState GetSimInitialState( SimulationChannelDescriptor* descriptor );
    const std::vector<U64>& GetSimTransitions( SimulationChannelDescriptor* descriptor );
}

#endif // QSPI_TESTS_MOCK_SDK_H
