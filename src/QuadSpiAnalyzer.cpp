#include "QuadSpiAnalyzer.h"
#include "QuadSpiAnalyzerSettings.h"
#include <AnalyzerChannelData.h>

QuadSpiAnalyzer::QuadSpiAnalyzer()
    : Analyzer2(),
      mSettings(),
      mChipSelect( nullptr ),
      mClock( nullptr ),
      mDataLines{ nullptr, nullptr, nullptr, nullptr },
      mSimulationInitilized( false )
{
    SetAnalyzerSettings( &mSettings );
    UseFrameV2();
}

QuadSpiAnalyzer::~QuadSpiAnalyzer()
{
    KillThread();
}

void QuadSpiAnalyzer::SetupResults()
{
    // SetupResults is called each time the analyzer is run. Because the same instance can be used for multiple runs, we need to clear the
    // results each time.
    mResults.reset( new QuadSpiAnalyzerResults( this, &mSettings ) );
    SetAnalyzerResults( mResults.get() );
    mResults->AddChannelBubblesWillAppearOn( mSettings.mData0Channel );
}

void QuadSpiAnalyzer::Setup()
{
    mChipSelect = GetAnalyzerChannelData( mSettings.mChipSelectChannel );
    mClock = GetAnalyzerChannelData( mSettings.mClockChannel );
    mDataLines[ 0 ] = GetAnalyzerChannelData( mSettings.mData0Channel );
    mDataLines[ 1 ] = GetAnalyzerChannelData( mSettings.mData1Channel );
    mDataLines[ 2 ] = mSettings.mData2Channel != UNDEFINED_CHANNEL ? GetAnalyzerChannelData( mSettings.mData2Channel ) : nullptr;
    mDataLines[ 3 ] = mSettings.mData3Channel != UNDEFINED_CHANNEL ? GetAnalyzerChannelData( mSettings.mData3Channel ) : nullptr;
}

void QuadSpiAnalyzer::AdvanceToActiveChipSelectEdge()
{
    if( mChipSelect->GetBitState() != mSettings.mChipSelectActiveState )
    {
        mChipSelect->AdvanceToNextEdge(); // the assert edge
    }
    else
    {
        // mid-transaction at the start of the capture (or after a resync): skip to the next full transaction
        mChipSelect->AdvanceToNextEdge(); // deassert
        mChipSelect->AdvanceToNextEdge(); // assert
    }

    U64 assert_sample = mChipSelect->GetSampleNumber();
    mClock->AdvanceToAbsPosition( assert_sample );
    for( auto* data_line : mDataLines )
    {
        if( data_line != nullptr )
            data_line->AdvanceToAbsPosition( assert_sample );
    }

    mResults->AddMarker( assert_sample, AnalyzerResults::Start, mSettings.mChipSelectChannel );

    FrameV2 frame_v2;
    mResults->AddFrameV2( frame_v2, "enable", assert_sample, assert_sample + 1 );
}

bool QuadSpiAnalyzer::WouldAdvancingTheClockToggleChipSelect()
{
    // handle the case where no more clock transitions exist in the currently streamed data but
    // chip select still deasserts, so the final transaction can be closed without waiting for a
    // clock edge that never comes. During live streaming the blocking position query below may
    // observe clock data that arrives after the DoMoreTransitions check, in which case decoding
    // simply continues.
    if( !mClock->DoMoreTransitionsExistInCurrentData() )
    {
        U64 next_chip_select_edge = mChipSelect->GetSampleOfNextEdge();
        if( !mClock->WouldAdvancingToAbsPositionCauseTransition( next_chip_select_edge ) )
            return true;
    }

    U64 next_clock_edge = mClock->GetSampleOfNextEdge();
    return mChipSelect->WouldAdvancingToAbsPositionCauseTransition( next_clock_edge );
}

