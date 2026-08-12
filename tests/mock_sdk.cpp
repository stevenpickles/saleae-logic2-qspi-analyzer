// In-process implementations of the Saleae Analyzer SDK classes used by the
// QSPI analyzer, sufficient to run the simulation generator and decoder inside
// a test executable. See mock_sdk.h for the test-facing accessors.

#include "mock_sdk.h"

#include <Analyzer.h>
#include <AnalyzerChannelData.h>
#include <AnalyzerHelpers.h>
#include <AnalyzerResults.h>
#include <AnalyzerSettingInterface.h>
#include <AnalyzerSettings.h>
#include <LogicPublicTypes.h>
#include <SimulationChannelDescriptor.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <sstream>

// ---------------------------------------------------------------------------
// global test state
// ---------------------------------------------------------------------------

namespace
{
    struct ChannelKey
    {
        U64 mDeviceId;
        U32 mChannelIndex;
        int mDataType;

        ChannelKey( const Channel& c ) : mDeviceId( c.mDeviceId ), mChannelIndex( c.mChannelIndex ), mDataType( c.mDataType )
        {
        }

        bool operator<( const ChannelKey& other ) const
        {
            if( mDeviceId != other.mDeviceId )
                return mDeviceId < other.mDeviceId;
            if( mChannelIndex != other.mChannelIndex )
                return mChannelIndex < other.mChannelIndex;
            return mDataType < other.mDataType;
        }
    };

    U32 g_sample_rate = 1000000;
    U64 g_op_count = 0;
    U64 g_op_limit = 100000000ull;
    U64 g_data_horizon = 0; // 0 = no horizon

    void CountOp()
    {
        if( ++g_op_count > g_op_limit )
            throw MockOpLimitExceeded();
    }
}

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------

Channel::Channel() : mDeviceId( 0 ), mChannelIndex( 0 ), mDataType( DIGITAL_CHANNEL )
{
}

Channel::Channel( const Channel& channel ) = default;

Channel::Channel( U64 device_id, U32 channel_index, ChannelDataType data_type )
    : mDeviceId( device_id ), mChannelIndex( channel_index ), mDataType( data_type )
{
}

Channel::~Channel() = default;

Channel& Channel::operator=( const Channel& channel ) = default;

bool Channel::operator==( const Channel& channel ) const
{
    return mDeviceId == channel.mDeviceId && mChannelIndex == channel.mChannelIndex && mDataType == channel.mDataType;
}

bool Channel::operator!=( const Channel& channel ) const
{
    return !( *this == channel );
}

bool Channel::operator>( const Channel& channel ) const
{
    return channel < *this;
}

bool Channel::operator<( const Channel& channel ) const
{
    if( mDeviceId != channel.mDeviceId )
        return mDeviceId < channel.mDeviceId;
    if( mChannelIndex != channel.mChannelIndex )
        return mChannelIndex < channel.mChannelIndex;
    return mDataType < channel.mDataType;
}

// ---------------------------------------------------------------------------
// AnalyzerChannelData -- transition-list-backed channel
// ---------------------------------------------------------------------------

class ChannelData
{
  public:
    BitState mInitialState;
    std::vector<U64> mTransitions; // sorted; sample where the new state takes effect
};

struct AnalyzerChannelDataData
{
    ChannelData* mChannelData;
    U64 mPosition;
    size_t mTransitionsApplied; // count of transitions with sample <= mPosition
};

namespace
{
    std::map<ChannelKey, AnalyzerChannelData*> g_channel_registry;
    std::vector<AnalyzerChannelData*> g_owned_channel_data;
    std::vector<ChannelData*> g_owned_backing_data;
}

AnalyzerChannelData::AnalyzerChannelData( ChannelData* channel_data )
{
    mData = new AnalyzerChannelDataData();
    mData->mChannelData = channel_data;
    mData->mPosition = 0;
    mData->mTransitionsApplied = 0;
    // apply any transitions at sample 0
    while( mData->mTransitionsApplied < channel_data->mTransitions.size() &&
           channel_data->mTransitions[ mData->mTransitionsApplied ] == 0 )
        ++mData->mTransitionsApplied;
}

AnalyzerChannelData::~AnalyzerChannelData()
{
    delete mData;
}

U64 AnalyzerChannelData::GetSampleNumber()
{
    return mData->mPosition;
}

BitState AnalyzerChannelData::GetBitState()
{
    bool odd = ( mData->mTransitionsApplied % 2 ) != 0;
    BitState initial = mData->mChannelData->mInitialState;
    return odd ? Toggle( initial ) : initial;
}

