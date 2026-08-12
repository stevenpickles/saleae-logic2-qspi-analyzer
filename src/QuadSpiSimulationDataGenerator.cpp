#include "QuadSpiSimulationDataGenerator.h"
#include "QuadSpiAnalyzerSettings.h"

#include <AnalyzerHelpers.h>

namespace
{
    const U8 kSimulatedCommand = 0xEB;
    const U64 kSimulatedAddress = 0x001000;
    const U8 kSimulatedData[] = { 0xDE, 0xAD, 0xBE, 0xEF };
}

QuadSpiSimulationDataGenerator::QuadSpiSimulationDataGenerator()
    : mSettings( nullptr ),
      mSimulationSampleRateHz( 0 ),
      mSamplesPerHalfPeriod( 0 ),
      mChipSelect( nullptr ),
      mClock( nullptr ),
      mDataLines{ nullptr, nullptr, nullptr, nullptr }
{
}

QuadSpiSimulationDataGenerator::~QuadSpiSimulationDataGenerator()
{
}

void QuadSpiSimulationDataGenerator::Initialize( U32 simulation_sample_rate, QuadSpiAnalyzerSettings* settings )
{
    mSimulationSampleRateHz = simulation_sample_rate;
    mSettings = settings;

    // simulate a clock at roughly 1/10th of the sample rate
    mSamplesPerHalfPeriod = simulation_sample_rate / 10 / 2;
    if( mSamplesPerHalfPeriod < 2 )
        mSamplesPerHalfPeriod = 2;

    BitState chip_select_inactive = mSettings->mChipSelectActiveState == BIT_LOW ? BIT_HIGH : BIT_LOW;

    mChipSelect = mQspiSimulationChannels.Add( mSettings->mChipSelectChannel, simulation_sample_rate, chip_select_inactive );
    mClock = mQspiSimulationChannels.Add( mSettings->mClockChannel, simulation_sample_rate, mSettings->mClockInactiveState );
    mDataLines[ 0 ] = mQspiSimulationChannels.Add( mSettings->mData0Channel, simulation_sample_rate, BIT_HIGH );
    mDataLines[ 1 ] = mQspiSimulationChannels.Add( mSettings->mData1Channel, simulation_sample_rate, BIT_HIGH );
    mDataLines[ 2 ] = mSettings->mData2Channel != UNDEFINED_CHANNEL
                          ? mQspiSimulationChannels.Add( mSettings->mData2Channel, simulation_sample_rate, BIT_HIGH )
                          : nullptr;
    mDataLines[ 3 ] = mSettings->mData3Channel != UNDEFINED_CHANNEL
                          ? mQspiSimulationChannels.Add( mSettings->mData3Channel, simulation_sample_rate, BIT_HIGH )
                          : nullptr;
}

U32 QuadSpiSimulationDataGenerator::GenerateSimulationData( U64 largest_sample_requested, U32 sample_rate,
                                                            SimulationChannelDescriptor** simulation_channel )
{
    U64 adjusted_largest_sample_requested =
        AnalyzerHelpers::AdjustSimulationTargetSample( largest_sample_requested, sample_rate, mSimulationSampleRateHz );

    while( mClock->GetCurrentSampleNumber() < adjusted_largest_sample_requested )
    {
        CreateTransaction();
    }

    *simulation_channel = mQspiSimulationChannels.GetArray();
    return mQspiSimulationChannels.GetCount();
}

