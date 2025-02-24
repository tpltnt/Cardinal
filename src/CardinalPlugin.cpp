/*
 * DISTRHO Cardinal Plugin
 * Copyright (C) 2021-2022 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the LICENSE file.
 */

#include <library.hpp>
#include <midi.hpp>
#include <patch.hpp>
#include <plugin.hpp>
#include <random.hpp>
#include <settings.hpp>
#include <system.hpp>

#include <app/Scene.hpp>
#include <engine/Engine.hpp>
#include <ui/common.hpp>
#include <window/Window.hpp>

#ifdef NDEBUG
# undef DEBUG
#endif

#if defined(HAVE_LIBLO) && defined(HEADLESS)
# include <lo/lo.h>
# include "extra/Thread.hpp"
#endif

#include <list>

#include "CardinalCommon.hpp"
#include "DistrhoPluginUtils.hpp"
#include "PluginContext.hpp"
#include "extra/Base64.hpp"

#ifdef DISTRHO_OS_WASM
# include <emscripten/emscripten.h>
#else
# include "extra/SharedResourcePointer.hpp"
#endif

static const constexpr uint kCardinalStateBaseCount = 3; // patch, screenshot, comment

#ifndef HEADLESS
# include "extra/ScopedValueSetter.hpp"
# include "WindowParameters.hpp"
static const constexpr uint kCardinalStateCount = kCardinalStateBaseCount + 2; // moduleInfos, windowSize
#else
# define kWindowParameterCount 0
static const constexpr uint kCardinalStateCount = kCardinalStateBaseCount;
#endif

extern const std::string CARDINAL_VERSION;

namespace rack {
namespace engine {
void Engine_setAboutToClose(Engine*);
}
}

START_NAMESPACE_DISTRHO

template<typename T>
static inline
bool d_isDiffHigherThanLimit(const T& v1, const T& v2, const T& limit)
{
    return v1 != v2 ? (v1 > v2 ? v1 - v2 : v2 - v1) > limit : false;
}

#if ! DISTRHO_PLUGIN_WANT_DIRECT_ACCESS
const char* UI::getBundlePath() const noexcept { return nullptr; }
#endif

// -----------------------------------------------------------------------------------------------------------

#ifdef DISTRHO_OS_WASM
static char* getPatchFileEncodedInURL() {
    return static_cast<char*>(EM_ASM_PTR({
        var searchParams = new URLSearchParams(window.location.search);
        var patch = searchParams.get('patch');
        if (!patch)
        return null;
        var length = lengthBytesUTF8(patch) + 1;
        var str = _malloc(length);
        stringToUTF8(patch, str, length);
        return str;
    }));
};

static char* getPatchRemoteURL() {
    return static_cast<char*>(EM_ASM_PTR({
        var searchParams = new URLSearchParams(window.location.search);
        var patch = searchParams.get('patchurl');
        if (!patch)
        return null;
        var length = lengthBytesUTF8(patch) + 1;
        var str = _malloc(length);
        stringToUTF8(patch, str, length);
        return str;
    }));
};

static char* getPatchStorageSlug() {
    return static_cast<char*>(EM_ASM_PTR({
        var searchParams = new URLSearchParams(window.location.search);
        var patch = searchParams.get('patchstorage');
        if (!patch)
        return null;
        var length = lengthBytesUTF8(patch) + 1;
        var str = _malloc(length);
        stringToUTF8(patch, str, length);
        return str;
    }));
};
#endif

// -----------------------------------------------------------------------------------------------------------

struct ScopedContext {
    ScopedContext(const CardinalBasePlugin* const plugin)
    {
        rack::contextSet(plugin->context);
    }

    ~ScopedContext()
    {
        rack::contextSet(nullptr);
    }
};


// -----------------------------------------------------------------------------------------------------------

class CardinalPlugin : public CardinalBasePlugin
{
   #ifdef DISTRHO_OS_WASM
    ScopedPointer<Initializer> fInitializer;
   #else
    SharedResourcePointer<Initializer> fInitializer;
   #endif

