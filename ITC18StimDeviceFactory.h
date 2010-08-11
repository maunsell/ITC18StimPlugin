/*
 *  ITC18StimDeviceFactory.h
 *  ITC18StimPlugin
 *
 *  Created by maunsell on 7/31/10.
 *  Copyright 2010 Harvard Medical School. All rights reserved.
 *
 */

#include "MWorksCore/ComponentFactory.h"

using namespace mw;

class ITC18StimDeviceFactory : public ComponentFactory {
	
	virtual shared_ptr<mw::Component> createObject(std::map<std::string, std::string> parameters, 
															mw::ComponentRegistry *reg);
};

