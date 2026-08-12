// QSPI analyzer decode tests.
//
// Two layers of coverage:
//  1. Golden tests: hand-authored transition lists written directly from the
//     QSPI spec, asserting exact decoded values and sample positions. These
//     anchor the decoder to ground truth independently of the simulator.
//  2. Permutation sweep: closed-loop simulator -> decoder runs across every
//     combination of per-phase bus width, address length, dummy cycles,
//     CPOL/CPHA and CS polarity, asserting the known simulated transaction
//     decodes exactly.

#include "../src/QuadSpiAnalyzer.h"
#include "mock_sdk.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// tiny test harness
// ---------------------------------------------------------------------------

static int g_checks = 0;
static int g_failures = 0;
static std::string g_context;

static void Fail( const std::string& message )
{
    ++g_failures;
    std::fprintf( stderr, "FAIL [%s] %s\n", g_context.c_str(), message.c_str() );
}

static void Check( bool ok, const std::string& message )
{
    ++g_checks;
    if( !ok )
        Fail( message );
}

static void CheckEq( U64 actual, U64 expected, const std::string& what )
{
    ++g_checks;
    if( actual != expected )
    {
        std::ostringstream stream;
        stream << what << ": expected 0x" << std::hex << expected << ", got 0x" << actual;
        Fail( stream.str() );
    }
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static Channel Ch( U32 index )
{
    return Channel( 0, index, DIGITAL_CHANNEL );
}

// builds a transition list by tracking level changes, for hand-authored waveforms
struct Wave
{
    BitState mInitial;
    BitState mCurrent;
    std::vector<U64> mTransitions;

    explicit Wave( BitState initial ) : mInitial( initial ), mCurrent( initial )
    {
    }

    void Set( U64 sample, BitState state )
    {
        if( mCurrent != state )
        {
            mTransitions.push_back( sample );
            mCurrent = state;
        }
    }
};

class TestQuadSpiAnalyzer : public QuadSpiAnalyzer
{
  public:
    QuadSpiAnalyzerSettings& Settings()
    {
        return mSettings;
    }
    QuadSpiAnalyzerResults* Results()
    {
        return mResults.get();
    }
};

struct Perm
{
    U32 mCommandWidth;
    U32 mAddressWidth;
    U32 mDataWidth;
    U32 mAddressBits;
    U32 mDummyCycles;
    BitState mClockIdle;
    AnalyzerEnums::Edge mDataValidEdge;
    BitState mChipSelectActive;
};

static std::string DescribePerm( const Perm& p )
{
    std::ostringstream stream;
    stream << "cmd_w=" << p.mCommandWidth << " addr_w=" << p.mAddressWidth << " data_w=" << p.mDataWidth << " addr_bits=" << p.mAddressBits
           << " dummy=" << p.mDummyCycles << " cpol=" << ( p.mClockIdle == BIT_HIGH ? 1 : 0 )
           << " cpha=" << ( p.mDataValidEdge == AnalyzerEnums::TrailingEdge ? 1 : 0 )
           << " cs_active=" << ( p.mChipSelectActive == BIT_HIGH ? "high" : "low" );
    return stream.str();
}

static void ApplyPerm( QuadSpiAnalyzerSettings& settings, const Perm& p, bool define_quad_lines )
{
    settings.mChipSelectChannel = Ch( 0 );
    settings.mClockChannel = Ch( 1 );
    settings.mData0Channel = Ch( 2 );
    settings.mData1Channel = Ch( 3 );
    settings.mData2Channel = define_quad_lines ? Ch( 4 ) : UNDEFINED_CHANNEL;
    settings.mData3Channel = define_quad_lines ? Ch( 5 ) : UNDEFINED_CHANNEL;
    settings.mCommandBusWidth = p.mCommandWidth;
    settings.mAddressBusWidth = p.mAddressWidth;
    settings.mDataBusWidth = p.mDataWidth;
    settings.mAddressBits = p.mAddressBits;
    settings.mDummyCycles = p.mDummyCycles;
    settings.mClockInactiveState = p.mClockIdle;
    settings.mDataValidEdge = p.mDataValidEdge;
    settings.mChipSelectActiveState = p.mChipSelectActive;
}

// runs WorkerThread until the mock capture data is exhausted
static bool RunWorkerThread( TestQuadSpiAnalyzer& analyzer )
{
    analyzer.SetupResults();
    try
    {
        analyzer.WorkerThread();
    }
    catch( MockEndOfData& )
    {
        return true;
    }
    catch( MockOpLimitExceeded& )
    {
        Fail( "decode exceeded operation limit -- possible infinite loop" );
        return false;
    }
    Fail( "WorkerThread returned instead of running out of data" );
    return false;
}

// ---------------------------------------------------------------------------
// permutation sweep: simulator -> decoder closed loop
// ---------------------------------------------------------------------------

static const U32 kSimSampleRate = 200; // gives 10 samples per clock half-period
static const U64 kSimTargetSample = 12000;

static void RunSweepCase( const Perm& p )
{
    g_context = DescribePerm( p );

    bool quad_used = p.mCommandWidth == 4 || p.mAddressWidth == 4 || p.mDataWidth == 4;

    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, quad_used );

    // generate the simulated capture
    MockSdk::ResetChannelData();
    MockSdk::SetSampleRate( kSimSampleRate );
    MockSdk::SetOpLimit( 20000000ull );

    QuadSpiSimulationDataGenerator generator;
    generator.Initialize( kSimSampleRate, &analyzer.Settings() );
    SimulationChannelDescriptor* descriptors = nullptr;
    U32 descriptor_count = generator.GenerateSimulationData( kSimTargetSample, kSimSampleRate, &descriptors );

    U32 expected_descriptors = quad_used ? 6 : 4;
    CheckEq( descriptor_count, expected_descriptors, "simulated channel count" );

    // feed the recorded waveforms into the decoder's channels
    for( U32 i = 0; i < descriptor_count; ++i )
    {
        SimulationChannelDescriptor* descriptor = &descriptors[ i ];
        MockSdk::SetChannelData( MockSdk::GetSimChannel( descriptor ),
                                 MockSdk::MakeChannelData( MockSdk::GetSimInitialState( descriptor ),
                                                           MockSdk::GetSimTransitions( descriptor ) ) );
    }

    if( !RunWorkerThread( analyzer ) )
        return;

    // verify the decoded frames
    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    size_t frames_per_transaction = 1 + ( p.mAddressBits > 0 ? 1 : 0 ) + ( p.mDummyCycles > 0 ? 1 : 0 ) + 4;

    Check( frames.size() >= 2 * frames_per_transaction, "at least two transactions decoded" );
    Check( frames.size() % frames_per_transaction == 0, "no unexpected error/partial frames" );
    if( frames.size() < 2 * frames_per_transaction )
        return;

    static const U8 kExpectedData[ 4 ] = { 0xDE, 0xAD, 0xBE, 0xEF };
    U64 address_mask = p.mAddressBits >= 64 ? ~0ull : ( 1ull << p.mAddressBits ) - 1;

    for( size_t transaction = 0; transaction < 2; ++transaction )
    {
        size_t index = transaction * frames_per_transaction;

        const Frame& command = frames[ index++ ];
        CheckEq( command.mType, FRAME_COMMAND, "command frame type" );
        CheckEq( command.mData1, 0xEB, "command opcode" );
        CheckEq( command.mData2, 8, "command bit count" );
        CheckEq( command.mFlags, 0, "command flags" );

        if( p.mAddressBits > 0 )
        {
            const Frame& address = frames[ index++ ];
            CheckEq( address.mType, FRAME_ADDRESS, "address frame type" );
            CheckEq( address.mData1, 0x001000 & address_mask, "address value" );
            CheckEq( address.mData2, p.mAddressBits, "address bit count" );
        }

        if( p.mDummyCycles > 0 )
        {
            const Frame& dummy = frames[ index++ ];
            CheckEq( dummy.mType, FRAME_DUMMY, "dummy frame type" );
            CheckEq( dummy.mData1, p.mDummyCycles, "dummy cycle count" );
        }

        for( size_t byte_index = 0; byte_index < 4; ++byte_index )
        {
            const Frame& data = frames[ index++ ];
            CheckEq( data.mType, FRAME_DATA, "data frame type" );
            CheckEq( data.mData2 >> 32, 8, "data bit count" );
            if( p.mDataWidth == 1 )
            {
                // single-IO data simulates a read: response on IO1, IO0 idles high
                CheckEq( data.mData1, 0xFF, "data IO0 lane (idle)" );
                CheckEq( data.mData2 & 0xFFFFFFFF, kExpectedData[ byte_index ], "data IO1 lane" );
            }
            else
            {
                CheckEq( data.mData1, kExpectedData[ byte_index ], "data value" );
            }
        }
    }

    // verify the FrameV2 stream for the first transaction
    const std::vector<RecordedFrameV2>& frames_v2 = MockSdk::GetFramesV2( analyzer.Results() );
    std::vector<std::string> expected_v2;
    expected_v2.push_back( "enable" );
    expected_v2.push_back( "command" );
    if( p.mAddressBits > 0 )
        expected_v2.push_back( "address" );
    if( p.mDummyCycles > 0 )
        expected_v2.push_back( "dummy" );
    for( int i = 0; i < 4; ++i )
        expected_v2.push_back( "data" );
    expected_v2.push_back( "disable" );

    Check( frames_v2.size() % expected_v2.size() == 0, "FrameV2 count is a whole number of transactions" );
    if( frames_v2.size() >= expected_v2.size() )
    {
        for( size_t i = 0; i < expected_v2.size(); ++i )
            Check( frames_v2[ i ].mType == expected_v2[ i ],
                   "FrameV2 sequence at " + std::to_string( i ) + ": expected " + expected_v2[ i ] + ", got " + frames_v2[ i ].mType );
        Check( frames_v2[ 1 ].mFields.count( "cmd" ) != 0 && frames_v2[ 1 ].mFields.at( "cmd" ) == "235",
               "FrameV2 command field cmd=235 (0xEB)" );
    }

    // verify sampling-edge marker direction on the clock channel
    bool sampling_edge_rising = ( p.mClockIdle == BIT_LOW && p.mDataValidEdge == AnalyzerEnums::LeadingEdge ) ||
                                ( p.mClockIdle == BIT_HIGH && p.mDataValidEdge == AnalyzerEnums::TrailingEdge );
    AnalyzerResults::MarkerType expected_marker = sampling_edge_rising ? AnalyzerResults::UpArrow : AnalyzerResults::DownArrow;
    size_t clock_markers = 0;
    bool clock_markers_correct = true;
    for( const RecordedMarker& marker : MockSdk::GetMarkers( analyzer.Results() ) )
    {
        if( marker.mChannel == Ch( 1 ) )
        {
            ++clock_markers;
            if( marker.mMarkerType != expected_marker )
                clock_markers_correct = false;
        }
    }
    Check( clock_markers > 0, "clock sampling markers present" );
    Check( clock_markers_correct, "clock sampling markers have the correct edge direction" );
}

