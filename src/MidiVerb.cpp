// MidiVerb: the Alesis MIDIverb / MIDIFex / MIDIVerb 2 ROM emulator of
// http://ibawizard.net/midiverb/ (dsp/midiverb.*), with the page's controls: device (ROM),
// program, dry/wet mix and feedback. 5 V is 1.0 of the page's Web Audio range. The machine runs
// on every second engine sample, as the page runs it on every second Web Audio sample.
#include "plugin.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "midiverb.hpp"
#include "widgets.hpp"

struct MidiVerb : Module {
    enum ParamId { ROM_PARAM, PROGRAM_PARAM, PREV_PARAM, NEXT_PARAM, MIX_PARAM, FEEDBACK_PARAM, PARAMS_LEN };
    enum InputId { LEFT_INPUT, RIGHT_INPUT, INPUTS_LEN };
    enum OutputId { LEFT_OUTPUT, RIGHT_OUTPUT, OUTPUTS_LEN };
    enum LightId { LIGHTS_LEN };

    static constexpr float kFullScale = 5.f;

    midiverb::Emulator emu;
    dsp::BooleanTrigger prevTrigger, nextTrigger;

    MidiVerb() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        std::vector<std::string> roms;
        for (int i = 0; i < midiverb::romCount(); i++) roms.push_back(midiverb::rom(i).name);
        configSwitch(ROM_PARAM, 0.f, midiverb::romCount() - 1, 2.f, "Device", roms);
        // The program as an index into the ROM's list (MIDIVerb and MIDIFex number from 1,
        // MIDIVerb 2 from 0); the page's default is program 1 of the MIDIVerb 2.
        configParam(PROGRAM_PARAM, 0.f, 99.f, 1.f, "Program index");
        paramQuantities[PROGRAM_PARAM]->snapEnabled = true;
        configButton(PREV_PARAM, "Previous program");
        configButton(NEXT_PARAM, "Next program");
        configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Dry / wet", "%", 0.f, 100.f);
        configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.f, "Feedback", "%", 0.f, 100.f);
        configInput(LEFT_INPUT, "Left");
        configInput(RIGHT_INPUT, "Right (normalled to left)");
        configOutput(LEFT_OUTPUT, "Left");
        configOutput(RIGHT_OUTPUT, "Right");
        configBypass(LEFT_INPUT, LEFT_OUTPUT);
        configBypass(RIGHT_INPUT, RIGHT_OUTPUT);
    }

    int romIndex() { return clamp((int)std::lround(params[ROM_PARAM].getValue()), 0, midiverb::romCount() - 1); }
    int programIndex() {
        return clamp((int)std::lround(params[PROGRAM_PARAM].getValue()), 0, midiverb::rom(romIndex()).programCount() - 1);
    }
    int programNumber() { return midiverb::rom(romIndex()).firstProgram + programIndex(); }

    void stepProgram(int delta) {
        const int n = midiverb::rom(romIndex()).programCount();
        int p = (programIndex() + delta) % n;
        if (p < 0) p += n;
        params[PROGRAM_PARAM].setValue(p);
    }

    void process(const ProcessArgs& args) override {
        if (prevTrigger.process(params[PREV_PARAM].getValue() > 0.f)) stepProgram(-1);
        if (nextTrigger.process(params[NEXT_PARAM].getValue() > 0.f)) stepProgram(1);
        const int rom = romIndex();
        if (rom != emu.romIndex()) {
            emu.selectRom(rom);
            if (programNumber() != emu.program()) emu.selectProgram(programNumber());
        } else if (programNumber() != emu.program()) {
            emu.selectProgram(programNumber());
        }
        emu.dryWetMix = params[MIX_PARAM].getValue();
        emu.feedbackAmount = params[FEEDBACK_PARAM].getValue();

        const float l = inputs[LEFT_INPUT].getVoltage();
        const float r = inputs[RIGHT_INPUT].isConnected() ? inputs[RIGHT_INPUT].getVoltage() : l;
        double ol, orr;
        emu.process(double(l / kFullScale), double(r / kFullScale), ol, orr);
        outputs[LEFT_OUTPUT].setVoltage(float(ol) * kFullScale);
        outputs[RIGHT_OUTPUT].setVoltage(float(orr) * kFullScale);
    }
};

// Sets ROM and program from the UI thread, as one undoable step.
static void selectFromUi(MidiVerb* m, int rom, int index) {
    history::ComplexAction* h = new history::ComplexAction;
    h->name = "change MidiVerb program";
    const int ids[2] = {MidiVerb::ROM_PARAM, MidiVerb::PROGRAM_PARAM};
    const float values[2] = {float(rom), float(index)};
    for (int k = 0; k < 2; k++) {
        history::ParamChange* p = new history::ParamChange;
        p->moduleId = m->id;
        p->paramId = ids[k];
        p->oldValue = m->params[ids[k]].getValue();
        p->newValue = values[k];
        h->push(p);
        APP->engine->setParamValue(m, ids[k], values[k]);
    }
    APP->history->push(h);
}

// Device, program number and name; a click opens the device and program menu, the wheel steps.
struct ProgramDisplay : widget::Widget {
    MidiVerb* module = nullptr;
    float scrollAccum = 0.f;