U32 AnalyzerChannelData::AdvanceToAbsPosition( U64 sample_number )
{
    CountOp();
    const std::vector<U64>& t = mData->mChannelData->mTransitions;
    U32 toggles = 0;
    while( mData->mTransitionsApplied < t.size() && t[ mData->mTransitionsApplied ] <= sample_number )
    {
        ++mData->mTransitionsApplied;
        ++toggles;
    }
    mData->mPosition = sample_number;
    return toggles;
}

U32 AnalyzerChannelData::Advance( U32 num_samples )
{
    return AdvanceToAbsPosition( mData->mPosition + num_samples );
}

void AnalyzerChannelData::AdvanceToNextEdge()
{
    CountOp();
    const std::vector<U64>& t = mData->mChannelData->mTransitions;
    if( mData->mTransitionsApplied >= t.size() )
        throw MockEndOfData();
    mData->mPosition = t[ mData->mTransitionsApplied ];
    ++mData->mTransitionsApplied;
}

U64 AnalyzerChannelData::GetSampleOfNextEdge()
{
    CountOp();
    const std::vector<U64>& t = mData->mChannelData->mTransitions;
    if( mData->mTransitionsApplied >= t.size() )
        throw MockEndOfData();
    return t[ mData->mTransitionsApplied ];
}

bool AnalyzerChannelData::WouldAdvancingCauseTransition( U32 num_samples )
{
    return WouldAdvancingToAbsPositionCauseTransition( mData->mPosition + num_samples );
}

bool AnalyzerChannelData::WouldAdvancingToAbsPositionCauseTransition( U64 sample_number )
{
    CountOp();
    const std::vector<U64>& t = mData->mChannelData->mTransitions;
    return mData->mTransitionsApplied < t.size() && t[ mData->mTransitionsApplied ] <= sample_number;
}

void AnalyzerChannelData::TrackMinimumPulseWidth()
{
}

U64 AnalyzerChannelData::GetMinimumPulseWidthSoFar()
{
    return 0;
}

bool AnalyzerChannelData::DoMoreTransitionsExistInCurrentData()
{
    CountOp();
    const std::vector<U64>& t = mData->mChannelData->mTransitions;
    if( mData->mTransitionsApplied >= t.size() )
        return false;
    return g_data_horizon == 0 || t[ mData->mTransitionsApplied ] <= g_data_horizon;
}

// ---------------------------------------------------------------------------
// Frame / FrameV2
// ---------------------------------------------------------------------------

Frame::Frame() : mStartingSampleInclusive( 0 ), mEndingSampleInclusive( 0 ), mData1( 0 ), mData2( 0 ), mType( 0 ), mFlags( 0 )
{
}

Frame::Frame( const Frame& frame ) = default;

Frame::~Frame() = default;

bool Frame::HasFlag( U8 flag )
{
    return ( mFlags & flag ) != 0;
}

class FrameV2Data
{
  public:
    std::map<std::string, std::string> mFields;
};

FrameV2::FrameV2()
{
    mInternals = new FrameV2Data();
}

FrameV2::~FrameV2()
{
    delete mInternals;
}

void FrameV2::AddString( const char* key, const char* value )
{
    mInternals->mFields[ key ] = value;
}

void FrameV2::AddDouble( const char* key, double value )
{
    std::ostringstream stream;
    stream << value;
    mInternals->mFields[ key ] = stream.str();
}

void FrameV2::AddInteger( const char* key, S64 value )
{
    std::ostringstream stream;
    stream << value;
    mInternals->mFields[ key ] = stream.str();
}

void FrameV2::AddBoolean( const char* key, bool value )
{
    mInternals->mFields[ key ] = value ? "true" : "false";
}

void FrameV2::AddByte( const char* key, U8 value )
{
    std::ostringstream stream;
    stream << static_cast<unsigned>( value );
    mInternals->mFields[ key ] = stream.str();
}

void FrameV2::AddByteArray( const char* key, const U8* data, U64 length )
{
    std::ostringstream stream;
    for( U64 i = 0; i < length; ++i )
        stream << static_cast<unsigned>( data[ i ] ) << ( i + 1 < length ? "," : "" );
    mInternals->mFields[ key ] = stream.str();
}

// ---------------------------------------------------------------------------
// AnalyzerResults -- records frames, FrameV2s and markers
// ---------------------------------------------------------------------------

