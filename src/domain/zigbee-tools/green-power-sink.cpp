/**
 * @file green-power-sink.cpp
 */

#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <map>

#include "green-power-sink.h"
#include "../ezsp-protocol/struct/ember-gp-address-struct.h"

#include "../../domain/zbmessage/zigbee-message.h"

#include "../../spi/GenericLogger.h"
#include "../../spi/ILogger.h"

// some defines to help understanding
#define GP_ENDPOINT 242

// cluster
#define GP_CLUSTER_ID   0x0021
// receive client command
#define GP_PROXY_COMMISIONING_MODE_CLIENT_CMD_ID    0x02

// GPF Command
#define GPF_SCENE_0_CMD		0x10
#define GPF_SCENE_1_CMD		0x11
#define GPF_SCENE_2_CMD		0x12
#define GPF_SCENE_3_CMD		0x13
#define GPF_SCENE_4_CMD		0x14
#define GPF_SCENE_5_CMD		0x15
#define GPF_SCENE_6_CMD		0x16
#define GPF_SCENE_7_CMD		0x17

#define GPF_STORE_SCENE_0_CMD		0x18
#define GPF_STORE_SCENE_1_CMD		0x19
#define GPF_STORE_SCENE_2_CMD		0x1A
#define GPF_STORE_SCENE_3_CMD		0x1B
#define GPF_STORE_SCENE_4_CMD		0x1C
#define GPF_STORE_SCENE_5_CMD		0x1D
#define GPF_STORE_SCENE_6_CMD		0x1E
#define GPF_STORE_SCENE_7_CMD		0x1F

#define GPF_OFF_CMD		0x20
#define GPF_ON_CMD		0x21
#define GPF_TOGGLE_CMD	0x22

#define GPF_UP_W_ON_OFF_CMD		0x34
#define GPF_STOP_CMD			0x35
#define GPF_DOWN_W_ON_OFF_CMD	0x36

#define GPF_COMMISSIONING_CMD	0xE0
#define GPF_DECOMMISSIONING_CMD	0xE1



CGpSink::CGpSink( CEzspDongle &i_dongle, CZigbeeMessaging &i_zb_messaging ) :
    dongle(i_dongle),
    zb_messaging(i_zb_messaging),
    sink_table(),
    sink_state(SINK_NOT_INIT),
    gpf_comm_frame(),
    observers()
{
    dongle.registerObserver(this);
}

void CGpSink::init(void)
{
    // initialize green power sink
    clogD << "Call EZSP_GP_SINK_TABLE_INIT" << std::endl;
    dongle.sendCommand(EZSP_GP_SINK_TABLE_INIT);

    // set state
    setSinkState(SINK_READY);    
}

void CGpSink::openCommissioningSession(void)
{
    // set local proxy in commissioning mode
    sendLocalGPProxyCommissioningMode();

    // set state
    setSinkState(SINK_COM_OPEN);
}

uint8_t CGpSink::registerGpd( uint32_t i_source_id )
{
    CGpSinkTableEntry l_entry = CGpSinkTableEntry(i_source_id);

    return sink_table.addEntry(l_entry);
}

void CGpSink::handleDongleState( EDongleState i_state )
{
}