void QuadSpiSimulationDataGenerator::SetDataLines( U32 chunk, U32 width, bool drive_response_lane )
{
    if( width == 1 )
    {
        // single-IO: commands and writes drive IO0; read responses drive IO1
        BitState bit = ( chunk & 1 ) != 0 ? BIT_HIGH : BIT_LOW;
        mDataLines[ 0 ]->TransitionIfNeeded( drive_response_lane ? BIT_HIGH : bit );
        mDataLines[ 1 ]->TransitionIfNeeded( drive_response_lane ? bit : BIT_HIGH );
    }
    else
    {
        for( U32 line = 0; line < width; ++line )
        {
            if( mDataLines[ line ] == nullptr )
                continue;
            BitState bit = ( ( chunk >> line ) & 1 ) != 0 ? BIT_HIGH : BIT_LOW;
            mDataLines[ line ]->TransitionIfNeeded( bit );
        }
    }

    // idle any defined lines above the active bus width
    for( U32 line = ( width == 1 ) ? 2 : width; line < 4; ++line )
    {
        if( mDataLines[ line ] != nullptr )
            mDataLines[ line ]->TransitionIfNeeded( BIT_HIGH );
    }
}

void QuadSpiSimulationDataGenerator::OutputClockCycle()
{
    // match the cycle shape OutputBits uses for the configured CPHA, so adjacent
    // cycles never place two clock transitions at the same sample
    if( mSettings->mDataValidEdge == AnalyzerEnums::LeadingEdge )
    {
        mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod );
        mClock->Transition(); // leading edge
        mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod );
        mClock->Transition(); // trailing edge
    }
    else
    {
        mClock->Transition(); // leading edge
        mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod );
        mClock->Transition(); // trailing edge
        mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod );
    }
}

void QuadSpiSimulationDataGenerator::OutputBits( U64 value, U32 bit_count, U32 width, bool drive_response_lane )
{
    U32 chunk_mask = ( 1u << width ) - 1;
    U32 clocks = bit_count / width;

    for( U32 clock_index = 0; clock_index < clocks; ++clock_index )
    {
        U32 chunk = static_cast<U32>( value >> ( bit_count - width * ( clock_index + 1 ) ) ) & chunk_mask;

        if( mSettings->mDataValidEdge == AnalyzerEnums::LeadingEdge )
        {
            // CPHA = 0: data must be stable before the leading (sampling) edge
            SetDataLines( chunk, width, drive_response_lane );
            mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod );
            mClock->Transition(); // leading edge -- sampled here
            mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod );
            mClock->Transition(); // trailing edge
        }
        else
        {
            // CPHA = 1: data changes on the leading edge, sampled on the trailing edge
            mClock->Transition(); // leading edge
            SetDataLines( chunk, width, drive_response_lane );
            mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod );
            mClock->Transition(); // trailing edge -- sampled here
            mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod );
        }
    }
}

void QuadSpiSimulationDataGenerator::OutputDummyCycles( U32 cycles )
{
    SetDataLines( 0xF, 4, false ); // bus floats high during dummy cycles
    for( U32 cycle = 0; cycle < cycles; ++cycle )
        OutputClockCycle();
}

void QuadSpiSimulationDataGenerator::CreateTransaction()
{
    // idle gap before the transaction
    mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod * 8 );

    mChipSelect->Transition(); // assert
    mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod * 2 );

    OutputBits( kSimulatedCommand, 8, mSettings->mCommandBusWidth, false );

    if( mSettings->mAddressBits > 0 )
    {
        U64 address_mask = mSettings->mAddressBits >= 64 ? ~0ull : ( 1ull << mSettings->mAddressBits ) - 1;
        OutputBits( kSimulatedAddress & address_mask, mSettings->mAddressBits, mSettings->mAddressBusWidth, false );
    }

    if( mSettings->mDummyCycles > 0 )
        OutputDummyCycles( mSettings->mDummyCycles );

    bool simulate_read_response = mSettings->mDataBusWidth == 1;
    for( U8 data_byte : kSimulatedData )
        OutputBits( data_byte, 8, mSettings->mDataBusWidth, simulate_read_response );

    // return the bus to idle and deassert
    SetDataLines( 0xF, 4, false );
    mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod * 2 );
    mChipSelect->Transition(); // deassert

    mQspiSimulationChannels.AdvanceAll( mSamplesPerHalfPeriod * 20 );
}
