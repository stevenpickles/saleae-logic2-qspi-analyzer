#ifndef QUADSPI_ANALYZER_H
#define QUADSPI_ANALYZER_H

#include <Analyzer.h>
#include "QuadSpiAnalyzerSettings.h"
#include "QuadSpiAnalyzerResults.h"
#include "QuadSpiSimulationDataGenerator.h"
#include <memory>

class ANALYZER_EXPORT QuadSpiAnalyzer : public Analyzer2
{
  public:
    QuadSpiAnalyzer();
    virtual ~QuadSpiAnalyzer();

    virtual void SetupResults();
    virtual void WorkerThread();

    virtual U32 GenerateSimulationData( U64 newest_sample_requested, U32 sample_rate, SimulationChannelDescriptor** simulation_channels );
    virtual U32 GetMinimumSampleRateHz();

    virtual const char* GetAnalyzerName() const;
    virtual bool NeedsRerun();

  protected: // decode helpers
    struct PhaseReadResult
    {
        U64 mValue = 0;    // bits assembled MSB-first from the bus (IO0 lane when width is 1)
        U64 mValueIo1 = 0; // width 1 only: bits sampled on IO1 (the response lane)
        U32 mBitsRead = 0;
        U64 mFirstSample = 0; // first sampled clock edge
        U64 mLastSample = 0;  // last sampled clock edge
        bool mTruncated = false; // chip select deasserted before all bits were read
    };

    void Setup();
    void AdvanceToActiveChipSelectEdge();
    bool WouldAdvancingTheClockToggleChipSelect();
    void ReadBits( U32 bit_count, U32 width, PhaseReadResult& result );
    bool AdvanceDummyCycles( U32 cycles, U64& first_sample, U64& last_sample );
    void EndTransaction();

  protected: // vars
    QuadSpiAnalyzerSettings mSettings;
    std::unique_ptr<QuadSpiAnalyzerResults> mResults;

    AnalyzerChannelData* mChipSelect;
    AnalyzerChannelData* mClock;
    AnalyzerChannelData* mDataLines[ 4 ]; // IO0..IO3; IO2/IO3 may be null

    QuadSpiSimulationDataGenerator mSimulationDataGenerator;
    bool mSimulationInitilized;
};

extern "C" ANALYZER_EXPORT const char* __cdecl GetAnalyzerName();
extern "C" ANALYZER_EXPORT Analyzer* __cdecl CreateAnalyzer();
extern "C" ANALYZER_EXPORT void __cdecl DestroyAnalyzer( Analyzer* analyzer );

#endif // QUADSPI_ANALYZER_H