static void RunSweep()
{
    const U32 widths[] = { 1, 2, 4 };
    const U32 address_bits[] = { 0, 8, 16, 24, 32 };
    const U32 dummy_cycles[] = { 0, 1, 6, 63 };
    const BitState levels[] = { BIT_LOW, BIT_HIGH };
    const AnalyzerEnums::Edge edges[] = { AnalyzerEnums::LeadingEdge, AnalyzerEnums::TrailingEdge };

    int cases = 0;
    for( U32 command_width : widths )
        for( U32 address_width : widths )
            for( U32 data_width : widths )
                for( U32 bits : address_bits )
                    for( U32 dummies : dummy_cycles )
                        for( BitState cpol : levels )
                            for( AnalyzerEnums::Edge cpha : edges )
                                for( BitState cs_active : levels )
                                {
                                    Perm p = { command_width, address_width, data_width, bits, dummies, cpol, cpha, cs_active };
                                    RunSweepCase( p );
                                    ++cases;
                                }
    std::printf( "sweep: %d permutations\n", cases );
}

// a few dual/single-only cases with IO2/IO3 left unconnected
static void RunOptionalChannelCases()
{
    const U32 widths[] = { 1, 2 };
    for( U32 command_width : widths )
        for( U32 data_width : widths )
        {
            Perm p = { command_width, command_width, data_width, 24, 4, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
            g_context = "optional-io23 " + DescribePerm( p );

            TestQuadSpiAnalyzer analyzer;
            ApplyPerm( analyzer.Settings(), p, false /* IO2/IO3 undefined */ );

            MockSdk::ResetChannelData();
            MockSdk::SetSampleRate( kSimSampleRate );
            MockSdk::SetOpLimit( 20000000ull );

            QuadSpiSimulationDataGenerator generator;
            generator.Initialize( kSimSampleRate, &analyzer.Settings() );
            SimulationChannelDescriptor* descriptors = nullptr;
            U32 descriptor_count = generator.GenerateSimulationData( kSimTargetSample, kSimSampleRate, &descriptors );
            CheckEq( descriptor_count, 4, "only 4 channels simulated when IO2/IO3 unset" );

            for( U32 i = 0; i < descriptor_count; ++i )
            {
                SimulationChannelDescriptor* descriptor = &descriptors[ i ];
                MockSdk::SetChannelData( MockSdk::GetSimChannel( descriptor ),
                                         MockSdk::MakeChannelData( MockSdk::GetSimInitialState( descriptor ),
                                                                   MockSdk::GetSimTransitions( descriptor ) ) );
            }

            if( !RunWorkerThread( analyzer ) )
                continue;

            const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
            size_t frames_per_transaction = 1 + 1 + 1 + 4;
            Check( frames.size() >= frames_per_transaction && frames[ 0 ].mType == FRAME_COMMAND && frames[ 0 ].mData1 == 0xEB,
                   "decode works with IO2/IO3 unconnected" );
        }
}

// ---------------------------------------------------------------------------
// golden tests: hand-authored waveforms
// ---------------------------------------------------------------------------

// Mode 0, CS active low. Command 0xEB on IO0 (single), then one data byte 0xA5
// in quad. Clock cycle = 20 samples: data set at T, rising edge T+10 (sampled),
// falling edge T+20.
static void GoldenBasicTransaction()
{
    g_context = "golden basic";

    Perm p = { 1, 4, 4, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );

    cs.Set( 100, BIT_LOW );

    // command 0xEB = 1110 1011, MSB first on IO0
    const int command_bits[ 8 ] = { 1, 1, 1, 0, 1, 0, 1, 1 };
    for( int k = 0; k < 8; ++k )
    {
        U64 t = 110 + 20 * k;
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_HIGH ); // rising: sampled here
        sck.Set( t + 20, BIT_LOW );
    }

    // data byte 0xA5 in quad: high nibble 0xA (IO3=1 IO2=0 IO1=1 IO0=0), low nibble 0x5 (IO3=0 IO2=1 IO1=0 IO0=1)
    io0.Set( 290, BIT_LOW );
    io1.Set( 290, BIT_HIGH );
    io2.Set( 290, BIT_LOW );
    io3.Set( 290, BIT_HIGH );
    sck.Set( 300, BIT_HIGH );
    sck.Set( 310, BIT_LOW );
    io0.Set( 310, BIT_HIGH );
    io1.Set( 310, BIT_LOW );
    io2.Set( 310, BIT_HIGH );
    io3.Set( 310, BIT_LOW );
    sck.Set( 320, BIT_HIGH );
    sck.Set( 330, BIT_LOW );

    cs.Set( 370, BIT_HIGH );

    MockSdk::ResetChannelData();
    MockSdk::SetSampleRate( kSimSampleRate );
    MockSdk::SetOpLimit( 1000000ull );
    MockSdk::SetChannelData( Ch( 0 ), MockSdk::MakeChannelData( cs.mInitial, cs.mTransitions ) );
    MockSdk::SetChannelData( Ch( 1 ), MockSdk::MakeChannelData( sck.mInitial, sck.mTransitions ) );
    MockSdk::SetChannelData( Ch( 2 ), MockSdk::MakeChannelData( io0.mInitial, io0.mTransitions ) );
    MockSdk::SetChannelData( Ch( 3 ), MockSdk::MakeChannelData( io1.mInitial, io1.mTransitions ) );
    MockSdk::SetChannelData( Ch( 4 ), MockSdk::MakeChannelData( io2.mInitial, io2.mTransitions ) );
    MockSdk::SetChannelData( Ch( 5 ), MockSdk::MakeChannelData( io3.mInitial, io3.mTransitions ) );

    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 2, "frame count" );
    if( frames.size() != 2 )
        return;

    CheckEq( frames[ 0 ].mType, FRAME_COMMAND, "command frame type" );
    CheckEq( frames[ 0 ].mData1, 0xEB, "command value" );
    CheckEq( frames[ 0 ].mStartingSampleInclusive, 120, "command starts at first sampled edge" );
    CheckEq( frames[ 0 ].mEndingSampleInclusive, 260, "command ends at last sampled edge" );

    CheckEq( frames[ 1 ].mType, FRAME_DATA, "data frame type" );
    CheckEq( frames[ 1 ].mData1, 0xA5, "data value" );
    CheckEq( frames[ 1 ].mStartingSampleInclusive, 300, "data starts at first sampled edge" );
    CheckEq( frames[ 1 ].mEndingSampleInclusive, 320, "data ends at last sampled edge" );

    const std::vector<RecordedFrameV2>& frames_v2 = MockSdk::GetFramesV2( analyzer.Results() );
    CheckEq( frames_v2.size(), 4, "FrameV2 count" );
    if( frames_v2.size() == 4 )
    {
        Check( frames_v2[ 0 ].mType == "enable" && frames_v2[ 0 ].mStartingSample == 100, "enable frame at CS assert" );
        Check( frames_v2[ 1 ].mType == "command", "command FrameV2" );
        Check( frames_v2[ 2 ].mType == "data" && frames_v2[ 2 ].mFields.at( "data" ) == "165", "data FrameV2 value 165 (0xA5)" );
        Check( frames_v2[ 3 ].mType == "disable" && frames_v2[ 3 ].mStartingSample == 370, "disable frame at CS deassert" );
    }
}

