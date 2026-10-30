// H-Chip Filter: the Emulator IV filter chip (dsp/hchip.*), programmed
// with the EOS filter recipes (dsp/emu_filters.*). One hchip::Voice per polyphony
// channel; the cutoff knob and CV map straight onto the voice's 16-bit interpolating variable x.
#include "plugin.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "emu_filters.hpp"
#include "hchip.hpp"
#include "widgets.hpp"

namespace {

// Module type index -> emu_filters menu entry. The morph filters (menu 17..20) take six extra
// parameters the module has no controls for and are left out.
const int kMenuOf[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                       21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33};
constexpr int kNumTypes = sizeof(kMenuOf) / sizeof(kMenuOf[0]);

// The sample clock the recipes were written for; the Hz readout is scaled by fs / 48 kHz.
constexpr float kChipClock = 48000.f;
// Rack audio: 5 V peak is 16-bit full scale (the E4's sample resolution).
constexpr float kVoltsToLsb = 32767.f / 5.f;
// Cutoff CV: 32 cutoff steps per volt, about one octave on the lowpass display law
// (20000 Hz * (1002 / 1024)^(255 - v): 31.9 steps per octave).
constexpr float kCutoffStepsPerVolt = 32.f;
// Q CV: the full 0..127 range over 10 V.
constexpr float kQStepsPerVolt = 12.7f;

const char* chipFreqText(int menu, int cutoff, float sampleRate, char* buf, size_t n) {
    const emuz::FilterDesc& d = emuz::filterDesc(menu);
    int maxf = 0, mul = 0;
    switch (d.law) {
        case emuz::FreqLaw::Lowpass: maxf = 20000; mul = 1002; break;
        case emuz::FreqLaw::Highpass: maxf = 18000; mul = 1003; break;
        case emuz::FreqLaw::Bandpass: maxf = 10000; mul = 1005; break;
        case emuz::FreqLaw::Shelf: maxf = 10000; mul = 1006; break;
        default: break;
    }
    if (maxf == 0) {
        std::snprintf(buf, n, "%d", cutoff);
    } else {
        float hz = emuz::filFreq(cutoff, maxf, mul) * sampleRate / kChipClock;
        std::snprintf(buf, n, "%.0f Hz", hz);
    }
    return buf;
}

}  // namespace

struct HChipFilter : Module {
    enum ParamId {
        TYPE_PARAM,
        CUTOFF_PARAM,
        CUTOFF_CV_PARAM,
        Q_PARAM,
        Q_CV_PARAM,
        DRIVE_PARAM,
        TYPE_PREV_PARAM,
        TYPE_NEXT_PARAM,
        PARAMS_LEN
    };
    enum InputId { IN_INPUT, CUTOFF_INPUT, Q_INPUT, INPUTS_LEN };
    enum OutputId { OUT_OUTPUT, OUTPUTS_LEN };
    enum LightId { CLIP_LIGHT, LIGHTS_LEN };

    hchip::Voice voices[PORT_MAX_CHANNELS];
    int voiceMenu[PORT_MAX_CHANNELS];
    int voiceQ[PORT_MAX_CHANNELS];
    hchip::Stats lastStats[PORT_MAX_CHANNELS];

    // Context menu: the datapath widths the patent leaves open.
    int multInBits = 16;      // LATCH 18: 16, 17 or 18 bits
    int productRound = 0;     // 0 floor, 1 nearest
    bool accSaturate = false;
    hchip::Params applied;

    // What channel 0 is doing, for the display.
    float displayCutoff = 128.f;
    int displayQ = 0;
    int displayMenu = 0;

    dsp::ClockDivider lightDivider;
    dsp::ClockDivider settingsDivider;
    dsp::BooleanTrigger prevTrigger, nextTrigger;
    float clipLevel = 0.f;

