// UniversalFunctionBlocksModule: the Universal renderer function block
// (UniversalRenderer), and three more that run on any device,
// such as a phone: Level (UniversalLevel) and Spectrum (UniversalSpectrum),
// which derive signals, and Dashboard (UniversalDashboard), a web page of
// live panels.
//
// The reference module's renderer (RefFBModuleRenderer) draws into a window
// of its own, which Android cannot open and macOS opens only from the main
// thread. This one lays out the same plot, with the same properties, and
// sends it as plot data on an output signal, which openDAQ Studio draws on a
// computer or a phone, locally or over the network.

#include "dashboard.h"
#include "derived.h"
#include "engineering.h"
#include "renderer.h"

#include <opendaq/module_exports.h>
#include <opendaq/module_factory.h>
#include <opendaq/module_impl.h>

namespace daq::modules::universal_renderer
{

class UniversalFunctionBlocksModule final : public Module
{
public:
    explicit UniversalFunctionBlocksModule(const ContextPtr& context)
        : Module("UniversalFunctionBlocksModule", VersionInfo(1, 0, 0), context, "UniversalFunctionBlocksModule")
    {
    }

    DictPtr<IString, IFunctionBlockType> onGetAvailableFunctionBlockTypes() override
    {
        auto types = Dict<IString, IFunctionBlockType>();
        for (const auto& type : {UniversalRendererFb::CreateType(moduleInfo),
                                 LevelFb::CreateType(moduleInfo),
                                 SpectrumFb::CreateType(moduleInfo),
                                 DashboardFb::CreateType(moduleInfo),
                                 VibrationFb::CreateType(moduleInfo),
                                 SoundLevelMeterFb::CreateType(moduleInfo)})
            types.set(type.getId(), type);
        return types;
    }

    FunctionBlockPtr onCreateFunctionBlock(const StringPtr& id,
                                           const ComponentPtr& parent,
                                           const StringPtr& localId,
                                           const PropertyObjectPtr& /*config*/) override
    {
        if (id == "UniversalRenderer")
            return createWithImplementation<IFunctionBlock, UniversalRendererFb>(moduleInfo, context, parent, localId);
        if (id == "UniversalLevel")
            return createWithImplementation<IFunctionBlock, LevelFb>(moduleInfo, context, parent, localId);
        if (id == "UniversalSpectrum")
            return createWithImplementation<IFunctionBlock, SpectrumFb>(moduleInfo, context, parent, localId);
        if (id == "UniversalDashboard")
            return createWithImplementation<IFunctionBlock, DashboardFb>(moduleInfo, context, parent, localId);
        if (id == "UniversalVibration")
            return createWithImplementation<IFunctionBlock, VibrationFb>(moduleInfo, context, parent, localId);
        if (id == "UniversalSoundLevelMeter")
            return createWithImplementation<IFunctionBlock, SoundLevelMeterFb>(moduleInfo, context, parent, localId);
        DAQ_THROW_EXCEPTION(NotFoundException, "This module has no function block type {}.", id);
    }
};

}  // namespace daq::modules::universal_renderer

DECLARE_MODULE_EXPORTS(UniversalFunctionBlocksModule)

using daq::modules::universal_renderer::UniversalFunctionBlocksModule;

DEFINE_MODULE_EXPORTS(UniversalFunctionBlocksModule)