struct AnalyzerResultsData
{
    std::vector<Frame> mFrames;
    std::vector<RecordedFrameV2> mFramesV2;
    std::vector<RecordedMarker> mMarkers;
    std::vector<std::string> mResultStrings;
    std::vector<std::string> mTabularText;
    std::vector<const char*> mResultStringPointers;
};

AnalyzerResults::AnalyzerResults()
{
    mData = new AnalyzerResultsData();
}

AnalyzerResults::~AnalyzerResults()
{
    delete mData;
}

void AnalyzerResults::AddFrameV2( const FrameV2& frame, const char* type, U64 starting_sample, U64 ending_sample )
{
    RecordedFrameV2 recorded;
    recorded.mType = type;
    recorded.mStartingSample = starting_sample;
    recorded.mEndingSample = ending_sample;
    recorded.mFields = frame.mInternals->mFields;
    mData->mFramesV2.push_back( recorded );
}

void AnalyzerResults::AddMarker( U64 sample_number, MarkerType marker_type, Channel& channel )
{
    RecordedMarker marker = { sample_number, marker_type, channel };
    mData->mMarkers.push_back( marker );
}

U64 AnalyzerResults::AddFrame( const Frame& frame )
{
    mData->mFrames.push_back( frame );
    return mData->mFrames.size() - 1;
}

U64 AnalyzerResults::CommitPacketAndStartNewPacket()
{
    return 0;
}

void AnalyzerResults::CancelPacketAndStartNewPacket()
{
}

void AnalyzerResults::AddPacketToTransaction( U64, U64 )
{
}

void AnalyzerResults::AddChannelBubblesWillAppearOn( const Channel& )
{
}

void AnalyzerResults::CommitResults()
{
}

U64 AnalyzerResults::GetNumFrames()
{
    return mData->mFrames.size();
}

U64 AnalyzerResults::GetNumPackets()
{
    return 0;
}

Frame AnalyzerResults::GetFrame( U64 frame_id )
{
    return mData->mFrames.at( static_cast<size_t>( frame_id ) );
}

void AnalyzerResults::ClearResultStrings()
{
    mData->mResultStrings.clear();
}

void AnalyzerResults::AddResultString( const char* str1, const char* str2, const char* str3, const char* str4, const char* str5,
                                       const char* str6 )
{
    std::string combined;
    const char* parts[] = { str1, str2, str3, str4, str5, str6 };
    for( const char* part : parts )
        if( part != NULL )
            combined += part;
    mData->mResultStrings.push_back( combined );
}

void AnalyzerResults::GetResultStrings( char const*** result_string_array, U32* num_strings )
{
    mData->mResultStringPointers.clear();
    for( const std::string& s : mData->mResultStrings )
        mData->mResultStringPointers.push_back( s.c_str() );
    *result_string_array = mData->mResultStringPointers.data();
    *num_strings = static_cast<U32>( mData->mResultStringPointers.size() );
}

namespace
{
    U64 g_export_cancel_after = 0; // 0 = never cancel
    U64 g_export_update_calls = 0;
}

bool AnalyzerResults::UpdateExportProgressAndCheckForCancel( U64, U64 )
{
    ++g_export_update_calls;
    return g_export_cancel_after != 0 && g_export_update_calls >= g_export_cancel_after;
}

void AnalyzerResults::ClearTabularText()
{
    mData->mTabularText.clear();
}

void AnalyzerResults::AddTabularText( const char* str1, const char* str2, const char* str3, const char* str4, const char* str5,
                                      const char* str6 )
{
    std::string combined;
    const char* parts[] = { str1, str2, str3, str4, str5, str6 };
    for( const char* part : parts )
        if( part != NULL )
            combined += part;
    mData->mTabularText.push_back( combined );
}

// ---------------------------------------------------------------------------
// Analyzer / Analyzer2
// ---------------------------------------------------------------------------

struct AnalyzerData
{
    AnalyzerSettings* mSettings = nullptr;
    AnalyzerResults* mResults = nullptr;
};

Analyzer::Analyzer()
{
    mData = new AnalyzerData();
}

Analyzer::~Analyzer()
{
    delete mData;
}

void Analyzer::SetAnalyzerSettings( AnalyzerSettings* settings )
{
    mData->mSettings = settings;
}

void Analyzer::KillThread()
{
}