    struct CutoffQuantity : ParamQuantity {
        std::string getLabel() override {
            HChipFilter* m = dynamic_cast<HChipFilter*>(module);
            return m ? emuz::filterDesc(m->currentMenu()).label0 : "Cutoff";
        }
        std::string getDisplayValueString() override {
            HChipFilter* m = dynamic_cast<HChipFilter*>(module);
            char buf[32];
            int c = clamp((int)std::lround(getValue()), 0, 255);
            if (!m) return std::to_string(c);
            return chipFreqText(m->currentMenu(), c, APP->engine->getSampleRate(), buf, sizeof buf);
        }
    };

    struct QQuantity : ParamQuantity {
        std::string getLabel() override {
            HChipFilter* m = dynamic_cast<HChipFilter*>(module);
            return m ? emuz::filterDesc(m->currentMenu()).label1 : "Q";
        }
        std::string getDisplayValueString() override {
            HChipFilter* m = dynamic_cast<HChipFilter*>(module);
            int q = clamp((int)std::lround(getValue()), 0, 127);
            if (!m) return std::to_string(q);
            std::string label, text;
            emuz::formatParam(m->currentMenu(), 1, q, label, text);
            return text;
        }
    };

    HChipFilter() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        std::vector<std::string> names;
        for (int i = 0; i < kNumTypes; i++) names.push_back(emuz::filterDesc(kMenuOf[i]).name);
        configSwitch(TYPE_PARAM, 0.f, kNumTypes - 1, 0.f, "Filter type", names);
        configParam<CutoffQuantity>(CUTOFF_PARAM, 0.f, 255.f, 128.f, "Cutoff");
        configParam(CUTOFF_CV_PARAM, -1.f, 1.f, 0.f, "Cutoff CV amount", "%", 0.f, 100.f);
        configParam<QQuantity>(Q_PARAM, 0.f, 127.f, 0.f, "Q");
        paramQuantities[Q_PARAM]->snapEnabled = true;
        configParam(Q_CV_PARAM, -1.f, 1.f, 0.f, "Q CV amount", "%", 0.f, 100.f);
        configParam(DRIVE_PARAM, -24.f, 24.f, 0.f, "Drive", " dB");
        configButton(TYPE_PREV_PARAM, "Previous filter type");
        configButton(TYPE_NEXT_PARAM, "Next filter type");
        configInput(IN_INPUT, "Audio");
        configInput(CUTOFF_INPUT, "Cutoff CV (32 steps per volt)");
        configInput(Q_INPUT, "Q CV (10 V for the full range)");
        configOutput(OUT_OUTPUT, "Audio");
        configLight(CLIP_LIGHT, "Chip saturation (delay memory, multiplier operand or accumulator)");
        configBypass(IN_INPUT, OUT_OUTPUT);
        lightDivider.setDivision(64);
        settingsDivider.setDivision(256);
        for (int c = 0; c < PORT_MAX_CHANNELS; c++) {
            voiceMenu[c] = -1;
            voiceQ[c] = -1;
        }
        applyWidths();
    }

    int currentType() { return clamp((int)std::lround(params[TYPE_PARAM].getValue()), 0, kNumTypes - 1); }

    int currentMenu() { return kMenuOf[currentType()]; }

    // Steps the type parameter, wrapping around at both ends.
    void stepType(int delta) {
        int t = (currentType() + delta) % kNumTypes;
        if (t < 0) t += kNumTypes;
        params[TYPE_PARAM].setValue(t);
    }

    hchip::Params widthParams() const {
        hchip::Params p;
        p.multInBits = clamp(multInBits, 16, 18);
        p.productRound = productRound ? hchip::Rounding::Nearest : hchip::Rounding::Floor;
        p.accSaturate = accSaturate;
        return p;
    }

    void applyWidths() {
        applied = widthParams();
        for (auto& v : voices) v.setParams(applied);
    }

    bool widthsChanged() const {
        hchip::Params p = widthParams();
        return p.multInBits != applied.multInBits || p.productRound != applied.productRound ||
               p.accSaturate != applied.accSaturate;
    }

    void onReset() override {
        for (int c = 0; c < PORT_MAX_CHANNELS; c++) {
            voices[c].resetState();
            voiceMenu[c] = -1;
            voiceQ[c] = -1;
        }
    }

    void process(const ProcessArgs& args) override {
        if (settingsDivider.process() && widthsChanged()) applyWidths();
        if (prevTrigger.process(params[TYPE_PREV_PARAM].getValue() > 0.f)) stepType(-1);
        if (nextTrigger.process(params[TYPE_NEXT_PARAM].getValue() > 0.f)) stepType(1);

        const int channels = std::max(1, inputs[IN_INPUT].getChannels());
        const int menu = currentMenu();
        const float cutoffKnob = params[CUTOFF_PARAM].getValue();
        const float cutoffCv = params[CUTOFF_CV_PARAM].getValue() * kCutoffStepsPerVolt;
        const float qKnob = params[Q_PARAM].getValue();
        const float qCv = params[Q_CV_PARAM].getValue() * kQStepsPerVolt;
        const float drive = std::pow(10.f, params[DRIVE_PARAM].getValue() / 20.f);
        const float inScale = kVoltsToLsb * drive;
        const float outScale = 1.f / inScale;
        const uint8_t none[6] = {0, 0, 0, 0, 0, 0};

        bool clipped = false;
        for (int c = 0; c < channels; c++) {
            hchip::Voice& v = voices[c];

            float cutoff = cutoffKnob + inputs[CUTOFF_INPUT].getPolyVoltage(c) * cutoffCv;
            cutoff = clamp(cutoff, 0.f, 255.f);
            int q = clamp((int)std::lround(qKnob + inputs[Q_INPUT].getPolyVoltage(c) * qCv), 0, 127);
            if (q != voiceQ[c] || menu != voiceMenu[c]) {
                emuz::Block b = emuz::computeFrames(menu, q, none);
                v.setWords(b.w, b.flags);
                voiceQ[c] = q;
                voiceMenu[c] = menu;
            }
            // The interpolating variable: a cutoff byte c is c * 0x0101, so 255 is 0xFFFF.
            v.setX((uint16_t)std::lround(cutoff * (65535.f / 255.f)));

            float in = inputs[IN_INPUT].getPolyVoltage(c) * inScale;
            int64_t x = (int64_t)std::lround(clamp(in, -8388607.f, 8388607.f));
            int64_t y = v.process(x);
            outputs[OUT_OUTPUT].setVoltage((float)y * outScale, c);

            if (c == 0) {
                displayCutoff = cutoff;
                displayQ = q;
                displayMenu = menu;
            }
        }
        outputs[OUT_OUTPUT].setChannels(channels);

        if (lightDivider.process()) {
            for (int c = 0; c < channels; c++) {
                hchip::Stats s = voices[c].stats();
                const hchip::Stats& l = lastStats[c];
                if (s.delaySat != l.delaySat || s.multInSat != l.multInSat || s.accOverflow != l.accOverflow)
                    clipped = true;
                lastStats[c] = s;
            }
            clipLevel = clipped ? 1.f : clipLevel * 0.85f;
            lights[CLIP_LIGHT].setBrightness(clipLevel);
        }
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "multInBits", json_integer(multInBits));
        json_object_set_new(root, "productRound", json_integer(productRound));
        json_object_set_new(root, "accSaturate", json_boolean(accSaturate));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "multInBits")) multInBits = json_integer_value(j);
        if (json_t* j = json_object_get(root, "productRound")) productRound = json_integer_value(j);
        if (json_t* j = json_object_get(root, "accSaturate")) accSaturate = json_boolean_value(j);
        applyWidths();
    }
};

