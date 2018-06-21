/* Shared Use License: This file is owned by Derivative Inc. (Derivative) and
 * can only be used, and/or modified for use, in conjunction with 
 * Derivative's TouchDesigner software, and only if you are a licensee who has
 * accepted Derivative's TouchDesigner license or assignment agreement (which
 * also govern the use of this file).  You may share a modified version of this
 * file with another authorized licensee of Derivative's TouchDesigner software.
 * Otherwise, no redistribution or sharing of this file, with or without
 * modification, is permitted.
 */

#include "TOPtoPSN.hpp"

#include <stdio.h>
#include <string.h>
#include <cmath>
#include <assert.h>

#include <iostream>
#include <sstream>
#include <string>

static const char LocationsParName[] = "Winchloctop";
static const char InterfaceParName[] = "Intaddr";
static const char UDPPortName[] = "Udpport";
static const char UDPDestAddrName[] = "Destaddr";


using namespace boost::asio;

// These functions are basic C function, which the DLL loader can find
// much easier than finding a C++ Class.
// The DLLEXPORT prefix is needed so the compile exports these functions from the .dll
// you are creating
extern "C"
{

DLLEXPORT
int32_t
GetCHOPAPIVersion(void)
{
	// Always return CHOP_CPLUSPLUS_API_VERSION in this function.
	return CHOP_CPLUSPLUS_API_VERSION;
}

DLLEXPORT
CHOP_CPlusPlusBase*
CreateCHOPInstance(const OP_NodeInfo* info)
{
	// Return a new instance of your class every time this is called.
	// It will be called once per CHOP that is using the .dll
	return new TOPtoPSN(info);
}

DLLEXPORT
void
DestroyCHOPInstance(CHOP_CPlusPlusBase* instance)
{
	// Delete the instance here, this will be called when
	// Touch is shutting down, when the CHOP using that instance is deleted, or
	// if the CHOP loads a different DLL
	delete (TOPtoPSN*)instance;
}

};


TOPtoPSN::TOPtoPSN(const OP_NodeInfo* info):
myNodeInfo(info),
io_context_(),
endpoint_(boost::asio::ip::make_address(PSN_DEFAULT_UDP_MULTICAST_ADDR), PSN_DEFAULT_UDP_PORT),
socket_(io_context_, endpoint_.protocol()),
psn_encoder_("Douglas"),
trackers_(),
messages_(0),
sentPackets_(0),
trackersSize_(0),
lastError_()
{
	myExecuteCount = 0;
}

TOPtoPSN::~TOPtoPSN()
{
}

void
TOPtoPSN::getGeneralInfo(CHOP_GeneralInfo* ginfo)
{
	// This will cause the node to cook every frame
	ginfo->cookEveryFrameIfAsked = false;
	ginfo->timeslice = false;
	ginfo->inputMatchIndex = 0;
}

bool
TOPtoPSN::getOutputInfo(CHOP_OutputInfo* info)
{
	return false;
	
	/*
	// If there is an input connected, we are going to match it's channel names etc
	// otherwise we'll specify our own.
	if (info->opInputs->getNumInputs() > 0)
	{
		return false;
	}
	else
	{
		info->numChannels = 1;

		// Since we are outputting a timeslice, the system will dictate
		// the numSamples and startIndex of the CHOP data
		//info->numSamples = 1;
		//info->startIndex = 0

		// For illustration we are going to output 120hz data
		info->sampleRate = 120;
		return true;
	}
	 */
}

const char*
TOPtoPSN::getChannelName(int32_t index, void* reserved)
{
	return "chan1";
}

void
TOPtoPSN::execute(const CHOP_Output* output,
							  OP_Inputs* inputs,
							  void* reserved)
{
	myExecuteCount++;
	
	const std::string interfaceAddress = inputs->getParString(InterfaceParName);
	if(interfaceAddress != prevInterfaceAddr_) {
		// New interface address
		// Set socket option
		try
		{
			const ip::address ifaddr = boost::asio::ip::make_address(interfaceAddress);
			socket_.set_option(ip::multicast::outbound_interface(ifaddr.to_v4()));
		}
		catch(const std::exception &e)
		{
			std::ostringstream message;
			message << "Failed to set new outbound interface address to " << interfaceAddress << ": " << e.what();
			lastError_ = message.str();
			std::cerr << message.str() << std::endl;
		}
		
		prevInterfaceAddr_ = interfaceAddress;
	}
	
	const auto locationsTOP = inputs->getParTOP(LocationsParName);
	
	OP_TOPInputDownloadOptions topOptions;
	topOptions.downloadType = OP_TOPInputDownloadType::Instant;
	topOptions.cpuMemPixelType = OP_CPUMemPixelType::RGBA32Float;
	topOptions.verticalFlip = false;
	
	const float *pixels = reinterpret_cast<const float*>(inputs->getTOPDataInCPUMemory(locationsTOP, &topOptions));
	
	if(pixels == nullptr)
	{
		// Nothing left to do this frame
		lastError_ = "No pixels";
		return;
	}
	
	try
	{
		const std::string destAddr = inputs->getParString(UDPDestAddrName);
		boost::asio::ip::udp::endpoint endpoint(boost::asio::ip::make_address(destAddr), inputs->getParInt(UDPPortName));
		
		updateTrackers(inputs, locationsTOP, pixels);
		
		if(myExecuteCount % 50 == 0) {
			sendInfo(inputs, endpoint);
		}
		
		sendTrackers(inputs, endpoint);
	}
	catch (const std::exception &e)
	{
		std::ostringstream message;
		message << "Error: " << e.what();
		lastError_ = message.str();
		std::cerr << message.str() << std::endl;
	}
}

