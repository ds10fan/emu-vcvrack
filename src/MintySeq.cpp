// minty-seq: a 7-track CV/gate recorder (dsp/mintyseq.hpp) with its own clock or an external
// one, A/B patterns per track, ten sets, buttons, trigger inputs and keyboard shortcuts for the
// transport. The selected track passes the live input through while a key is held.
#include "plugin.hpp"

#include <atomic>
#include <cmath>
#include <string>

#include "mintyseq.hpp"
#include "widgets.hpp"

struct MintySeq : Module {
    enum ParamId {
        BPM_PARAM,
        PLAY_PARAM,
        REC_PARAM,
        CLEAR_PARAM,
        SET_PREV_PARAM,
        SET_NEXT_PARAM,
        ENUMS(SEL_PARAM, minty::kTracks),
        ENUMS(AB_PARAM, minty::kTracks),
        PARAMS_LEN
    };
    enum InputId { CV_INPUT, GATE_INPUT, CLOCK_INPUT, RESET_INPUT, PLAY_INPUT, REC_INPUT, CLEAR_INPUT, INPUTS_LEN };
    enum OutputId { ENUMS(CV_OUTPUT, minty::kTracks), ENUMS(GATE_OUTPUT, minty::kTracks), CLICK_OUTPUT, OUTPUTS_LEN };
    enum LightId { PLAY_LIGHT, REC_LIGHT, ENUMS(SEL_LIGHT, minty::kTracks), ENUMS(AB_LIGHT, minty::kTracks), LIGHTS_LEN };

    // Requests from the UI thread (keyboard), consumed in process().
    enum Request : uint32_t {
        REQ_RUN = 1u << 0,
        REQ_REC = 1u << 1,
        REQ_CLEAR = 1u << 2,
        REQ_SET_PREV = 1u << 3,
        REQ_SET_NEXT = 1u << 4,
        REQ_AB = 1u << 5,
        REQ_TRACK_UP = 1u << 6,
        REQ_TRACK_DOWN = 1u << 7,
    };
    std::atomic<uint32_t> requests{0};
    std::atomic<int> requestTrack{-1};  // select this track (from the UI)

    minty::Sequencer seq;
    dsp::BooleanTrigger playBtn, recBtn, clearBtn, prevBtn, nextBtn, selBtn[minty::kTracks], abBtn[minty::kTracks];
    dsp::SchmittTrigger gateIn, clockIn, resetIn, playIn, recIn, clearIn;
    float gapTimer[minty::kTracks] = {0, 0, 0, 0, 0, 0, 0};
    bool liveGate = false;
    dsp::ClockDivider lightDivider;

    // Metronome click: white noise through a high-pass with an exponential decay, on every beat,
    // louder and longer on the first beat of a bar.
    uint32_t noiseState = 0x9E3779B9u;
    float clickEnv = 0.f, clickDecay = 0.f;
    float hp1In = 0.f, hp1Out = 0.f, hp2In = 0.f, hp2Out = 0.f;
    static constexpr float kClickHpHz = 2000.f;
    static constexpr float kClickBeatMs = 12.f, kClickBarMs = 25.f;
    static constexpr float kClickBeatLevel = 0.6f, kClickBarLevel = 1.f;
    static constexpr float kClickVolts = 5.f;

    float whiteNoise() {
        noiseState ^= noiseState << 13;
        noiseState ^= noiseState >> 17;
        noiseState ^= noiseState << 5;
        return float(int32_t(noiseState)) / 2147483648.f;
    }