// Sets the type parameter from the UI thread, as an undoable step.
static void setTypeParam(HChipFilter* module, int type) {
    type = clamp(type, 0, kNumTypes - 1);
    if (type == module->currentType()) return;
    history::ParamChange* h = new history::ParamChange;
    h->name = "change filter type";
    h->moduleId = module->id;
    h->paramId = HChipFilter::TYPE_PARAM;
    h->oldValue = module->params[HChipFilter::TYPE_PARAM].getValue();
    h->newValue = type;
    APP->engine->setParamValue(module, HChipFilter::TYPE_PARAM, type);
    APP->history->push(h);
}

// Two lines: the filter type, then the E4's readout of cutoff and second parameter. It is also
// the type selector: a click opens the list of types, the scroll wheel steps through them.
struct TypeDisplay : widget::Widget {
    HChipFilter* module = nullptr;
    float scrollAccum = 0.f;

    void createTypeMenu() {
        ui::Menu* menu = createMenu();
        menu->addChild(createMenuLabel("Filter type"));
        for (int i = 0; i < kNumTypes; i++) {
            menu->addChild(createCheckMenuItem(
                emuz::filterDesc(kMenuOf[i]).name, "",
                [=]() { return module->currentType() == i; },
                [=]() { setTypeParam(module, i); }));
        }
    }

