/*
 *  ITC18StimDevice Plugin for MWorks
 *
 * 2010 JHRM
 */

#include "ITC18StimDevice.h"
#include "boost/bind.hpp"
#include <MWorksCore/Component.h>
#include <unistd.h>
#include <assert.h>

#define kDebugITC18StimDevice	1

#define kBufferLength		2048
#define kDIDeadtimeUS		5000	
#define kDIReportTimeUS		5000
#define kDriftTimeLimitMS	0.010
#define kDriftFractionLimit	0.001
#define kGatePorchMS		25
#define kGarbageLength		3					// Invalid entries at the start of sequence
#define kGateBit			0
#define	kITC18TicksPerMS	800L				// Time base for ITC18
#define kITC18TickTimeUS	1.25
#define	kMaxChannels		4
#define kPulseMarkerBit		0

#define	kITC18ReadPeriodUS		25000
#define	kReadTaskWarnSlopUS		100000
#define	kReadTaskFailSlopUS		200000

static short ADInstructions[] = {ITC18_INPUT_AD0, ITC18_INPUT_AD1, ITC18_INPUT_AD2,  ITC18_INPUT_AD3};
static short DAInstructions[] = {ITC18_OUTPUT_DA0, ITC18_OUTPUT_DA1, ITC18_OUTPUT_DA2,  ITC18_OUTPUT_DA3};

using namespace mw;

/* Notes to self MH 100422
 This is how we do setup and cleanup
    * Constructor [called at plugin load time]
        Set instance variables
    * core calls initialize()
        -> variableSetup()
    * startup()  [called by core; once, I think]
    * startDeviceIO()  [called by core; every trial]
    * stopDeviceIO()   [called by core; every trial]
    * shutdown() [called by core; once, I think]
    * Destructor
 
 What we do:
    Constructor [sets up instance variables]
    
*/

/********************************************************************************************************************
 External functions for scheduling.  Placed up here because I haven't figured out the correct declaration syntax.
 ********************************************************************************************************************/
/*
void *readLaunch(const shared_ptr<ITC18StimDevice> &pITC18StimDevice) {
	
	pITC18StimDevice->readData();
	return NULL;
}
*/
/********************************************************************************************************************
 Constructor and destructor functions
********************************************************************************************************************/

ITC18StimDevice::ITC18StimDevice(bool _noAlternativeDevice,
								 const boost::shared_ptr <Scheduler> &a_scheduler,
                                 const boost::shared_ptr <Variable> _preTrigger,
                                 const boost::shared_ptr <Variable> _train_duration_ms,
                                 const boost::shared_ptr <Variable> _current_pulses,
                                 const boost::shared_ptr <Variable> _biphasic_pulses, 
                                 const boost::shared_ptr <Variable> _pulse_amplitude,
                                 const boost::shared_ptr <Variable> _pulse_width_us,
                                 const boost::shared_ptr <Variable> _pulse_freq_hz,
                                 const boost::shared_ptr <Variable> _ua_per_v) {

	if (VERBOSE_IO_DEVICE >= 2) {
		mprintf("ITC18StimDevice: constructor");
	}
	noAlternativeDevice = _noAlternativeDevice;
	scheduler = a_scheduler;
	preTrigger = _preTrigger;
	trainDurationMS = _train_duration_ms;
	currentPulses = _current_pulses;
	biphasicPulses = _biphasic_pulses;
	pulseAmplitude = _pulse_amplitude;
	pulseWidthUS = _pulse_width_us;
	pulseFreqHz = _pulse_freq_hz;
	UAPerV = _ua_per_v;

	ITC18Running = false;
	itc = NULL;
}

// Copy constructor should never be called

ITC18StimDevice::ITC18StimDevice(const ITC18StimDevice& copy) {
	
    assert(0); 
}

// Destructor

ITC18StimDevice::~ITC18StimDevice(){ 
	
	if (VERBOSE_IO_DEVICE >= 2) {
		mprintf("ITC18StimDevice: destructor");
	}
	if (pulseScheduleNode != NULL) {
		boost::mutex::scoped_lock locker(pulseScheduleNodeLock); 
        pulseScheduleNode->cancel();
		pulseScheduleNode->kill();
    }
}

/********************************************************************************************************************
 Initialization functions
********************************************************************************************************************/

// Attempt to find the ITC18 and initialize it. If no ITC18 is present and there is no alternative device, we return 
// success and run the ITC18StimDevice without hardware attached.

