// G-Chip: a sampler whose playback resampler is the US 5,111,727 interpolator (dsp/gchip.*).
// Loads a WAV (context menu, drag and drop, or a click on the display), plays it on a gate with
// a V/oct pitch, with the loop modes Off (one-shot), Gate (loop while the gate is high, then
// play to the end) and On (loop until retriggered). One voice per polyphony channel.
#include "plugin.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <osdialog.h>

#include "gchip.hpp"
#include "wav.hpp"
#include "widgets.hpp"

struct GChip : Module {
    enum ParamId { TUNE_PARAM, FINE_PARAM, START_PARAM, LOOP_START_PARAM, LOOP_END_PARAM, LOOP_MODE_PARAM, PARAMS_LEN };
    enum InputId { VOCT_INPUT, GATE_INPUT, INPUTS_LEN };
    enum OutputId { OUT_OUTPUT, EOC_OUTPUT, OUTPUTS_LEN };
    enum LightId { LIGHTS_LEN };

    static constexpr float kFullScale = 5.f;  // volts for a 16-bit full-scale sample

    // The sample: `loaded` is what the UI shows (set by the loader), `pending` waits for the audio
    // thread, which moves it into `engineSample` and parks the old one in `retired` for the UI
    // thread to free. Voices hold raw pointers into engineSample, so a swap stops them first.
    std::mutex sampleMutex;
    std::shared_ptr<gchip::Sample> loaded, pending, retired;
    std::atomic<bool> pendingFlag{false};
    std::shared_ptr<gchip::Sample> engineSample;
    std::string samplePath;   // as stored in the patch
    std::string sampleName;   // for the display
    std::string sampleInfo;   // "48000 Hz, 44032 samples, 0.92 s"
    std::string loadError;

    bool fixedPoint = false;

    gchip::Voice voices[PORT_MAX_CHANNELS];
    dsp::SchmittTrigger gates[PORT_MAX_CHANNELS];
    dsp::PulseGenerator eoc[PORT_MAX_CHANNELS];
    std::atomic<int64_t> playPos[PORT_MAX_CHANNELS];

    struct TuneQuantity : ParamQuantity {
        std::string getDisplayValueString() override {
            float v = getValue();
            return string::f("%+.2f", v);
        }
    };