    void onButton(const ButtonEvent& e) override {
        Widget::onButton(e);
        if (e.isConsumed() || !module) return;
        if (e.action == GLFW_PRESS && (e.button == GLFW_MOUSE_BUTTON_LEFT || e.button == GLFW_MOUSE_BUTTON_RIGHT)) {
            e.consume(this);
            createTypeMenu();
        }
    }

    void onHoverScroll(const HoverScrollEvent& e) override {
        if (!module) return;
        e.consume(this);
        // One wheel notch is 50 units (10 on macOS); trackpads deliver it in small pieces.
#if defined ARCH_MAC
        const float notch = 10.f;
#else
        const float notch = 50.f;
#endif
        scrollAccum += e.scrollDelta.y;
        int steps = (int)(scrollAccum / notch);
        if (steps != 0) {
            scrollAccum -= steps * notch;
            setTypeParam(module, module->currentType() + steps);
        }
    }

    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
        nvgFillColor(args.vg, nvgRGB(0x12, 0x12, 0x14));
        nvgFill(args.vg);
        nvgStrokeWidth(args.vg, 1.f);
        nvgStrokeColor(args.vg, nvgRGB(0x40, 0x40, 0x44));
        nvgStroke(args.vg);
        Widget::draw(args);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
            if (font) {
                std::string line1 = "2 Pole Low-pass";
                std::string line2 = "1146 Hz  Q 64";
                if (module) {
                    int menu = module->displayMenu;
                    const emuz::FilterDesc& d = emuz::filterDesc(menu);
                    line1 = d.name;
                    char buf[32];
                    int c = clamp((int)std::lround(module->displayCutoff), 0, 255);
                    chipFreqText(menu, c, APP->engine->getSampleRate(), buf, sizeof buf);
                    std::string label, text;
                    emuz::formatParam(menu, 1, module->displayQ, label, text);
                    line2 = std::string(buf) + "  " + label + " " + text;
                }
                nvgFontFaceId(args.vg, font->handle);
                nvgTextLetterSpacing(args.vg, 0.f);
                nvgFillColor(args.vg, nvgRGB(0xff, 0xb0, 0x30));
                nvgFontSize(args.vg, 12.f);
                nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgText(args.vg, 5.f, box.size.y * 0.3f, line1.c_str(), nullptr);
                nvgFontSize(args.vg, 11.f);
                nvgFillColor(args.vg, nvgRGB(0xe0, 0x90, 0x20));
                nvgText(args.vg, 5.f, box.size.y * 0.72f, line2.c_str(), nullptr);
            }
        }
        Widget::drawLayer(args, layer);
    }
};