    MintySeq() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(BPM_PARAM, 30.f, 300.f, 127.f, "Tempo", " BPM");
        paramQuantities[BPM_PARAM]->snapEnabled = true;
        configButton(PLAY_PARAM, "Start / stop (Space)");
        configButton(REC_PARAM, "Record on the selected track (Enter)");
        configButton(CLEAR_PARAM, "Clear the selected track's pattern (Shift+Backspace)");
        configButton(SET_PREV_PARAM, "Previous set (Left)");
        configButton(SET_NEXT_PARAM, "Next set (Right)");
        for (int t = 0; t < minty::kTracks; t++) {
            configButton(SEL_PARAM + t, string::f("Select track %d (Up / Down)", t + 1));
            configButton(AB_PARAM + t, string::f("Track %d pattern A / B (Tab for the selected track)", t + 1));
            configOutput(CV_OUTPUT + t, string::f("Track %d CV", t + 1));
            configOutput(GATE_OUTPUT + t, string::f("Track %d gate", t + 1));
            configLight(SEL_LIGHT + t, string::f("Track %d selected", t + 1));
            configLight(AB_LIGHT + t, string::f("Track %d plays pattern B", t + 1));
        }
        configInput(CV_INPUT, "CV to record");
        configInput(GATE_INPUT, "Gate to record");
        configInput(CLOCK_INPUT, "Clock, one pulse per sixteenth (overrides the tempo)");
        configInput(RESET_INPUT, "Reset to bar 1");
        configInput(PLAY_INPUT, "Start / stop trigger");
        configInput(REC_INPUT, "Record trigger");
        configInput(CLEAR_INPUT, "Clear trigger");
        configOutput(CLICK_OUTPUT, "Metronome click (audio), accented on bar 1");
        configLight(PLAY_LIGHT, "Running");
        configLight(REC_LIGHT, "Recording");
        lightDivider.setDivision(32);
    }

    void request(uint32_t r) { requests.fetch_or(r); }

    void onReset() override {
        seq.stop();
        seq.clearAll();
        seq.currentSet = 0;
        seq.currentTrack = 0;
        liveGate = false;
    }

    void process(const ProcessArgs& args) override {
        // keyboard and buttons
        uint32_t r = requests.exchange(0);
        if (playBtn.process(params[PLAY_PARAM].getValue() > 0.f)) r |= REQ_RUN;
        if (recBtn.process(params[REC_PARAM].getValue() > 0.f)) r |= REQ_REC;
        if (clearBtn.process(params[CLEAR_PARAM].getValue() > 0.f)) r |= REQ_CLEAR;
        if (prevBtn.process(params[SET_PREV_PARAM].getValue() > 0.f)) r |= REQ_SET_PREV;
        if (nextBtn.process(params[SET_NEXT_PARAM].getValue() > 0.f)) r |= REQ_SET_NEXT;
        if (playIn.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 1.f)) r |= REQ_RUN;
        if (recIn.process(inputs[REC_INPUT].getVoltage(), 0.1f, 1.f)) r |= REQ_REC;
        if (clearIn.process(inputs[CLEAR_INPUT].getVoltage(), 0.1f, 1.f)) r |= REQ_CLEAR;
        for (int t = 0; t < minty::kTracks; t++) {
            if (selBtn[t].process(params[SEL_PARAM + t].getValue() > 0.f)) seq.selectTrack(t);
            if (abBtn[t].process(params[AB_PARAM + t].getValue() > 0.f)) seq.toggleAB(t);
        }
        int rt = requestTrack.exchange(-1);
        if (rt >= 0) seq.selectTrack(rt);
        if (r & REQ_RUN) seq.toggleRun();
        if (r & REQ_REC) seq.toggleRecord();
        if (r & REQ_CLEAR) seq.clearCurrent();
        if (r & REQ_SET_PREV) seq.selectSet(seq.currentSet - 1);
        if (r & REQ_SET_NEXT) seq.selectSet(seq.currentSet + 1);
        if (r & REQ_AB) seq.toggleAB(seq.currentTrack);
        if (r & REQ_TRACK_UP) seq.selectTrack(seq.currentTrack - 1);
        if (r & REQ_TRACK_DOWN) seq.selectTrack(seq.currentTrack + 1);
        if (resetIn.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) seq.reset();

        // clock
        const double bpm = params[BPM_PARAM].getValue();
        const bool pulse = clockIn.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
        if (inputs[CLOCK_INPUT].isConnected())
            seq.tickExternal(pulse, args.sampleTime, 60.0 / bpm / 4.0);
        else
            seq.tickInternal(bpm / 60.0 * 4.0 * args.sampleTime);

        // the input: note on records, and the selected track monitors it
        const float cvIn = inputs[CV_INPUT].getVoltage();
        const bool wasHigh = liveGate;
        const bool rising = gateIn.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 1.f);
        liveGate = gateIn.isHigh();
        if (rising) seq.noteOn(cvIn);
        else if (wasHigh && !liveGate) seq.noteOff();

        // outputs
        for (int t = 0; t < minty::kTracks; t++) {
            float cv = seq.trackCv(t);
            bool g = seq.trackGate(t);
            if (seq.takeRetrigger(t)) gapTimer[t] = 1e-3f;
            if (gapTimer[t] > 0.f) {
                gapTimer[t] -= args.sampleTime;
                g = false;
            }
            if (t == seq.currentTrack && liveGate) {
                cv = cvIn;
                g = true;
            }
            outputs[CV_OUTPUT + t].setVoltage(cv);
            outputs[GATE_OUTPUT + t].setVoltage(g ? 10.f : 0.f);
        }

        // click
        int stepIndex;
        if (seq.takeStepEvent(stepIndex) && stepIndex % 4 == 0) {
            const bool bar = stepIndex == 0;
            clickEnv = bar ? kClickBarLevel : kClickBeatLevel;
            const float ms = bar ? kClickBarMs : kClickBeatMs;
            clickDecay = std::exp(-1.f / (ms * 1e-3f * args.sampleRate));
        }
        float click = 0.f;
        if (clickEnv > 1e-4f) {
            // two one-pole high-passes
            const float a = std::exp(-2.f * float(M_PI) * kClickHpHz * args.sampleTime);
            float x = whiteNoise();
            hp1Out = a * (hp1Out + x - hp1In);
            hp1In = x;
            hp2Out = a * (hp2Out + hp1Out - hp2In);
            hp2In = hp1Out;
            click = hp2Out * clickEnv * kClickVolts;
            clickEnv *= clickDecay;
        } else {
            clickEnv = 0.f;
        }
        outputs[CLICK_OUTPUT].setVoltage(click);

        if (lightDivider.process()) {
            lights[PLAY_LIGHT].setBrightness(seq.running ? 1.f : 0.f);
            lights[REC_LIGHT].setBrightness(seq.recording ? 1.f : 0.f);
            for (int t = 0; t < minty::kTracks; t++) {
                lights[SEL_LIGHT + t].setBrightness(t == seq.currentTrack ? 1.f : 0.f);
                lights[AB_LIGHT + t].setBrightness(seq.ab(t) ? 1.f : 0.f);
            }
        }
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "currentSet", json_integer(seq.currentSet));
        json_object_set_new(root, "currentTrack", json_integer(seq.currentTrack));
        json_t* setsJ = json_array();
        for (int s = 0; s < minty::kSets; s++) {
            json_t* setJ = json_object();
            json_t* abJ = json_array();
            for (int t = 0; t < minty::kTracks; t++) json_array_append_new(abJ, json_integer(seq.sets[s].ab[t]));
            json_object_set_new(setJ, "ab", abJ);
            json_t* tracksJ = json_array();
            for (int t = 0; t < minty::kTracks; t++) {
                json_t* patternsJ = json_array();
                for (int p = 0; p < minty::kPatterns; p++) {
                    json_t* stepsJ = json_array();
                    const minty::Pattern& pat = seq.pattern(s, t, p);
                    for (int i = 0; i < minty::kSteps; i++) {
                        const minty::Step& st = pat.steps[i];
                        if (!st.gate && !st.tie) continue;
                        json_t* stepJ = json_array();
                        json_array_append_new(stepJ, json_integer(i));
                        json_array_append_new(stepJ, json_real(st.cv));
                        json_array_append_new(stepJ, json_integer(st.gate ? 1 : 0));
                        json_array_append_new(stepJ, json_integer(st.tie ? 1 : 0));
                        json_array_append_new(stepsJ, stepJ);
                    }
                    json_array_append_new(patternsJ, stepsJ);
                }
                json_array_append_new(tracksJ, patternsJ);
            }
            json_object_set_new(setJ, "tracks", tracksJ);
            json_array_append_new(setsJ, setJ);
        }
        json_object_set_new(root, "sets", setsJ);
        return root;
    }

    void dataFromJson(json_t* root) override {
        seq.stop();
        seq.clearAll();
        if (json_t* j = json_object_get(root, "currentSet")) seq.currentSet = clamp((int)json_integer_value(j), 0, minty::kSets - 1);
        if (json_t* j = json_object_get(root, "currentTrack")) seq.currentTrack = clamp((int)json_integer_value(j), 0, minty::kTracks - 1);
        json_t* setsJ = json_object_get(root, "sets");
        if (!setsJ) return;
        for (int s = 0; s < minty::kSets; s++) {
            json_t* setJ = json_array_get(setsJ, s);
            if (!setJ) break;
            json_t* abJ = json_object_get(setJ, "ab");
            json_t* tracksJ = json_object_get(setJ, "tracks");
            for (int t = 0; t < minty::kTracks; t++) {
                if (abJ)
                    if (json_t* a = json_array_get(abJ, t)) seq.sets[s].ab[t] = clamp((int)json_integer_value(a), 0, minty::kPatterns - 1);
                json_t* patternsJ = tracksJ ? json_array_get(tracksJ, t) : nullptr;
                if (!patternsJ) continue;
                for (int p = 0; p < minty::kPatterns; p++) {
                    json_t* stepsJ = json_array_get(patternsJ, p);
                    if (!stepsJ) continue;
                    size_t n = json_array_size(stepsJ);
                    for (size_t k = 0; k < n; k++) {
                        json_t* stepJ = json_array_get(stepsJ, k);
                        if (!stepJ || json_array_size(stepJ) < 4) continue;
                        int i = (int)json_integer_value(json_array_get(stepJ, 0));
                        if (i < 0 || i >= minty::kSteps) continue;
                        minty::Step& st = seq.pattern(s, t, p).steps[i];
                        st.cv = (float)json_number_value(json_array_get(stepJ, 1));
                        st.gate = json_integer_value(json_array_get(stepJ, 2)) != 0;
                        st.tie = json_integer_value(json_array_get(stepJ, 3)) != 0;
                    }
                }
            }
        }
    }
};