AnalyzerChannelData* Analyzer::GetAnalyzerChannelData( Channel& channel )
{
    std::map<ChannelKey, AnalyzerChannelData*>::iterator it = g_channel_registry.find( ChannelKey( channel ) );
    if( it == g_channel_registry.end() )
    {
        std::fprintf( stderr, "mock_sdk: no channel data registered for device %llu channel %u\n",
                      static_cast<unsigned long long>( channel.mDeviceId ), channel.mChannelIndex );
        std::abort();
    }
    return it->second;
}

void Analyzer::ReportProgress( U64 )
{
}

void Analyzer::SetAnalyzerResults( AnalyzerResults* results )
{
    mData->mResults = results;
}

U32 Analyzer::GetSimulationSampleRate()
{
    return g_sample_rate;
}

U32 Analyzer::GetSampleRate()
{
    return g_sample_rate;
}

U64 Analyzer::GetTriggerSample()
{
    return 0;
}

void Analyzer::UseFrameV2()
{
}

void Analyzer::CheckIfThreadShouldExit()
{
}

Analyzer2::Analyzer2() : Analyzer()
{
}

void Analyzer2::SetupResults()
{
}

// ---------------------------------------------------------------------------
// AnalyzerSettings and setting interfaces
// ---------------------------------------------------------------------------

struct AnalyzerSettingsData
{
    std::string mErrorText;
    std::string mReturnString;
    std::vector<AnalyzerSettingInterface*> mInterfaces;
};

AnalyzerSettings::AnalyzerSettings()
{
    mData = new AnalyzerSettingsData();
}

AnalyzerSettings::~AnalyzerSettings()
{
    delete mData;
}

void AnalyzerSettings::ClearChannels()
{
}

void AnalyzerSettings::AddChannel( Channel&, const char*, bool )
{
}

void AnalyzerSettings::SetErrorText( const char* error_text )
{
    mData->mErrorText = error_text;
}

void AnalyzerSettings::AddInterface( AnalyzerSettingInterface* analyzer_setting_interface )
{
    mData->mInterfaces.push_back( analyzer_setting_interface );
}

void AnalyzerSettings::AddExportOption( U32, const char* )
{
}

void AnalyzerSettings::AddExportExtension( U32, const char*, const char* )
{
}

const char* AnalyzerSettings::SetReturnString( const char* str )
{
    mData->mReturnString = str;
    return mData->mReturnString.c_str();
}

struct AnalyzerSettingInterfaceData
{
    std::string mTitle;
    std::string mTooltip;
};

AnalyzerSettingInterface::AnalyzerSettingInterface()
{
    mData = new AnalyzerSettingInterfaceData();
}

AnalyzerSettingInterface::~AnalyzerSettingInterface()
{
    delete mData;
}

void AnalyzerSettingInterface::operator delete( void* p )
{
    std::free( p );
}

void* AnalyzerSettingInterface::operator new( size_t size )
{
    return std::malloc( size );
}

AnalyzerInterfaceTypeId AnalyzerSettingInterface::GetType()
{
    return INTERFACE_BASE;
}

void AnalyzerSettingInterface::SetTitleAndTooltip( const char* title, const char* tooltip )
{
    mData->mTitle = title;
    mData->mTooltip = tooltip;
}

const char* AnalyzerSettingInterface::GetToolTip()
{
    return mData->mTooltip.c_str();
}

const char* AnalyzerSettingInterface::GetTitle()
{
    return mData->mTitle.c_str();
}

bool AnalyzerSettingInterface::IsDisabled()
{
    return false;
}

struct AnalyzerSettingInterfaceChannelData
{
    Channel mChannel;
    bool mNoneAllowed = false;
};

AnalyzerSettingInterfaceChannel::AnalyzerSettingInterfaceChannel()
{
    mChannelData = new AnalyzerSettingInterfaceChannelData();
}

AnalyzerSettingInterfaceChannel::~AnalyzerSettingInterfaceChannel()
{
    delete mChannelData;
}

AnalyzerInterfaceTypeId AnalyzerSettingInterfaceChannel::GetType()
{
    return INTERFACE_CHANNEL;
}

Channel AnalyzerSettingInterfaceChannel::GetChannel()
{
    return mChannelData->mChannel;
}

void AnalyzerSettingInterfaceChannel::SetChannel( const Channel& channel )
{
    mChannelData->mChannel = channel;
}

bool AnalyzerSettingInterfaceChannel::GetSelectionOfNoneIsAllowed()
{
    return mChannelData->mNoneAllowed;
}