   #if DISTRHO_PLUGIN_NUM_INPUTS != 0
    /* If host audio ins == outs we can get issues for inplace processing.
     * So allocate a float array that will serve as safe copy for those cases.
     */
    float** fAudioBufferCopy;
   #endif

    std::string fAutosavePath;
    uint64_t fNextExpectedFrame;

    struct {
        String comment;
        String screenshot;
       #ifndef HEADLESS
        String windowSize;
       #endif
    } fState;

    // bypass handling
    bool fWasBypassed;
    MidiEvent bypassMidiEvents[16];

   #ifndef HEADLESS
    // real values, not VCV interpreted ones
    float fWindowParameters[kWindowParameterCount];
   #endif

public:
    CardinalPlugin()
        : CardinalBasePlugin(kModuleParameters + kWindowParameterCount + 1, 0, kCardinalStateCount),
         #ifdef DISTRHO_OS_WASM
          fInitializer(new Initializer(this, static_cast<const CardinalBaseUI*>(nullptr))),
         #else
          fInitializer(this, static_cast<const CardinalBaseUI*>(nullptr)),
         #endif
         #if DISTRHO_PLUGIN_NUM_INPUTS != 0
          fAudioBufferCopy(nullptr),
         #endif
          fNextExpectedFrame(0),
          fWasBypassed(false)
    {
       #ifndef HEADLESS
        fWindowParameters[kWindowParameterShowTooltips] = 1.0f;
        fWindowParameters[kWindowParameterCableOpacity] = 50.0f;
        fWindowParameters[kWindowParameterCableTension] = 75.0f;
        fWindowParameters[kWindowParameterRackBrightness] = 100.0f;
        fWindowParameters[kWindowParameterHaloBrightness] = 25.0f;
        fWindowParameters[kWindowParameterKnobMode] = 0.0f;
        fWindowParameters[kWindowParameterWheelKnobControl] = 0.0f;
        fWindowParameters[kWindowParameterWheelSensitivity] = 1.0f;
        fWindowParameters[kWindowParameterLockModulePositions] = 0.0f;
        fWindowParameters[kWindowParameterUpdateRateLimit] = 0.0f;
        fWindowParameters[kWindowParameterBrowserSort] = 3.0f;
        fWindowParameters[kWindowParameterBrowserZoom] = 50.0f;
        fWindowParameters[kWindowParameterInvertZoom] = 0.0f;
        fWindowParameters[kWindowParameterSqueezeModulePositions] = 1.0f;
       #endif

        // create unique temporary path for this instance
        try {
            char uidBuf[24];
            const std::string tmp = rack::system::getTempDirectory();

            for (int i=1;; ++i)
            {
                std::snprintf(uidBuf, sizeof(uidBuf), "Cardinal.%04d", i);
                const std::string trypath = rack::system::join(tmp, uidBuf);

                if (! rack::system::exists(trypath))
                {
                    if (rack::system::createDirectories(trypath))
                        fAutosavePath = trypath;
                    break;
                }
            }
        } DISTRHO_SAFE_EXCEPTION("create unique temporary path");

        // initialize midi events used when entering bypassed state
        std::memset(bypassMidiEvents, 0, sizeof(bypassMidiEvents));

        for (uint8_t i=0; i<16; ++i)
        {
            bypassMidiEvents[i].size = 3;
            bypassMidiEvents[i].data[0] = 0xB0 + i;
            bypassMidiEvents[i].data[1] = 0x7B;
        }

        const float sampleRate = getSampleRate();
        rack::settings::sampleRate = sampleRate;

        context->bufferSize = getBufferSize();
        context->sampleRate = sampleRate;

        const ScopedContext sc(this);

        context->engine = new rack::engine::Engine;
        context->engine->setSampleRate(sampleRate);

        context->history = new rack::history::State;
        context->patch = new rack::patch::Manager;
        context->patch->autosavePath = fAutosavePath;
        context->patch->templatePath = fInitializer->templatePath;
        context->patch->factoryTemplatePath = fInitializer->factoryTemplatePath;

        context->event = new rack::widget::EventState;
        context->scene = new rack::app::Scene;
        context->event->rootWidget = context->scene;

        if (! isDummyInstance())
            context->window = new rack::window::Window;

       #ifdef DISTRHO_OS_WASM
        if ((rack::patchStorageSlug = getPatchStorageSlug()) == nullptr &&
            (rack::patchRemoteURL = getPatchRemoteURL()) == nullptr &&
            (rack::patchFromURL = getPatchFileEncodedInURL()) == nullptr)
       #endif
        {
            context->patch->loadTemplate();
            context->scene->rackScroll->reset();
            // swap to factory template after first load
            context->patch->templatePath = context->patch->factoryTemplatePath;
        }

       #ifdef CARDINAL_INIT_OSC_THREAD
        fInitializer->oscPlugin = this;
       #endif
    }

