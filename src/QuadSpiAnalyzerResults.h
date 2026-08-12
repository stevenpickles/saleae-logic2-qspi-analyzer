#ifndef QUADSPI_ANALYZER_RESULTS_H
#define QUADSPI_ANALYZER_RESULTS_H

#include <AnalyzerResults.h>

// Frame::mType values. Field usage per type:
//   FRAME_COMMAND: mData1 = opcode,        mData2 = bits captured
//   FRAME_ADDRESS: mData1 = address,       mData2 = bits captured
//   FRAME_DUMMY:   mData1 = clock cycles
//   FRAME_DATA:    mData1 = IO0-lane byte, mData2 = ( bits captured << 32 ) | IO1-lane byte (IO1 lane meaningful in SIO only)
//   FRAME_ERROR:   no data
enum QspiFrameType
{
    FRAME_COMMAND = 0,
    FRAME_ADDRESS,
    FRAME_DUMMY,
    FRAME_DATA,
    FRAME_ERROR
};

// set on frames cut short by chip select deasserting mid-phase
#define QSPI_TRUNCATED_FLAG ( 1 << 0 )

class QuadSpiAnalyzer;
class QuadSpiAnalyzerSettings;

class QuadSpiAnalyzerResults : public AnalyzerResults
{
  public:
    QuadSpiAnalyzerResults( QuadSpiAnalyzer* analyzer, QuadSpiAnalyzerSettings* settings );
    virtual ~QuadSpiAnalyzerResults();

    virtual void GenerateBubbleText( U64 frame_index, Channel& channel, DisplayBase display_base );
    virtual void GenerateExportFile( const char* file, DisplayBase display_base, U32 export_type_user_id );

    virtual void GenerateFrameTabularText( U64 frame_index, DisplayBase display_base );
    virtual void GeneratePacketTabularText( U64 packet_id, DisplayBase display_base );
    virtual void GenerateTransactionTabularText( U64 transaction_id, DisplayBase display_base );

  protected: // vars
    QuadSpiAnalyzerSettings* mSettings;
    QuadSpiAnalyzer* mAnalyzer;
};

#endif // QUADSPI_ANALYZER_RESULTS_H