void AnalyzerSettingInterfaceChannel::SetSelectionOfNoneIsAllowed( bool is_allowed )
{
    mChannelData->mNoneAllowed = is_allowed;
}

struct AnalyzerSettingInterfaceNumberListData
{
    double mValue = 0;
    std::vector<double> mNumbers;
    std::vector<std::string> mStrings;
    std::vector<std::string> mTooltips;
};

AnalyzerSettingInterfaceNumberList::AnalyzerSettingInterfaceNumberList()
{
    mNumberListData = new AnalyzerSettingInterfaceNumberListData();
}

AnalyzerSettingInterfaceNumberList::~AnalyzerSettingInterfaceNumberList()
{
    delete mNumberListData;
}

AnalyzerInterfaceTypeId AnalyzerSettingInterfaceNumberList::GetType()
{
    return INTERFACE_NUMBER_LIST;
}

double AnalyzerSettingInterfaceNumberList::GetNumber()
{
    return mNumberListData->mValue;
}

void AnalyzerSettingInterfaceNumberList::SetNumber( double number )
{
    mNumberListData->mValue = number;
}

U32 AnalyzerSettingInterfaceNumberList::GetListboxNumbersCount()
{
    return static_cast<U32>( mNumberListData->mNumbers.size() );
}

double AnalyzerSettingInterfaceNumberList::GetListboxNumber( U32 index )
{
    return mNumberListData->mNumbers.at( index );
}

U32 AnalyzerSettingInterfaceNumberList::GetListboxStringsCount()
{
    return static_cast<U32>( mNumberListData->mStrings.size() );
}

const char* AnalyzerSettingInterfaceNumberList::GetListboxString( U32 index )
{
    return mNumberListData->mStrings.at( index ).c_str();
}

U32 AnalyzerSettingInterfaceNumberList::GetListboxTooltipsCount()
{
    return static_cast<U32>( mNumberListData->mTooltips.size() );
}

const char* AnalyzerSettingInterfaceNumberList::GetListboxTooltip( U32 index )
{
    return mNumberListData->mTooltips.at( index ).c_str();
}

void AnalyzerSettingInterfaceNumberList::AddNumber( double number, const char* str, const char* tooltip )
{
    mNumberListData->mNumbers.push_back( number );
    mNumberListData->mStrings.push_back( str );
    mNumberListData->mTooltips.push_back( tooltip );
}

void AnalyzerSettingInterfaceNumberList::ClearNumbers()
{
    mNumberListData->mNumbers.clear();
    mNumberListData->mStrings.clear();
    mNumberListData->mTooltips.clear();
}

struct AnalyzerSettingInterfaceIntegerData
{
    int mValue = 0;
    int mMin = 0;
    int mMax = 0;
};

AnalyzerSettingInterfaceInteger::AnalyzerSettingInterfaceInteger()
{
    mIntegerData = new AnalyzerSettingInterfaceIntegerData();
}

AnalyzerSettingInterfaceInteger::~AnalyzerSettingInterfaceInteger()
{
    delete mIntegerData;
}

AnalyzerInterfaceTypeId AnalyzerSettingInterfaceInteger::GetType()
{
    return INTERFACE_INTEGER;
}

int AnalyzerSettingInterfaceInteger::GetInteger()
{
    return mIntegerData->mValue;
}

void AnalyzerSettingInterfaceInteger::SetInteger( int integer )
{
    mIntegerData->mValue = integer;
}

int AnalyzerSettingInterfaceInteger::GetMax()
{
    return mIntegerData->mMax;
}

int AnalyzerSettingInterfaceInteger::GetMin()
{
    return mIntegerData->mMin;
}

void AnalyzerSettingInterfaceInteger::SetMax( int max )
{
    mIntegerData->mMax = max;
}

void AnalyzerSettingInterfaceInteger::SetMin( int min )
{
    mIntegerData->mMin = min;
}

// Text and Bool interfaces are unused by the analyzer, but their vtables are
// emitted in every TU because the classes are declared dllexport -- provide
// just enough to satisfy the linker.

AnalyzerSettingInterfaceText::~AnalyzerSettingInterfaceText()
{
}

AnalyzerInterfaceTypeId AnalyzerSettingInterfaceText::GetType()
{
    return INTERFACE_TEXT;
}

AnalyzerSettingInterfaceBool::~AnalyzerSettingInterfaceBool()
{
}

AnalyzerInterfaceTypeId AnalyzerSettingInterfaceBool::GetType()
{
    return INTERFACE_BOOL;
}