    ~CardinalPlugin() override
    {
       #ifdef CARDINAL_INIT_OSC_THREAD
        fInitializer->oscPlugin = nullptr;
       #endif

        {
            const ScopedContext sc(this);
            context->patch->clear();

            // do a little dance to prevent context scene deletion from saving to temp dir
           #ifndef HEADLESS
            const ScopedValueSetter<bool> svs(rack::settings::headless, true);
           #endif
            Engine_setAboutToClose(context->engine);
            delete context;
        }

        if (! fAutosavePath.empty())
            rack::system::removeRecursively(fAutosavePath);
    }

    CardinalPluginContext* getRackContext() const noexcept
    {
        return context;
    }

protected:
   /* --------------------------------------------------------------------------------------------------------
    * Information */

    const char* getLabel() const override
    {
        return DISTRHO_PLUGIN_LABEL;
    }

    const char* getDescription() const override
    {
        return ""
        "Cardinal is a free and open-source virtual modular synthesizer plugin.\n"
        "It is based on the popular VCV Rack but with a focus on being a fully self-contained plugin version.\n"
        "It is not an official VCV project, and it is not affiliated with it in any way.\n"
        "\n"
        "Cardinal contains Rack, some 3rd-party modules and a few internal utilities.\n"
        "It does not load external modules and does not connect to the official Rack library/store.\n";
    }

    const char* getMaker() const override
    {
        return "DISTRHO";
    }

    const char* getHomePage() const override
    {
        return "https://github.com/DISTRHO/Cardinal";
    }

    const char* getLicense() const override
    {
        return "GPLv3+";
    }

    uint32_t getVersion() const override
    {
        return d_version(0, 22, 12);
    }

    int64_t getUniqueId() const override
    {
       #if CARDINAL_VARIANT_MAIN || CARDINAL_VARIANT_NATIVE
        return d_cconst('d', 'C', 'd', 'n');
       #elif CARDINAL_VARIANT_MINI
        return d_cconst('d', 'C', 'd', 'M');
       #elif CARDINAL_VARIANT_FX
        return d_cconst('d', 'C', 'n', 'F');
       #elif CARDINAL_VARIANT_SYNTH
        return d_cconst('d', 'C', 'n', 'S');
       #else
        #error cardinal variant not set
       #endif
    }

   /* --------------------------------------------------------------------------------------------------------
    * Init */

    void initAudioPort(const bool input, uint32_t index, AudioPort& port) override
    {
       #if CARDINAL_VARIANT_MAIN
        if (index < 8)
        {
            port.groupId = index / 2;
        }
        else
        {
            port.hints = kAudioPortIsCV | kCVPortHasPositiveUnipolarRange | kCVPortHasScaledRange | kCVPortIsOptional;
            index -= 8;
        }
       #elif CARDINAL_VARIANT_MINI || CARDINAL_VARIANT_NATIVE || CARDINAL_VARIANT_FX || CARDINAL_VARIANT_SYNTH
        if (index < 2)
            port.groupId = kPortGroupStereo;
       #endif

        CardinalBasePlugin::initAudioPort(input, index, port);
    }

