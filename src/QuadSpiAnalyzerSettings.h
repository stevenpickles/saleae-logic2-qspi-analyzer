#ifndef QUADSPI_ANALYZER_SETTINGS
#define QUADSPI_ANALYZER_SETTINGS

#include <AnalyzerSettings.h>
#include <AnalyzerTypes.h>

class QuadSpiAnalyzerSettings : public AnalyzerSettings
{
public:
	QuadSpiAnalyzerSettings();
	virtual ~QuadSpiAnalyzerSettings();

	virtual bool SetSettingsFromInterfaces();
	void UpdateInterfacesFromSettings();
	virtual void LoadSettings( const char* settings );
	virtual const char* SaveSettings();

	
	Channel mInputChannel;
	U32 mBitRate;

protected:
	AnalyzerSettingInterfaceChannel	mInputChannelInterface;
	AnalyzerSettingInterfaceInteger	mBitRateInterface;
};

#endif //QUADSPI_ANALYZER_SETTINGS
