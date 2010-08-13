/*
 *  ITC18StimDeviceFactory.cpp
 *  ITC18StimPlugin
 *
 *  Created by maunsell on 7/31/10.
 *  Copyright 2010 Harvard Medical School. All rights reserved.
 *
 */

#include "ITC18StimDeviceFactory.h"
#include "ITC18StimDevice.h"

//using namespace mw;

boost::shared_ptr<mw::Component> ITC18StimDeviceFactory::createObject(std::map<std::string, std::string> parameters,
																	  mw::ComponentRegistry *reg) {
	
	bool noAlternativeDevice;
	const char *attributeList[] = {"prime", "running", "train_duration_ms", "current_pulses", "biphasic_pulses",
		"pulse_amplitude", "pulse_width_us", "pulse_freq_hz", "ua_per_v"};
	mw::GenericDataType typeList[] = {M_BOOLEAN, M_BOOLEAN, M_INTEGER, M_BOOLEAN, M_BOOLEAN, M_INTEGER, 
		M_INTEGER, M_INTEGER, M_INTEGER};
	boost::shared_ptr<mw::Variable> variableList[sizeof(attributeList)/sizeof(const char *)];
	
	for (long index = 0; index < sizeof(attributeList) / sizeof(const char *); index++) {
		REQUIRE_ATTRIBUTES(parameters, attributeList[index]);
		variableList[index] = boost::shared_ptr<mw::Variable>(new mw::ConstantVariable(Datum(typeList[index], 0)));	
		if (parameters.find(attributeList[index]) != parameters.end()) {
			variableList[index] = reg->getVariable(parameters.find(attributeList[index])->second);	
			checkAttribute(variableList[index], parameters.find("reference_id")->second, 
						   attributeList[index], parameters.find(attributeList[index])->second);
		}
	}
	boost::shared_ptr <mw::Scheduler> scheduler = mw::Scheduler::instance(true);
	noAlternativeDevice = (parameters.find("alt") == parameters.end());
	
	boost::shared_ptr <mw::Component> new_daq = boost::shared_ptr<mw::Component>(new ITC18StimDevice(
				 noAlternativeDevice, scheduler, variableList[0], variableList[1], variableList[2], variableList[3], 
				 variableList[4], variableList[5], variableList[6], variableList[7], variableList[8]));
	return new_daq;
}	