// ---------------------------------------------------------------------------
// SimpleArchive -- whitespace-separated token stream
// ---------------------------------------------------------------------------

struct SimpleArchiveData
{
    std::string mOutput;
    std::vector<std::string> mTokens;
    size_t mReadIndex = 0;

    void Append( const std::string& token )
    {
        if( !mOutput.empty() )
            mOutput += ' ';
        mOutput += token;
    }

    bool Next( std::string& token )
    {
        if( mReadIndex >= mTokens.size() )
            return false;
        token = mTokens[ mReadIndex++ ];
        return true;
    }
};

SimpleArchive::SimpleArchive()
{
    mData = new SimpleArchiveData();
}

SimpleArchive::~SimpleArchive()
{
    delete mData;
}

void SimpleArchive::SetString( const char* archive_string )
{
    mData->mTokens.clear();
    mData->mReadIndex = 0;
    std::istringstream stream( archive_string );
    std::string token;
    while( stream >> token )
        mData->mTokens.push_back( token );
}

const char* SimpleArchive::GetString()
{
    return mData->mOutput.c_str();
}

namespace
{
    template <typename T> bool ArchiveWrite( SimpleArchiveData* data, T value )
    {
        std::ostringstream stream;
        stream << value;
        data->Append( stream.str() );
        return true;
    }

    template <typename T> bool ArchiveRead( SimpleArchiveData* data, T& value )
    {
        std::string token;
        if( !data->Next( token ) )
            return false;
        std::istringstream stream( token );
        stream >> value;
        return !stream.fail();
    }
}

bool SimpleArchive::operator<<( U64 data )
{
    return ArchiveWrite( mData, data );
}
bool SimpleArchive::operator<<( U32 data )
{
    return ArchiveWrite( mData, data );
}
bool SimpleArchive::operator<<( S64 data )
{
    return ArchiveWrite( mData, data );
}
bool SimpleArchive::operator<<( S32 data )
{
    return ArchiveWrite( mData, data );
}
bool SimpleArchive::operator<<( double data )
{
    return ArchiveWrite( mData, data );
}
bool SimpleArchive::operator<<( bool data )
{
    return ArchiveWrite( mData, static_cast<int>( data ) );
}
bool SimpleArchive::operator<<( const char* data )
{
    mData->Append( data );
    return true;
}
bool SimpleArchive::operator<<( Channel& data )
{
    return ArchiveWrite( mData, data.mDeviceId ) && ArchiveWrite( mData, data.mChannelIndex ) &&
           ArchiveWrite( mData, static_cast<int>( data.mDataType ) );
}

bool SimpleArchive::operator>>( U64& data )
{
    return ArchiveRead( mData, data );
}
bool SimpleArchive::operator>>( U32& data )
{
    return ArchiveRead( mData, data );
}
bool SimpleArchive::operator>>( S64& data )
{
    return ArchiveRead( mData, data );
}
bool SimpleArchive::operator>>( S32& data )
{
    return ArchiveRead( mData, data );
}
bool SimpleArchive::operator>>( double& data )
{
    return ArchiveRead( mData, data );
}
bool SimpleArchive::operator>>( bool& data )
{
    int value = 0;
    if( !ArchiveRead( mData, value ) )
        return false;
    data = value != 0;
    return true;
}
bool SimpleArchive::operator>>( char const** data )
{
    static std::string s_last;
    if( !mData->Next( s_last ) )
        return false;
    *data = s_last.c_str();
    return true;
}
bool SimpleArchive::operator>>( Channel& data )
{
    int type = 0;
    if( !ArchiveRead( mData, data.mDeviceId ) || !ArchiveRead( mData, data.mChannelIndex ) || !ArchiveRead( mData, type ) )
        return false;
    data.mDataType = static_cast<ChannelDataType>( type );
    return true;
}

// ---------------------------------------------------------------------------
// AnalyzerHelpers (subset used by the analyzer)
// ---------------------------------------------------------------------------

