#include "QuadSpiAnalyzerSettings.h"

#include <AnalyzerHelpers.h>

namespace
{
    void AddBusWidthOptions( AnalyzerSettingInterfaceNumberList& interface )
    {
        interface.AddNumber( 1.0, "Single (SIO)", "One data line (IO0; responses on IO1)" );
        interface.AddNumber( 2.0, "Dual (DIO)", "Two data lines (IO1:IO0), 2 bits per clock" );
        interface.AddNumber( 4.0, "Quad (QIO)", "Four data lines (IO3:IO0), 4 bits per clock" );
    }
}

QuadSpiAnalyzerSettings::QuadSpiAnalyzerSettings()
    : mChipSelectChannel( UNDEFINED_CHANNEL ),
      mClockChannel( UNDEFINED_CHANNEL ),
      mData0Channel( UNDEFINED_CHANNEL ),
      mData1Channel( UNDEFINED_CHANNEL ),
      mData2Channel( UNDEFINED_CHANNEL ),
      mData3Channel( UNDEFINED_CHANNEL ),
      mCommandBusWidth( 1 ),
      mAddressBusWidth( 4 ),
      mDataBusWidth( 4 ),
      mAddressBits( 24 ),
      mDummyCycles( 6 ),
      mClockInactiveState( BIT_LOW ),
      mDataValidEdge( AnalyzerEnums::LeadingEdge ),
      mChipSelectActiveState( BIT_LOW ),
      mChipSelectChannelInterface( new AnalyzerSettingInterfaceChannel() ),
      mClockChannelInterface( new AnalyzerSettingInterfaceChannel() ),
      mData0ChannelInterface( new AnalyzerSettingInterfaceChannel() ),
      mData1ChannelInterface( new AnalyzerSettingInterfaceChannel() ),
      mData2ChannelInterface( new AnalyzerSettingInterfaceChannel() ),
      mData3ChannelInterface( new AnalyzerSettingInterfaceChannel() ),
      mCommandBusWidthInterface( new AnalyzerSettingInterfaceNumberList() ),
      mAddressBusWidthInterface( new AnalyzerSettingInterfaceNumberList() ),
      mDataBusWidthInterface( new AnalyzerSettingInterfaceNumberList() ),
      mAddressBitsInterface( new AnalyzerSettingInterfaceNumberList() ),
      mDummyCyclesInterface( new AnalyzerSettingInterfaceInteger() ),
      mClockInactiveStateInterface( new AnalyzerSettingInterfaceNumberList() ),
      mDataValidEdgeInterface( new AnalyzerSettingInterfaceNumberList() ),
      mChipSelectActiveStateInterface( new AnalyzerSettingInterfaceNumberList() )
{
    mChipSelectChannelInterface->SetTitleAndTooltip( "CS", "Chip Select" );
    mChipSelectChannelInterface->SetChannel( mChipSelectChannel );

    mClockChannelInterface->SetTitleAndTooltip( "SCK", "Serial Clock" );
    mClockChannelInterface->SetChannel( mClockChannel );

    mData0ChannelInterface->SetTitleAndTooltip( "IO0 (MOSI)", "Data line 0" );
    mData0ChannelInterface->SetChannel( mData0Channel );

    mData1ChannelInterface->SetTitleAndTooltip( "IO1 (MISO)", "Data line 1" );
    mData1ChannelInterface->SetChannel( mData1Channel );

    mData2ChannelInterface->SetTitleAndTooltip( "IO2 (WP)", "Data line 2 (required for quad width)" );
    mData2ChannelInterface->SetChannel( mData2Channel );
    mData2ChannelInterface->SetSelectionOfNoneIsAllowed( true );

    mData3ChannelInterface->SetTitleAndTooltip( "IO3 (HOLD)", "Data line 3 (required for quad width)" );
    mData3ChannelInterface->SetChannel( mData3Channel );
    mData3ChannelInterface->SetSelectionOfNoneIsAllowed( true );

    mCommandBusWidthInterface->SetTitleAndTooltip( "Command Width", "IO lines used during the command (opcode) phase" );
    AddBusWidthOptions( *mCommandBusWidthInterface );
    mCommandBusWidthInterface->SetNumber( mCommandBusWidth );

    mAddressBusWidthInterface->SetTitleAndTooltip( "Address Width", "IO lines used during the address phase" );
    AddBusWidthOptions( *mAddressBusWidthInterface );
    mAddressBusWidthInterface->SetNumber( mAddressBusWidth );

    mDataBusWidthInterface->SetTitleAndTooltip( "Data Width", "IO lines used during the data phase" );
    AddBusWidthOptions( *mDataBusWidthInterface );
    mDataBusWidthInterface->SetNumber( mDataBusWidth );

    mAddressBitsInterface->SetTitleAndTooltip( "Address Bits", "Length of the address phase" );
    mAddressBitsInterface->AddNumber( 0.0, "No address phase", "Transactions go straight from command to dummy/data" );
    mAddressBitsInterface->AddNumber( 8.0, "8 bits", "" );
    mAddressBitsInterface->AddNumber( 16.0, "16 bits", "" );
    mAddressBitsInterface->AddNumber( 24.0, "24 bits", "Typical for QSPI flash up to 16 MB" );
    mAddressBitsInterface->AddNumber( 32.0, "32 bits", "Typical for QSPI flash over 16 MB" );
    mAddressBitsInterface->SetNumber( mAddressBits );

    mDummyCyclesInterface->SetTitleAndTooltip( "Dummy Cycles",
                                               "Clock cycles between the address and data phases. Count continuous-read "
                                               "mode cycles here as well." );
    mDummyCyclesInterface->SetMin( 0 );
    mDummyCyclesInterface->SetMax( 63 );
    mDummyCyclesInterface->SetInteger( mDummyCycles );

    mClockInactiveStateInterface->SetTitleAndTooltip( "Clock Polarity (CPOL)", "Clock line state when inactive" );
    mClockInactiveStateInterface->AddNumber( BIT_LOW, "CPOL = 0 (clock idles low)", "" );
    mClockInactiveStateInterface->AddNumber( BIT_HIGH, "CPOL = 1 (clock idles high)", "" );
    mClockInactiveStateInterface->SetNumber( mClockInactiveState );

    mDataValidEdgeInterface->SetTitleAndTooltip( "Clock Phase (CPHA)", "Clock edge on which data is sampled" );
    mDataValidEdgeInterface->AddNumber( AnalyzerEnums::LeadingEdge, "CPHA = 0 (data valid on leading edge)", "" );
    mDataValidEdgeInterface->AddNumber( AnalyzerEnums::TrailingEdge, "CPHA = 1 (data valid on trailing edge)", "" );
    mDataValidEdgeInterface->SetNumber( mDataValidEdge );

    mChipSelectActiveStateInterface->SetTitleAndTooltip( "CS Active State", "Chip select level during a transaction" );
    mChipSelectActiveStateInterface->AddNumber( BIT_LOW, "Active Low (typical)", "" );
    mChipSelectActiveStateInterface->AddNumber( BIT_HIGH, "Active High", "" );
    mChipSelectActiveStateInterface->SetNumber( mChipSelectActiveState );

    AddInterface( mChipSelectChannelInterface.get() );
    AddInterface( mClockChannelInterface.get() );
    AddInterface( mData0ChannelInterface.get() );
    AddInterface( mData1ChannelInterface.get() );
    AddInterface( mData2ChannelInterface.get() );
    AddInterface( mData3ChannelInterface.get() );
    AddInterface( mCommandBusWidthInterface.get() );
    AddInterface( mAddressBusWidthInterface.get() );
    AddInterface( mDataBusWidthInterface.get() );
    AddInterface( mAddressBitsInterface.get() );
    AddInterface( mDummyCyclesInterface.get() );
    AddInterface( mClockInactiveStateInterface.get() );
    AddInterface( mDataValidEdgeInterface.get() );
    AddInterface( mChipSelectActiveStateInterface.get() );

    AddExportOption( 0, "Export as csv file" );
    AddExportExtension( 0, "csv", "csv" );

    AddChannelLabels( false );
}

