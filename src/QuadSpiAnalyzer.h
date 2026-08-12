#ifndef QUADSPI_ANALYZER_H
#define QUADSPI_ANALYZER_H

#include <Analyzer.h>
#include "QuadSpiAnalyzerSettings.h"
#include "QuadSpiAnalyzerResults.h"
#include "QuadSpiSimulationDataGenerator.h"
#include <memory>

class ANALYZER_EXPORT QuadSpiAnalyzer : public Analyzer2
{
public:
	QuadSpiAnalyzer();
	virtual ~QuadSpiAnalyzer();

	virtual void SetupResults();
	virtual void WorkerThread();

	virtual U32 GenerateSimulationData( U64 newest_sample_requested, U32 sample_rate, SimulationChannelDescriptor** simulation_channels );
	virtual U32 GetMinimumSampleRateHz();

	virtual const char* GetAnalyzerName() const;
	virtual bool NeedsRerun();

protected: //vars
	QuadSpiAnalyzerSettings mSettings;
	std::unique_ptr<QuadSpiAnalyzerResults> mResults;
	AnalyzerChannelData* mSerial;

	QuadSpiSimulationDataGenerator mSimulationDataGenerator;
	bool mSimulationInitilized;
};

extern "C" ANALYZER_EXPORT const char* __cdecl GetAnalyzerName();
extern "C" ANALYZER_EXPORT Analyzer* __cdecl CreateAnalyzer( );
extern "C" ANALYZER_EXPORT void __cdecl DestroyAnalyzer( Analyzer* analyzer );

#endif //QUADSPI_ANALYZER_H