void AnalyzerHelpers::GetNumberString( U64 number, DisplayBase display_base, U32 num_data_bits, char* result_string,
                                       U32 result_string_max_length )
{
    if( num_data_bits == 0 )
        num_data_bits = 8;

    std::string result;
    switch( display_base )
    {
    case Decimal:
    {
        std::ostringstream stream;
        stream << number;
        result = stream.str();
        break;
    }
    case Binary:
    {
        for( U32 bit = num_data_bits; bit-- > 0; )
            result += ( ( number >> bit ) & 1 ) != 0 ? '1' : '0';
        break;
    }
    default: // Hexadecimal and everything else
    {
        U32 nibbles = ( num_data_bits + 3 ) / 4;
        char buffer[ 32 ];
        std::snprintf( buffer, sizeof( buffer ), "0x%0*llX", static_cast<int>( nibbles ), static_cast<unsigned long long>( number ) );
        result = buffer;
        break;
    }
    }

    std::strncpy( result_string, result.c_str(), result_string_max_length - 1 );
    result_string[ result_string_max_length - 1 ] = '\0';
}

void AnalyzerHelpers::GetTimeString( U64 sample, U64 trigger_sample, U32 sample_rate_hz, char* result_string, U32 result_string_max_length )
{
    double seconds = static_cast<double>( static_cast<S64>( sample - trigger_sample ) ) / static_cast<double>( sample_rate_hz );
    std::snprintf( result_string, result_string_max_length, "%.9f", seconds );
}

bool AnalyzerHelpers::DoChannelsOverlap( const Channel* channel_array, U32 num_channels )
{
    for( U32 i = 0; i < num_channels; ++i )
        for( U32 j = i + 1; j < num_channels; ++j )
            if( channel_array[ i ] == channel_array[ j ] && channel_array[ i ] != UNDEFINED_CHANNEL )
                return true;
    return false;
}

U64 AnalyzerHelpers::AdjustSimulationTargetSample( U64 target_sample, U32 sample_rate, U32 simulation_sample_rate )
{
    if( sample_rate == simulation_sample_rate )
        return target_sample;
    return static_cast<U64>( static_cast<double>( target_sample ) * static_cast<double>( simulation_sample_rate ) /
                             static_cast<double>( sample_rate ) );
}

// ---------------------------------------------------------------------------
// SimulationChannelDescriptor -- records the waveform the generator produces
// ---------------------------------------------------------------------------

struct SimulationChannelDescriptorData
{
    Channel mChannel;
    U32 mSampleRate = 0;
    BitState mInitialState = BIT_LOW;
    BitState mCurrentState = BIT_LOW;
    U64 mCurrentSample = 0;
    std::vector<U64> mTransitions;
};

// descriptor copies share the same recorded data; never freed (test process only)
SimulationChannelDescriptor::SimulationChannelDescriptor()
{
    mData = new SimulationChannelDescriptorData();
}

SimulationChannelDescriptor::SimulationChannelDescriptor( const SimulationChannelDescriptor& other )
{
    mData = other.mData;
}

SimulationChannelDescriptor::~SimulationChannelDescriptor()
{
}

SimulationChannelDescriptor& SimulationChannelDescriptor::operator=( const SimulationChannelDescriptor& other )
{
    mData = other.mData;
    return *this;
}

void SimulationChannelDescriptor::Transition()
{
    mData->mTransitions.push_back( mData->mCurrentSample );
    mData->mCurrentState = Toggle( mData->mCurrentState );
}

void SimulationChannelDescriptor::TransitionIfNeeded( BitState bit_state )
{
    if( mData->mCurrentState != bit_state )
        Transition();
}

void SimulationChannelDescriptor::Advance( U32 num_samples_to_advance )
{
    mData->mCurrentSample += num_samples_to_advance;
}

BitState SimulationChannelDescriptor::GetCurrentBitState()
{
    return mData->mCurrentState;
}

U64 SimulationChannelDescriptor::GetCurrentSampleNumber()
{
    return mData->mCurrentSample;
}

void SimulationChannelDescriptor::SetChannel( Channel& channel )
{
    mData->mChannel = channel;
}

void SimulationChannelDescriptor::SetSampleRate( U32 sample_rate_hz )
{
    mData->mSampleRate = sample_rate_hz;
}

void SimulationChannelDescriptor::SetInitialBitState( BitState initial_bit_state )
{
    mData->mInitialState = initial_bit_state;
    mData->mCurrentState = initial_bit_state;
}

Channel SimulationChannelDescriptor::GetChannel()
{
    return mData->mChannel;
}

U32 SimulationChannelDescriptor::GetSampleRate()
{
    return mData->mSampleRate;
}

BitState SimulationChannelDescriptor::GetInitialBitState()
{
    return mData->mInitialState;
}

void* SimulationChannelDescriptor::GetData()
{
    return mData;
}

struct SimulationChannelDescriptorGroupData
{
    std::deque<SimulationChannelDescriptor> mDescriptors;
    std::vector<SimulationChannelDescriptor> mContiguous;
};