    GChip() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam<TuneQuantity>(TUNE_PARAM, -24.f, 24.f, 0.f, "Tune", " st");
        configParam(FINE_PARAM, -100.f, 100.f, 0.f, "Fine tune", " cents");
        configParam(START_PARAM, 0.f, 1.f, 0.f, "Start", "%", 0.f, 100.f);
        configParam(LOOP_START_PARAM, 0.f, 1.f, 0.f, "Loop start", "%", 0.f, 100.f);
        configParam(LOOP_END_PARAM, 0.f, 1.f, 1.f, "Loop end", "%", 0.f, 100.f);
        configSwitch(LOOP_MODE_PARAM, 0.f, 2.f, 0.f, "Loop", {"Off", "While gate is high", "On"});
        configInput(VOCT_INPUT, "1 V/oct pitch");
        configInput(GATE_INPUT, "Gate");
        configOutput(OUT_OUTPUT, "Audio");
        configOutput(EOC_OUTPUT, "End of sample trigger");
        for (auto& p : playPos) p.store(-1);
    }

    // ---- loading (UI thread) ----
    bool loadSample(const std::string& path, bool applyLoopPoints) {
        wav::File f;
        std::string err;
        if (!wav::load(path, f, err)) {
            std::lock_guard<std::mutex> lock(sampleMutex);
            loadError = system::getFilename(path) + ": " + err;
            return false;
        }
        auto s = std::make_shared<gchip::Sample>();
        s->data = std::move(f.mono);
        s->rate = f.rate;
        s->hasLoop = f.hasLoop;
        s->loopStart = f.loopStart;
        s->loopEnd = f.loopEnd;
        {
            std::lock_guard<std::mutex> lock(sampleMutex);
            loaded = s;
            pending = s;
            pendingFlag.store(true);
            retired.reset();
            samplePath = path;
            sampleName = system::getFilename(path);
            sampleInfo = string::f("%.0f Hz  %d-bit%s %s  %lld smp  %.2f s", f.rate, f.bitsPerSample,
                                   f.isFloat ? " float" : "", f.channels == 1 ? "mono" : (f.channels == 2 ? "stereo" : string::f("%d ch", f.channels).c_str()),
                                   (long long)s->data.size(), double(s->data.size()) / f.rate);
            loadError.clear();
        }
        if (applyLoopPoints && !s->data.empty()) {
            const double len = double(s->data.size());
            APP->engine->setParamValue(this, START_PARAM, 0.f);
            if (s->hasLoop) {
                APP->engine->setParamValue(this, LOOP_START_PARAM, float(s->loopStart / len));
                APP->engine->setParamValue(this, LOOP_END_PARAM, float(s->loopEnd / len));
            } else {
                APP->engine->setParamValue(this, LOOP_START_PARAM, 0.f);
                APP->engine->setParamValue(this, LOOP_END_PARAM, 1.f);
            }
        }
        return true;
    }

    void clearSample() {
        std::lock_guard<std::mutex> lock(sampleMutex);
        loaded.reset();
        pending.reset();
        pendingFlag.store(true);
        samplePath.clear();
        sampleName.clear();
        sampleInfo.clear();
        loadError.clear();
    }

    // The UI thread frees the sample the audio thread retired.
    void collectRetired() {
        std::shared_ptr<gchip::Sample> old;
        {
            std::lock_guard<std::mutex> lock(sampleMutex);
            old.swap(retired);
        }
    }

    std::shared_ptr<gchip::Sample> uiSample() {
        std::lock_guard<std::mutex> lock(sampleMutex);
        return loaded;
    }

    // ---- audio thread ----
    void takePending() {
        std::lock_guard<std::mutex> lock(sampleMutex);
        for (auto& v : voices) v.stop();
        for (auto& p : playPos) p.store(-1);
        retired = engineSample;
        engineSample = pending;
        pending.reset();
        pendingFlag.store(false);
    }

    void onReset() override {
        for (auto& v : voices) v.stop();
        clearSample();
    }

    void process(const ProcessArgs& args) override {
        if (pendingFlag.load()) takePending();
        const gchip::Sample* s = engineSample.get();
        const int channels = std::max(1, std::max(inputs[GATE_INPUT].getChannels(), inputs[VOCT_INPUT].getChannels()));
        const int64_t len = s ? int64_t(s->data.size()) : 0;
        const gchip::LoopMode mode = gchip::LoopMode(clamp((int)std::lround(params[LOOP_MODE_PARAM].getValue()), 0, 2));
        const float tune = params[TUNE_PARAM].getValue() / 12.f + params[FINE_PARAM].getValue() / 1200.f;
        const double rateRatio = s ? s->rate / args.sampleRate : 1.0;

        for (int c = 0; c < channels; c++) {
            gchip::Voice& v = voices[c];
            float gate = inputs[GATE_INPUT].getPolyVoltage(c);
            if (gates[c].process(gate, 0.1f, 1.f)) {
                if (s && len > 0) {
                    int64_t start = int64_t(std::lround(params[START_PARAM].getValue() * double(len - 1)));
                    int64_t ls = int64_t(std::lround(params[LOOP_START_PARAM].getValue() * double(len)));
                    int64_t le = int64_t(std::lround(params[LOOP_END_PARAM].getValue() * double(len)));
                    v.trigger(s, start, ls, le, mode);
                }
            } else if (gate < 0.1f) {
                v.release();
            }
            if (v.active()) {
                double ratio = rateRatio * std::exp2(double(tune) + inputs[VOCT_INPUT].getPolyVoltage(c));
                if (ratio < 1e-6) ratio = 1e-6;
                if (ratio > 64.0) ratio = 64.0;
                v.setIncrement(uint64_t(std::llround(ratio * 4294967296.0)));
            }
            bool ended = false;
            double y = v.process(fixedPoint, ended);
            if (ended) eoc[c].trigger(1e-3f);
            outputs[OUT_OUTPUT].setVoltage(float(y) * (kFullScale / 32768.f), c);
            outputs[EOC_OUTPUT].setVoltage(eoc[c].process(args.sampleTime) ? 10.f : 0.f, c);
            playPos[c].store(v.position(), std::memory_order_relaxed);
        }
        for (int c = channels; c < PORT_MAX_CHANNELS; c++) playPos[c].store(-1, std::memory_order_relaxed);
        outputs[OUT_OUTPUT].setChannels(channels);
        outputs[EOC_OUTPUT].setChannels(channels);
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        std::lock_guard<std::mutex> lock(sampleMutex);
        json_object_set_new(root, "samplePath", json_string(samplePath.c_str()));
        json_object_set_new(root, "fixedPoint", json_boolean(fixedPoint));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "fixedPoint")) fixedPoint = json_boolean_value(j);
        if (json_t* j = json_object_get(root, "samplePath")) {
            std::string path = json_string_value(j);
            if (!path.empty()) loadSample(path, false);
        }
    }
};