// CS deasserts after only 4 command bits -> truncated error frame
static void GoldenTruncatedCommand()
{
    g_context = "golden truncated command";

    Perm p = { 1, 1, 1, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH );

    cs.Set( 100, BIT_LOW );
    // first 4 bits of 0xEB: 1, 1, 1, 0 -- then CS deasserts at 195, before the 5th rising edge at 200
    const int command_bits[ 4 ] = { 1, 1, 1, 0 };
    for( int k = 0; k < 4; ++k )
    {
        U64 t = 110 + 20 * k;
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_HIGH );
        sck.Set( t + 20, BIT_LOW );
    }
    cs.Set( 195, BIT_HIGH );
    sck.Set( 200, BIT_HIGH ); // clock keeps running briefly after deassert
    sck.Set( 210, BIT_LOW );

    MockSdk::ResetChannelData();
    MockSdk::SetSampleRate( kSimSampleRate );
    MockSdk::SetOpLimit( 1000000ull );
    MockSdk::SetChannelData( Ch( 0 ), MockSdk::MakeChannelData( cs.mInitial, cs.mTransitions ) );
    MockSdk::SetChannelData( Ch( 1 ), MockSdk::MakeChannelData( sck.mInitial, sck.mTransitions ) );
    MockSdk::SetChannelData( Ch( 2 ), MockSdk::MakeChannelData( io0.mInitial, io0.mTransitions ) );
    MockSdk::SetChannelData( Ch( 3 ), MockSdk::MakeChannelData( io1.mInitial, io1.mTransitions ) );
    MockSdk::SetChannelData( Ch( 4 ), MockSdk::MakeChannelData( BIT_HIGH, std::vector<U64>() ) );
    MockSdk::SetChannelData( Ch( 5 ), MockSdk::MakeChannelData( BIT_HIGH, std::vector<U64>() ) );

    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 1, "frame count" );
    if( frames.empty() )
        return;

    CheckEq( frames[ 0 ].mType, FRAME_ERROR, "truncated command becomes error frame" );
    Check( ( frames[ 0 ].mFlags & QSPI_TRUNCATED_FLAG ) != 0, "truncated flag set" );
    Check( ( frames[ 0 ].mFlags & DISPLAY_AS_ERROR_FLAG ) != 0, "error display flag set" );
    CheckEq( frames[ 0 ].mData1, 0xE, "partial command bits value (1110)" );
    CheckEq( frames[ 0 ].mData2, 4, "partial command bit count" );
}

