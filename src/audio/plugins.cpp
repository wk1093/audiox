#include "audio/context.hpp"
#include "audio/plugin_registry.hpp"
#include <algorithm>
#include <cstring>

#ifndef AUDIOX_GRAPH_COMPILER_PATH
#define AUDIOX_GRAPH_COMPILER_PATH "/sbin/audiox-graph-compiler"
#endif

std::unique_ptr<audiox::plugin::PluginHost>
AudioContext::preparePlugin(const char *id, uint8_t type, std::string *error) {
    if (error) error->clear();
    const auto *metadata = audiox::plugin::metadataFor(type);
    if (!metadata || !audiox::plugin::available(type)) {
        printf("[PLUGIN] [WARN] Effect %s: package unavailable\n", id);
        if (error) *error = "Plugin package unavailable";
        return nullptr;
    }
    AudioxParameterV1 specifications[AUDIOX_PLUGIN_MAX_PARAMS]{};
    for (unsigned i = 0; i < metadata->paramCount; ++i) {
        const auto &param = metadata->params[i];
        specifications[i] = {param.name, param.label, param.min, param.max,
                             param.defaultValue};
    }
    auto host = std::make_unique<audiox::plugin::PluginHost>();
    const std::string module =
        std::string(audiox::plugin::ROOT) + "/" + metadata->id + "/module.bc";
    if (!host->prepare(module.c_str(), metadata->id, specifications,
                       metadata->paramCount, SAMPLE_RATE, BUFFER_FRAMES, 2,
                       AUDIOX_GRAPH_COMPILER_PATH)) {
        printf("[PLUGIN] [WARN] Effect %s: %s\n", id, host->error());
        if (error) *error = host->error();
        return nullptr;
    }
    return host;
}

void AudioContext::attachPlugin(
    const char *id, audiox::effects::SlotParams &params,
    std::unique_ptr<audiox::plugin::PluginHost> host) {
    retirePlugin(id);
    params.pluginHost = host.get();
    if (host) {
        pluginHosts[id] = std::move(host);
        pluginFaultReported[id] = false;
    }
}

void AudioContext::retirePlugin(const char *id) {
    pluginRuntimeStates.erase(id);
    auto found = pluginHosts.find(id);
    if (found == pluginHosts.end())
        return;
    retiredPlugins.push_back(
        {effectStatesSeq.load(std::memory_order_relaxed) + 1,
         std::move(found->second)});
    pluginHosts.erase(found);
    pluginFaultReported.erase(id);
}

void AudioContext::pollPlugins() {
    std::vector<std::unique_ptr<audiox::plugin::PluginHost>> expired;
    std::vector<std::pair<std::string, std::string>> faults;
    {
        std::lock_guard<std::mutex> lock(effectsMutex);
        for (const auto &entry : pluginHosts) {
            entry.second->poll();
            if (!pluginFaultReported[entry.first] &&
                entry.second->status() ==
                    audiox::plugin::PluginHost::Status::Fault) {
                faults.emplace_back(entry.first, entry.second->error());
                pluginFaultReported[entry.first] = true;
            }
        }
        const uint32_t observed =
            pluginObservedSeq.load(std::memory_order_acquire);
        retiredPlugins.erase(
            std::remove_if(retiredPlugins.begin(), retiredPlugins.end(),
                           [&](RetiredPlugin &plugin) {
                               const bool released =
                                   !processingThreadStarted ||
                                   static_cast<int32_t>(observed -
                                                        plugin.sequence) >= 0;
                               if (released)
                                   expired.push_back(std::move(plugin.host));
                               return released;
                           }),
            retiredPlugins.end());
    }
    for (const auto &fault : faults)
        printf("[PLUGIN] [WARN] Effect %s: %s\n", fault.first.c_str(),
               fault.second.c_str());
}

bool AudioContext::pluginInUse(const char *type) const {
    std::lock_guard<std::mutex> lock(effectsMutex);
    for (const auto &effect : effectStates)
        if (!strcmp(audiox::effects::effectTypeToString(effect.second.type),
                    type))
            return true;
    return false;
}

int AudioContext::reloadPlugins(const char *type) {
    std::string error;
    if (!audiox::plugin::loadCatalog(error)) {
        printf("[PLUGIN] [WARN] %s\n", error.c_str());
        return RET_ERR;
    }
    bool ok = true;
    const auto saved =
        app && app->config ? app->config->readMidiMapFile() : MidiMapData{};
    std::vector<std::pair<std::string, uint8_t>> instances;
    {
        std::lock_guard<std::mutex> lock(effectsMutex);
        for (const auto &effect : effectStates) {
            if (audiox::plugin::isPluginType(effect.second.type) &&
                (!type || !strcmp(type, audiox::effects::effectTypeToString(
                                            effect.second.type))))
                instances.emplace_back(effect.first, effect.second.type);
        }
    }
    for (const auto &instance : instances) {
        auto host = preparePlugin(instance.first.c_str(), instance.second);
        if (!host) {
            ok = false;
            continue;
        }
        std::lock_guard<std::mutex> lock(effectsMutex);
        auto found = effectStates.find(instance.first);
        if (found == effectStates.end() ||
            found->second.type != instance.second)
            continue;
        auto &effect = *found;
        if (!audiox::plugin::isPluginType(effect.second.type) ||
            (type && strcmp(type, audiox::effects::effectTypeToString(
                                      effect.second.type))))
            continue;
        auto params = effect.second;
        if (!params.pluginHost) {
            audiox::effects::setSlotDefaultsForType(&params, params.type);
            for (uint32_t i = 0;
                 i < saved.effectParamCount && i < MIDI_EFFECT_PARAMS_MAX; ++i)
                if (!strcmp(saved.effectParams[i].effectId,
                            effect.first.c_str()) &&
                    audiox::effects::setSlotParamValue(
                        &params, saved.effectParams[i].param,
                        saved.effectParams[i].value) != RET_OK)
                    printf("[PLUGIN] [WARN] Effect %s: invalid saved parameter "
                           "%s\n",
                           effect.first.c_str(), saved.effectParams[i].param);
        }
        attachPlugin(effect.first.c_str(), params, std::move(host));
        effect.second = params;
        effectStatesSeq.fetch_add(1U, std::memory_order_release);
    }
    return ok ? RET_OK : RET_ERR;
}
