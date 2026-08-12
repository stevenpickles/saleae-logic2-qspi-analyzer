#ifndef QUADSPI_ANALYZER_SETTINGS
#define QUADSPI_ANALYZER_SETTINGS

#include <AnalyzerSettings.h>
#include <AnalyzerTypes.h>

#include <memory>

class QuadSpiAnalyzerSettings : public AnalyzerSettings
{
  public:
    QuadSpiAnalyzerSettings();
    virtual ~QuadSpiAnalyzerSettings();

    virtual bool SetSettingsFromInterfaces();
    void UpdateInterfacesFromSettings();
    virtual void LoadSettings( const char* settings );
    virtual const char* SaveSettings();

    Channel mChipSelectChannel;
    Channel mClockChannel;
    Channel mData0Channel; // IO0 / MOSI
    Channel mData1Channel; // IO1 / MISO
    Channel mData2Channel; // IO2 / WP    (optional; required for quad width)
    Channel mData3Channel; // IO3 / HOLD  (optional; required for quad width)

    U32 mCommandBusWidth; // 1 | 2 | 4 IO lines used during the command phase
    U32 mAddressBusWidth; // 1 | 2 | 4 IO lines used during the address phase
    U32 mDataBusWidth;    // 1 | 2 | 4 IO lines used during the data phase
    U32 mAddressBits;     // 0 | 8 | 16 | 24 | 32 (0 = no address phase)
    U32 mDummyCycles;     // clock cycles between address and data phases

    BitState mClockInactiveState;      // CPOL: BIT_LOW = CPOL0, BIT_HIGH = CPOL1
    AnalyzerEnums::Edge mDataValidEdge; // CPHA: LeadingEdge = CPHA0, TrailingEdge = CPHA1
    BitState mChipSelectActiveState;   // BIT_LOW = active low (typical), BIT_HIGH = active high

  protected:
    std::unique_ptr<AnalyzerSettingInterfaceChannel> mChipSelectChannelInterface;
    std::unique_ptr<AnalyzerSettingInterfaceChannel> mClockChannelInterface;
    std::unique_ptr<AnalyzerSettingInterfaceChannel> mData0ChannelInterface;
    std::unique_ptr<AnalyzerSettingInterfaceChannel> mData1ChannelInterface;
    std::unique_ptr<AnalyzerSettingInterfaceChannel> mData2ChannelInterface;
    std::unique_ptr<AnalyzerSettingInterfaceChannel> mData3ChannelInterface;

    std::unique_ptr<AnalyzerSettingInterfaceNumberList> mCommandBusWidthInterface;
    std::unique_ptr<AnalyzerSettingInterfaceNumberList> mAddressBusWidthInterface;
    std::unique_ptr<AnalyzerSettingInterfaceNumberList> mDataBusWidthInterface;
    std::unique_ptr<AnalyzerSettingInterfaceNumberList> mAddressBitsInterface;
    std::unique_ptr<AnalyzerSettingInterfaceInteger> mDummyCyclesInterface;
    std::unique_ptr<AnalyzerSettingInterfaceNumberList> mClockInactiveStateInterface;
    std::unique_ptr<AnalyzerSettingInterfaceNumberList> mDataValidEdgeInterface;
    std::unique_ptr<AnalyzerSettingInterfaceNumberList> mChipSelectActiveStateInterface;

  private:
    void AddChannelLabels( bool channels_are_used );
};

#endif // QUADSPI_ANALYZER_SETTINGS