// clock is high when CS asserts (mode 0) -> error frame, then the next
// transaction decodes normally
static void GoldenClockNotIdle()
{
    g_context = "golden clock not idle";

    Perm p = { 1, 1, 1, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH );

    // bad transaction: clock stuck high across the CS assert
    sck.Set( 50, BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    sck.Set( 150, BIT_LOW );
    cs.Set( 200, BIT_HIGH );

    // good transaction: command 0x9F = 1001 1111
    cs.Set( 300, BIT_LOW );
    const int command_bits[ 8 ] = { 1, 0, 0, 1, 1, 1, 1, 1 };
    for( int k = 0; k < 8; ++k )
    {
        U64 t = 310 + 20 * k;
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_HIGH );
        sck.Set( t + 20, BIT_LOW );
    }
    cs.Set( 490, BIT_HIGH );

    MockSdk::ResetChannelData();
    MockSdk::SetSampleRate( kSimSampleRate );
    MockSdk::SetOpLimit( 1000000ull );
    MockSdk::SetChannelData( Ch( 0 ), MockSdk::MakeChannelData( cs.mInitial, cs.mTransitions ) );
    MockSdk::SetChannelData( Ch( 1 ), MockSdk::MakeChannelData( sck.mInitial, sck.mTransitions ) );
    MockSdk::SetChannelData( Ch( 2 ), MockSdk::MakeChannelData( io0.mInitial, io0.mTransitions ) );
    MockSdk::SetChannelData( Ch( 3 ), MockSdk::MakeChannelData( io1.mInitial, io1.mTransitions ) );
    MockSdk::SetChannelData( Ch( 4 ), MockSdk::MakeChannelData( BIT_HIGH, std::vector<U64>() ) );
    MockSdk::SetChannelData( Ch( 5 ), MockSdk::MakeChannelData( BIT_HIGH, std::vector<U64>() ) );

    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 2, "frame count" );
    if( frames.size() != 2 )
        return;

    CheckEq( frames[ 0 ].mType, FRAME_ERROR, "clock-not-idle error frame" );
    Check( ( frames[ 0 ].mFlags & DISPLAY_AS_ERROR_FLAG ) != 0, "error display flag set" );
    Check( ( frames[ 0 ].mFlags & QSPI_TRUNCATED_FLAG ) == 0, "not a truncation" );

    CheckEq( frames[ 1 ].mType, FRAME_COMMAND, "next transaction decodes normally" );
    CheckEq( frames[ 1 ].mData1, 0x9F, "next transaction command value" );
}

// ---------------------------------------------------------------------------
// settings tests
// ---------------------------------------------------------------------------

static void SettingsRoundTrip()
{
    g_context = "settings round trip";

    QuadSpiAnalyzerSettings original;
    original.mChipSelectChannel = Ch( 5 );
    original.mClockChannel = Ch( 4 );
    original.mData0Channel = Ch( 3 );
    original.mData1Channel = Ch( 2 );
    original.mData2Channel = Ch( 1 );
    original.mData3Channel = Ch( 0 );
    original.mCommandBusWidth = 2;
    original.mAddressBusWidth = 4;
    original.mDataBusWidth = 1;
    original.mAddressBits = 32;
    original.mDummyCycles = 17;
    original.mClockInactiveState = BIT_HIGH;
    original.mDataValidEdge = AnalyzerEnums::TrailingEdge;
    original.mChipSelectActiveState = BIT_HIGH;

    std::string saved = original.SaveSettings();

    QuadSpiAnalyzerSettings loaded;
    loaded.LoadSettings( saved.c_str() );

    Check( loaded.mChipSelectChannel == original.mChipSelectChannel, "CS channel" );
    Check( loaded.mClockChannel == original.mClockChannel, "clock channel" );
    Check( loaded.mData0Channel == original.mData0Channel, "IO0 channel" );
    Check( loaded.mData1Channel == original.mData1Channel, "IO1 channel" );
    Check( loaded.mData2Channel == original.mData2Channel, "IO2 channel" );
    Check( loaded.mData3Channel == original.mData3Channel, "IO3 channel" );
    CheckEq( loaded.mCommandBusWidth, original.mCommandBusWidth, "command width" );
    CheckEq( loaded.mAddressBusWidth, original.mAddressBusWidth, "address width" );
    CheckEq( loaded.mDataBusWidth, original.mDataBusWidth, "data width" );
    CheckEq( loaded.mAddressBits, original.mAddressBits, "address bits" );
    CheckEq( loaded.mDummyCycles, original.mDummyCycles, "dummy cycles" );
    CheckEq( loaded.mClockInactiveState, original.mClockInactiveState, "CPOL" );
    CheckEq( loaded.mDataValidEdge, original.mDataValidEdge, "CPHA" );
    CheckEq( loaded.mChipSelectActiveState, original.mChipSelectActiveState, "CS active state" );
}