QuadSpiAnalyzerSettings::~QuadSpiAnalyzerSettings()
{
}

void QuadSpiAnalyzerSettings::AddChannelLabels( bool channels_are_used )
{
    ClearChannels();
    AddChannel( mChipSelectChannel, "CS", channels_are_used );
    AddChannel( mClockChannel, "SCK", channels_are_used );
    AddChannel( mData0Channel, "IO0", channels_are_used );
    AddChannel( mData1Channel, "IO1", channels_are_used );
    AddChannel( mData2Channel, "IO2", channels_are_used && ( mData2Channel != UNDEFINED_CHANNEL ) );
    AddChannel( mData3Channel, "IO3", channels_are_used && ( mData3Channel != UNDEFINED_CHANNEL ) );
}

bool QuadSpiAnalyzerSettings::SetSettingsFromInterfaces()
{
    Channel chip_select = mChipSelectChannelInterface->GetChannel();
    Channel clock = mClockChannelInterface->GetChannel();
    Channel data0 = mData0ChannelInterface->GetChannel();
    Channel data1 = mData1ChannelInterface->GetChannel();
    Channel data2 = mData2ChannelInterface->GetChannel();
    Channel data3 = mData3ChannelInterface->GetChannel();

    Channel channels[ 6 ] = { chip_select, clock, data0, data1, data2, data3 };
    if( AnalyzerHelpers::DoChannelsOverlap( channels, 6 ) )
    {
        SetErrorText( "Please select a different channel for each signal." );
        return false;
    }

    if( chip_select == UNDEFINED_CHANNEL || clock == UNDEFINED_CHANNEL || data0 == UNDEFINED_CHANNEL || data1 == UNDEFINED_CHANNEL )
    {
        SetErrorText( "Please select channels for CS, SCK, IO0 and IO1." );
        return false;
    }

    U32 command_width = static_cast<U32>( mCommandBusWidthInterface->GetNumber() );
    U32 address_width = static_cast<U32>( mAddressBusWidthInterface->GetNumber() );
    U32 data_width = static_cast<U32>( mDataBusWidthInterface->GetNumber() );

    bool quad_used = ( command_width == 4 ) || ( address_width == 4 ) || ( data_width == 4 );
    if( quad_used && ( data2 == UNDEFINED_CHANNEL || data3 == UNDEFINED_CHANNEL ) )
    {
        SetErrorText( "Quad width requires IO2 and IO3 channels." );
        return false;
    }

    mChipSelectChannel = chip_select;
    mClockChannel = clock;
    mData0Channel = data0;
    mData1Channel = data1;
    mData2Channel = data2;
    mData3Channel = data3;

    mCommandBusWidth = command_width;
    mAddressBusWidth = address_width;
    mDataBusWidth = data_width;
    mAddressBits = static_cast<U32>( mAddressBitsInterface->GetNumber() );
    mDummyCycles = static_cast<U32>( mDummyCyclesInterface->GetInteger() );
    mClockInactiveState = static_cast<BitState>( static_cast<U32>( mClockInactiveStateInterface->GetNumber() ) );
    mDataValidEdge = static_cast<AnalyzerEnums::Edge>( static_cast<U32>( mDataValidEdgeInterface->GetNumber() ) );
    mChipSelectActiveState = static_cast<BitState>( static_cast<U32>( mChipSelectActiveStateInterface->GetNumber() ) );

    AddChannelLabels( true );

    return true;
}