void CGpSink::handleEzspRxMessage( EEzspCmd i_cmd, std::vector<uint8_t> i_msg_receive )
{
    switch( i_cmd )
    {
        case EZSP_GP_SINK_TABLE_INIT:
        {
            clogI << "EZSP_GP_SINK_TABLE_INIT RSP" << std::endl;
        }
        break;        
        case EZSP_GPEP_INCOMING_MESSAGE_HANDLER:
        {
            EEmberStatus l_status = static_cast<EEmberStatus>(i_msg_receive.at(0));

            // build gpf frame from ezsp rx message
            CGpFrame gpf = CGpFrame(i_msg_receive);


            // Start DEBUG
            clogD << "EZSP_GPEP_INCOMING_MESSAGE_HANDLER status : " << CEzspEnum::EEmberStatusToString(l_status) <<
                ", link : " << unsigned(i_msg_receive.at(1)) <<
                ", sequence number : " << unsigned(i_msg_receive.at(2)) <<
                ", gp address : " << gpf <</*
                ", last hop rssi : " << unsigned(last_hop_rssi) << 
                ", from : "<< std::hex << std::setw(4) << std::setfill('0') << unsigned(sender) << */
                std::endl;

/*
            std::stringstream bufDump;
            for (size_t i =0; i<i_msg_receive.size(); i++) {
                bufDump << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(i_msg_receive[i]) << " ";
            }
            clogI << "raw : " << bufDump.str() << std::endl;
*/
            // Stop DEBUG

            /**
             * trame gpf:
             * - no cryptée : on essaye de la validé en donner la TC link key (zig...009), dans le cas ou il s'agit d'une trame de commissioning
             * - crypté :
             *      - on cherche dans la table du sink (créé une class sink table entry comme décrit tabnle 25 section A3.3.2.2.2 du doc Green Power Basic spec v1.0)
             *      - si trouvé on essaye de la validé en passant la key associéé
             * - si la validation réussi on notifie le gpf au observateur
             */

            if( GPD_NO_SECURITY == gpf.getSecurity() )
            {
                // to test notify
                notifyObserversOfRxGpFrame( gpf );

                // if we are in Commissioning and this is a commissioning frame : use it !
                if( (SINK_COM_OPEN == sink_state) && (GPF_COMMISSIONING_CMD == gpf.getCommandId()) )
                {
                    // find entry in sink table
                    gpSinkTableFindOrAllocateEntry(gpf.getSourceId());

                    // save incomming message
                    gpf_comm_frame = gpf;

                    // set new state
                    setSinkState(SINK_COM_IN_PROGRESS);
                }
            }
            else
            {
                // look up if product is register
                uint8_t l_sink_entry_idx = sink_table.getEntryIndexForSourceId( gpf.getSourceId() );
                if( GP_SINK_INVALID_ENTRY != l_sink_entry_idx )
                {
                    // to test notify
                    notifyObserversOfRxGpFrame( gpf );
                }
            }
        }
        break;

        case EZSP_GP_SINK_TABLE_FIND_OR_ALLOCATE_ENTRY:
        {
            if( SINK_COM_IN_PROGRESS == sink_state )
            {
                // retrieve entry
                gpSinkGetEntry( i_msg_receive.at(0) );
            }
        }
        break;

        case EZSP_GP_SINK_TABLE_GET_ENTRY:
        {
            if( SINK_COM_IN_PROGRESS == sink_state )
            {
                // set the entry with new parameters            
                std::vector<uint8_t> l_struct;
                uint32_t l_src_id = gpf_comm_frame.getSourceId();
                // Internal status of the sink table entry.
                l_struct.push_back(0x01); // active, 0xff : disable
                // The tunneling options (this contains both options and extendedOptions from the spec). WARNING 16 bits !!!
                l_struct.push_back(0xA8);
                l_struct.push_back(0x02);
                // The addressing info of the GPD.
                l_struct.push_back(0x00); // short address
                l_struct.push_back(static_cast<uint8_t>(l_src_id&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>8)&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>16)&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>24)&0xFF));
                l_struct.push_back(static_cast<uint8_t>(l_src_id&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>8)&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>16)&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>24)&0xFF));
                l_struct.push_back(0x00); // endpoint -> not used
                // The device id for the GPD.
                l_struct.push_back(gpf_comm_frame.getPayload().at(0)); // 0x02
                // The list of sinks (hardcoded to 2 which is the spec minimum).
                l_struct.push_back(0xFF); // unused
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0xFF); // unused
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                // The assigned alias for the GPD.
                l_struct.push_back(static_cast<uint8_t>(l_src_id&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>8)&0xFF));

                // The groupcast radius.
                l_struct.push_back(0x00);

                // The security options field.
                l_struct.push_back(0x12);

                // The security frame counter of the GPD.
                l_struct.push_back(gpf_comm_frame.getPayload().at(23));
                l_struct.push_back(gpf_comm_frame.getPayload().at(24));
                l_struct.push_back(gpf_comm_frame.getPayload().at(25));
                l_struct.push_back(gpf_comm_frame.getPayload().at(26));

                // The key to use for GPD. 59 13 29 50 28 9D 14 FD 73 F9 C3 25 D4 57 AB B5
                l_struct.push_back(0x59);
                l_struct.push_back(0x13);
                l_struct.push_back(0x29);
                l_struct.push_back(0x50);
                l_struct.push_back(0x28);
                l_struct.push_back(0x9D);
                l_struct.push_back(0x14);
                l_struct.push_back(0xFD);
                l_struct.push_back(0x73);
                l_struct.push_back(0xF9);
                l_struct.push_back(0xC3);
                l_struct.push_back(0x25);
                l_struct.push_back(0xD4);
                l_struct.push_back(0x57);
                l_struct.push_back(0xAB);
                l_struct.push_back(0xB5);
                // call
                gpSinkSetEntry(0,l_struct);

            }
        }
        break;

        case EZSP_GP_SINK_TABLE_SET_ENTRY:
        {
            if( SINK_COM_IN_PROGRESS == sink_state )
            {
                // do proxy pairing
                std::vector<uint8_t> l_struct;
                uint32_t l_src_id = gpf_comm_frame.getSourceId();
                // The options field of the GP Pairing command
                l_struct.push_back(0xA8); 
                l_struct.push_back(0xE5); 
                l_struct.push_back(0x02); 
                l_struct.push_back(0x00); 
                // The addressing info of the target GPD.
                l_struct.push_back(0x00); // short address
                l_struct.push_back(static_cast<uint8_t>(l_src_id&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>8)&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>16)&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>24)&0xFF));
                l_struct.push_back(static_cast<uint8_t>(l_src_id&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>8)&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>16)&0xFF));
                l_struct.push_back(static_cast<uint8_t>((l_src_id>>24)&0xFF));
                l_struct.push_back(0x00); // endpoint -> not used
                // The communication mode of the GP Sink.
                l_struct.push_back(0x01);
                // The network address of the GP Sink.
                l_struct.push_back(0xFF);
                l_struct.push_back(0xFF);
                // The group ID of the GP Sink.
                l_struct.push_back(l_src_id&0xFF);
                l_struct.push_back((l_src_id>>8)&0xFF);
                // The alias assigned to the GPD.
                l_struct.push_back(0xFF);
                l_struct.push_back(0xFF);
                // The IEEE address of the GP Sink.
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                l_struct.push_back(0x00);
                // The key to use for GPD. 59 13 29 50 28 9D 14 FD 73 F9 C3 25 D4 57 AB B5
                l_struct.push_back(0x59);
                l_struct.push_back(0x13);
                l_struct.push_back(0x29);
                l_struct.push_back(0x50);
                l_struct.push_back(0x28);
                l_struct.push_back(0x9D);
                l_struct.push_back(0x14);
                l_struct.push_back(0xFD);
                l_struct.push_back(0x73);
                l_struct.push_back(0xF9);
                l_struct.push_back(0xC3);
                l_struct.push_back(0x25);
                l_struct.push_back(0xD4);
                l_struct.push_back(0x57);
                l_struct.push_back(0xAB);
                l_struct.push_back(0xB5);
                // The security frame counter of the GPD.
                l_struct.push_back(gpf_comm_frame.getPayload().at(23));
                l_struct.push_back(gpf_comm_frame.getPayload().at(24));
                l_struct.push_back(gpf_comm_frame.getPayload().at(25));
                l_struct.push_back(gpf_comm_frame.getPayload().at(26));
                // The forwarding radius.
                l_struct.push_back(0x00);
                // call
                gpProxyTableProcessGpPairing(l_struct);                
            }
        }
        break;

        case EZSP_GP_PROXY_TABLE_PROCESS_GP_PAIRING:
        {
            if( SINK_COM_IN_PROGRESS == sink_state )
            {
                clogI << "CGpSink::ezspHandler EZSP_GP_PROXY_TABLE_PROCESS_GP_PAIRING gpPairingAdded : " << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(i_msg_receive[0]) << std::endl;

                // set new state
                setSinkState(SINK_READY);                
            }
        }
        break;

        default:
        {
            /* DEBUG VIEW
            std::stringstream bufDump;

            for (size_t i =0; i<i_msg_receive.size(); i++) {
                bufDump << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(i_msg_receive[i]) << " ";
            }
            clogI << "CGpSink::ezspHandler : " << bufDump.str() << std::endl;
            */
        }
        break;
    }
}