static void SettingsValidation()
{
    g_context = "settings validation";

    QuadSpiAnalyzerSettings settings;
    settings.mChipSelectChannel = Ch( 0 );
    settings.mClockChannel = Ch( 1 );
    settings.mData0Channel = Ch( 2 );
    settings.mData1Channel = Ch( 3 );
    settings.mData2Channel = UNDEFINED_CHANNEL;
    settings.mData3Channel = UNDEFINED_CHANNEL;
    settings.mCommandBusWidth = 1;
    settings.mAddressBusWidth = 1;
    settings.mDataBusWidth = 4; // quad without IO2/IO3 -> invalid
    settings.UpdateInterfacesFromSettings();
    Check( settings.SetSettingsFromInterfaces() == false, "quad width without IO2/IO3 is rejected" );

    settings.mDataBusWidth = 2;
    settings.UpdateInterfacesFromSettings();
    Check( settings.SetSettingsFromInterfaces() == true, "dual width without IO2/IO3 is accepted" );

    settings.mData1Channel = Ch( 2 ); // duplicate of IO0
    settings.UpdateInterfacesFromSettings();
    Check( settings.SetSettingsFromInterfaces() == false, "duplicate channels are rejected" );

    // full-quad configuration with all channels defined is accepted
    settings.mData1Channel = Ch( 3 );
    settings.mData2Channel = Ch( 4 );
    settings.mData3Channel = Ch( 5 );
    settings.mCommandBusWidth = 4;
    settings.mAddressBusWidth = 4;
    settings.mDataBusWidth = 4;
    settings.UpdateInterfacesFromSettings();
    Check( settings.SetSettingsFromInterfaces() == true, "quad width with IO2/IO3 defined is accepted" );

    // quad via command or address width alone also requires IO2/IO3
    settings.mData2Channel = UNDEFINED_CHANNEL;
    settings.mData3Channel = UNDEFINED_CHANNEL;
    settings.mAddressBusWidth = 1;
    settings.mDataBusWidth = 1;
    settings.UpdateInterfacesFromSettings();
    Check( settings.SetSettingsFromInterfaces() == false, "quad command width without IO2/IO3 is rejected" );
    settings.mCommandBusWidth = 1;
    settings.mAddressBusWidth = 4;
    settings.UpdateInterfacesFromSettings();
    Check( settings.SetSettingsFromInterfaces() == false, "quad address width without IO2/IO3 is rejected" );

    // quad with only IO3 missing is still rejected
    settings.mData2Channel = Ch( 4 );
    settings.UpdateInterfacesFromSettings();
    Check( settings.SetSettingsFromInterfaces() == false, "quad width with only IO3 missing is rejected" );

    // each required channel is validated individually
    const char* required_names[ 4 ] = { "CS", "SCK", "IO0", "IO1" };
    for( int missing = 0; missing < 4; ++missing )
    {
        QuadSpiAnalyzerSettings required;
        required.mChipSelectChannel = missing == 0 ? UNDEFINED_CHANNEL : Ch( 0 );
        required.mClockChannel = missing == 1 ? UNDEFINED_CHANNEL : Ch( 1 );
        required.mData0Channel = missing == 2 ? UNDEFINED_CHANNEL : Ch( 2 );
        required.mData1Channel = missing == 3 ? UNDEFINED_CHANNEL : Ch( 3 );
        required.mData2Channel = UNDEFINED_CHANNEL;
        required.mData3Channel = UNDEFINED_CHANNEL;
        required.mCommandBusWidth = 1;
        required.mAddressBusWidth = 1;
        required.mDataBusWidth = 1;
        required.UpdateInterfacesFromSettings();
        Check( required.SetSettingsFromInterfaces() == false, std::string( "missing " ) + required_names[ missing ] + " is rejected" );
    }
}

// ---------------------------------------------------------------------------
// coverage-driven tests: rarely-hit branches, rendering, API surface
// ---------------------------------------------------------------------------

static void RegisterWaves( Wave& cs, Wave& sck, Wave& io0, Wave& io1, Wave* io2, Wave* io3 )
{
    MockSdk::ResetChannelData();
    MockSdk::SetSampleRate( kSimSampleRate );
    MockSdk::SetOpLimit( 1000000ull );
    MockSdk::SetChannelData( Ch( 0 ), MockSdk::MakeChannelData( cs.mInitial, cs.mTransitions ) );
    MockSdk::SetChannelData( Ch( 1 ), MockSdk::MakeChannelData( sck.mInitial, sck.mTransitions ) );
    MockSdk::SetChannelData( Ch( 2 ), MockSdk::MakeChannelData( io0.mInitial, io0.mTransitions ) );
    MockSdk::SetChannelData( Ch( 3 ), MockSdk::MakeChannelData( io1.mInitial, io1.mTransitions ) );
    if( io2 != nullptr )
        MockSdk::SetChannelData( Ch( 4 ), MockSdk::MakeChannelData( io2->mInitial, io2->mTransitions ) );
    if( io3 != nullptr )
        MockSdk::SetChannelData( Ch( 5 ), MockSdk::MakeChannelData( io3->mInitial, io3->mTransitions ) );
}

// capture starts with CS already active -> the decoder must skip the partial
// transaction and decode the next complete one
static void GoldenCsActiveAtStart()
{
    g_context = "golden CS active at start";

    Perm p = { 1, 1, 1, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_LOW ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
    cs.Set( 50, BIT_HIGH ); // deassert of the partial transaction
    cs.Set( 100, BIT_LOW ); // start of the complete transaction

    const int command_bits[ 8 ] = { 1, 1, 1, 0, 1, 0, 1, 1 };
    for( int k = 0; k < 8; ++k )
    {
        U64 t = 110 + 20 * k;
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_HIGH );
        sck.Set( t + 20, BIT_LOW );
    }
    cs.Set( 300, BIT_HIGH );

    RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 1, "frame count" );
    if( !frames.empty() )
        CheckEq( frames[ 0 ].mData1, 0xEB, "command decoded after skipping partial transaction" );
}

// quad command truncated after a single nibble -> single-sample frame gets the
// +1 ending-sample fallback
static void GoldenTruncatedSingleChunkCommand()
{
    g_context = "golden truncated single-chunk command";

    Perm p = { 4, 4, 4, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    // one quad nibble 0xE (IO3=1 IO2=1 IO1=1 IO0=0), sampled on the rising edge at 120
    io0.Set( 110, BIT_LOW );
    sck.Set( 120, BIT_HIGH );
    sck.Set( 130, BIT_LOW );
    cs.Set( 140, BIT_HIGH ); // deassert before the second nibble
    sck.Set( 160, BIT_HIGH );
    sck.Set( 170, BIT_LOW );

    RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 1, "frame count" );
    if( frames.empty() )
        return;
    CheckEq( frames[ 0 ].mType, FRAME_ERROR, "truncated frame type" );
    CheckEq( frames[ 0 ].mData1, 0xE, "partial nibble value" );
    CheckEq( frames[ 0 ].mData2, 4, "partial bit count" );
    CheckEq( frames[ 0 ].mStartingSampleInclusive, 120, "frame start" );
    CheckEq( frames[ 0 ].mEndingSampleInclusive, 121, "single-sample frame extends end by one" );
}

// CPHA=0: CS deasserts between a mid-word sampling edge and its trailing edge
static void GoldenCpha0MidCycleTruncation()
{
    g_context = "golden CPHA0 mid-cycle truncation";

    Perm p = { 1, 1, 1, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    // bits 1,1,1,0,1 sampled at rising edges 120..200; CS deasserts at 205, before the falling edge at 210
    const int command_bits[ 5 ] = { 1, 1, 1, 0, 1 };
    for( int k = 0; k < 5; ++k )
    {
        U64 t = 110 + 20 * k;
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_HIGH );
        if( k < 4 )
            sck.Set( t + 20, BIT_LOW );
    }
    cs.Set( 205, BIT_HIGH );
    sck.Set( 210, BIT_LOW );

    RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 1, "frame count" );
    if( frames.empty() )
        return;
    CheckEq( frames[ 0 ].mType, FRAME_ERROR, "truncated frame type" );
    CheckEq( frames[ 0 ].mData1, 0x1D, "partial bits 11101" );
    CheckEq( frames[ 0 ].mData2, 5, "partial bit count" );
}