void QuadSpiAnalyzer::ReadBits( U32 bit_count, U32 width, PhaseReadResult& result )
{
    result = PhaseReadResult();

    U32 clocks = bit_count / width;
    for( U32 clock_index = 0; clock_index < clocks; ++clock_index )
    {
        if( WouldAdvancingTheClockToggleChipSelect() )
        {
            result.mTruncated = true;
            return;
        }
        mClock->AdvanceToNextEdge(); // leading edge of the cycle

        if( mSettings.mDataValidEdge == AnalyzerEnums::TrailingEdge )
        {
            if( WouldAdvancingTheClockToggleChipSelect() )
            {
                result.mTruncated = true;
                return;
            }
            mClock->AdvanceToNextEdge(); // trailing edge -- the sampling edge for CPHA = 1
        }

        U64 sample = mClock->GetSampleNumber();
        if( result.mBitsRead == 0 )
            result.mFirstSample = sample;
        result.mLastSample = sample;

        U32 chunk = 0;
        for( U32 line = 0; line < width; ++line )
        {
            AnalyzerChannelData* data_line = mDataLines[ line ];
            U32 bit = 0;
            if( data_line != nullptr )
            {
                data_line->AdvanceToAbsPosition( sample );
                bit = data_line->GetBitState() == BIT_HIGH ? 1 : 0;
            }
            chunk |= bit << line;
        }
        result.mValue = ( result.mValue << width ) | chunk;

        if( width == 1 )
        {
            // single-IO transfers carry commands/writes on IO0 and responses on IO1; capture both lanes
            mDataLines[ 1 ]->AdvanceToAbsPosition( sample );
            U32 io1_bit = mDataLines[ 1 ]->GetBitState() == BIT_HIGH ? 1 : 0;
            result.mValueIo1 = ( result.mValueIo1 << 1 ) | io1_bit;
        }

        result.mBitsRead += width;

        mResults->AddMarker( sample, mClock->GetBitState() == BIT_HIGH ? AnalyzerResults::UpArrow : AnalyzerResults::DownArrow,
                             mSettings.mClockChannel );

        if( mSettings.mDataValidEdge == AnalyzerEnums::LeadingEdge )
        {
            // complete the clock cycle through the trailing edge -- unless chip select ends the transaction first,
            // which after a fully captured word is a normal termination
            if( WouldAdvancingTheClockToggleChipSelect() )
            {
                if( result.mBitsRead != bit_count )
                    result.mTruncated = true;
                return;
            }
            mClock->AdvanceToNextEdge();
        }
    }
}

bool QuadSpiAnalyzer::AdvanceDummyCycles( U32 cycles, U64& first_sample, U64& last_sample )
{
    for( U32 cycle = 0; cycle < cycles; ++cycle )
    {
        for( U32 edge = 0; edge < 2; ++edge )
        {
            if( WouldAdvancingTheClockToggleChipSelect() )
                return false;
            mClock->AdvanceToNextEdge();
            if( cycle == 0 && edge == 0 )
                first_sample = mClock->GetSampleNumber();
            last_sample = mClock->GetSampleNumber();
        }
    }
    return true;
}

void QuadSpiAnalyzer::EndTransaction()
{
    mChipSelect->AdvanceToNextEdge(); // the deassert edge
    U64 deassert_sample = mChipSelect->GetSampleNumber();

    mClock->AdvanceToAbsPosition( deassert_sample );
    for( auto* data_line : mDataLines )
    {
        if( data_line != nullptr )
            data_line->AdvanceToAbsPosition( deassert_sample );
    }

    mResults->AddMarker( deassert_sample, AnalyzerResults::Stop, mSettings.mChipSelectChannel );

    FrameV2 frame_v2;
    mResults->AddFrameV2( frame_v2, "disable", deassert_sample, deassert_sample + 1 );

    mResults->CommitPacketAndStartNewPacket();
    mResults->CommitResults();
    ReportProgress( deassert_sample );
}