bool CGpSink::registerObserver(CGpObserver* observer)
{
    return this->observers.emplace(observer).second;
}

bool CGpSink::unregisterObserver(CGpObserver* observer)
{
    return static_cast<bool>(this->observers.erase(observer));
}

void CGpSink::notifyObserversOfRxGpFrame( CGpFrame i_gpf ) {
    for(auto observer : this->observers) {
        observer->handleRxGpFrame( i_gpf );
    }
}

void CGpSink::sendLocalGPProxyCommissioningMode(void)
{
    // forge GP Proxy Commissioning Mode command
    // assume we are coordinator of network and our nodeId is 0

    CZigBeeMsg l_gp_comm_msg;
    std::vector<uint8_t> l_gp_comm_payload;

    // options:
    // bit0 (Action) : 0b1 / request to enter commissioning mode
    // bit1-3 (exit mode) : 0b010 / On first Pairing success
    // bit4 (channel present) : 0b0 / shall always be set to 0 according current spec.
    // bit5 (unicast communication) : 0b0 / send GP Commissioning Notification commands in broadcast
    // bit6-7 (reserved)
    l_gp_comm_payload.push_back(0x05); 

    // comm windows 2 bytes
    // present only if exit mode flag On commissioning Window expiration (bit0) is set

    // channel 1 byte
    // never present with current specification


    // create message sending from ep242 to ep242 using green power profile
    l_gp_comm_msg.SetSpecific(GP_PROFILE_ID, PUBLIC_CODE, GP_ENDPOINT, 
                                GP_CLUSTER_ID, GP_PROXY_COMMISIONING_MODE_CLIENT_CMD_ID,
                                E_DIR_SERVER_TO_CLIENT, l_gp_comm_payload, 0, 0, 0);

    // WARNING use ep 242 as sources
    l_gp_comm_msg.aps.src_ep = GP_ENDPOINT;
    
    //
    clogI << "SEND UNICAST : OPEN GP COMMISSIONING\n";
    zb_messaging.SendUnicast(0,l_gp_comm_msg);
}