bool ITC18StimDevice::initialize() {
	
	if (VERBOSE_IO_DEVICE >= 2) {
		mprintf("ITC18StimDevice: initialize");
	}
    this->variableSetup();
	openITC18();
	if (itc != NULL && VERBOSE_IO_DEVICE >= 0) {
        mprintf("ITC18StimDevice::initialize: found ITC18StimDevice");
    }
	if (itc == NULL && noAlternativeDevice) {
        mprintf("ITC18StimDevice::initialize: no ITC18 or alternative device, running without ITC18 hardware");
	}
	markParametersDirty();								// flag that instructions need to be made
	checkParameters();									// and make and load those instructions
	return ((itc != NULL) || noAlternativeDevice);
}

void ITC18StimDevice::variableSetup() {
	
//	set up to detect when pulse train parameters change

	weak_ptr<ITC18StimDevice> weak_self_ref(getSelfPtr<ITC18StimDevice>());
	shared_ptr<VariableNotification> notif(new ITC18StimDeviceVariableNotification(weak_self_ref));
	this->trainDurationMS->addNotification(notif);
	this->currentPulses->addNotification(notif);
	this->biphasicPulses->addNotification(notif);
	this->pulseAmplitude->addNotification(notif);
	this->pulseWidthUS->addNotification(notif);
	this->pulseFreqHz->addNotification(notif);
	this->UAPerV->addNotification(notif);
	
	// preTrigger
    
	weak_ptr<ITC18StimDevice> weak_self_ref3(getSelfPtr<ITC18StimDevice>());
	shared_ptr<VariableNotification> notif3(new ITC18StimDevicePreTriggerNotification(weak_self_ref3));
	this->preTrigger->addNotification(notif3); 
}

/********************************************************************************************************************
 Object functions
********************************************************************************************************************/

// Check whether stimulus parameters have changed and make new ITC18 instructions as needed

void ITC18StimDevice::checkParameters(void) {
	
	PulseTrainData train;
	
//	if (!parametersDirty) {
//		mprintf(" ITC18StimDevice:checkParameters -- nothing has changed");
//		return;
//	}
	mprintf(" ITC18StimDevice:checkParameters -- needs new instructions");
	train.currentPulses = currentPulses->getValue();				// true for current, false for voltage
	train.amplitude = pulseAmplitude->getValue();
	train.DAChannel = 0;
	train.doPulseMarkers = true;
	train.doGate = true;
	train.durationMS = trainDurationMS->getValue();
	train.frequencyHZ = pulseFreqHz->getValue();
	train.fullRangeV = POSITIVEVOLT;
	train.gateBit = 0;
	train.gatePorchMS = 25;
	train.pulseBiphasic = biphasicPulses->getValue();
	train.pulseMarkerBit = 1;
	train.pulseWidthUS = pulseWidthUS->getValue();
	train.UAPerV = UAPerV->getValue();

	makeInstructionsFromTrainData(&train, 1L);
	parametersDirty = false;
}

// Close the ITC18.  We do a round-about with the pointers to make sure that the
// pointer is nulled out before we close the ITC.  This is needed so that interrupt
// driven routines won't use the itc after ITC18_Close has been called.

void ITC18StimDevice::closeITC18(void) {

	void *pLocal; 

	stopDeviceIO();
	if (itc != NULL) {
		pLocal = itc;
		itc = NULL;
		boost::mutex::scoped_lock lock(ITC18DeviceLock); 
		ITC18_Close(pLocal);
	}
}

// Get the number of entries ready to be read from the FIFO.  We assume that the device has been locked before
// this method is called

int	ITC18StimDevice::getAvailable() {

	int available, overflow;
	
	ITC18_GetFIFOReadAvailableOverflow(itc, &available, &overflow);
	if (overflow != 0) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "ITC18StimDevice::getAvailable: Fatal FIFO overflow.");
		exit(0);
	}
	return available;
}

void ITC18StimDevice::markParametersDirty(void) {
	
	parametersDirty = true;
	mprintf("marking parameters dirty");
}

// Open and initialize the ITC18 -- success is indicated by a non-NULL value in itc.
//  (PCI):  ITC18_Open(itc, 0)
//  (USB):  ITC18_Open(itc, 0x10000)
//			ITC18_Open(itc, 0x10001) for second device