   #if CARDINAL_VARIANT_MAIN
    void initPortGroup(const uint32_t index, PortGroup& portGroup) override
    {
        switch (index)
        {
        case 0:
            portGroup.name = "Audio 1+2";
            portGroup.symbol = "audio_1_and_2";
            break;
        case 1:
            portGroup.name = "Audio 3+4";
            portGroup.symbol = "audio_3_and_4";
            break;
        case 2:
            portGroup.name = "Audio 5+6";
            portGroup.symbol = "audio_5_and_6";
            break;
        case 3:
            portGroup.name = "Audio 7+8";
            portGroup.symbol = "audio_7_and_8";
            break;
        }
    }
   #endif

    void initParameter(const uint32_t index, Parameter& parameter) override
    {
        if (index < kModuleParameters)
        {
            parameter.name = "Parameter ";
            parameter.name += String(index + 1);
            parameter.symbol = "param_";
            parameter.symbol += String(index + 1);
            parameter.unit = "v";
            parameter.hints = kParameterIsAutomatable;
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 10.0f;
            return;
        }

        if (index == kModuleParameters)
        {
            parameter.initDesignation(kParameterDesignationBypass);
            return;
        }

       #ifndef HEADLESS
        switch (index - kModuleParameters - 1)
        {
        case kWindowParameterShowTooltips:
            parameter.name = "Show tooltips";
            parameter.symbol = "tooltips";
            parameter.hints = kParameterIsAutomatable|kParameterIsInteger|kParameterIsBoolean;
            parameter.ranges.def = 1.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 1.0f;
            break;
        case kWindowParameterCableOpacity:
            parameter.name = "Cable opacity";
            parameter.symbol = "cableOpacity";
            parameter.unit = "%";
            parameter.hints = kParameterIsAutomatable;
            parameter.ranges.def = 50.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 100.0f;
            break;
        case kWindowParameterCableTension:
            parameter.name = "Cable tension";
            parameter.symbol = "cableTension";
            parameter.unit = "%";
            parameter.hints = kParameterIsAutomatable;
            parameter.ranges.def = 75.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 100.0f;
            break;
        case kWindowParameterRackBrightness:
            parameter.name = "Room brightness";
            parameter.symbol = "rackBrightness";
            parameter.unit = "%";
            parameter.hints = kParameterIsAutomatable;
            parameter.ranges.def = 100.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 100.0f;
            break;
        case kWindowParameterHaloBrightness:
            parameter.name = "Light Bloom";
            parameter.symbol = "haloBrightness";
            parameter.unit = "%";
            parameter.hints = kParameterIsAutomatable;
            parameter.ranges.def = 25.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 100.0f;
            break;
        case kWindowParameterKnobMode:
            parameter.name = "Knob mode";
            parameter.symbol = "knobMode";
            parameter.hints = kParameterIsAutomatable|kParameterIsInteger;
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 2.0f;
            parameter.enumValues.count = 3;
            parameter.enumValues.restrictedMode = true;
            parameter.enumValues.values = new ParameterEnumerationValue[3];
            parameter.enumValues.values[0].label = "Linear";
            parameter.enumValues.values[0].value = 0.0f;
            parameter.enumValues.values[1].label = "Absolute rotary";
            parameter.enumValues.values[1].value = 1.0f;
            parameter.enumValues.values[2].label = "Relative rotary";
            parameter.enumValues.values[2].value = 2.0f;
            break;
        case kWindowParameterWheelKnobControl:
            parameter.name = "Scroll wheel knob control";
            parameter.symbol = "knobScroll";
            parameter.hints = kParameterIsAutomatable|kParameterIsInteger|kParameterIsBoolean;
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 1.0f;
            break;
        case kWindowParameterWheelSensitivity:
            parameter.name = "Scroll wheel knob sensitivity";
            parameter.symbol = "knobScrollSensitivity";
            parameter.hints = kParameterIsAutomatable|kParameterIsLogarithmic;
            parameter.ranges.def = 1.0f;
            parameter.ranges.min = 0.1f;
            parameter.ranges.max = 10.0f;
            break;
        case kWindowParameterLockModulePositions:
            parameter.name = "Lock module positions";
            parameter.symbol = "lockModules";
            parameter.hints = kParameterIsAutomatable|kParameterIsInteger|kParameterIsBoolean;
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 1.0f;
            break;
        case kWindowParameterUpdateRateLimit:
            parameter.name = "Update rate limit";
            parameter.symbol = "rateLimit";
            parameter.hints = kParameterIsAutomatable|kParameterIsInteger;
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 2.0f;
            parameter.enumValues.count = 3;
            parameter.enumValues.restrictedMode = true;
            parameter.enumValues.values = new ParameterEnumerationValue[3];
            parameter.enumValues.values[0].label = "None";
            parameter.enumValues.values[0].value = 0.0f;
            parameter.enumValues.values[1].label = "2x";
            parameter.enumValues.values[1].value = 1.0f;
            parameter.enumValues.values[2].label = "4x";
            parameter.enumValues.values[2].value = 2.0f;
            break;
        case kWindowParameterBrowserSort:
            parameter.name = "Browser sort";
            parameter.symbol = "browserSort";
            parameter.hints = kParameterIsAutomatable|kParameterIsInteger;
            parameter.ranges.def = 3.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 5.0f;
            parameter.enumValues.count = 6;
            parameter.enumValues.restrictedMode = true;
            parameter.enumValues.values = new ParameterEnumerationValue[6];
            parameter.enumValues.values[0].label = "Updated";
            parameter.enumValues.values[0].value = 0.0f;
            parameter.enumValues.values[1].label = "Last used";
            parameter.enumValues.values[1].value = 1.0f;
            parameter.enumValues.values[2].label = "Most used";
            parameter.enumValues.values[2].value = 2.0f;
            parameter.enumValues.values[3].label = "Brand";
            parameter.enumValues.values[3].value = 3.0f;
            parameter.enumValues.values[4].label = "Name";
            parameter.enumValues.values[4].value = 4.0f;
            parameter.enumValues.values[5].label = "Random";
            parameter.enumValues.values[5].value = 5.0f;
            break;
        case kWindowParameterBrowserZoom:
            parameter.name = "Browser zoom";
            parameter.symbol = "browserZoom";
            parameter.hints = kParameterIsAutomatable;
            parameter.unit = "%";
            parameter.ranges.def = 50.0f;
            parameter.ranges.min = 25.0f;
            parameter.ranges.max = 200.0f;
            parameter.enumValues.count = 7;
            parameter.enumValues.restrictedMode = true;
            parameter.enumValues.values = new ParameterEnumerationValue[7];
            parameter.enumValues.values[0].label = "25";
            parameter.enumValues.values[0].value = 25.0f;
            parameter.enumValues.values[1].label = "35";
            parameter.enumValues.values[1].value = 35.0f;
            parameter.enumValues.values[2].label = "50";
            parameter.enumValues.values[2].value = 50.0f;
            parameter.enumValues.values[3].label = "71";
            parameter.enumValues.values[3].value = 71.0f;
            parameter.enumValues.values[4].label = "100";
            parameter.enumValues.values[4].value = 100.0f;
            parameter.enumValues.values[5].label = "141";
            parameter.enumValues.values[5].value = 141.0f;
            parameter.enumValues.values[6].label = "200";
            parameter.enumValues.values[6].value = 200.0f;
            break;
        case kWindowParameterInvertZoom:
            parameter.name = "Invert zoom";
            parameter.symbol = "invertZoom";
            parameter.hints = kParameterIsAutomatable|kParameterIsInteger|kParameterIsBoolean;
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 1.0f;
            break;
        case kWindowParameterSqueezeModulePositions:
            parameter.name = "Auto-squeeze module positions";
            parameter.symbol = "squeezeModules";
            parameter.hints = kParameterIsAutomatable|kParameterIsInteger|kParameterIsBoolean;
            parameter.ranges.def = 1.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 1.0f;
            break;
        }
       #endif
    }