// Waveform, start and loop markers, play heads, name and format; click to load.
struct SampleDisplay : widget::OpaqueWidget {
    GChip* module = nullptr;
    const gchip::Sample* cachedSample = nullptr;
    int cachedColumns = 0;
    std::vector<std::pair<float, float>> peaks;  // per column: min, max in -1..1

    void loadDialog() {
        if (!module) return;
        std::string dir;
        {
            std::lock_guard<std::mutex> lock(module->sampleMutex);
            dir = module->samplePath.empty() ? "" : system::getDirectory(module->samplePath);
        }
        osdialog_filters* filters = osdialog_filters_parse("WAV:wav,WAV");
        char* path = osdialog_file(OSDIALOG_OPEN, dir.empty() ? nullptr : dir.c_str(), nullptr, filters);
        osdialog_filters_free(filters);
        if (path) {
            module->loadSample(path, true);
            std::free(path);
        }
    }

    void onButton(const ButtonEvent& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            e.consume(this);
            loadDialog();
            return;
        }
        OpaqueWidget::onButton(e);
    }

    void updatePeaks(const gchip::Sample* s, int columns) {
        if (s == cachedSample && columns == cachedColumns) return;
        cachedSample = s;
        cachedColumns = columns;
        peaks.assign(columns, {0.f, 0.f});
        if (!s || s->data.empty() || columns <= 0) return;
        const size_t n = s->data.size();
        for (int c = 0; c < columns; c++) {
            size_t a = size_t(double(c) / columns * n), b = size_t(double(c + 1) / columns * n);
            if (b <= a) b = a + 1;
            if (b > n) b = n;
            float lo = 1.f, hi = -1.f;
            // At most 512 points per column keep long files cheap; peaks are approximate.
            size_t step = std::max<size_t>(1, (b - a) / 512);
            for (size_t i = a; i < b; i += step) {
                float v = s->data[i] / 32768.f;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            peaks[c] = {lo, hi};
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
        OpaqueWidget::draw(args);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) {
            OpaqueWidget::drawLayer(args, layer);
            return;
        }
        std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        const float textH = 11.f;
        const float waveTop = textH + 3.f, waveBot = box.size.y - textH - 3.f;
        const float waveH = waveBot - waveTop;
        const float mid = waveTop + waveH / 2;
        std::shared_ptr<gchip::Sample> s = module ? module->uiSample() : nullptr;
        std::string name, info, error;
        if (module) {
            std::lock_guard<std::mutex> lock(module->sampleMutex);
            name = module->sampleName;
            info = module->sampleInfo;
            error = module->loadError;
        } else {
            name = "sample.wav";
            info = "48000 Hz  16-bit mono  44032 smp  0.92 s";
        }

        // waveform
        nvgSave(args.vg);
        nvgScissor(args.vg, 1, waveTop, box.size.x - 2, waveH);
        nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xb0, 0x30, 0x60));
        nvgStrokeWidth(args.vg, 1.f);
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 1, mid);
        nvgLineTo(args.vg, box.size.x - 1, mid);
        nvgStroke(args.vg);
        int columns = int(box.size.x) - 2;
        if (s && !s->data.empty()) {
            updatePeaks(s.get(), columns);
            nvgBeginPath(args.vg);
            for (int c = 0; c < columns; c++) {
                float x = 1.f + c + 0.5f;
                float y0 = mid - peaks[c].second * waveH / 2, y1 = mid - peaks[c].first * waveH / 2;
                if (y1 - y0 < 1.f) y1 = y0 + 1.f;
                nvgMoveTo(args.vg, x, y0);
                nvgLineTo(args.vg, x, y1);
            }
            nvgStrokeColor(args.vg, nvgRGB(0xff, 0xb0, 0x30));
            nvgStroke(args.vg);
            // loop region and start marker
            if (module) {
                float ls = module->params[GChip::LOOP_START_PARAM].getValue();
                float le = module->params[GChip::LOOP_END_PARAM].getValue();
                bool loopOn = module->params[GChip::LOOP_MODE_PARAM].getValue() > 0.5f;
                if (loopOn && le > ls) {
                    nvgBeginPath(args.vg);
                    nvgRect(args.vg, 1 + ls * columns, waveTop, (le - ls) * columns, waveH);
                    nvgFillColor(args.vg, nvgRGBA(0x40, 0xa0, 0xff, 0x30));
                    nvgFill(args.vg);
                    nvgBeginPath(args.vg);
                    nvgMoveTo(args.vg, 1 + ls * columns, waveTop);
                    nvgLineTo(args.vg, 1 + ls * columns, waveBot);
                    nvgMoveTo(args.vg, 1 + le * columns, waveTop);
                    nvgLineTo(args.vg, 1 + le * columns, waveBot);
                    nvgStrokeColor(args.vg, nvgRGB(0x40, 0xa0, 0xff));
                    nvgStroke(args.vg);
                }
                float st = module->params[GChip::START_PARAM].getValue();
                nvgBeginPath(args.vg);
                nvgMoveTo(args.vg, 1 + st * columns, waveTop);
                nvgLineTo(args.vg, 1 + st * columns, waveBot);
                nvgStrokeColor(args.vg, nvgRGB(0x40, 0xe0, 0x60));
                nvgStroke(args.vg);
                // play heads
                nvgBeginPath(args.vg);
                for (int c = 0; c < PORT_MAX_CHANNELS; c++) {
                    int64_t p = module->playPos[c].load(std::memory_order_relaxed);
                    if (p < 0) continue;
                    float x = 1 + float(double(p) / double(s->data.size())) * columns;
                    nvgMoveTo(args.vg, x, waveTop);
                    nvgLineTo(args.vg, x, waveBot);
                }
                nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0xc0));
                nvgStroke(args.vg);
            }
        }
        nvgRestore(args.vg);

        if (font) {
            nvgFontFaceId(args.vg, font->handle);
            nvgFontSize(args.vg, textH);
            nvgTextLetterSpacing(args.vg, 0.f);
            nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgScissor(args.vg, 1, 0, box.size.x - 2, box.size.y);
            if (!error.empty()) {
                nvgFillColor(args.vg, nvgRGB(0xff, 0x50, 0x50));
                nvgText(args.vg, 4.f, textH / 2 + 2.f, error.c_str(), nullptr);
            } else {
                nvgFillColor(args.vg, nvgRGB(0xff, 0xb0, 0x30));
                nvgText(args.vg, 4.f, textH / 2 + 2.f, name.empty() ? "click to load a WAV" : name.c_str(), nullptr);
            }
            nvgFillColor(args.vg, nvgRGB(0xe0, 0x90, 0x20));
            nvgFontSize(args.vg, 10.f);
            nvgText(args.vg, 4.f, box.size.y - textH / 2 - 2.f, info.c_str(), nullptr);
            nvgResetScissor(args.vg);
        }
        OpaqueWidget::drawLayer(args, layer);
    }
};