void ITC18StimDevice::openITC18(void) {
	
	void *pLocal;
	
	boost::mutex::scoped_lock lock(ITC18DeviceLock); 
	if (itc != NULL) {				// If open, close and re-open
		pLocal = itc;				// save pointer to reuse memory for ITC structure
		itc = NULL;
		ITC18_Close(pLocal);		// direct call to ITC driver
	}
	else {
		pLocal = new char[ITC18_GetStructureSize()];
	}
	// Now the ITC is closed, and we have a valid sized pointer  
	
	usingUSB = false;
	if (ITC18_Open(pLocal, 0) != noErr) {	// try with PCI first, then USB
		usingUSB = true;
		if (ITC18_Open(pLocal, 0x10000) != noErr) {     // try USB
			mwarning(M_IODEVICE_MESSAGE_DOMAIN, "ITC18StimDevice::openITC18: Failed to open ITC18 using PCI or USB"); 
			ITC18_Close(pLocal);
			if (kDebugITC18StimDevice) {
				FIFOSize = 0x1 << 20;								// set for debugging
			}
			return;
		}
	}
	if (ITC18_Initialize(pLocal, ITC18_STANDARD) != noErr) {
		merror(M_IODEVICE_MESSAGE_DOMAIN, "ITC18StimDevice::openITC18: Failed to initialize ITC18"); 
		ITC18_Close(pLocal);
		return;
	}
	ITC18_SetDigitalInputMode(pLocal, true, false);				// latch and do not invert
	ITC18_SetExternalTriggerMode(pLocal, false, false);			// no external trigger
	FIFOSize = ITC18_GetFIFOSize(pLocal);
	itc = pLocal;
}

// Collect AD values from the ITC18 as they become ready.  This method is schedule to occur periodically by 
// startDeviceIO.

// For now we have not included reading of AD samples.  This could be added in the future if MWorks is up to it.

/*

bool ITC18StimDevice::readData(void) {
	
	short index, *pSamples;
	long sets, set;
	int available;
	static bool garbageFlushed = false;
	
	if (itc == NULL || !ITC18Running) {
		return false;
	}
	
	// When a sequence is started, the first three entries in the FIFO are garbage.  They should be thrown out.  
	
	boost::mutex::scoped_lock lock(ITC18DeviceLock);
	available = getAvailable();
	if (!garbageFlushed) {
		if (available < kGarbageLength + 1) {
			return false;
		}
		ITC18_ReadFIFO(itc, kGarbageLength, samples);
		garbageFlushed = true;
		available = getAvailable();
	}
	
	// Wait for the stimulus to be over.
	
	if ((available = getAvailable()) < bufferLengthBytes) {
		return false;
	}
	
	// When all the samples are available, read them and unpack them
	// Initialize buffers for reading values;
	
	samples = (short *)malloc(sizeof(short) * bufferLengthBytes);
	for (index = 0; index < channels; index++) {
		channelSamples[index] = (short *)malloc(sizeof(short) * bufferLengthSets);
	}
	
	ITC18_ReadFIFO(itc, bufferLengthBytes, samples);							// read all available sets
	for (set = 0; set < sets; set++) {									// process each set
		pSamples = &samples[(channels + 1) * set];						// point to start of a set
		for (index = 0; index < channels; index++) {					// for every channel
			channelSamples[index][set] = *pSamples++;
		}
	}
	samplesReady = true;
	
	// ??? We need to figure out how to get these data available to MWorks.  For now, just throw them away.
	
	free(samples);
	for (index = 0; index < channels; index++) {
		free(channelSamples[index]);
	}
	return true;
}

 */

// Start the scheduled IO on the ITC18StimDevice.  This starts a thread that reads the input ports

bool ITC18StimDevice::startDeviceIO(void) {
	
	if (VERBOSE_IO_DEVICE >= 1) {
		mprintf("ITC18StimDevice: startDeviceIO");
	}
	if (ITC18Running) {
		merror(M_IODEVICE_MESSAGE_DOMAIN, 
               "ITC18StimDevice startDeviceIO: request was made without first stopping IO, aborting");
        return false;
	}
	
	if (itc == NULL) {
		return false;
	}
	boost::mutex::scoped_lock lock(ITC18DeviceLock); 
	ITC18Running = true;
	ITC18_Start(itc, false, true, false, false);				// Start ITC-18, no external trigger, output enabled
//	 shared_ptr<ITC18StimDevice> this_one = shared_from_this();
//	 pollScheduleNode = scheduler->scheduleUS(std::string(FILELINE ": ") + tag, (MWTime)0, kITC18ReadPeriodUS,
//						M_REPEAT_INDEFINITELY, boost::bind(readLaunch, this_one), M_DEFAULT_IODEVICE_PRIORITY,
//						kReadTaskWarnSlopUS, kReadTaskFailSlopUS, M_MISSED_EXECUTION_DROP);
//	schedule_nodes.push_back(pollScheduleNode);       
	return true;
}