    void initState(const uint32_t index, State& state) override
    {
        switch (index)
        {
        case 0:
            state.hints = kStateIsBase64Blob | kStateIsOnlyForDSP;
            state.key = "patch";
            state.label = "Patch";
            break;
        case 1:
            state.hints = kStateIsHostReadable | kStateIsBase64Blob;
            state.key = "screenshot";
            state.label = "Screenshot";
            break;
        case 2:
            state.hints = kStateIsHostWritable;
            state.key = "comment";
            state.label = "Comment";
            break;
        case 3:
            state.hints = kStateIsOnlyForUI;
            state.key = "moduleInfos";
            state.label = "moduleInfos";
            break;
        case 4:
            state.hints = kStateIsOnlyForUI;
            state.key = "windowSize";
            state.label = "Window size";
            break;
        }
    }

   /* --------------------------------------------------------------------------------------------------------
    * Internal data */

    float getParameterValue(uint32_t index) const override
    {
        // host mapped parameters
        if (index < kModuleParameters)
            return context->parameters[index];

        // bypass
        if (index == kModuleParameters)
            return context->bypassed ? 1.0f : 0.0f;

       #ifndef HEADLESS
        // window related parameters
        index -= kModuleParameters + 1;

        if (index < kWindowParameterCount)
            return fWindowParameters[index];
       #endif

        return 0.0f;
    }