// Status: set, selected track and pattern, position; tempo source and transport state.
struct StatusDisplay : widget::Widget {
    MintySeq* module = nullptr;

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
                std::string l1 = "SET 1   TRACK 1 A   BAR 1.1";
                std::string l2 = "127 BPM            STOP";
                if (module) {
                    const minty::Sequencer& q = module->seq;
                    l1 = string::f("SET %d   TRACK %d %c   BAR %d.%d", q.currentSet + 1, q.currentTrack + 1,
                                   q.ab(q.currentTrack) ? 'B' : 'A', q.bar() + 1, q.beat() + 1);
                    bool ext = module->inputs[MintySeq::CLOCK_INPUT].isConnected();
                    std::string tempo = ext ? "EXT CLOCK" : string::f("%d BPM", (int)std::lround(module->params[MintySeq::BPM_PARAM].getValue()));
                    l2 = string::f("%-10s  %s  %s", tempo.c_str(), q.running ? "RUN " : "STOP", q.recording ? "REC" : "   ");
                }
                nvgFontFaceId(args.vg, font->handle);
                nvgTextLetterSpacing(args.vg, 0.f);
                nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFontSize(args.vg, 12.f);
                nvgFillColor(args.vg, nvgRGB(0x60, 0xe0, 0xb0));
                nvgText(args.vg, 5.f, box.size.y * 0.3f, l1.c_str(), nullptr);
                nvgFontSize(args.vg, 11.f);
                nvgFillColor(args.vg, nvgRGB(0x40, 0xb0, 0x90));
                nvgText(args.vg, 5.f, box.size.y * 0.72f, l2.c_str(), nullptr);
            }
        }
        Widget::drawLayer(args, layer);
    }
};

