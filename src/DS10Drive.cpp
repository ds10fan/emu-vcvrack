// DS10 Drive: out = mix(in, clamp(in * drive * 4, -1, 1), drive) per sample (dsp/ds10_drive.hpp),
// with 5 V as full scale. Polyphonic; drive = knob + CV * attenuverter / 10 V, clamped to 0..1.
#include "plugin.hpp"

#include "ds10_drive.hpp"
#include "widgets.hpp"

struct DS10Drive : Module {
    enum ParamId { DRIVE_PARAM, DRIVE_CV_PARAM, PARAMS_LEN };
    enum InputId { IN_INPUT, DRIVE_INPUT, INPUTS_LEN };
    enum OutputId { OUT_OUTPUT, OUTPUTS_LEN };
    enum LightId { LIGHTS_LEN };

    static constexpr float kFullScale = 5.f;

    DS10Drive() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(DRIVE_PARAM, 0.f, 1.f, 0.f, "Drive", "%", 0.f, 100.f);
        configParam(DRIVE_CV_PARAM, -1.f, 1.f, 0.f, "Drive CV amount", "%", 0.f, 100.f);
        configInput(IN_INPUT, "Audio");
        configInput(DRIVE_INPUT, "Drive CV (10 V for the full range)");
        configOutput(OUT_OUTPUT, "Audio");
        configBypass(IN_INPUT, OUT_OUTPUT);
    }

    void process(const ProcessArgs& args) override {
        const int channels = std::max(1, inputs[IN_INPUT].getChannels());
        const float knob = params[DRIVE_PARAM].getValue();
        const float cv = params[DRIVE_CV_PARAM].getValue() / 10.f;
        for (int c = 0; c < channels; c++) {
            float drive = clamp(knob + inputs[DRIVE_INPUT].getPolyVoltage(c) * cv, 0.f, 1.f);
            float in = inputs[IN_INPUT].getPolyVoltage(c) / kFullScale;
            outputs[OUT_OUTPUT].setVoltage(ds10::oneSample(in, drive) * kFullScale, c);
        }
        outputs[OUT_OUTPUT].setChannels(channels);
    }
};

struct DS10DriveWidget : ModuleWidget {
    DS10DriveWidget(DS10Drive* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/DS10Drive.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        PanelLabels* labels = new PanelLabels;
        labels->box = Rect(Vec(0, 0), box.size);
        labels->labels = {
            {Vec(15.24f, 6.f), "DS10", 13.f, true, kPanelLight},
            {Vec(15.24f, 11.5f), "DRIVE", 8.f, false, kPanelGrey},
            {Vec(15.24f, 50.5f), "DRIVE", 8.f, true, kPanelDark},
            {Vec(15.24f, 80.5f), "CV", 7.f, false, kPanelDark},
            {Vec(15.24f, 89.5f), "IN", 8.f, true, kPanelDark},
            {Vec(15.24f, 109.6f), "OUT", 8.f, true, kPanelLight},
        };
        addChild(labels);

        addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(15.24f, 40.f)), module, DS10Drive::DRIVE_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(15.24f, 60.f)), module, DS10Drive::DRIVE_CV_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.24f, 73.f)), module, DS10Drive::DRIVE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.24f, 96.f)), module, DS10Drive::IN_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(15.24f, 116.f)), module, DS10Drive::OUT_OUTPUT));
    }
};

Model* modelDS10Drive = createModel<DS10Drive, DS10DriveWidget>("DS10Drive");