    void setParameterValue(uint32_t index, float value) override
    {
        // host mapped parameters
        if (index < kModuleParameters)
        {
            context->parameters[index] = value;
            return;
        }

        // bypass
        if (index == kModuleParameters)
        {
            context->bypassed = value > 0.5f;
            return;
        }

       #ifndef HEADLESS
        // window related parameters
        index -= kModuleParameters + 1;

        if (index < kWindowParameterCount)
        {
            fWindowParameters[index] = value;
            return;
        }
       #endif
    }

    String getState(const char* const key) const override
    {
       #ifndef HEADLESS
        if (std::strcmp(key, "moduleInfos") == 0)
        {
            json_t* const rootJ = json_object();
            DISTRHO_SAFE_ASSERT_RETURN(rootJ != nullptr, String());

            for (const auto& pluginPair : rack::settings::moduleInfos)
            {
                json_t* const pluginJ = json_object();
                DISTRHO_SAFE_ASSERT_CONTINUE(pluginJ != nullptr);

                for (const auto& modulePair : pluginPair.second)
                {
                    json_t* const moduleJ = json_object();
                    DISTRHO_SAFE_ASSERT_CONTINUE(moduleJ != nullptr);

                    const rack::settings::ModuleInfo& m(modulePair.second);

                    // To make setting.json smaller, only set properties if not default values.
                    if (m.favorite)
                        json_object_set_new(moduleJ, "favorite", json_boolean(m.favorite));
                    if (m.added > 0)
                        json_object_set_new(moduleJ, "added", json_integer(m.added));
                    if (std::isfinite(m.lastAdded))
                        json_object_set_new(moduleJ, "lastAdded", json_real(m.lastAdded));

                    if (json_object_size(moduleJ))
                        json_object_set_new(pluginJ, modulePair.first.c_str(), moduleJ);
                    else
                        json_decref(moduleJ);
                }

                if (json_object_size(pluginJ))
                    json_object_set_new(rootJ, pluginPair.first.c_str(), pluginJ);
                else
                    json_decref(pluginJ);
            }

            const String info(json_dumps(rootJ, JSON_COMPACT), false);
            json_decref(rootJ);

            return info;
        }

        if (std::strcmp(key, "windowSize") == 0)
            return fState.windowSize;
       #endif

        if (std::strcmp(key, "comment") == 0)
            return fState.comment;
        if (std::strcmp(key, "screenshot") == 0)
            return fState.screenshot;

        if (std::strcmp(key, "patch") != 0)
            return String();
        if (fAutosavePath.empty())
            return String();

        std::vector<uint8_t> data;

        {
            const ScopedContext sc(this);

            context->engine->prepareSave();
            context->patch->saveAutosave();
            context->patch->cleanAutosave();
            // context->history->setSaved();

            try {
                data = rack::system::archiveDirectory(fAutosavePath, 1);
            } DISTRHO_SAFE_EXCEPTION_RETURN("getState archiveDirectory", String());
        }

        return String::asBase64(data.data(), data.size());
    }