// One track's 64 steps as cells, with the play head; the selected track is framed. Clicking it
// selects the track.
struct TrackLane : widget::OpaqueWidget {
    MintySeq* module = nullptr;
    int track = 0;

    void onButton(const ButtonEvent& e) override {
        if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            module->requestTrack.store(track);
            e.consume(this);
            return;
        }
        OpaqueWidget::onButton(e);
    }

    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 1.5f);
        nvgFillColor(args.vg, nvgRGB(0x12, 0x12, 0x14));
        nvgFill(args.vg);
        OpaqueWidget::draw(args);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            const float cellW = box.size.x / minty::kSteps;
            bool selected = module && module->seq.currentTrack == track;
            if (module) {
                const minty::Pattern& p = module->seq.playingPattern(track);
                for (int i = 0; i < minty::kSteps; i++) {
                    const minty::Step& s = p.steps[i];
                    if (!s.gate && !s.tie) continue;
                    nvgBeginPath(args.vg);
                    nvgRect(args.vg, i * cellW + 0.5f, 2.f, cellW - 1.f, box.size.y - 4.f);
                    nvgFillColor(args.vg, s.gate ? nvgRGB(0x60, 0xe0, 0xb0) : nvgRGB(0x30, 0x80, 0x68));
                    nvgFill(args.vg);
                }
                // bar lines
                nvgBeginPath(args.vg);
                for (int b = 1; b < 4; b++) {
                    nvgMoveTo(args.vg, b * 16 * cellW, 1.f);
                    nvgLineTo(args.vg, b * 16 * cellW, box.size.y - 1.f);
                }
                nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x30));
                nvgStrokeWidth(args.vg, 1.f);
                nvgStroke(args.vg);
                if (module->seq.running) {
                    nvgBeginPath(args.vg);
                    nvgRect(args.vg, module->seq.pos() * cellW, 0.f, cellW, box.size.y);
                    nvgFillColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x60));
                    nvgFill(args.vg);
                }
            }
            nvgBeginPath(args.vg);
            nvgRoundedRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f, 1.5f);
            nvgStrokeColor(args.vg, selected ? nvgRGB(0xff, 0xb0, 0x30) : nvgRGB(0x40, 0x40, 0x44));
            nvgStrokeWidth(args.vg, selected ? 1.5f : 1.f);
            nvgStroke(args.vg);
        }
        OpaqueWidget::drawLayer(args, layer);
    }
};