SimulationChannelDescriptorGroup::SimulationChannelDescriptorGroup()
{
    mData = new SimulationChannelDescriptorGroupData();
}

SimulationChannelDescriptorGroup::~SimulationChannelDescriptorGroup()
{
    delete mData;
}

SimulationChannelDescriptor* SimulationChannelDescriptorGroup::Add( Channel& channel, U32 sample_rate, BitState intial_bit_state )
{
    mData->mDescriptors.push_back( SimulationChannelDescriptor() );
    SimulationChannelDescriptor* descriptor = &mData->mDescriptors.back();
    descriptor->SetChannel( channel );
    descriptor->SetSampleRate( sample_rate );
    descriptor->SetInitialBitState( intial_bit_state );
    return descriptor;
}

void SimulationChannelDescriptorGroup::AdvanceAll( U32 num_samples_to_advance )
{
    for( SimulationChannelDescriptor& descriptor : mData->mDescriptors )
        descriptor.Advance( num_samples_to_advance );
}

SimulationChannelDescriptor* SimulationChannelDescriptorGroup::GetArray()
{
    mData->mContiguous.assign( mData->mDescriptors.begin(), mData->mDescriptors.end() );
    return mData->mContiguous.data();
}

U32 SimulationChannelDescriptorGroup::GetCount()
{
    return static_cast<U32>( mData->mDescriptors.size() );
}

// ---------------------------------------------------------------------------
// test-facing accessors
// ---------------------------------------------------------------------------

namespace MockSdk
{
    void ResetChannelData()
    {
        for( AnalyzerChannelData* data : g_owned_channel_data )
            delete data;
        for( ChannelData* backing : g_owned_backing_data )
            delete backing;
        g_owned_channel_data.clear();
        g_owned_backing_data.clear();
        g_channel_registry.clear();
        g_op_count = 0;
        g_data_horizon = 0;
    }

    AnalyzerChannelData* MakeChannelData( BitState initial_state, const std::vector<U64>& transitions )
    {
        ChannelData* backing = new ChannelData();
        backing->mInitialState = initial_state;
        backing->mTransitions = transitions;
        std::sort( backing->mTransitions.begin(), backing->mTransitions.end() );
        AnalyzerChannelData* data = new AnalyzerChannelData( backing );
        g_owned_backing_data.push_back( backing );
        g_owned_channel_data.push_back( data );
        return data;
    }

    void SetChannelData( const Channel& channel, AnalyzerChannelData* data )
    {
        g_channel_registry[ ChannelKey( channel ) ] = data;
    }

    void SetSampleRate( U32 sample_rate_hz )
    {
        g_sample_rate = sample_rate_hz;
    }

    void SetOpLimit( U64 max_channel_data_operations )
    {
        g_op_limit = max_channel_data_operations;
        g_op_count = 0;
    }

    void SetDataHorizon( U64 sample )
    {
        g_data_horizon = sample;
    }

    const std::vector<Frame>& GetFrames( AnalyzerResults* results )
    {
        return results->GetAnalyzerResultsData()->mFrames;
    }

    const std::vector<RecordedFrameV2>& GetFramesV2( AnalyzerResults* results )
    {
        return results->GetAnalyzerResultsData()->mFramesV2;
    }

    const std::vector<RecordedMarker>& GetMarkers( AnalyzerResults* results )
    {
        return results->GetAnalyzerResultsData()->mMarkers;
    }

    const std::vector<std::string>& GetTabularText( AnalyzerResults* results )
    {
        return results->GetAnalyzerResultsData()->mTabularText;
    }

    void SetExportCancelAfter( U64 update_calls )
    {
        g_export_cancel_after = update_calls;
        g_export_update_calls = 0;
    }

    Channel GetSimChannel( SimulationChannelDescriptor* descriptor )
    {
        return descriptor->GetChannel();
    }

    BitState GetSimInitialState( SimulationChannelDescriptor* descriptor )
    {
        return descriptor->GetInitialBitState();
    }

    const std::vector<U64>& GetSimTransitions( SimulationChannelDescriptor* descriptor )
    {
        return static_cast<SimulationChannelDescriptorData*>( descriptor->GetData() )->mTransitions;
    }
}

// AnalyzerResults exposes its data struct through this "don't use" hook, which
// conveniently is exactly what a test harness is for.
AnalyzerResultsData* AnalyzerResults::GetAnalyzerResultsData()
{
    return mData;
}
