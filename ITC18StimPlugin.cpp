/*
 *  ITC18 Stim Plugin for MWorks
 *
 *  2010 JHRM
 *
 */

#include "ITC18StimPlugin.h"
#include "ITC18StimDeviceFactory.h"

using namespace mw;

Plugin *getPlugin(){
    return new ITC18StimPlugin();
}

void ITC18StimPlugin::registerComponents(shared_ptr<ComponentRegistry> registry) {
	
	registry->registerFactory(std::string("iodevice/itc18stim"), (ComponentFactory *)(new ITC18StimDeviceFactory()));
}