struct MintySeqWidget : ModuleWidget {
    MintySeqWidget(MintySeq* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/MintySeq.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        const float W = 111.76f;  // 22 HP
        PanelLabels* labels = new PanelLabels;
        labels->box = Rect(Vec(0, 0), box.size);
        labels->labels = {
            {Vec(W / 2, 7.5f), "MINTY-SEQ", 15.f, true, kPanelLight},
            {Vec(W / 2, 13.2f), "7-track sixteenth recorder", 7.f, false, kPanelGrey},
            {Vec(76.f, 30.8f), "BPM", 6.f, false, kPanelDark},
            {Vec(89.f, 30.8f), "CLOCK", 6.f, false, kPanelDark},
            {Vec(102.f, 30.8f), "RESET", 6.f, false, kPanelDark},
            {Vec(9.f, 35.2f), "SEL", 6.f, true, kPanelDark},
            {Vec(19.f, 35.2f), "A/B", 6.f, true, kPanelDark},
            {Vec(88.f, 35.2f), "CV", 6.f, true, kPanelDark},
            {Vec(102.f, 35.2f), "GATE", 6.f, true, kPanelDark},
            {Vec(12.f, 99.3f), "PLAY", 6.f, true, kPanelDark},
            {Vec(24.f, 99.3f), "REC", 6.f, true, kPanelDark},
            {Vec(36.f, 99.3f), "CLEAR", 6.f, true, kPanelDark},
            {Vec(65.f, 99.3f), "SET", 6.f, true, kPanelDark},
            {Vec(60.f, 103.5f), "<", 8.f, true, kPanelDark},
            {Vec(70.f, 103.5f), ">", 8.f, true, kPanelDark},
            {Vec(9.f, 110.2f), "CV", 7.f, true, kPanelLight},
            {Vec(22.f, 110.2f), "GATE", 7.f, true, kPanelLight},
            {Vec(40.f, 110.2f), "PLAY", 7.f, true, kPanelLight},
            {Vec(53.f, 110.2f), "REC", 7.f, true, kPanelLight},
            {Vec(66.f, 110.2f), "CLEAR", 7.f, true, kPanelLight},
            {Vec(100.f, 110.2f), "CLICK", 7.f, true, kPanelLight},
        };
        addChild(labels);

        StatusDisplay* status = createWidget<StatusDisplay>(mm2px(Vec(3.5f, 17.f)));
        status->box.size = mm2px(Vec(63.f, 12.f));
        status->module = module;
        addChild(status);

        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(76.f, 23.f)), module, MintySeq::BPM_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(89.f, 23.f)), module, MintySeq::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(102.f, 23.f)), module, MintySeq::RESET_INPUT));

        for (int t = 0; t < minty::kTracks; t++) {
            const float y = 39.5f + 9.f * t;
            addParam(createParamCentered<LEDButton>(mm2px(Vec(9.f, y)), module, MintySeq::SEL_PARAM + t));
            addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(9.f, y)), module, MintySeq::SEL_LIGHT + t));
            addParam(createParamCentered<LEDButton>(mm2px(Vec(19.f, y)), module, MintySeq::AB_PARAM + t));
            addChild(createLightCentered<MediumLight<BlueLight>>(mm2px(Vec(19.f, y)), module, MintySeq::AB_LIGHT + t));
            TrackLane* lane = createWidget<TrackLane>(mm2px(Vec(25.5f, y - 3.4f)));
            lane->box.size = mm2px(Vec(53.f, 6.8f));
            lane->module = module;
            lane->track = t;
            addChild(lane);
            addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(88.f, y)), module, MintySeq::CV_OUTPUT + t));
            addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(102.f, y)), module, MintySeq::GATE_OUTPUT + t));
        }

        addParam(createParamCentered<LEDButton>(mm2px(Vec(12.f, 103.5f)), module, MintySeq::PLAY_PARAM));
        addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(12.f, 103.5f)), module, MintySeq::PLAY_LIGHT));
        addParam(createParamCentered<LEDButton>(mm2px(Vec(24.f, 103.5f)), module, MintySeq::REC_PARAM));
        addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(24.f, 103.5f)), module, MintySeq::REC_LIGHT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(36.f, 103.5f)), module, MintySeq::CLEAR_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(55.f, 103.5f)), module, MintySeq::SET_PREV_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(75.f, 103.5f)), module, MintySeq::SET_NEXT_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(9.f, 116.5f)), module, MintySeq::CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.f, 116.5f)), module, MintySeq::GATE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.f, 116.5f)), module, MintySeq::PLAY_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(53.f, 116.5f)), module, MintySeq::REC_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(66.f, 116.5f)), module, MintySeq::CLEAR_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(100.f, 116.5f)), module, MintySeq::CLICK_OUTPUT));
    }

    // Keyboard shortcuts, while the mouse is over the module or after it was clicked. Keys are
    // chosen outside Rack's computer-keyboard MIDI map (letters, digits, brackets, punctuation)
    // and outside Rack's own module shortcuts (Delete/Backspace without modifiers, Ctrl combos).
    bool handleKey(int key, int mods, int action) {
        MintySeq* m = dynamic_cast<MintySeq*>(module);
        if (!m || action != GLFW_PRESS) return false;
        const int mod = mods & RACK_MOD_MASK;
        const bool none = mod == 0, shift = mod == GLFW_MOD_SHIFT;
        switch (key) {
            case GLFW_KEY_SPACE: if (none) { m->request(MintySeq::REQ_RUN); return true; } break;
            case GLFW_KEY_ENTER:
            case GLFW_KEY_KP_ENTER: if (none) { m->request(MintySeq::REQ_REC); return true; } break;
            case GLFW_KEY_BACKSPACE:
            case GLFW_KEY_DELETE: if (shift) { m->request(MintySeq::REQ_CLEAR); return true; } break;
            case GLFW_KEY_UP: if (none) { m->request(MintySeq::REQ_TRACK_UP); return true; } break;
            case GLFW_KEY_DOWN: if (none) { m->request(MintySeq::REQ_TRACK_DOWN); return true; } break;
            case GLFW_KEY_TAB: if (none) { m->request(MintySeq::REQ_AB); return true; } break;
            case GLFW_KEY_LEFT: if (none) { m->request(MintySeq::REQ_SET_PREV); return true; } break;
            case GLFW_KEY_RIGHT: if (none) { m->request(MintySeq::REQ_SET_NEXT); return true; } break;
            default: break;
        }
        return false;
    }

    void onHoverKey(const HoverKeyEvent& e) override {
        if (handleKey(e.key, e.mods, e.action)) {
            e.consume(this);
            return;
        }
        ModuleWidget::onHoverKey(e);
    }

    void onSelectKey(const SelectKeyEvent& e) override {
        if (handleKey(e.key, e.mods, e.action)) {
            e.consume(this);
            return;
        }
        ModuleWidget::onSelectKey(e);
    }

    void appendContextMenu(Menu* menu) override {
        MintySeq* m = dynamic_cast<MintySeq*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Keys (mouse over the module, or after clicking it)"));
        menu->addChild(createMenuLabel("Space start/stop   Enter record   Shift+Backspace clear"));
        menu->addChild(createMenuLabel("Up/Down track   Tab A/B   Left/Right set"));
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem("Clear the selected track's pattern", "", [=]() { m->request(MintySeq::REQ_CLEAR); }));
        menu->addChild(createMenuItem("Clear everything (all sets)", "", [=]() {
            m->seq.stop();
            m->seq.clearAll();
        }));
    }
};

Model* modelMintySeq = createModel<MintySeq, MintySeqWidget>("MintySeq");