// CPHA=0: CS deasserts after the final sampling edge but before the trailing
// edge -- a fully captured word, not a truncation
static void GoldenCpha0FullWordCsBeforeTrailingEdge()
{
    g_context = "golden CPHA0 full word, CS before trailing edge";

    Perm p = { 1, 1, 1, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    const int command_bits[ 8 ] = { 1, 1, 1, 0, 1, 0, 1, 1 };
    for( int k = 0; k < 8; ++k )
    {
        U64 t = 110 + 20 * k;
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_HIGH );
        if( k < 7 )
            sck.Set( t + 20, BIT_LOW );
    }
    cs.Set( 265, BIT_HIGH ); // after the last sampling edge at 260, before its trailing edge
    sck.Set( 270, BIT_LOW );

    RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 1, "frame count" );
    if( frames.empty() )
        return;
    CheckEq( frames[ 0 ].mType, FRAME_COMMAND, "full command, no truncation" );
    CheckEq( frames[ 0 ].mData1, 0xEB, "command value" );
}

// CPHA=1: CS deasserts between the leading edge and the trailing (sampling) edge
static void GoldenCpha1MidCycleTruncation()
{
    g_context = "golden CPHA1 mid-cycle truncation";

    Perm p = { 1, 1, 1, 0, 0, BIT_LOW, AnalyzerEnums::TrailingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    // CPHA1: data changes on the rising (leading) edge, sampled on the falling (trailing) edge
    const int command_bits[ 2 ] = { 1, 0 };
    for( int k = 0; k < 2; ++k )
    {
        U64 t = 110 + 20 * k;
        sck.Set( t, BIT_HIGH );
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_LOW ); // sampled here
    }
    sck.Set( 150, BIT_HIGH ); // third leading edge
    cs.Set( 155, BIT_HIGH );  // deassert before the trailing edge at 160
    sck.Set( 160, BIT_LOW );

    RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 1, "frame count" );
    if( frames.empty() )
        return;
    CheckEq( frames[ 0 ].mType, FRAME_ERROR, "truncated frame type" );
    CheckEq( frames[ 0 ].mData1, 0x2, "partial bits 10" );
    CheckEq( frames[ 0 ].mData2, 2, "partial bit count" );
}

// CS pulses with no clock edges at all -> no frames, just enable/disable
static void GoldenZeroClockTransaction()
{
    g_context = "golden zero-clock transaction";

    Perm p = { 1, 1, 1, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    cs.Set( 120, BIT_HIGH );

    RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
    if( !RunWorkerThread( analyzer ) )
        return;

    CheckEq( MockSdk::GetFrames( analyzer.Results() ).size(), 0, "no frames for a clockless CS pulse" );
    const std::vector<RecordedFrameV2>& frames_v2 = MockSdk::GetFramesV2( analyzer.Results() );
    CheckEq( frames_v2.size(), 2, "only enable/disable FrameV2s" );
    if( frames_v2.size() == 2 )
        Check( frames_v2[ 0 ].mType == "enable" && frames_v2[ 1 ].mType == "disable", "enable then disable" );
}

// address phase truncated after one quad nibble (also hits the +1 fallback)
static void GoldenTruncatedAddress()
{
    g_context = "golden truncated address";

    Perm p = { 1, 4, 4, 8, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    const int command_bits[ 8 ] = { 1, 1, 1, 0, 1, 0, 1, 1 };
    for( int k = 0; k < 8; ++k )
    {
        U64 t = 110 + 20 * k;
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_HIGH );
        sck.Set( t + 20, BIT_LOW );
    }
    // one address nibble 0x5 (IO3=0 IO2=1 IO1=0 IO0=1) sampled at 300
    io0.Set( 290, BIT_HIGH );
    io1.Set( 290, BIT_LOW );
    io2.Set( 290, BIT_HIGH );
    io3.Set( 290, BIT_LOW );
    sck.Set( 300, BIT_HIGH );
    sck.Set( 310, BIT_LOW );
    cs.Set( 315, BIT_HIGH ); // deassert before the second nibble
    sck.Set( 320, BIT_HIGH );
    sck.Set( 330, BIT_LOW );

    RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 2, "frame count" );
    if( frames.size() != 2 )
        return;
    CheckEq( frames[ 0 ].mType, FRAME_COMMAND, "command decodes fully" );
    CheckEq( frames[ 1 ].mType, FRAME_ERROR, "truncated address frame" );
    Check( ( frames[ 1 ].mFlags & QSPI_TRUNCATED_FLAG ) != 0, "truncated flag set" );
    CheckEq( frames[ 1 ].mData1, 0x5, "partial address nibble" );
    CheckEq( frames[ 1 ].mData2, 4, "partial address bit count" );
    CheckEq( frames[ 1 ].mEndingSampleInclusive, 301, "single-sample frame extends end by one" );
}

// dummy phase truncations: after one edge (frame with +1 end) and before any edge (no frame)
static void GoldenTruncatedDummy()
{
    g_context = "golden dummy truncated after one edge";

    Perm p = { 1, 1, 1, 0, 4, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    {
        TestQuadSpiAnalyzer analyzer;
        ApplyPerm( analyzer.Settings(), p, true );

        Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
        cs.Set( 100, BIT_LOW );
        const int command_bits[ 8 ] = { 1, 1, 1, 0, 1, 0, 1, 1 };
        for( int k = 0; k < 8; ++k )
        {
            U64 t = 110 + 20 * k;
            io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
            sck.Set( t + 10, BIT_HIGH );
            sck.Set( t + 20, BIT_LOW );
        }
        sck.Set( 290, BIT_HIGH ); // first dummy edge
        cs.Set( 295, BIT_HIGH );  // deassert before the second dummy edge
        sck.Set( 300, BIT_LOW );

        RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
        if( RunWorkerThread( analyzer ) )
        {
            const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
            CheckEq( frames.size(), 2, "frame count" );
            if( frames.size() == 2 )
            {
                CheckEq( frames[ 1 ].mType, FRAME_ERROR, "truncated dummy frame" );
                Check( ( frames[ 1 ].mFlags & QSPI_TRUNCATED_FLAG ) != 0, "truncated flag set" );
                CheckEq( frames[ 1 ].mStartingSampleInclusive, 290, "dummy frame start" );
                CheckEq( frames[ 1 ].mEndingSampleInclusive, 291, "single-edge dummy frame extends end by one" );
            }
        }
    }

    g_context = "golden dummy truncated before any edge";
    {
        TestQuadSpiAnalyzer analyzer;
        ApplyPerm( analyzer.Settings(), p, true );

        Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
        cs.Set( 100, BIT_LOW );
        const int command_bits[ 8 ] = { 1, 1, 1, 0, 1, 0, 1, 1 };
        for( int k = 0; k < 8; ++k )
        {
            U64 t = 110 + 20 * k;
            io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
            sck.Set( t + 10, BIT_HIGH );
            sck.Set( t + 20, BIT_LOW );
        }
        cs.Set( 280, BIT_HIGH );  // deassert before any dummy edge
        sck.Set( 300, BIT_HIGH ); // a later clock edge exists but belongs to nothing
        sck.Set( 310, BIT_LOW );

        RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
        if( RunWorkerThread( analyzer ) )
        {
            const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
            CheckEq( frames.size(), 1, "only the command frame" );
            if( !frames.empty() )
                CheckEq( frames[ 0 ].mType, FRAME_COMMAND, "command decodes fully" );
        }
    }
}

// CS deasserts after the command but before any address clock -> no address frame at all
static void GoldenZeroBitAddressTruncation()
{
    g_context = "golden zero-bit address truncation";

    Perm p = { 1, 1, 1, 8, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    const int command_bits[ 8 ] = { 1, 1, 1, 0, 1, 0, 1, 1 };
    for( int k = 0; k < 8; ++k )
    {
        U64 t = 110 + 20 * k;
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_HIGH );
        sck.Set( t + 20, BIT_LOW );
    }
    cs.Set( 280, BIT_HIGH );  // deassert before any address clock
    sck.Set( 300, BIT_HIGH ); // later clock edge belongs to nothing
    sck.Set( 310, BIT_LOW );

    RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 1, "only the command frame" );
    if( !frames.empty() )
        CheckEq( frames[ 0 ].mType, FRAME_COMMAND, "command decodes fully" );
}