/*
void CGpSink::gpCloseCommissioning()
{
    std::vector<uint8_t> l_payload;

    // The action to perform on the GP TX queue (true to add, false to remove).
    l_payload.push_back(0x00);

    // Whether to use ClearChannelAssessment when transmitting the GPDF.
    l_payload.push_back(0x00);

    // The Address of the destination GPD.
    l_payload.push_back(0x00);
    l_payload.push_back(0xFF);
    l_payload.push_back(0xFF);
    l_payload.push_back(0xFF);
    l_payload.push_back(0xFF);
    l_payload.push_back(0xFF);
    l_payload.push_back(0xFF);
    l_payload.push_back(0xFF);
    l_payload.push_back(0xFF);
    l_payload.push_back(0x7F);

    // The GPD command ID to send.
    l_payload.push_back(0x00);

    // The length of the GP command payload.
    l_payload.push_back(0x01);

    // The GP command payload.
    l_payload.push_back(0x00);

    // The handle to refer to the GPDF.
    l_payload.push_back(0x00);

    // How long to keep the GPDF in the TX Queue.
    l_payload.push_back(0x00);

    clogI << "EZSP_D_GP_SEND : CLOSE GP COMMISSIONING\n";
    dongle.sendCommand(EZSP_D_GP_SEND,l_payload);
}
*/