    void openMenu() {
        MidiVerb* m = module;
        ui::Menu* menu = createMenu();
        menu->addChild(createMenuLabel("Device"));
        for (int k = 0; k < midiverb::romCount(); k++) {
            menu->addChild(createCheckMenuItem(midiverb::rom(k).name, "", [=]() { return m->romIndex() == k; },
                                               [=]() { if (m->romIndex() != k) selectFromUi(m, k, 0); }));
        }
        menu->addChild(new MenuSeparator);
        const int rom = m->romIndex();
        const midiverb::RomInfo& r = midiverb::rom(rom);
        const int n = r.programCount();
        for (int g = 0; g < n; g += 10) {
            const int hi = std::min(n, g + 10) - 1;
            std::string label = string::f("%d - %d", r.firstProgram + g, r.firstProgram + hi);
            bool here = m->programIndex() >= g && m->programIndex() <= hi;
            menu->addChild(createSubmenuItem(label, here ? CHECKMARK_STRING : "", [=](ui::Menu* sub) {
                for (int i = g; i <= hi; i++) {
                    std::string t = string::f("%d  %s", r.firstProgram + i, r.effectNames[i]);
                    sub->addChild(createCheckMenuItem(t, "", [=]() { return m->programIndex() == i; },
                                                      [=]() { selectFromUi(m, rom, i); }));
                }
            }));
        }
    }

    void onButton(const ButtonEvent& e) override {
        Widget::onButton(e);
        if (e.isConsumed() || !module) return;
        if (e.action == GLFW_PRESS && (e.button == GLFW_MOUSE_BUTTON_LEFT || e.button == GLFW_MOUSE_BUTTON_RIGHT)) {
            e.consume(this);
            openMenu();
        }
    }

    void onHoverScroll(const HoverScrollEvent& e) override {
        if (!module) return;
        e.consume(this);
#if defined ARCH_MAC
        const float notch = 10.f;
#else
        const float notch = 50.f;
#endif
        scrollAccum += e.scrollDelta.y;
        int steps = (int)(scrollAccum / notch);
        if (steps != 0) {
            scrollAccum -= steps * notch;
            const int n = midiverb::rom(module->romIndex()).programCount();
            int p = clamp(module->programIndex() - steps, 0, n - 1);
            if (p != module->programIndex()) selectFromUi(module, module->romIndex(), p);
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
                std::string l1 = "MIDIVerb 2   1", l2 = "Small Bright .1 Sec";
                if (module) {
                    const midiverb::RomInfo& r = midiverb::rom(module->romIndex());
                    l1 = string::f("%-10s %3d", r.name, module->programNumber());
                    l2 = r.effectNames[module->programIndex()];
                }
                nvgFontFaceId(args.vg, font->handle);
                nvgTextLetterSpacing(args.vg, 0.f);
                nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgScissor(args.vg, 1, 0, box.size.x - 2, box.size.y);
                nvgFontSize(args.vg, 13.f);
                nvgFillColor(args.vg, nvgRGB(0xff, 0x50, 0x40));
                nvgText(args.vg, 5.f, box.size.y * 0.3f, l1.c_str(), nullptr);
                nvgFontSize(args.vg, 11.f);
                nvgFillColor(args.vg, nvgRGB(0xe0, 0x40, 0x30));
                nvgText(args.vg, 5.f, box.size.y * 0.72f, l2.c_str(), nullptr);
                nvgResetScissor(args.vg);
            }
        }
        Widget::drawLayer(args, layer);
    }
};

struct MidiVerbWidget : ModuleWidget {
    MidiVerbWidget(MidiVerb* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/MidiVerb.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        const float W = 50.8f;  // 10 HP
        PanelLabels* labels = new PanelLabels;
        labels->box = Rect(Vec(0, 0), box.size);
        labels->labels = {
            {Vec(W / 2, 7.5f), "MIDIVERB", 14.f, true, kPanelLight},
            {Vec(W / 2, 13.2f), "ROM emulation", 7.f, false, kPanelGrey},
            {Vec(W / 2, 35.5f), "PROGRAM", 7.f, true, kPanelDark},
            {Vec(17.f, 46.5f), "<", 8.f, true, kPanelDark},
            {Vec(33.8f, 46.5f), ">", 8.f, true, kPanelDark},
            {Vec(13.f, 54.f), "MIX", 8.f, true, kPanelDark},
            {Vec(37.8f, 54.f), "FEEDBACK", 7.f, true, kPanelDark},
            {Vec(13.f, 87.5f), "IN", 8.f, true, kPanelDark},
            {Vec(37.8f, 87.5f), "OUT", 8.f, true, kPanelDark},
            {Vec(5.f, 96.f), "L", 7.f, true, kPanelDark},
            {Vec(5.f, 111.f), "R", 7.f, true, kPanelDark},
        };
        addChild(labels);

        ProgramDisplay* display = createWidget<ProgramDisplay>(mm2px(Vec(3.5f, 18.f)));
        display->box.size = mm2px(Vec(43.8f, 13.f));
        display->module = module;
        addChild(display);

        addParam(createParamCentered<TL1105>(mm2px(Vec(17.f, 41.f)), module, MidiVerb::PREV_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(33.8f, 41.f)), module, MidiVerb::NEXT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.f, 64.f)), module, MidiVerb::MIX_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(37.8f, 64.f)), module, MidiVerb::FEEDBACK_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.f, 96.f)), module, MidiVerb::LEFT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.f, 111.f)), module, MidiVerb::RIGHT_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(37.8f, 96.f)), module, MidiVerb::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(37.8f, 111.f)), module, MidiVerb::RIGHT_OUTPUT));
    }
};

Model* modelMidiVerb = createModel<MidiVerb, MidiVerbWidget>("MidiVerb");