// data byte truncated after one quad nibble -> partial data error frame
static void GoldenPartialDataByte()
{
    g_context = "golden partial data byte";

    Perm p = { 1, 4, 4, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    const int command_bits[ 8 ] = { 1, 1, 1, 0, 1, 0, 1, 1 };
    for( int k = 0; k < 8; ++k )
    {
        U64 t = 110 + 20 * k;
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_HIGH );
        sck.Set( t + 20, BIT_LOW );
    }
    // one data nibble 0xA, then CS deasserts mid-byte
    io0.Set( 290, BIT_LOW );
    io1.Set( 290, BIT_HIGH );
    io2.Set( 290, BIT_LOW );
    io3.Set( 290, BIT_HIGH );
    sck.Set( 300, BIT_HIGH );
    sck.Set( 310, BIT_LOW );
    cs.Set( 315, BIT_HIGH );
    sck.Set( 320, BIT_HIGH );
    sck.Set( 330, BIT_LOW );

    RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 2, "frame count" );
    if( frames.size() != 2 )
        return;
    CheckEq( frames[ 1 ].mType, FRAME_ERROR, "partial data byte becomes error frame" );
    Check( ( frames[ 1 ].mFlags & QSPI_TRUNCATED_FLAG ) != 0, "truncated flag set" );
    CheckEq( frames[ 1 ].mData1, 0xA, "partial data nibble" );
    CheckEq( frames[ 1 ].mData2 >> 32, 4, "partial data bit count" );
}

// quad decode with IO2/IO3 unregistered (validation bypassed): missing lanes read as 0
static void GoldenQuadWithMissingIo23()
{
    g_context = "golden quad with missing IO2/IO3";

    Perm p = { 4, 4, 4, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, false /* IO2/IO3 undefined */ );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    // two nibbles with IO0=IO1=1: decoded as 0x3 per nibble -> byte 0x33
    for( int k = 0; k < 2; ++k )
    {
        U64 t = 110 + 20 * k;
        sck.Set( t + 10, BIT_HIGH );
        sck.Set( t + 20, BIT_LOW );
    }
    cs.Set( 170, BIT_HIGH );

    RegisterWaves( cs, sck, io0, io1, nullptr, nullptr );
    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 1, "frame count" );
    if( !frames.empty() )
        CheckEq( frames[ 0 ].mData1, 0x33, "missing IO2/IO3 lanes decode as 0 bits" );
}

