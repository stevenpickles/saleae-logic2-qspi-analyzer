#ifndef QUADSPI_SIMULATION_DATA_GENERATOR_H
#define QUADSPI_SIMULATION_DATA_GENERATOR_H

#include <SimulationChannelDescriptor.h>

class QuadSpiAnalyzerSettings;

class QuadSpiSimulationDataGenerator
{
  public:
    QuadSpiSimulationDataGenerator();
    ~QuadSpiSimulationDataGenerator();

    void Initialize( U32 simulation_sample_rate, QuadSpiAnalyzerSettings* settings );
    U32 GenerateSimulationData( U64 newest_sample_requested, U32 sample_rate, SimulationChannelDescriptor** simulation_channel );

  protected:
    void CreateTransaction();
    void OutputBits( U64 value, U32 bit_count, U32 width, bool drive_response_lane );
    void OutputDummyCycles( U32 cycles );
    void SetDataLines( U32 chunk, U32 width, bool drive_response_lane );
    void OutputClockCycle();

    QuadSpiAnalyzerSettings* mSettings;
    U32 mSimulationSampleRateHz;
    U32 mSamplesPerHalfPeriod;

    SimulationChannelDescriptorGroup mQspiSimulationChannels;
    SimulationChannelDescriptor* mChipSelect;
    SimulationChannelDescriptor* mClock;
    SimulationChannelDescriptor* mDataLines[ 4 ]; // IO0..IO3; IO2/IO3 may be null
};

#endif // QUADSPI_SIMULATION_DATA_GENERATOR_H