    void setState(const char* const key, const char* const value) override
    {
       #ifndef HEADLESS
        if (std::strcmp(key, "moduleInfos") == 0)
        {
            json_error_t error;
            json_t* const rootJ = json_loads(value, 0, &error);
            DISTRHO_SAFE_ASSERT_RETURN(rootJ != nullptr,);

            const char* pluginSlug;
            json_t* pluginJ;

            json_object_foreach(rootJ, pluginSlug, pluginJ)
            {
                const char* moduleSlug;
                json_t* moduleJ;

                json_object_foreach(pluginJ, moduleSlug, moduleJ)
                {
                    rack::settings::ModuleInfo m;

                    if (json_t* const favoriteJ = json_object_get(moduleJ, "favorite"))
                        m.favorite = json_boolean_value(favoriteJ);

                    if (json_t* const addedJ = json_object_get(moduleJ, "added"))
                        m.added = json_integer_value(addedJ);

                    if (json_t* const lastAddedJ = json_object_get(moduleJ, "lastAdded"))
                        m.lastAdded = json_number_value(lastAddedJ);

                    rack::settings::moduleInfos[pluginSlug][moduleSlug] = m;
                }
            }

            json_decref(rootJ);
            return;
        }
        if (std::strcmp(key, "windowSize") == 0)
        {
            fState.windowSize = value;
            return;
        }
       #endif

        if (std::strcmp(key, "comment") == 0)
        {
            fState.comment = value;
            return;
        }

        if (std::strcmp(key, "screenshot") == 0)
        {
            fState.screenshot = value;
           #if defined(HAVE_LIBLO) && !defined(HEADLESS)
            patchUtils::sendScreenshotToRemote(value);
           #endif
            return;
        }

        if (std::strcmp(key, "patch") != 0)
            return;
        if (fAutosavePath.empty())
            return;

        const std::vector<uint8_t> data(d_getChunkFromBase64String(value));

        DISTRHO_SAFE_ASSERT_RETURN(data.size() >= 4,);

        rack::system::removeRecursively(fAutosavePath);
        rack::system::createDirectories(fAutosavePath);

        static constexpr const char zstdMagic[] = "\x28\xb5\x2f\xfd";

        if (std::memcmp(data.data(), zstdMagic, sizeof(zstdMagic)) != 0)
        {
            FILE* const f = std::fopen(rack::system::join(fAutosavePath, "patch.json").c_str(), "w");
            DISTRHO_SAFE_ASSERT_RETURN(f != nullptr,);

            std::fwrite(data.data(), data.size(), 1, f);
            std::fclose(f);
        }
        else
        {
            try {
                rack::system::unarchiveToDirectory(data, fAutosavePath);
            } DISTRHO_SAFE_EXCEPTION_RETURN("setState unarchiveToDirectory",);
        }

        const ScopedContext sc(this);

        try {
            context->patch->loadAutosave();
        } DISTRHO_SAFE_EXCEPTION_RETURN("setState loadAutosave",);

        // context->history->setSaved();
    }

   /* --------------------------------------------------------------------------------------------------------
    * Process */

    void activate() override
    {
        context->bufferSize = getBufferSize();

       #if DISTRHO_PLUGIN_NUM_INPUTS != 0
        fAudioBufferCopy = new float*[DISTRHO_PLUGIN_NUM_INPUTS];
        for (int i=0; i<DISTRHO_PLUGIN_NUM_INPUTS; ++i)
            fAudioBufferCopy[i] = new float[context->bufferSize];
       #endif

        fNextExpectedFrame = 0;
    }

    void deactivate() override
    {
       #if DISTRHO_PLUGIN_NUM_INPUTS != 0
        if (fAudioBufferCopy != nullptr)
        {
            for (int i=0; i<DISTRHO_PLUGIN_NUM_INPUTS; ++i)
                delete[] fAudioBufferCopy[i];
            delete[] fAudioBufferCopy;
            fAudioBufferCopy = nullptr;
        }
       #endif
    }