// streamed capture: clock data runs out mid-command but more arrives while the
// decoder waits -- decoding must continue seamlessly across the horizon
static void GoldenStreamingHorizonMidCommand()
{
    g_context = "golden streaming horizon mid-command";

    Perm p = { 1, 1, 1, 0, 0, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    Wave cs( BIT_HIGH ), sck( BIT_LOW ), io0( BIT_HIGH ), io1( BIT_HIGH ), io2( BIT_HIGH ), io3( BIT_HIGH );
    cs.Set( 100, BIT_LOW );
    const int command_bits[ 8 ] = { 1, 1, 1, 0, 1, 0, 1, 1 };
    for( int k = 0; k < 8; ++k )
    {
        U64 t = 110 + 20 * k;
        io0.Set( t, command_bits[ k ] != 0 ? BIT_HIGH : BIT_LOW );
        sck.Set( t + 10, BIT_HIGH );
        sck.Set( t + 20, BIT_LOW );
    }
    cs.Set( 300, BIT_HIGH );

    RegisterWaves( cs, sck, io0, io1, &io2, &io3 );
    MockSdk::SetDataHorizon( 150 ); // clock edges after 150 only become visible to blocking queries

    if( !RunWorkerThread( analyzer ) )
        return;

    const std::vector<Frame>& frames = MockSdk::GetFrames( analyzer.Results() );
    CheckEq( frames.size(), 1, "frame count" );
    if( !frames.empty() )
    {
        CheckEq( frames[ 0 ].mType, FRAME_COMMAND, "command decodes across the streaming horizon" );
        CheckEq( frames[ 0 ].mData1, 0xEB, "command value" );
    }
}

// trivial API surface: names, rerun, minimum sample rate, C exports, simulation entry point
static void ApiSurface()
{
    g_context = "api surface";

    MockSdk::ResetChannelData();
    MockSdk::SetSampleRate( kSimSampleRate );

    TestQuadSpiAnalyzer analyzer;
    Check( std::string( analyzer.GetAnalyzerName() ) == "QSPI", "member analyzer name" );
    Check( std::string( GetAnalyzerName() ) == "QSPI", "exported analyzer name" );
    Check( analyzer.NeedsRerun() == false, "NeedsRerun" );
    CheckEq( analyzer.GetMinimumSampleRateHz(), 10000, "minimum sample rate" );

    Analyzer* created = CreateAnalyzer();
    Check( created != nullptr, "CreateAnalyzer" );
    DestroyAnalyzer( created );
    DestroyAnalyzer( nullptr ); // deleting null is a no-op

    // SetupResults must be callable repeatedly (Logic 2 reuses analyzer instances across runs)
    analyzer.SetupResults();
    analyzer.SetupResults();

    // GenerateSimulationData twice: first call initializes, second reuses
    Perm p = { 1, 4, 4, 24, 6, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    ApplyPerm( analyzer.Settings(), p, true );
    SimulationChannelDescriptor* descriptors = nullptr;
    CheckEq( analyzer.GenerateSimulationData( 5000, kSimSampleRate, &descriptors ), 6, "simulation channel count (init)" );
    CheckEq( analyzer.GenerateSimulationData( 6000, kSimSampleRate, &descriptors ), 6, "simulation channel count (reuse)" );
}

// bubble text, tabular text and CSV export for every frame type
static void ResultsRendering()
{
    g_context = "results rendering";

    MockSdk::ResetChannelData();
    MockSdk::SetSampleRate( kSimSampleRate );
    MockSdk::SetExportCancelAfter( 0 );

    Perm p = { 1, 4, 4, 24, 6, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    QuadSpiAnalyzerResults results( &analyzer, &analyzer.Settings() );

    Frame frame;
    frame.mType = FRAME_COMMAND;
    frame.mData1 = 0xEB;
    frame.mData2 = 8;
    frame.mStartingSampleInclusive = 100;
    frame.mEndingSampleInclusive = 200;
    results.AddFrame( frame );

    frame.mType = FRAME_ADDRESS;
    frame.mData1 = 0x1000;
    frame.mData2 = 24;
    results.AddFrame( frame );

    frame.mType = FRAME_DUMMY;
    frame.mData1 = 6;
    frame.mData2 = 0;
    results.AddFrame( frame );

    frame.mType = FRAME_DATA;
    frame.mData1 = 0xA5;
    frame.mData2 = ( 8ull << 32 );
    results.AddFrame( frame );

    frame.mType = FRAME_ERROR;
    frame.mFlags = QSPI_TRUNCATED_FLAG | DISPLAY_AS_ERROR_FLAG;
    results.AddFrame( frame );

    frame.mFlags = DISPLAY_AS_ERROR_FLAG; // clock-not-idle error
    results.AddFrame( frame );

    Channel bubble_channel = Ch( 2 );
    char const** strings = nullptr;
    U32 string_count = 0;

    for( U64 i = 0; i < 6; ++i )
    {
        results.GenerateBubbleText( i, bubble_channel, Hexadecimal );
        results.GetResultStrings( &strings, &string_count );
        Check( string_count > 0, "bubble strings produced for frame " + std::to_string( i ) );
        results.GenerateFrameTabularText( i, Hexadecimal );
        Check( !MockSdk::GetTabularText( &results ).empty(), "tabular text produced for frame " + std::to_string( i ) );
    }
    results.GenerateBubbleText( 0, bubble_channel, Decimal );
    results.GeneratePacketTabularText( 0, Hexadecimal );
    results.GenerateTransactionTabularText( 0, Hexadecimal );

    results.GenerateBubbleText( 0, bubble_channel, Hexadecimal );
    results.GetResultStrings( &strings, &string_count );
    Check( string_count == 3 && std::string( strings[ 2 ] ) == "CMD 0xEB", "command bubble long form" );

    const char* export_path = "qspi_export_test.csv";
    results.GenerateExportFile( export_path, Hexadecimal, 0 );
    {
        std::ifstream file( export_path );
        std::string contents( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
        Check( contents.find( "Time [s],Phase,Value" ) == 0, "export header" );
        Check( contents.find( "Command,0xEB" ) != std::string::npos, "export command row" );
        Check( contents.find( "Address,0x001000" ) != std::string::npos, "export address row" );
        Check( contents.find( "Dummy,6" ) != std::string::npos, "export dummy row" );
        Check( contents.find( "Data,0xA5" ) != std::string::npos, "export data row" );
        Check( contents.find( "Error" ) != std::string::npos, "export error row" );
    }

    // export cancel path
    MockSdk::SetExportCancelAfter( 2 );
    results.GenerateExportFile( export_path, Hexadecimal, 0 );
    MockSdk::SetExportCancelAfter( 0 );
    std::remove( export_path );

    // single-IO data rendering shows both lanes
    g_context = "results rendering (single IO)";
    Perm p1 = { 1, 1, 1, 24, 6, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer1;
    ApplyPerm( analyzer1.Settings(), p1, true );
    QuadSpiAnalyzerResults results1( &analyzer1, &analyzer1.Settings() );

    frame = Frame();
    frame.mType = FRAME_DATA;
    frame.mData1 = 0xFF;
    frame.mData2 = ( 8ull << 32 ) | 0xDE;
    frame.mStartingSampleInclusive = 100;
    frame.mEndingSampleInclusive = 200;
    results1.AddFrame( frame );

    results1.GenerateBubbleText( 0, bubble_channel, Hexadecimal );
    results1.GetResultStrings( &strings, &string_count );
    Check( string_count == 2 && std::string( strings[ 1 ] ) == "IO0: 0xFF; IO1: 0xDE", "single-IO data bubble shows both lanes" );

    const char* export_path1 = "qspi_export_test_sio.csv";
    results1.GenerateExportFile( export_path1, Hexadecimal, 0 );
    {
        std::ifstream file( export_path1 );
        std::string contents( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
        Check( contents.find( "Data,0xFF,0xDE" ) != std::string::npos, "single-IO export shows IO1 column" );
    }
    std::remove( export_path1 );
}

// simulation half-period clamp for very low sample rates
static void SimulationLowSampleRateClamp()
{
    g_context = "simulation low sample rate clamp";

    Perm p = { 1, 1, 1, 8, 1, BIT_LOW, AnalyzerEnums::LeadingEdge, BIT_LOW };
    TestQuadSpiAnalyzer analyzer;
    ApplyPerm( analyzer.Settings(), p, true );

    QuadSpiSimulationDataGenerator generator;
    generator.Initialize( 20, &analyzer.Settings() ); // 20/10/2 = 1, clamps to 2
    SimulationChannelDescriptor* descriptors = nullptr;
    U32 descriptor_count = generator.GenerateSimulationData( 2000, 20, &descriptors );
    CheckEq( descriptor_count, 6, "channels simulated at clamped rate" );
    Check( !MockSdk::GetSimTransitions( &descriptors[ 1 ] ).empty(), "clock toggles at clamped rate" );
}

// ---------------------------------------------------------------------------

int main()
{
    GoldenBasicTransaction();
    GoldenTruncatedCommand();
    GoldenClockNotIdle();
    GoldenCsActiveAtStart();
    GoldenTruncatedSingleChunkCommand();
    GoldenCpha0MidCycleTruncation();
    GoldenCpha0FullWordCsBeforeTrailingEdge();
    GoldenCpha1MidCycleTruncation();
    GoldenZeroClockTransaction();
    GoldenTruncatedAddress();
    GoldenZeroBitAddressTruncation();
    GoldenTruncatedDummy();
    GoldenPartialDataByte();
    GoldenQuadWithMissingIo23();
    GoldenStreamingHorizonMidCommand();
    ApiSurface();
    ResultsRendering();
    SimulationLowSampleRateClamp();
    SettingsRoundTrip();
    SettingsValidation();
    RunOptionalChannelCases();
    RunSweep();

    std::printf( "%d checks, %d failures\n", g_checks, g_failures );
    return g_failures == 0 ? 0 : 1;
}