void QuadSpiAnalyzer::WorkerThread()
{
    Setup();

    for( ;; )
    {
        AdvanceToActiveChipSelectEdge();
        CheckIfThreadShouldExit();

        bool transaction_ok = true;

        // the clock must be idle (at its CPOL level) when chip select asserts
        if( mClock->GetBitState() != mSettings.mClockInactiveState )
        {
            U64 assert_sample = mChipSelect->GetSampleNumber();

            Frame frame;
            frame.mType = FRAME_ERROR;
            frame.mFlags = DISPLAY_AS_ERROR_FLAG;
            frame.mData1 = 0;
            frame.mData2 = 0;
            frame.mStartingSampleInclusive = assert_sample;
            frame.mEndingSampleInclusive = assert_sample + 1;
            mResults->AddFrame( frame );

            mResults->AddMarker( assert_sample, AnalyzerResults::ErrorSquare, mSettings.mClockChannel );

            FrameV2 frame_v2;
            frame_v2.AddString( "error", "clock not idle when CS asserted" );
            mResults->AddFrameV2( frame_v2, "error", assert_sample, assert_sample + 1 );

            EndTransaction();
            continue;
        }

        // command phase
        PhaseReadResult command;
        ReadBits( 8, mSettings.mCommandBusWidth, command );
        if( command.mBitsRead > 0 )
        {
            Frame frame;
            frame.mType = command.mTruncated ? FRAME_ERROR : FRAME_COMMAND;
            frame.mFlags = command.mTruncated ? ( QSPI_TRUNCATED_FLAG | DISPLAY_AS_ERROR_FLAG ) : 0;
            frame.mData1 = command.mValue;
            frame.mData2 = command.mBitsRead;
            frame.mStartingSampleInclusive = command.mFirstSample;
            frame.mEndingSampleInclusive = command.mLastSample > command.mFirstSample ? command.mLastSample : command.mFirstSample + 1;
            mResults->AddFrame( frame );

            FrameV2 frame_v2;
            if( command.mTruncated )
            {
                frame_v2.AddString( "error", "CS deasserted during command phase" );
                mResults->AddFrameV2( frame_v2, "error", frame.mStartingSampleInclusive, frame.mEndingSampleInclusive );
            }
            else
            {
                frame_v2.AddByte( "cmd", static_cast<U8>( command.mValue ) );
                mResults->AddFrameV2( frame_v2, "command", frame.mStartingSampleInclusive, frame.mEndingSampleInclusive );
            }
        }
        if( command.mTruncated )
            transaction_ok = false;

        // address phase
        if( transaction_ok && mSettings.mAddressBits > 0 )
        {
            PhaseReadResult address;
            ReadBits( mSettings.mAddressBits, mSettings.mAddressBusWidth, address );
            if( address.mBitsRead > 0 )
            {
                Frame frame;
                frame.mType = address.mTruncated ? FRAME_ERROR : FRAME_ADDRESS;
                frame.mFlags = address.mTruncated ? ( QSPI_TRUNCATED_FLAG | DISPLAY_AS_ERROR_FLAG ) : 0;
                frame.mData1 = address.mValue;
                frame.mData2 = address.mBitsRead;
                frame.mStartingSampleInclusive = address.mFirstSample;
                frame.mEndingSampleInclusive = address.mLastSample > address.mFirstSample ? address.mLastSample : address.mFirstSample + 1;
                mResults->AddFrame( frame );

                FrameV2 frame_v2;
                if( address.mTruncated )
                {
                    frame_v2.AddString( "error", "CS deasserted during address phase" );
                    mResults->AddFrameV2( frame_v2, "error", frame.mStartingSampleInclusive, frame.mEndingSampleInclusive );
                }
                else
                {
                    frame_v2.AddInteger( "address", static_cast<S64>( address.mValue ) );
                    frame_v2.AddInteger( "bits", mSettings.mAddressBits );
                    mResults->AddFrameV2( frame_v2, "address", frame.mStartingSampleInclusive, frame.mEndingSampleInclusive );
                }
            }
            if( address.mTruncated )
                transaction_ok = false;
        }

        // dummy phase
        if( transaction_ok && mSettings.mDummyCycles > 0 )
        {
            U64 first_sample = 0;
            U64 last_sample = 0;
            bool complete = AdvanceDummyCycles( mSettings.mDummyCycles, first_sample, last_sample );
            if( first_sample != 0 )
            {
                Frame frame;
                frame.mType = complete ? FRAME_DUMMY : FRAME_ERROR;
                frame.mFlags = complete ? 0 : ( QSPI_TRUNCATED_FLAG | DISPLAY_AS_ERROR_FLAG );
                frame.mData1 = mSettings.mDummyCycles;
                frame.mData2 = 0;
                frame.mStartingSampleInclusive = first_sample;
                frame.mEndingSampleInclusive = last_sample > first_sample ? last_sample : first_sample + 1;
                mResults->AddFrame( frame );

                FrameV2 frame_v2;
                if( complete )
                {
                    frame_v2.AddInteger( "cycles", mSettings.mDummyCycles );
                    mResults->AddFrameV2( frame_v2, "dummy", frame.mStartingSampleInclusive, frame.mEndingSampleInclusive );
                }
                else
                {
                    frame_v2.AddString( "error", "CS deasserted during dummy cycles" );
                    mResults->AddFrameV2( frame_v2, "error", frame.mStartingSampleInclusive, frame.mEndingSampleInclusive );
                }
            }
            if( !complete )
                transaction_ok = false;
        }

        // data phase: bytes until chip select deasserts
        while( transaction_ok )
        {
            PhaseReadResult data;
            ReadBits( 8, mSettings.mDataBusWidth, data );

            bool partial = data.mTruncated && data.mBitsRead > 0;
            if( data.mBitsRead > 0 )
            {
                Frame frame;
                frame.mType = partial ? FRAME_ERROR : FRAME_DATA;
                frame.mFlags = partial ? ( QSPI_TRUNCATED_FLAG | DISPLAY_AS_ERROR_FLAG ) : 0;
                frame.mData1 = data.mValue;
                frame.mData2 = ( static_cast<U64>( data.mBitsRead ) << 32 ) | data.mValueIo1;
                frame.mStartingSampleInclusive = data.mFirstSample;
                frame.mEndingSampleInclusive = data.mLastSample > data.mFirstSample ? data.mLastSample : data.mFirstSample + 1;
                mResults->AddFrame( frame );

                FrameV2 frame_v2;
                if( partial )
                {
                    frame_v2.AddString( "error", "CS deasserted mid-byte during data phase" );
                    mResults->AddFrameV2( frame_v2, "error", frame.mStartingSampleInclusive, frame.mEndingSampleInclusive );
                }
                else if( mSettings.mDataBusWidth == 1 )
                {
                    frame_v2.AddByte( "mosi", static_cast<U8>( data.mValue ) );
                    frame_v2.AddByte( "miso", static_cast<U8>( data.mValueIo1 ) );
                    mResults->AddFrameV2( frame_v2, "data", frame.mStartingSampleInclusive, frame.mEndingSampleInclusive );
                }
                else
                {
                    frame_v2.AddByte( "data", static_cast<U8>( data.mValue ) );
                    mResults->AddFrameV2( frame_v2, "data", frame.mStartingSampleInclusive, frame.mEndingSampleInclusive );
                }

                mResults->CommitResults();
                ReportProgress( frame.mEndingSampleInclusive );
                CheckIfThreadShouldExit();
            }

            if( data.mTruncated )
                break;
        }

        EndTransaction();
        CheckIfThreadShouldExit();
    }
}

bool QuadSpiAnalyzer::NeedsRerun()
{
    return false;
}

U32 QuadSpiAnalyzer::GenerateSimulationData( U64 minimum_sample_index, U32 device_sample_rate,
                                             SimulationChannelDescriptor** simulation_channels )
{
    if( mSimulationInitilized == false )
    {
        mSimulationDataGenerator.Initialize( GetSimulationSampleRate(), &mSettings );
        mSimulationInitilized = true;
    }

    return mSimulationDataGenerator.GenerateSimulationData( minimum_sample_index, device_sample_rate, simulation_channels );
}

U32 QuadSpiAnalyzer::GetMinimumSampleRateHz()
{
    // no bit rate setting exists to derive this from; rely on the user sampling fast enough for their clock
    return 10000;
}

const char* QuadSpiAnalyzer::GetAnalyzerName() const
{
    return "QSPI";
}

const char* GetAnalyzerName()
{
    return "QSPI";
}

Analyzer* CreateAnalyzer()
{
    return new QuadSpiAnalyzer();
}

void DestroyAnalyzer( Analyzer* analyzer )
{
    delete analyzer;
}