/*
void CGpSink::gpBrCommissioningNotification( uint32_t i_gpd_src_id, uint8_t i_seq_number )
{
    std::vector<uint8_t> l_proxy_br_payload;

    // The source from which to send the broadcast
    l_proxy_br_payload.push_back(static_cast<uint8_t>(i_gpd_src_id&0xFF));
    l_proxy_br_payload.push_back(static_cast<uint8_t>((i_gpd_src_id>>8)&0xFF));

    // The destination to which to send the broadcast. This must be one of the three ZigBee broadcast addresses.
    l_proxy_br_payload.push_back(0xFD);
    l_proxy_br_payload.push_back(0xFF);

    // The network sequence number for the broadcast
    l_proxy_br_payload.push_back(i_seq_number);

    // The APS frame for the message.
    CAPSFrame l_aps;
    l_aps.SetDefaultAPS(GP_PROFILE_ID,GP_CLUSTER_ID,GP_ENDPOINT);
    l_aps.src_ep = GP_ENDPOINT;
    std::vector<uint8_t> l_ember_aps = l_aps.GetEmberAPS();
    l_proxy_br_payload.insert(l_proxy_br_payload.end(), l_ember_aps.begin(), l_ember_aps.end());

    // The message will be delivered to all nodes within radius hops of the sender. A radius of zero is converted to EMBER_MAX_HOPS.
    l_proxy_br_payload.push_back(0x00);

    // A value chosen by the Host. This value is used in the ezspMessageSentHandler response to refer to this message.
    l_proxy_br_payload.push_back(0x00);

    // The length of the messageContents parameter in bytes.
    l_proxy_br_payload.push_back(49);

    // The broadcast message.
    l_proxy_br_payload.push_back(0x11);
    l_proxy_br_payload.push_back(0x06);
    l_proxy_br_payload.push_back(0x04);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0x08);
    l_proxy_br_payload.push_back(0x50);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0x51);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0x24);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0xe0);
    l_proxy_br_payload.push_back(0x1f);
    l_proxy_br_payload.push_back(0x02);
    l_proxy_br_payload.push_back(0xc5);
    l_proxy_br_payload.push_back(0xf2);
    l_proxy_br_payload.push_back(0xa8);
    l_proxy_br_payload.push_back(0xac);
    l_proxy_br_payload.push_back(0x43);
    l_proxy_br_payload.push_back(0x76);
    l_proxy_br_payload.push_back(0x30);
    l_proxy_br_payload.push_back(0x80);
    l_proxy_br_payload.push_back(0x89);
    l_proxy_br_payload.push_back(0x5f);
    l_proxy_br_payload.push_back(0x3c);
    l_proxy_br_payload.push_back(0xd5);
    l_proxy_br_payload.push_back(0xdc);
    l_proxy_br_payload.push_back(0x9a);
    l_proxy_br_payload.push_back(0xd8);
    l_proxy_br_payload.push_back(0x87);
    l_proxy_br_payload.push_back(0x1c);
    l_proxy_br_payload.push_back(0x0d);
    l_proxy_br_payload.push_back(0x15);
    l_proxy_br_payload.push_back(0xde);
    l_proxy_br_payload.push_back(0x17);
    l_proxy_br_payload.push_back(0x2b);
    l_proxy_br_payload.push_back(0x24);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0x04);
    l_proxy_br_payload.push_back(0x02);
    l_proxy_br_payload.push_back(0x20);
    l_proxy_br_payload.push_back(0x21);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0x00);
    l_proxy_br_payload.push_back(0xdc);

    clogI << "EZSP_PROXY_BROADCAST\n";
    dongle.sendCommand(EZSP_PROXY_BROADCAST,l_proxy_br_payload);
}
*/
/*
void CAppDemo::ezspGetExtendedValue( uint8_t i_value_id, uint32_t i_characteristic )
{
    std::vector<uint8_t> l_payload;

    // Identifies which extended value ID to read.
    l_payload.push_back(i_value_id);

    // Identifies which characteristics of the extended value ID to read. These are specific to the value being read.
    l_payload.push_back((uint8_t)(i_characteristic&0xFF));
    l_payload.push_back((uint8_t)((i_characteristic>>8)&0xFF));
    l_payload.push_back((uint8_t)((i_characteristic>>16)&0xFF));
    l_payload.push_back((uint8_t)((i_characteristic>>24)&0xFF));

    clogI << "EZSP_GET_EXTENDED_VALUE\n";
    dongle.sendCommand(EZSP_GET_EXTENDED_VALUE,l_payload);
}
*/