bool ITC18StimDevice::stopDeviceIO() {
	
    // Stop the ITC18StimDevice collecting data.  This is typically called at the end of each trial.
    
	if (VERBOSE_IO_DEVICE >= 1) {
		mprintf("ITC18StimDevice: stopDeviceIO");
	}
	
	// stop all the scheduled DI checking (i.e. stop calls to "updateChannel")
	
	if (pollScheduleNode != NULL) {
		boost::mutex::scoped_lock(pollScheduleNodeLock);
        pollScheduleNode->cancel();
    }
	boost::mutex::scoped_lock lock(ITC18DeviceLock); 
	if (itc != NULL) {
		ITC18_Stop(itc);
	}
	ITC18Running = false;
	return true;
}

/* 
 Make the instruction sequence for the ITC18.
 
 activeChannels specifies how many of the ITC18 DACs will be used to output pulse trains. pTrain is an arrany of
 PulseTrainData structures that give the parameters for each channel.  However, channels are currently forced to the
 same values for: doGate, gateBit, gatePorchMS, doPulseMarkers, pulseMarkerBit, pulseBiphasic, durationMS, 
 pulseWidthUS, and frequencyHZ.  The only values that are independent by channel are: DACChanel, amplitude, 
 fullRangeV, currentPulses, and UAPerV.  The shared entries are taken from the first PulseTrainData struct in pTrain, 
 and ignored in subsequent structs.
 */
 