struct GChipWidget : ModuleWidget {
    SampleDisplay* display = nullptr;

    GChipWidget(GChip* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/GChip.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        PanelLabels* labels = new PanelLabels;
        labels->box = Rect(Vec(0, 0), box.size);
        labels->labels = {
            {Vec(35.56f, 7.5f), "G-CHIP", 15.f, true, kPanelLight},
            {Vec(35.56f, 13.2f), "US 5,111,727 sample interpolator", 7.f, false, kPanelGrey},
            {Vec(14.f, 55.5f), "TUNE", 8.f, true, kPanelDark},
            {Vec(38.f, 55.5f), "START", 8.f, true, kPanelDark},
            {Vec(59.f, 55.5f), "LOOP", 8.f, true, kPanelDark},
            {Vec(66.5f, 60.7f), "ON", 5.5f, false, kPanelDark},
            {Vec(66.5f, 64.f), "GATE", 5.5f, false, kPanelDark},
            {Vec(66.5f, 67.3f), "OFF", 5.5f, false, kPanelDark},
            {Vec(14.f, 75.5f), "FINE", 8.f, true, kPanelDark},
            {Vec(38.f, 75.5f), "L.START", 8.f, true, kPanelDark},
            {Vec(60.f, 75.5f), "L.END", 8.f, true, kPanelDark},
            {Vec(35.56f, 96.f), "drop a WAV here or click the display", 6.5f, false, nvgRGB(0x70, 0x70, 0x70)},
            {Vec(11.f, 109.6f), "V/OCT", 7.f, true, kPanelLight},
            {Vec(27.5f, 109.6f), "GATE", 7.f, true, kPanelLight},
            {Vec(44.f, 109.6f), "EOC", 7.f, true, kPanelLight},
            {Vec(60.5f, 109.6f), "OUT", 7.f, true, kPanelLight},
        };
        addChild(labels);

        display = createWidget<SampleDisplay>(mm2px(Vec(3.5f, 17.f)));
        display->box.size = mm2px(Vec(64.12f, 34.f));
        display->module = module;
        addChild(display);

        addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(14.f, 64.f)), module, GChip::TUNE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(38.f, 64.f)), module, GChip::START_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(60.f, 64.f)), module, GChip::LOOP_MODE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(14.f, 84.f)), module, GChip::FINE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(38.f, 84.f)), module, GChip::LOOP_START_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(60.f, 84.f)), module, GChip::LOOP_END_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(11.f, 116.f)), module, GChip::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(27.5f, 116.f)), module, GChip::GATE_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(44.f, 116.f)), module, GChip::EOC_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(60.5f, 116.f)), module, GChip::OUT_OUTPUT));
    }

    void step() override {
        if (GChip* m = dynamic_cast<GChip*>(module)) m->collectRetired();
        ModuleWidget::step();
    }

    void onPathDrop(const PathDropEvent& e) override {
        GChip* m = dynamic_cast<GChip*>(module);
        if (m && !e.paths.empty()) {
            std::string ext = string::lowercase(system::getExtension(e.paths[0]));
            if (ext == ".wav") {
                m->loadSample(e.paths[0], true);
                e.consume(this);
                return;
            }
        }
        ModuleWidget::onPathDrop(e);
    }

    void appendContextMenu(Menu* menu) override {
        GChip* m = dynamic_cast<GChip*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        std::string path;
        {
            std::lock_guard<std::mutex> lock(m->sampleMutex);
            path = m->samplePath;
        }
        menu->addChild(createMenuItem("Load WAV...", "", [=]() { display->loadDialog(); }));
        menu->addChild(createMenuItem("Clear sample", "", [=]() { m->clearSample(); }, path.empty()));
        if (!path.empty()) menu->addChild(createMenuLabel(path));
        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Fixed-point datapath (16-bit coefficients, 8-bit blend)", "", &m->fixedPoint));
    }
};

Model* modelGChip = createModel<GChip, GChipWidget>("GChip");