/*
void CAppDemo::gpSinkTableLookup( uint32_t i_src_id )
{
    std::vector<uint8_t> l_payload;

    // EmberGpAddress addr The address to search for.
    l_payload.push_back(0x00);
    l_payload.push_back((uint8_t)(i_src_id&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>8)&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>16)&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>24)&0xFF));
    l_payload.push_back((uint8_t)(i_src_id&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>8)&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>16)&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>24)&0xFF));
    l_payload.push_back(0x00);

    clogI << "EZSP_GP_SINK_TABLE_LOOKUP\n";
    dongle.sendCommand(EZSP_GP_SINK_TABLE_LOOKUP,l_payload);    
}
*/

/**
 *  Finds or allocates a sink entry
 */
void CGpSink::gpSinkTableFindOrAllocateEntry( uint32_t i_src_id )
{
    std::vector<uint8_t> l_payload;

    // An EmberGpAddress struct containing a copy of the gpd address to be found.
    l_payload.push_back(0x00);
    l_payload.push_back((uint8_t)(i_src_id&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>8)&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>16)&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>24)&0xFF));
    l_payload.push_back((uint8_t)(i_src_id&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>8)&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>16)&0xFF));
    l_payload.push_back((uint8_t)((i_src_id>>24)&0xFF));
    l_payload.push_back(0x00);

    dongle.sendCommand(EZSP_GP_SINK_TABLE_FIND_OR_ALLOCATE_ENTRY,l_payload);    
}

/**
 * Retrieves the sink table entry stored at the passed index.
 */
void CGpSink::gpSinkGetEntry( uint8_t i_index )
{
    std::vector<uint8_t> l_payload;

    // The index of the requested sink table entry.
    l_payload.push_back(i_index);

    clogI << "EZSP_GP_SINK_TABLE_GET_ENTRY\n";
    dongle.sendCommand(EZSP_GP_SINK_TABLE_GET_ENTRY,l_payload);    
}


/**
 * Retrieves the sink table entry stored at the passed index.
 */
void CGpSink::gpSinkSetEntry( uint8_t i_index, std::vector<uint8_t> i_struct )
{
    std::vector<uint8_t> l_payload;

    // The index of the requested sink table entry.
    l_payload.push_back(i_index);

    // struct
    l_payload.insert(l_payload.end(), i_struct.begin(), i_struct.end());

    clogI << "EZSP_GP_SINK_TABLE_SET_ENTRY\n";
    dongle.sendCommand(EZSP_GP_SINK_TABLE_SET_ENTRY,l_payload);    
}


/**
 * Update the GP Proxy table based on a GP pairing.
 */
void CGpSink::gpProxyTableProcessGpPairing( std::vector<uint8_t> i_param )
{
    std::vector<uint8_t> l_payload;

    // \todo used seperate paramter or a dedicated struct class
    l_payload.insert(l_payload.end(), i_param.begin(), i_param.end());

    clogI << "EZSP_GP_PROXY_TABLE_PROCESS_GP_PAIRING\n";
    dongle.sendCommand(EZSP_GP_PROXY_TABLE_PROCESS_GP_PAIRING,l_payload);    
}


/**
 * Clear all GP tables
 */
/*
void CAppDemo::gpClearAllTables( void )
{
    // sink table
    dongle.sendCommand(EZSP_GP_SINK_TABLE_CLEAR_ALL); 
}
*/



/**
 * utility function can managed error state
 */
void CGpSink::setSinkState( ESinkState i_state )
{
    sink_state = i_state;

    const std::map<ESinkState,std::string> MyEnumStrings {
        { SINK_NOT_INIT, "SINK_NOT_INIT" },
        { SINK_READY, "SINK_READY" },
        { SINK_ERROR, "SINK_ERROR" },
        { SINK_COM_OPEN, "SINK_COM_OPEN" },
        { SINK_COM_IN_PROGRESS, "SINK_COM_IN_PROGRESS" },
    };

    auto  it  = MyEnumStrings.find(sink_state); /* FIXME: we issue a warning, but the variable app_state is now out of bounds */
    std::string error_str = it == MyEnumStrings.end() ? "OUT_OF_RANGE" : it->second;
    clogI << "SINK State change : " << error_str << std::endl;
}