    void run(const float** const inputs, float** const outputs, const uint32_t frames,
             const MidiEvent* const midiEvents, const uint32_t midiEventCount) override
    {
        rack::contextSet(context);

        const bool bypassed = context->bypassed;

        {
            const TimePosition& timePos(getTimePosition());

            bool reset = timePos.playing && (timePos.frame == 0 || d_isDiffHigherThanLimit(fNextExpectedFrame, timePos.frame, (uint64_t)2));

            // ignore hosts which cannot supply time frame position
            if (context->playing == timePos.playing && timePos.frame == 0 && context->frame == 0)
                reset = false;

            context->playing = timePos.playing;
            context->bbtValid = timePos.bbt.valid;
            context->frame = timePos.frame;

            if (timePos.bbt.valid)
            {
                const double samplesPerTick = 60.0 * getSampleRate()
                                            / timePos.bbt.beatsPerMinute
                                            / timePos.bbt.ticksPerBeat;
                context->bar = timePos.bbt.bar;
                context->beat = timePos.bbt.beat;
                context->beatsPerBar = timePos.bbt.beatsPerBar;
                context->beatType = timePos.bbt.beatType;
                context->barStartTick = timePos.bbt.barStartTick;
                context->beatsPerMinute = timePos.bbt.beatsPerMinute;
                context->tick = timePos.bbt.tick;
                context->ticksPerBeat = timePos.bbt.ticksPerBeat;
                context->ticksPerClock = timePos.bbt.ticksPerBeat / timePos.bbt.beatType;
                context->ticksPerFrame = 1.0 / samplesPerTick;
                context->tickClock = std::fmod(timePos.bbt.tick, context->ticksPerClock);
            }

            context->reset = reset;
            fNextExpectedFrame = timePos.playing ? timePos.frame + frames : 0;
        }

        // separate buffers, use them
        if (inputs != outputs && (inputs == nullptr || inputs[0] != outputs[0]))
        {
            context->dataIns = inputs;
            context->dataOuts = outputs;
        }
        // inline processing, use a safe copy
        else
        {
           #if DISTRHO_PLUGIN_NUM_INPUTS != 0
            for (int i=0; i<DISTRHO_PLUGIN_NUM_INPUTS; ++i)
            {
               #if CARDINAL_VARIANT_MAIN
                // can be null on main variant
                if (inputs[i] != nullptr)
               #endif
                    std::memcpy(fAudioBufferCopy[i], inputs[i], sizeof(float)*frames);
            }
            context->dataIns = fAudioBufferCopy;
           #else
            context->dataIns = nullptr;
           #endif
            context->dataOuts = outputs;
        }

        for (int i=0; i<DISTRHO_PLUGIN_NUM_OUTPUTS; ++i)
        {
           #if CARDINAL_VARIANT_MAIN
            // can be null on main variant
            if (outputs[i] != nullptr)
           #endif
                std::memset(outputs[i], 0, sizeof(float)*frames);
        }

        if (bypassed)
        {
            if (fWasBypassed != bypassed)
            {
                context->midiEvents = bypassMidiEvents;
                context->midiEventCount = 16;
            }
            else
            {
                context->midiEvents = nullptr;
                context->midiEventCount = 0;
            }
        }
        else
        {
            context->midiEvents = midiEvents;
            context->midiEventCount = midiEventCount;
        }

        ++context->processCounter;
        context->engine->stepBlock(frames);

        fWasBypassed = bypassed;
    }

    void sampleRateChanged(const double newSampleRate) override
    {
        rack::contextSet(context);
        rack::settings::sampleRate = newSampleRate;
        context->sampleRate = newSampleRate;
        context->engine->setSampleRate(newSampleRate);
    }

    // -------------------------------------------------------------------------------------------------------

private:
   /**
      Set our plugin class as non-copyable and add a leak detector just in case.
    */
    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CardinalPlugin)
};

CardinalPluginContext* getRackContextFromPlugin(void* const ptr)
{
    return static_cast<CardinalPlugin*>(ptr)->getRackContext();
}

/* ------------------------------------------------------------------------------------------------------------
 * Plugin entry point, called by DPF to create a new plugin instance. */

Plugin* createPlugin()
{
    return new CardinalPlugin();
}

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DISTRHO