struct HChipFilterWidget : ModuleWidget {
    HChipFilterWidget(HChipFilter* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/HChipFilter.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        const NVGcolor dark = kPanelDark;
        const NVGcolor light = kPanelLight;
        PanelLabels* labels = new PanelLabels;
        labels->box = Rect(Vec(0, 0), box.size);
        labels->labels = {
            {Vec(30.48f, 7.5f), "H-CHIP", 15.f, true, light},
            {Vec(30.48f, 13.2f), "US 5,170,369 filter section", 7.f, false, kPanelGrey},
            {Vec(12.f, 34.2f), "TYPE", 8.f, true, dark},
            {Vec(7.5f, 47.8f), "<", 8.f, true, dark},
            {Vec(16.5f, 47.8f), ">", 8.f, true, dark},
            {Vec(48.96f, 34.2f), "DRIVE", 8.f, true, dark},
            {Vec(17.5f, 71.3f), "CUTOFF", 8.f, true, dark},
            {Vec(43.46f, 71.3f), "Q", 8.f, true, dark},
            {Vec(17.5f, 104.6f), "CV", 7.f, false, dark},
            {Vec(43.46f, 104.6f), "CV", 7.f, false, dark},
            {Vec(12.f, 109.6f), "IN", 8.f, true, light},
            {Vec(48.96f, 109.6f), "OUT", 8.f, true, light},
            {Vec(30.48f, 109.6f), "CLIP", 6.f, false, light},
        };
        addChild(labels);

        TypeDisplay* display = createWidget<TypeDisplay>(mm2px(Vec(3.5f, 17.f)));
        display->box.size = mm2px(Vec(53.96f, 12.f));
        display->module = module;
        addChild(display);

        addParam(createParamCentered<TL1105>(mm2px(Vec(7.5f, 42.f)), module, HChipFilter::TYPE_PREV_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(16.5f, 42.f)), module, HChipFilter::TYPE_NEXT_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(48.96f, 42.f)), module, HChipFilter::DRIVE_PARAM));
        addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(17.5f, 59.f)), module, HChipFilter::CUTOFF_PARAM));
        addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(43.46f, 59.f)), module, HChipFilter::Q_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(17.5f, 80.f)), module, HChipFilter::CUTOFF_CV_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(43.46f, 80.f)), module, HChipFilter::Q_CV_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.5f, 94.f)), module, HChipFilter::CUTOFF_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.46f, 94.f)), module, HChipFilter::Q_INPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.f, 116.f)), module, HChipFilter::IN_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(48.96f, 116.f)), module, HChipFilter::OUT_OUTPUT));
        addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(30.48f, 116.f)), module, HChipFilter::CLIP_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        HChipFilter* module = dynamic_cast<HChipFilter*>(this->module);
        if (!module) return;
        menu->addChild(new MenuSeparator);

        std::vector<std::string> names;
        for (int i = 0; i < kNumTypes; i++) names.push_back(emuz::filterDesc(kMenuOf[i]).name);
        menu->addChild(createIndexSubmenuItem(
            "Filter type", names,
            [=]() { return (size_t)module->currentType(); },
            [=](size_t i) { setTypeParam(module, (int)i); }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Chip datapath (not fixed by the patent)"));
        menu->addChild(createIndexSubmenuItem(
            "Multiplier operand (LATCH 18)", {"16 bits, saturating", "17 bits, saturating", "18 bits, saturating"},
            [=]() { return clamp(module->multInBits - 16, 0, 2); },
            [=](int i) { module->multInBits = 16 + i; }));
        menu->addChild(createIndexSubmenuItem(
            "Product rounding", {"Floor (arithmetic shift)", "Nearest"},
            [=]() { return module->productRound; },
            [=](int i) { module->productRound = i; }));
        menu->addChild(createBoolPtrMenuItem("Saturating accumulator", "", &module->accSaturate));
    }
};

Model* modelHChipFilter = createModel<HChipFilter, HChipFilterWidget>("HChipFilter");