void TOPtoPSN::updateTrackers(OP_Inputs *inputs, const OP_TOPInput *top, const float *pixels)
{
	const int numberOfTrackers = top->height;
	
	for ( int i = 0; i < numberOfTrackers; i++)
	{
		std::stringstream name;
		name << "Kinetic " << (i+1);
		
		auto &tracker = trackers_[i];
		
		tracker.id_ = i;
		tracker.name_ = name.str();
		
		const int startPixel = i*4;
		tracker.pos_.x = 0; //pixels[startPixel+0];
		tracker.pos_.y = pixels[startPixel+1];
		tracker.pos_.z = 0; //pixels[startPixel+2];
	}
	
	psn_encoder_.set_trackers(trackers_);
	trackersSize_ = trackers_.size();
}

void TOPtoPSN::sendTrackers(OP_Inputs *inputs, const boost::asio::ip::udp::endpoint &endpoint)
{
	::std::list< ::std::string > data_packets ;
	
	const auto timestamp = messages_;
	
	if ( psn_encoder_.encode_data( data_packets , timestamp ) )
	{
//		::std::cout << "--------------------PSN SERVER-----------------" << ::std::endl ;
//		::std::cout << "Sending PSN_DATA_PACKET : "
//		<< "Frame Id = " << (int)psn_encoder_.get_last_encode_data_packet_frame_id()
//		<< " , Packet Count = " << data_packets.size() << ::std::endl ;
		
		::std::list< ::std::string >::iterator it ;
		for ( it = data_packets.begin() ; it != data_packets.end() ; ++it )
		{
			// Uncomment these lines if you want to simulate a packet drop now and then
			//static uint64_t packet_drop = 0 ;
			//if ( packet_drop++ % 100 != 0 )
			socket_.send_to(boost::asio::buffer(*it), endpoint_);
			//socket_server.send_message( PSN_DEFAULT_UDP_MULTICAST_ADDR , PSN_DEFAULT_UDP_PORT , *it ) ;
			sentPackets_++;
		}
		
//		::std::cout << "-----------------------------------------------" << ::std::endl ;
	}
}

void TOPtoPSN::sendInfo(OP_Inputs *inputs, const boost::asio::ip::udp::endpoint &endpoint)
{
	::std::list< ::std::string > info_packets ;
	
	const auto timestamp = messages_;
	
	if ( psn_encoder_.encode_info( info_packets , timestamp ) )
	{
//		::std::cout << "--------------------PSN SERVER-----------------" << ::std::endl ;
//		::std::cout << "Sending PSN_INFO_PACKET : "
//		<< "Frame Id = " << (int)psn_encoder_.get_last_encode_info_packet_frame_id()
//		<< " , Packet Count = " << info_packets.size() << ::std::endl ;
		
		::std::list< ::std::string >::iterator it ;
		for ( it = info_packets.begin() ; it != info_packets.end() ; ++it )
		{
			//socket_server.send_message( PSN_DEFAULT_UDP_MULTICAST_ADDR , PSN_DEFAULT_UDP_PORT , *it ) ;
			socket_.send_to(boost::asio::buffer(*it), endpoint_);
			sentPackets_++;
		}
		
//		::std::cout << "-----------------------------------------------" << ::std::endl ;
	}
}

int32_t
TOPtoPSN::getNumInfoCHOPChans()
{
	// We return the number of channel we want to output to any Info CHOP
	// connected to the CHOP. In this example we are just going to send one channel.
	return 1;
}

void
TOPtoPSN::getInfoCHOPChan(int32_t index,
										OP_InfoCHOPChan* chan)
{
	// This function will be called once for each channel we said we'd want to return
	// In this example it'll only be called once.

	if (index == 0)
	{
		chan->name = "executeCount";
		chan->value = (float)myExecuteCount;
	}
}

bool		
TOPtoPSN::getInfoDATSize(OP_InfoDATSize* infoSize)
{
	infoSize->rows = 5;
	infoSize->cols = 2;
	// Setting this to false means we'll be assigning values to the table
	// one row at a time. True means we'll do it one column at a time.
	infoSize->byColumn = false;
	return true;
}

