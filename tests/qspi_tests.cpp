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
}

// ---------------------------------------------------------------------------

int main()
{
    GoldenBasicTransaction();
    GoldenTruncatedCommand();
    GoldenClockNotIdle();
    SettingsRoundTrip();
    SettingsValidation();
    RunOptionalChannelCases();
    RunSweep();

    std::printf( "%d checks, %d failures\n", g_checks, g_failures );
    return g_failures == 0 ? 0 : 1;
}
