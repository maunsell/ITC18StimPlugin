/*
 *  ITC18 Stim Plugin for MWorks
 *
 *  Created by Mark Histed on 4/21/2010
 *    (based on Nidaq plugin code by Jon Hendry and John Maunsell)
 *
 */

#include "MWorksCore/GenericData.h"
#include "MWorksCore/Utilities.h"
#include "MWorksCore/Plugin.h"
#include "MWorksCore/IODevice.h"
#include "ITC/ITC18.h"						// Instrutech header
#include <ITC/Itcmm.h>

#undef VERBOSE_IO_DEVICE
#define VERBOSE_IO_DEVICE 0					// verbosity level is 0-2, 2 is maximum

#define noErr       0

typedef struct {
	bool	currentPulses;					// true for current, false for voltage
	float   amplitude;						// amplitude in uA or V.
	long	DAChannel;
	bool	doPulseMarkers;
	bool	doGate;
	long	durationMS;
	float   frequencyHZ;
	float   fullRangeV;
	long	gateBit;
	long	gatePorchMS;				// time that gates leads and trails stimulus
	bool	pulseBiphasic;
	long	pulseMarkerBit;
	long	pulseWidthUS;
	float   UAPerV;
} PulseTrainData;

using namespace std;

namespace mw {

class ITC18StimDevice : public IODevice {

protected:  	
	boost::mutex					active_mutex;
	boost::shared_ptr <Variable>	biphasicPulses;
	long							bufferLengthSamples;		// number of stimulus instructions/samples
	long							bufferLengthSets;			// number of stimulus sample sets
	long							channels;					// number of active channels
	short							*channelSamples[ITC18_NUMBEROFDACOUTPUTS];
	boost::shared_ptr <Variable>	currentPulses;
	MWTime							highTimeUS;					// Used to compute length of scheduled high/low pulses
	long							FIFOSize;
	void							*itc;
	boost::mutex					ITC18DeviceLock;
	bool							ITC18JustStarted;
	bool							ITC18Running;
	bool							noAlternativeDevice;
	bool							parametersDirty;
	shared_ptr<ScheduleTask>		pollScheduleNode;
	boost::mutex					pollScheduleNodeLock;
	bool							primed;
	boost::shared_ptr <Variable>	pulseAmplitude;
	boost::shared_ptr <Variable>	pulseDurationMS;
	shared_ptr<ScheduleTask>		pulseScheduleNode;
	boost::mutex					pulseScheduleNodeLock;				
	boost::shared_ptr <Variable>	pulseWidthUS;
	boost::shared_ptr <Variable>	pulseFreqHz;
	short							*samples; 
	bool							samplesReady;
	boost::shared_ptr <Scheduler>	scheduler;
	boost::shared_ptr <Variable>	trainDurationMS;
	boost::shared_ptr <Variable>	UAPerV;
	bool							usingUSB;
	
	// raw hardware functions
	
	void openITC18(void);
	void closeITC18();
	int	getAvailable();
	bool loadInstructionsFromTrainData(PulseTrainData *, long);
	void replaceShortsInRange(short *buffer, short *replacement, long offset, long numShorts);
    
public:
	
	boost::shared_ptr <Variable>	prime;
	boost::shared_ptr <Variable>	run;
	boost::shared_ptr <Variable>	running;

	ITC18StimDevice(bool noAlternativeDevice,
					const boost::shared_ptr <Scheduler> &a_scheduler,
					const boost::shared_ptr <Variable> _prime,
					const boost::shared_ptr <Variable> _run,
					const boost::shared_ptr <Variable> _running,
					const boost::shared_ptr <Variable> _train_duration_ms,
					const boost::shared_ptr <Variable> _current_pulses,
					const boost::shared_ptr <Variable> _biphasic_pulses, 
					const boost::shared_ptr <Variable> _pulse_amplitude,
					const boost::shared_ptr <Variable> _pulse_width_us,
					const boost::shared_ptr <Variable> _pulse_freq_hz,
					const boost::shared_ptr <Variable> _ua_per_v);
	ITC18StimDevice(const ITC18StimDevice& copy);
	~ITC18StimDevice();
	
	virtual bool startup();
	virtual bool shutdown();	
	virtual bool initialize();
	virtual bool startDeviceIO();
	virtual bool startStimulus();
	virtual bool stopDeviceIO();		
	virtual bool stopStimulus();		
	
	void changeRunState(void);
	void loadInstructions(void);
	bool readData(void);
	void markParametersDirty(void);
	void variableSetup();

	shared_ptr<ITC18StimDevice> shared_from_this() { 
		return static_pointer_cast<ITC18StimDevice>(IODevice::shared_from_this());
	}

};

class ITC18StimDevicePrimeNotification : public VariableNotification {
	
protected:
	weak_ptr<ITC18StimDevice> daq;
public:
	ITC18StimDevicePrimeNotification(weak_ptr<ITC18StimDevice> _daq){
		daq = _daq;
	}
	virtual void notify(const Datum &data, MWTime timeUS){
		shared_ptr<ITC18StimDevice> shared_daq(daq);
		shared_daq->loadInstructions();
	}
};

class ITC18StimDeviceRunNotification : public VariableNotification {
	
protected:
	weak_ptr<ITC18StimDevice> daq;
public:
	ITC18StimDeviceRunNotification(weak_ptr<ITC18StimDevice> _daq){
		daq = _daq;
	}
	virtual void notify(const Datum &data, MWTime timeUS){
		shared_ptr<ITC18StimDevice> shared_daq(daq);
		shared_daq->changeRunState();
	}
};

class ITC18StimDeviceVariableNotification : public VariableNotification {
	
protected:
	weak_ptr<ITC18StimDevice> daq;
public:
	ITC18StimDeviceVariableNotification(weak_ptr<ITC18StimDevice> _daq) {
		daq = _daq;
	}
	virtual void notify(const Datum& data, MWTime timeUS) {
		shared_ptr<ITC18StimDevice> shared_daq(daq);
		shared_daq->markParametersDirty();
	}
};
	
} // namespace mw