void
TOPtoPSN::getInfoDATEntries(int32_t index,
										int32_t nEntries,
										OP_InfoDATEntries* entries)
{
	// It's safe to use static buffers here because Touch will make it's own
	// copies of the strings immediately after this call returns
	// (so the buffers can be reuse for each column/row)
	static char tempBuffer1[4096];
	static char tempBuffer2[4096];

	if (index == 0)
	{
		// Set the value for the first column
#ifdef WIN32
		strcpy_s(tempBuffer1, "executeCount");
#else // macOS
        strlcpy(tempBuffer1, "executeCount", sizeof(tempBuffer1));
#endif
		entries->values[0] = tempBuffer1;

		// Set the value for the second column
#ifdef WIN32
		sprintf_s(tempBuffer2, "%d", myExecuteCount);
#else // macOS
        snprintf(tempBuffer2, sizeof(tempBuffer2), "%d", myExecuteCount);
#endif
		entries->values[1] = tempBuffer2;
	}
	else if(index == 1)
	{
		// Set the value for the first column
#ifdef WIN32
		strcpy_s(tempBuffer1, "sentPackets");
#else // macOS
		strlcpy(tempBuffer1, "sentPackets", sizeof(tempBuffer1));
#endif
		entries->values[0] = tempBuffer1;
		
		// Set the value for the second column
#ifdef WIN32
		sprintf_s(tempBuffer2, "%d", sentPackets_);
#else // macOS
		snprintf(tempBuffer2, sizeof(tempBuffer2), "%d", sentPackets_);
#endif
		entries->values[1] = tempBuffer2;
	}
	else if(index == 2)
	{
		// Set the value for the first column
#ifdef WIN32
		strcpy_s(tempBuffer1, "interfaceAddress");
#else // macOS
		strlcpy(tempBuffer1, "interfaceAddress", sizeof(tempBuffer1));
#endif
		entries->values[0] = tempBuffer1;
		
		// Set the value for the second column
#ifdef WIN32
		sprintf_s(tempBuffer2, "%s", prevInterfaceAddr_.c_str());
#else // macOS
		snprintf(tempBuffer2, sizeof(tempBuffer2), "%s", prevInterfaceAddr_.c_str());
#endif
		entries->values[1] = tempBuffer2;
	}
	else if(index == 3)
	{
		// Set the value for the first column
#ifdef WIN32
		strcpy_s(tempBuffer1, "numberOfTrackers");
#else // macOS
		strlcpy(tempBuffer1, "numberOfTrackers", sizeof(tempBuffer1));
#endif
		entries->values[0] = tempBuffer1;
		
		// Set the value for the second column
#ifdef WIN32
		sprintf_s(tempBuffer2, "%d", trackersSize_);
#else // macOS
		snprintf(tempBuffer2, sizeof(tempBuffer2), "%d", trackersSize_);
#endif
		entries->values[1] = tempBuffer2;
	}
	else if(index == 4)
	{
		// Set the value for the first column
#ifdef WIN32
		strcpy_s(tempBuffer1, "lastError");
#else // macOS
		strlcpy(tempBuffer1, "lastError", sizeof(tempBuffer1));
#endif
		entries->values[0] = tempBuffer1;
		
		// Set the value for the second column
#ifdef WIN32
		sprintf_s(tempBuffer2, "%s", lastError_.c_str());
#else // macOS
		snprintf(tempBuffer2, sizeof(tempBuffer2), "%s", lastError_.c_str());
#endif
		entries->values[1] = tempBuffer2;

	}
}

void
TOPtoPSN::setupParameters(OP_ParameterManager* manager)
{
	// TOP
	{
		OP_StringParameter sp;
		
		sp.name = LocationsParName;
		sp.label = "Winch Locations TOP";
		
		sp.defaultValue = "Sine";
		
		OP_ParAppendResult res = manager->appendTOP(sp);
		assert(res == OP_ParAppendResult::Success);
	}
	
	// Interface Address
	{
		OP_StringParameter sp;
		sp.name = InterfaceParName;
		sp.label = "Network Interface Address";
		sp.defaultValue = "192.168.0.91";
		
		OP_ParAppendResult res = manager->appendString(sp);
		assert(res == OP_ParAppendResult::Success);
	}
	
	// Destination Multicast Address
	{
		OP_StringParameter sp;
		sp.name = UDPDestAddrName;
		sp.label = "Destination Address";
		sp.defaultValue = PSN_DEFAULT_UDP_MULTICAST_ADDR;
		
		OP_ParAppendResult res = manager->appendString(sp);
		assert(res == OP_ParAppendResult::Success);
	}
	
	// Destination UDP Port
	{
		OP_NumericParameter sp;
		sp.name = UDPPortName;
		sp.label = "Destination Port";
		sp.defaultValues[0] = PSN_DEFAULT_UDP_PORT;
		sp.minValues[0] = 1;
		sp.maxValues[0] = 65535;
		
		OP_ParAppendResult res = manager->appendInt(sp);
		assert(res == OP_ParAppendResult::Success);
	}
}

void 
TOPtoPSN::pulsePressed(const char* name)
{
	if (!strcmp(name, "Reset"))
	{
		//myOffset = 0.0;
	}
}