bool ITC18StimDevice::makeInstructionsFromTrainData(PulseTrainData *pTrain, long activeChannels) {

	short values[kMaxChannels + 1], gateAndPulseBits, gateBits, *sPtr, *tPtr;
	long index, sampleSetsInTrain, sampleSetsPerPhase, sampleSetIndex, sampleSetsPerPulse, ticksPerInstruction;
	long gatePorchUS, sampleSetsInPorch, porchBufferLength;
	long pulseCount, durationUS, instructionsPerSampleSet, valueIndex;
	int writeAvailable, result;
	float sampleSetPeriodUS, instructionPeriodUS, pulsePeriodUS, rangeFraction[kMaxChannels];
	short *trainValues, *pulseValues, *porchValues;
	int ITCInstructions[kMaxChannels + 1];
	
	if (itc == NULL && !kDebugITC18StimDevice) { 
		return false; 
	}
	samplesReady = false;										// flag no samples are ready
	
	// We take common values from the first entry, on the assumption that others have been checked and are the same
	
	channels = min(activeChannels, ITC18_NUMBEROFDACOUTPUTS);
	instructionsPerSampleSet = channels + 1;			// one per DAC, plus one for digital out
	gatePorchUS = (pTrain->doGate) ? pTrain->gatePorchMS * 1000.0 : 0;
	durationUS = pTrain->durationMS * 1000.0;
	
	// First determine the DASample period.  The instructions specify the entire stimulus train, plus the front and
	// back porches for the gate.  We require the entire stimulus instruction to fit within the ITC-18 FIFO.
	// Starting with the fastest tick rate, we divide down to allow for enough DA (channels) and Digital (1) 
	// samples, and a factor of safety (2x)

    
	ticksPerInstruction = ITC18_MINIMUM_TICKS;
	while ((durationUS + 2 * gatePorchUS) / (kITC18TickTimeUS * ticksPerInstruction) > 
																FIFOSize / (instructionsPerSampleSet * 2)) {
		ticksPerInstruction++;
	}
	if (ticksPerInstruction > ITC18_MAXIMUM_TICKS) {
		return false;
	}

	// Precompute values.  Every portion of the stimulus has an integer number of sample sets.
	
	instructionPeriodUS = ticksPerInstruction * kITC18TickTimeUS;
	sampleSetPeriodUS = instructionPeriodUS * instructionsPerSampleSet;
	sampleSetsPerPhase = round(pTrain->pulseWidthUS / sampleSetPeriodUS);
	sampleSetsPerPulse = sampleSetsPerPhase * ((pTrain->pulseBiphasic) ? 2 : 1);
	sampleSetsInPorch = gatePorchUS / sampleSetPeriodUS;		// DA samples in each gate porch
	sampleSetsInTrain = durationUS / sampleSetPeriodUS;		// DA samples in train
	bufferLengthSets = sampleSetsInTrain + 2 * sampleSetsInPorch;
	pulsePeriodUS = ((pTrain->frequencyHZ > 0) ? 1.0 / pTrain->frequencyHZ * 1000000.0 : 0);
	gateBits = ((pTrain->doGate) ? (0x1 << pTrain->gateBit) : 0);
	gateAndPulseBits = gateBits | ((pTrain->doPulseMarkers) ? (0x1 << pTrain->pulseMarkerBit) : 0);
		
	// Create and load an array with instructions that make up one pulse (DA and digital)
	
	if (sampleSetsPerPulse > 0) {
		for (index = 0; index < channels; index++) {
			rangeFraction[index] = (pTrain[index].amplitude / pTrain[index].fullRangeV) /
			((pTrain[index].currentPulses) ? pTrain[index].UAPerV : 1000);
		}
		assert(pulseValues = (short *)malloc(sampleSetsPerPulse * instructionsPerSampleSet * sizeof(short)));
		for (index = 0; index < channels; index++) {			// create first phase instruction set
			values[index] = rangeFraction[index] * 0x7fff;		//	force fractions positive for first phase
		}
		values[index] = gateAndPulseBits;						//	digital output word
		for (sampleSetIndex = 0; sampleSetIndex < sampleSetsPerPhase; sampleSetIndex++) {	// load first phase
			replaceShortsInRange(pulseValues, values, sampleSetIndex * instructionsPerSampleSet, 
																	instructionsPerSampleSet);
		}
		if (pTrain->pulseBiphasic) {							// do second phase for biphasic pulses
			for (index = 0; index < channels; index++) {
				values[index] = -rangeFraction[index] * 0x7fff;		// invert amplitude
			}
			values[index] = gateAndPulseBits;						// digital output word
			for (sampleSetIndex = 0; sampleSetIndex < sampleSetsPerPhase; sampleSetIndex++) {
				replaceShortsInRange(pulseValues, values, 
									 (sampleSetsPerPhase + sampleSetIndex) * instructionsPerSampleSet, 
									 instructionsPerSampleSet);
			}
		}
	}
	mprintf("instructionsPerSet %d, setsPerPulse %d", instructionsPerSampleSet, sampleSetsPerPulse);
	for (index = 49000; index < (sampleSetsPerPulse * instructionsPerSampleSet) - 8; index += 8) {
		mprintf("%4hx %4hx %4hx %4hx %4hx %4hx %4hx %4hx", 
				pulseValues[index + 0], pulseValues[index + 1], pulseValues[index + 2], pulseValues[index + 3], 
				pulseValues[index + 4], pulseValues[index + 5], pulseValues[index + 6], pulseValues[index + 7]);
	}
	for ( ; index < sampleSetsPerPulse * instructionsPerSampleSet; index++) {
		mprintf("%4hx", pulseValues[index]);
	}
	
	// Create an array with the entire output sequence.  If there is a gating signal,
	// we add that to the digital output values.  bufferLengthBytes is always at least as long as instructionsPerSampleSet.
	
	bufferLengthBytes = max(sampleSetsInTrain * instructionsPerSampleSet, instructionsPerSampleSet);
	assert(trainValues = (short *)calloc(bufferLengthBytes, sizeof(short)));
	if (gateBits > 0) {									// load digital output commands for the gate (if any)
		for (sPtr = trainValues, index = 0; index < sampleSetsInTrain; index++) {
			sPtr += channels;							// skip over analog values
			*(sPtr)++ = gateBits;						// set the gate bits
		}
	}
	
	// Add the pulses to the train instructions.  If the stimulation frequency is zero, or the train duration
	// is less than one pulse, or the pulse width is zero, do nothing.

	if ((pulsePeriodUS > 0) && (sampleSetsPerPhase > 0)) {
		for (pulseCount = 0; ; pulseCount++) {
			sampleSetIndex = pulseCount * pulsePeriodUS / sampleSetPeriodUS;	// find offset in instructions
			valueIndex = sampleSetIndex * instructionsPerSampleSet;
			if ((valueIndex + sampleSetsPerPulse * ((pTrain->pulseBiphasic) ? 2 : 1) + 1) >= bufferLengthBytes) {
				break;										// no room for another pulse
			}
			replaceShortsInRange(trainValues, pulseValues, valueIndex,  sampleSetsPerPulse * instructionsPerSampleSet);
		}
	}
	
	free(pulseValues);
	
	// If there the gate has a front and back porch, add the porches to the instructions.  Make a buffer that is big
	// enough for the stimulus train and the front and back porches, make the front porch, then copy the stimulus 
	// train, then copy the front porch to the back porch.
	
	if (sampleSetsInPorch > 0) {
		porchBufferLength = sampleSetsInPorch * instructionsPerSampleSet;
		assert(porchValues = (short *)malloc((2 * porchBufferLength + bufferLengthBytes) * sizeof(short)));
		sPtr = porchValues;
		for (index = 0; index < sampleSetsInPorch; index++) {
			sPtr += channels;							// skip over analog values
			*(sPtr)++ = gateBits;						// set the gate bits
		}
		for (tPtr = trainValues, index = 0; index < bufferLengthBytes; index++) {
			*sPtr++ = *tPtr++;
		}
		for (tPtr = porchValues, index = 0; index < porchBufferLength; index++) {
			*sPtr++ = *tPtr++;
		}
		free(trainValues);								// release unneeded data
		trainValues = porchValues;						// make trainValues point to the whole set
		bufferLengthBytes += 2 * porchBufferLength;		// tally the buffer length with both porches
	}
	
	// Change the last digital output word in the back gate porch to close gate (in case it's open)
	
	trainValues[bufferLengthBytes - 1] = 0x00;
	
	// Set up the ITC for the stimulus train.  Do everything except the start
	
	for (index = 0; index < channels; index++) {
		ITCInstructions[index] = DAInstructions[pTrain[index].DAChannel] | ITC18_OUTPUT_UPDATE;
//		ADInstructions[pTrain[index].DAChannel] | DAInstructions[pTrain[index].DAChannel] | 
//		ITC18_INPUT_UPDATE | ITC18_OUTPUT_UPDATE;
	} 
	ITCInstructions[index] = ITC18_OUTPUT_DIGITAL1 | ITC18_INPUT_SKIP | ITC18_OUTPUT_UPDATE;
	if (itc != NULL) {									// don't access ITC if we're debugging
		boost::mutex::scoped_lock lock(ITC18DeviceLock);
		ITC18_SetSequence(itc, channels + 1, ITCInstructions); 
		ITC18_StopAndInitialize(itc, true, true);
		ITC18_GetFIFOWriteAvailable(itc, &writeAvailable);
		if (writeAvailable < sampleSetsInTrain) {
			merror(M_IODEVICE_MESSAGE_DOMAIN, "LLITC18PulseTrainDevice: ITC18 write buffer was full.");
			free(trainValues);
			return false;
		}
		result = ITC18_WriteFIFO(itc, bufferLengthBytes, trainValues);
		if (result != noErr) { 
			mprintf("Error ITC18_WriteFIFO, result: %d", result);
			free(trainValues);
		   return false;
		}
		ITC18_SetSamplingInterval(itc, ticksPerInstruction, false);
	}	
/*	
	for (index = 49000; index < bufferLengthBytes - 8; index += 8) {
		mprintf("%4hx %4hx %4hx %4hx %4hx %4hx %4hx %4hx", 
				trainValues[index + 0], trainValues[index + 1], trainValues[index + 2], trainValues[index + 3], 
				trainValues[index + 4], trainValues[index + 5], trainValues[index + 6], trainValues[index + 7]);
	}
	for ( ; index < bufferLengthBytes; index++) {
		mprintf("%4hx %4hx %4hx %4hx %4hx %4hx %4hx %4hx", trainValues[index]);
	}
*/
	free(trainValues);
	return true;
}

void ITC18StimDevice::replaceShortsInRange(short *buffer, short *replacement, long offset, long numShorts) {
	
	long index;
	
	buffer += offset;
	for (index = 0; index < numShorts; index++) {
		*buffer++ = *replacement++;
	}
}

bool ITC18StimDevice::startup() {
	if (VERBOSE_IO_DEVICE >= 2) {
		mprintf("ITC18StimDevice: startup");
	}
	return true;
}

bool ITC18StimDevice::shutdown(){
	if (VERBOSE_IO_DEVICE >= 2) {
		mprintf("ITC18StimDevice: shutdown");
	}
	return true;
}

