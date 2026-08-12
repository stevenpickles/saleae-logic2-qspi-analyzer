#include "QuadSpiAnalyzerResults.h"
#include <AnalyzerHelpers.h>
#include "QuadSpiAnalyzer.h"
#include "QuadSpiAnalyzerSettings.h"
#include <fstream>
#include <string>

namespace
{
    const char* FramePhaseName( const Frame& frame )
    {
        switch( frame.mType )
        {
        case FRAME_COMMAND:
            return "Command";
        case FRAME_ADDRESS:
            return "Address";
        case FRAME_DUMMY:
            return "Dummy";
        case FRAME_DATA:
            return "Data";
        default:
            return "Error";
        }
    }
}

QuadSpiAnalyzerResults::QuadSpiAnalyzerResults( QuadSpiAnalyzer* analyzer, QuadSpiAnalyzerSettings* settings )
    : AnalyzerResults(), mSettings( settings ), mAnalyzer( analyzer )
{
}

QuadSpiAnalyzerResults::~QuadSpiAnalyzerResults()
{
}

void QuadSpiAnalyzerResults::GenerateBubbleText( U64 frame_index, Channel& channel, DisplayBase display_base )
{
    ClearResultStrings();
    Frame frame = GetFrame( frame_index );

    char number_str[ 128 ];
    char io1_str[ 128 ];
    std::string long_text;

    switch( frame.mType )
    {
    case FRAME_COMMAND:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, static_cast<U32>( frame.mData2 ), number_str, sizeof( number_str ) );
        AddResultString( "C" );
        AddResultString( "CMD" );
        AddResultString( "CMD ", number_str );
        break;

    case FRAME_ADDRESS:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, static_cast<U32>( frame.mData2 ), number_str, sizeof( number_str ) );
        AddResultString( "A" );
        AddResultString( "ADDR" );
        AddResultString( "ADDR ", number_str );
        break;

    case FRAME_DUMMY:
        AnalyzerHelpers::GetNumberString( frame.mData1, Decimal, 8, number_str, sizeof( number_str ) );
        AddResultString( "D" );
        AddResultString( "DUMMY" );
        AddResultString( "DUMMY x", number_str );
        break;

    case FRAME_DATA:
    {
        U32 bits = static_cast<U32>( frame.mData2 >> 32 );
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, bits, number_str, sizeof( number_str ) );
        AddResultString( number_str );
        if( mSettings->mDataBusWidth == 1 )
        {
            AnalyzerHelpers::GetNumberString( frame.mData2 & 0xFFFFFFFF, display_base, bits, io1_str, sizeof( io1_str ) );
            long_text = std::string( "IO0: " ) + number_str + "; IO1: " + io1_str;
            AddResultString( long_text.c_str() );
        }
        break;
    }

    default: // FRAME_ERROR
        AddResultString( "!" );
        if( frame.HasFlag( QSPI_TRUNCATED_FLAG ) )
        {
            AddResultString( "Truncated" );
            AddResultString( "Truncated: CS deasserted mid-phase" );
        }
        else
        {
            AddResultString( "Error" );
            AddResultString( "Error: clock not idle at CS assert" );
        }
        break;
    }
}

void QuadSpiAnalyzerResults::GenerateExportFile( const char* file, DisplayBase display_base, U32 export_type_user_id )
{
    std::ofstream file_stream( file, std::ios::out );

    U64 trigger_sample = mAnalyzer->GetTriggerSample();
    U32 sample_rate = mAnalyzer->GetSampleRate();

    file_stream << "Time [s],Phase,Value,IO1 Value (SIO data only)" << std::endl;

    U64 num_frames = GetNumFrames();
    for( U64 i = 0; i < num_frames; i++ )
    {
        Frame frame = GetFrame( i );

        char time_str[ 128 ];
        AnalyzerHelpers::GetTimeString( frame.mStartingSampleInclusive, trigger_sample, sample_rate, time_str, sizeof( time_str ) );

        char number_str[ 128 ] = "";
        char io1_str[ 128 ] = "";

        switch( frame.mType )
        {
        case FRAME_COMMAND:
        case FRAME_ADDRESS:
            AnalyzerHelpers::GetNumberString( frame.mData1, display_base, static_cast<U32>( frame.mData2 ), number_str,
                                              sizeof( number_str ) );
            break;
        case FRAME_DUMMY:
            AnalyzerHelpers::GetNumberString( frame.mData1, Decimal, 8, number_str, sizeof( number_str ) );
            break;
        case FRAME_DATA:
        {
            U32 bits = static_cast<U32>( frame.mData2 >> 32 );
            AnalyzerHelpers::GetNumberString( frame.mData1, display_base, bits, number_str, sizeof( number_str ) );
            if( mSettings->mDataBusWidth == 1 )
                AnalyzerHelpers::GetNumberString( frame.mData2 & 0xFFFFFFFF, display_base, bits, io1_str, sizeof( io1_str ) );
            break;
        }
        default:
            break;
        }

        file_stream << time_str << "," << FramePhaseName( frame ) << "," << number_str << "," << io1_str << std::endl;

        if( UpdateExportProgressAndCheckForCancel( i, num_frames ) == true )
        {
            file_stream.close();
            return;
        }
    }

    UpdateExportProgressAndCheckForCancel( num_frames, num_frames );
    file_stream.close();
}

void QuadSpiAnalyzerResults::GenerateFrameTabularText( U64 frame_index, DisplayBase display_base )
{
#ifdef SUPPORTS_PROTOCOL_SEARCH
    Frame frame = GetFrame( frame_index );
    ClearTabularText();

    char number_str[ 128 ];

    switch( frame.mType )
    {
    case FRAME_COMMAND:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, static_cast<U32>( frame.mData2 ), number_str, sizeof( number_str ) );
        AddTabularText( "CMD ", number_str );
        break;
    case FRAME_ADDRESS:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, static_cast<U32>( frame.mData2 ), number_str, sizeof( number_str ) );
        AddTabularText( "ADDR ", number_str );
        break;
    case FRAME_DUMMY:
        AnalyzerHelpers::GetNumberString( frame.mData1, Decimal, 8, number_str, sizeof( number_str ) );
        AddTabularText( "DUMMY x", number_str );
        break;
    case FRAME_DATA:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, static_cast<U32>( frame.mData2 >> 32 ), number_str,
                                          sizeof( number_str ) );
        AddTabularText( number_str );
        break;
    default:
        AddTabularText( frame.HasFlag( QSPI_TRUNCATED_FLAG ) ? "Truncated: CS deasserted mid-phase" : "Error: clock not idle at CS assert" );
        break;
    }
#endif
}

void QuadSpiAnalyzerResults::GeneratePacketTabularText( U64 packet_id, DisplayBase display_base )
{
    // not supported
}

void QuadSpiAnalyzerResults::GenerateTransactionTabularText( U64 transaction_id, DisplayBase display_base )
{
    // not supported
}