void QuadSpiAnalyzerSettings::UpdateInterfacesFromSettings()
{
    mChipSelectChannelInterface->SetChannel( mChipSelectChannel );
    mClockChannelInterface->SetChannel( mClockChannel );
    mData0ChannelInterface->SetChannel( mData0Channel );
    mData1ChannelInterface->SetChannel( mData1Channel );
    mData2ChannelInterface->SetChannel( mData2Channel );
    mData3ChannelInterface->SetChannel( mData3Channel );
    mCommandBusWidthInterface->SetNumber( mCommandBusWidth );
    mAddressBusWidthInterface->SetNumber( mAddressBusWidth );
    mDataBusWidthInterface->SetNumber( mDataBusWidth );
    mAddressBitsInterface->SetNumber( mAddressBits );
    mDummyCyclesInterface->SetInteger( mDummyCycles );
    mClockInactiveStateInterface->SetNumber( mClockInactiveState );
    mDataValidEdgeInterface->SetNumber( mDataValidEdge );
    mChipSelectActiveStateInterface->SetNumber( mChipSelectActiveState );
}

void QuadSpiAnalyzerSettings::LoadSettings( const char* settings )
{
    SimpleArchive text_archive;
    text_archive.SetString( settings );

    U32 clock_inactive_state;
    U32 data_valid_edge;
    U32 chip_select_active_state;

    text_archive >> mChipSelectChannel;
    text_archive >> mClockChannel;
    text_archive >> mData0Channel;
    text_archive >> mData1Channel;
    text_archive >> mData2Channel;
    text_archive >> mData3Channel;
    text_archive >> mCommandBusWidth;
    text_archive >> mAddressBusWidth;
    text_archive >> mDataBusWidth;
    text_archive >> mAddressBits;
    text_archive >> mDummyCycles;
    text_archive >> clock_inactive_state;
    text_archive >> data_valid_edge;
    text_archive >> chip_select_active_state;

    mClockInactiveState = static_cast<BitState>( clock_inactive_state );
    mDataValidEdge = static_cast<AnalyzerEnums::Edge>( data_valid_edge );
    mChipSelectActiveState = static_cast<BitState>( chip_select_active_state );

    AddChannelLabels( true );

    UpdateInterfacesFromSettings();
}

const char* QuadSpiAnalyzerSettings::SaveSettings()
{
    SimpleArchive text_archive;

    text_archive << mChipSelectChannel;
    text_archive << mClockChannel;
    text_archive << mData0Channel;
    text_archive << mData1Channel;
    text_archive << mData2Channel;
    text_archive << mData3Channel;
    text_archive << mCommandBusWidth;
    text_archive << mAddressBusWidth;
    text_archive << mDataBusWidth;
    text_archive << mAddressBits;
    text_archive << mDummyCycles;
    text_archive << static_cast<U32>( mClockInactiveState );
    text_archive << static_cast<U32>( mDataValidEdge );
    text_archive << static_cast<U32>( mChipSelectActiveState );

    return SetReturnString( text_archive.GetString() );
}
