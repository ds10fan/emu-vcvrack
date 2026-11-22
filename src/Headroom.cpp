// Headroom: one GAIN knob (-36..+36 dB). IN 1 -> OUT 1 is multiplied by the gain, IN 2 -> OUT 2 is
// divided by it, so whatever sits between OUT 1 and IN 2 runs hotter (or quieter) at unchanged
// overall level. Polyphonic.
#include "plugin.hpp"

#include "widgets.hpp"

struct Headroom : Module {
    enum ParamId { GAIN_PARAM, PARAMS_LEN };
    enum InputId { IN1_INPUT, IN2_INPUT, INPUTS_LEN };
    enum OutputId { OUT1_OUTPUT, OUT2_OUTPUT, OUTPUTS_LEN };
    enum LightId { LIGHTS_LEN };

    Headroom() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(GAIN_PARAM, -36.f, 36.f, 0.f, "Gain", " dB");
        configInput(IN1_INPUT, "Audio 1 (gained up)");
        configInput(IN2_INPUT, "Audio 2 (gained back down)");
        configOutput(OUT1_OUTPUT, "Audio 1");
        configOutput(OUT2_OUTPUT, "Audio 2");
        configBypass(IN1_INPUT, OUT1_OUTPUT);
        configBypass(IN2_INPUT, OUT2_OUTPUT);
    }

    void process(const ProcessArgs& args) override {
        const float up = std::pow(10.f, params[GAIN_PARAM].getValue() / 20.f);
        const float down = 1.f / up;
        scale(inputs[IN1_INPUT], outputs[OUT1_OUTPUT], up);
        scale(inputs[IN2_INPUT], outputs[OUT2_OUTPUT], down);
    }

    static void scale(Input& in, Output& out, float gain) {
        const int channels = std::max(1, in.getChannels());
        for (int c = 0; c < channels; c++)
            out.setVoltage(in.getVoltage(c) * gain, c);
        out.setChannels(channels);
    }
};

struct HeadroomWidget : ModuleWidget {
    HeadroomWidget(Headroom* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Headroom.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        const float x1 = 8.2f, x2 = 22.28f;
        PanelLabels* labels = new PanelLabels;
        labels->box = Rect(Vec(0, 0), box.size);
        labels->labels = {
            {Vec(15.24f, 6.f), "HEAD", 13.f, true, kPanelLight},
            {Vec(15.24f, 11.5f), "ROOM", 8.f, false, kPanelGrey},
            {Vec(15.24f, 50.5f), "GAIN", 8.f, true, kPanelDark},
            {Vec(15.24f, 55.f), "±36 dB", 7.f, false, kPanelDark},
            {Vec(x1, 80.5f), "+dB", 8.f, true, kPanelDark},
            {Vec(x2, 80.5f), "−dB", 8.f, true, kPanelDark},
            {Vec(x1, 89.5f), "IN 1", 7.f, false, kPanelDark},
            {Vec(x2, 89.5f), "IN 2", 7.f, false, kPanelDark},
            {Vec(x1, 109.6f), "OUT 1", 7.f, false, kPanelLight},
            {Vec(x2, 109.6f), "OUT 2", 7.f, false, kPanelLight},
        };
        addChild(labels);

        addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(15.24f, 40.f)), module, Headroom::GAIN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1, 96.f)), module, Headroom::IN1_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x2, 96.f)), module, Headroom::IN2_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x1, 116.f)), module, Headroom::OUT1_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x2, 116.f)), module, Headroom::OUT2_OUTPUT));
    }
};

Model* modelHeadroom = createModel<Headroom, HeadroomWidget>("Headroom");
