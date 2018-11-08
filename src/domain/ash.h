#ifndef ASH_H
#define ASH_H

#include <cstdint>
#include <vector>

#include "timer.h"


typedef enum {
  ASH_RESET_FAILED,
  ASH_ACK,
  ASH_NACK
}EAshInfo;

class CAshCallback
{
public:
    virtual void ashCbInfo( EAshInfo info );
};

class CAsh
{
public:
    CAsh(CAshCallback *ipCb);

    std::vector<uint8_t> resetNCPFrame(void);

    std::vector<uint8_t> AckFrame(void);

    std::vector<uint8_t> DataFrame(std::vector<uint8_t> i_data);

    std::vector<uint8_t> decode(std::vector<uint8_t> *i_data);

    bool isConnected(void){ return stateConnected; }


private:
    uint8_t ackNum;
    uint8_t frmNum;
    uint8_t seq_num;
    bool stateConnected;
    CTimer timer;
    CAshCallback *pCb;

    std::vector<uint8_t> in_msg;

    uint16_t computeCRC( std::vector<uint8_t> i_msg );
    std::vector<uint8_t> stuffedOutputData(std::vector<uint8_t> i_msg);
    std::vector<uint8_t> dataRandomise(std::vector<uint8_t> i_data, uint8_t start);
};


#endif // ASH_H