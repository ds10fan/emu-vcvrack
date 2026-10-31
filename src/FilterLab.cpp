// Filter Lab: write filters in fscript (dsp/fscript.*) in an editor on the panel, compiled as you
// type, and switch between models (the factory models of dsp/filterlab_presets.cpp and your own).
//
// Threads: the source, the model list and compilation live on the UI thread. A successful compile
// becomes a Bundle (program + 16 prepared voices) handed to the audio thread through an atomic
// pointer; the audio thread carries the old voices' state over (same-named variables) and hands the
// old bundle back through a second atomic pointer for the UI thread to free.
#include "plugin.hpp"

#include <atomic>
#include <osdialog.h>

#include "filterlab_presets.hpp"
#include "fscript.hpp"
#include "oversampler.hpp"
#include "widgets.hpp"

namespace {

const int kMaxVoices = 16;

struct Bundle {
    fscript::Program prog;
    fscript::Voice voices[kMaxVoices];
    double sampleRate = 0;  // the rate the voices were reset at
    bool keepState = false;
};

struct Entry {
    std::string name;
    std::string code;
    int factory = -1;  // index into kPresets, -1 for a user model
};

}  // namespace

struct FilterLab : Module {
    enum ParamId {
        P1_PARAM, P2_PARAM, P3_PARAM, P4_PARAM,
        ATT1_PARAM, ATT2_PARAM, ATT3_PARAM, ATT4_PARAM,
        PARAMS_LEN
    };
    enum InputId { IN_INPUT, IN2_INPUT, CV1_INPUT, CV2_INPUT, CV3_INPUT, CV4_INPUT, INPUTS_LEN };
    enum OutputId { OUT_OUTPUT, OUT2_OUTPUT, OUTPUTS_LEN };
    enum LightId { ERROR_LIGHT, RESET_LIGHT, LIGHTS_LEN };

    static constexpr float kFullScale = 5.f;

    // ---- audio thread
    Bundle* cur = nullptr;
    std::atomic<Bundle*> pending{nullptr};
    std::atomic<Bundle*> trash{nullptr};
    std::atomic<bool> resetRequest{false};
    std::atomic<int> osRequest{0};
    int osFactor = 1;
    filterlab::Oversampler osIn[kMaxVoices], osIn2[kMaxVoices], osOut[kMaxVoices], osOut2[kMaxVoices];
    float pSmooth[kMaxVoices][4] = {};
    float resetLightTime = 0.f;
    std::atomic<int> resetCount{0};

    // ---- UI thread
    std::vector<Entry> entries;
    int current = 0;
    int oversample = 1;      // saved; osRequest carries it to the audio thread
    float fontSize = 10.5f;  // editor font, saved
    bool liveCompile = true;
    uint64_t codeVersion = 1;  // bumps when the code is replaced from outside the editor
    // last compile
    std::atomic<bool> compileOk{false};
    fscript::Error error;
    std::string labels[4];
    std::string inLabels[2] = {"IN", "IN2"};
    std::string outLabels[2] = {"OUT", "OUT2"};
    int instructionCount = 0;

    FilterLab() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        for (int i = 0; i < 4; i++) {
            configParam(P1_PARAM + i, 0.f, 1.f, 0.5f, string::f("P%d", i + 1), "%", 0.f, 100.f);
            configParam(ATT1_PARAM + i, -1.f, 1.f, 0.f, string::f("P%d CV amount", i + 1), "%", 0.f, 100.f);
            configInput(CV1_INPUT + i, string::f("P%d CV (10 V for the full range; also cv%d in the script)", i + 1, i + 1));
        }
        configInput(IN_INPUT, "Audio (in)");
        configInput(IN2_INPUT, "Audio 2 (in2)");
        configOutput(OUT_OUTPUT, "Audio (out)");
        configOutput(OUT2_OUTPUT, "Audio 2 (out2)");
        configLight(ERROR_LIGHT, "Compile error");
        configLight(RESET_LIGHT, "Voice reset (the filter blew up: NaN or infinity) or loop budget exceeded");
        configBypass(IN_INPUT, OUT_OUTPUT);

        for (int i = 0; i < filterlab::kPresetCount; i++) {
            Entry e;
            e.name = filterlab::kPresets[i].name;
            e.code = filterlab::kPresets[i].code;
            e.factory = i;
            entries.push_back(e);
        }
        osRequest = 1;
        compileCurrent(false);
    }

    ~FilterLab() {
        delete cur;
        delete pending.exchange(nullptr);
        delete trash.exchange(nullptr);
    }

    // ------------------------------------------------------------------ UI thread

    Entry& entry() { return entries[current]; }

    bool isModified(int i) const {
        const Entry& e = entries[i];
        return e.factory >= 0 && e.code != filterlab::kPresets[e.factory].code;
    }

    void collectTrash() { delete trash.exchange(nullptr); }

    // Compiles the current entry's code; on success hands it to the audio thread.
    bool compileCurrent(bool keepState) {
        collectTrash();
        Bundle* b = new Bundle;
        fscript::Error err;
        if (!fscript::compile(entry().code, b->prog, err)) {
            delete b;
            compileOk = false;
            error = err;
            return false;
        }
        compileOk = true;
        error = fscript::Error();
        instructionCount = (int)b->prog.code.size();
        for (int i = 0; i < 4; i++) {
            labels[i] = b->prog.paramLabel[i];
            paramQuantities[P1_PARAM + i]->name =
                labels[i].empty() ? string::f("P%d", i + 1) : string::f("P%d: %s", i + 1, labels[i].c_str());
        }
        inLabels[1] = b->prog.in2Label.empty() ? "IN2" : b->prog.in2Label;
        outLabels[0] = b->prog.outLabel.empty() ? "OUT" : b->prog.outLabel;
        outLabels[1] = b->prog.out2Label.empty() ? "OUT2" : b->prog.out2Label;
        inputInfos[IN2_INPUT]->name = "Audio 2 (in2)" + (b->prog.in2Label.empty() ? "" : ": " + b->prog.in2Label);
        outputInfos[OUT_OUTPUT]->name = "Audio (out)" + (b->prog.outLabel.empty() ? "" : ": " + b->prog.outLabel);
        outputInfos[OUT2_OUTPUT]->name = "Audio 2 (out2)" + (b->prog.out2Label.empty() ? "" : ": " + b->prog.out2Label);
        if (entry().factory < 0 && !b->prog.name.empty()) entry().name = b->prog.name;

        // Reset the voices here (memory may be large); the audio thread redoes it if the rate moved.
        double sr = (APP && APP->engine ? APP->engine->getSampleRate() : 48000.0) * oversample;
        for (int c = 0; c < kMaxVoices; c++) {
            fscript::prepare(b->prog, b->voices[c]);
            for (int i = 0; i < 4; i++) b->voices[c].mem[fscript::B_P1 + i] = params[P1_PARAM + i].getValue();
            fscript::reset(b->prog, b->voices[c], sr, c);
        }
        b->sampleRate = sr;
        b->keepState = keepState;
        delete pending.exchange(b);
        return true;
    }

    void select(int i) {
        int n = (int)entries.size();
        current = ((i % n) + n) % n;
        codeVersion++;
        compileCurrent(false);
    }

    void setCode(const std::string& code) {
        entry().code = code;
    }

    void replaceCode(const std::string& code) {
        entry().code = code;
        codeVersion++;
        compileCurrent(false);
    }

    void addUser(const std::string& name, const std::string& code) {
        Entry e;
        e.name = name;
        e.code = code;
        entries.push_back(e);
        select((int)entries.size() - 1);
    }

    void removeCurrent() {
        if (entry().factory >= 0) return;
        entries.erase(entries.begin() + current);
        select(std::min(current, (int)entries.size() - 1));
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        resetRequest = true;
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        entries.clear();
        for (int i = 0; i < filterlab::kPresetCount; i++) {
            Entry en;
            en.name = filterlab::kPresets[i].name;
            en.code = filterlab::kPresets[i].code;
            en.factory = i;
            entries.push_back(en);
        }
        oversample = 1;
        osRequest = 1;
        select(0);
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "current", json_string(entry().name.c_str()));
        json_object_set_new(root, "oversample", json_integer(oversample));
        json_object_set_new(root, "fontSize", json_real(fontSize));
        json_object_set_new(root, "liveCompile", json_boolean(liveCompile));
        json_t* models = json_array();
        for (int i = 0; i < (int)entries.size(); i++) {
            const Entry& e = entries[i];
            if (e.factory >= 0 && !isModified(i)) continue;
            json_t* m = json_object();
            json_object_set_new(m, "name", json_string(e.name.c_str()));
            json_object_set_new(m, "code", json_string(e.code.c_str()));
            json_object_set_new(m, "factory", json_boolean(e.factory >= 0));
            json_array_append_new(models, m);
        }
        json_object_set_new(root, "models", models);
        return root;
    }

    void dataFromJson(json_t* root) override {
        entries.clear();
        for (int i = 0; i < filterlab::kPresetCount; i++) {
            Entry e;
            e.name = filterlab::kPresets[i].name;
            e.code = filterlab::kPresets[i].code;
            e.factory = i;
            entries.push_back(e);
        }
        json_t* models = json_object_get(root, "models");
        size_t idx;
        json_t* m;
        json_array_foreach(models, idx, m) {
            const char* name = json_string_value(json_object_get(m, "name"));
            const char* code = json_string_value(json_object_get(m, "code"));
            if (!name || !code) continue;
            bool factory = json_is_true(json_object_get(m, "factory"));
            bool placed = false;
            if (factory) {
                for (Entry& e : entries) {
                    if (e.factory >= 0 && e.name == name) {
                        e.code = code;
                        placed = true;
                        break;
                    }
                }
            }
            if (!placed) {
                Entry e;
                e.name = name;
                e.code = code;
                entries.push_back(e);
            }
        }
        current = 0;
        if (const char* c = json_string_value(json_object_get(root, "current"))) {
            for (int i = 0; i < (int)entries.size(); i++)
                if (entries[i].name == c) current = i;
        }
        if (json_t* j = json_object_get(root, "oversample")) oversample = clamp((int)json_integer_value(j), 1, 8);
        if (json_t* j = json_object_get(root, "fontSize")) fontSize = clamp((float)json_number_value(j), 7.f, 16.f);
        if (json_t* j = json_object_get(root, "liveCompile")) liveCompile = json_is_true(j);
        osRequest = oversample;
        codeVersion++;
        compileCurrent(false);
    }

    // ------------------------------------------------------------------ audio thread

    void resetVoices(double sr) {
        if (!cur) return;
        for (int c = 0; c < kMaxVoices; c++) fscript::reset(cur->prog, cur->voices[c], sr, c);
        cur->sampleRate = sr;
    }

    void process(const ProcessArgs& args) override {
        if (int f = osRequest.exchange(0)) {
            osFactor = f;
            for (int c = 0; c < kMaxVoices; c++) {
                osIn[c].setFactor(f);
                osIn2[c].setFactor(f);
                osOut[c].setFactor(f);
                osOut2[c].setFactor(f);
            }
            resetRequest = true;
        }
        const double sr = (double)args.sampleRate * osFactor;

        if (Bundle* nb = pending.exchange(nullptr)) {
            if (nb->sampleRate != sr) {
                for (int c = 0; c < kMaxVoices; c++) fscript::reset(nb->prog, nb->voices[c], sr, c);
                nb->sampleRate = sr;
            }
            if (nb->keepState && cur && cur->sampleRate == sr) {
                for (int c = 0; c < kMaxVoices; c++)
                    fscript::transfer(cur->prog, cur->voices[c], nb->prog, nb->voices[c]);
            }
            Bundle* old = cur;
            cur = nb;
            delete trash.exchange(old);  // normally empty: the UI thread collects it
        }
        if (resetRequest.exchange(false) || (cur && cur->sampleRate != sr)) resetVoices(sr);

        int channels = 1;
        for (int i = 0; i < INPUTS_LEN; i++) channels = std::max(channels, inputs[i].getChannels());

        const float smooth = 1.f - std::exp(-1.f / (0.002f * args.sampleRate));
        bool blew = false;
        for (int c = 0; c < channels; c++) {
            float o = 0.f, o2 = 0.f;
            if (cur) {
                fscript::Voice& v = cur->voices[c];
                double* mem = v.mem.data();
                for (int i = 0; i < 4; i++) {
                    float cv = inputs[CV1_INPUT + i].getPolyVoltage(c);
                    float target = clamp(params[P1_PARAM + i].getValue() + params[ATT1_PARAM + i].getValue() * cv / 10.f, 0.f, 1.f);
                    pSmooth[c][i] += (target - pSmooth[c][i]) * smooth;
                    mem[fscript::B_P1 + i] = pSmooth[c][i];
                    mem[fscript::B_CV1 + i] = cv;
                }
                float up[filterlab::Oversampler::kMaxFactor], up2[filterlab::Oversampler::kMaxFactor];
                float yo[filterlab::Oversampler::kMaxFactor], yo2[filterlab::Oversampler::kMaxFactor];
                osIn[c].up(inputs[IN_INPUT].getPolyVoltage(c) / kFullScale, up);
                osIn2[c].up(inputs[IN2_INPUT].getPolyVoltage(c) / kFullScale, up2);
                bool ok = true;
                for (int k = 0; k < osFactor; k++) {
                    mem[fscript::B_IN] = up[k];
                    mem[fscript::B_IN2] = up2[k];
                    ok &= fscript::run(cur->prog, v);
                    yo[k] = (float)mem[fscript::B_OUT];
                    yo2[k] = (float)mem[fscript::B_OUT2];
                }
                o = osOut[c].down(yo);
                o2 = osOut2[c].down(yo2);
                if (!ok || !std::isfinite(o) || !std::isfinite(o2) || !std::isfinite(mem[fscript::B_OUT]) ||
                    !std::isfinite(mem[fscript::B_OUT2])) {
                    blew = true;
                    fscript::reset(cur->prog, v, sr, c);
                    osOut[c].reset();
                    osOut2[c].reset();
                    o = o2 = 0.f;
                }
            }
            outputs[OUT_OUTPUT].setVoltage(clamp(o * kFullScale, -12.f, 12.f), c);
            outputs[OUT2_OUTPUT].setVoltage(clamp(o2 * kFullScale, -12.f, 12.f), c);
        }
        outputs[OUT_OUTPUT].setChannels(channels);
        outputs[OUT2_OUTPUT].setChannels(channels);

        if (blew) {
            resetLightTime = 0.3f;
            resetCount++;
        }
        resetLightTime = std::max(0.f, resetLightTime - args.sampleTime);
        lights[RESET_LIGHT].setBrightness(resetLightTime > 0.f ? 1.f : 0.f);
        lights[ERROR_LIGHT].setBrightness(compileOk ? 0.f : 1.f);
    }
};

// =================================================================================================
// The code editor

namespace {

const char* const kKeywords[] = {"if", "else", "for", "in", "init", "buf", "const", "fn"};
const char* const kBuiltins[] = {"in", "in2", "p1", "p2", "p3", "p4", "cv1", "cv2", "cv3", "cv4", "sr",
                                 "dt", "ch", "out", "out2", "pi", "tau", "e"};

bool isWordChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

bool inList(const std::string& w, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (w == list[i]) return true;
    return false;
}

}  // namespace

struct CodeEditor : ui::TextField {
    FilterLab* module = nullptr;
    uint64_t seenVersion = 0;
    int scrollLine = 0;
    int scrollCol = 0;
    float charW = 6.f;
    double lastEdit = 0;
    bool dirty = false;
    float scrollAccum = 0.f;

    struct Snapshot {
        std::string text;
        int cursor, selection;
    };
    std::vector<Snapshot> undo, redo;
    double lastTypeTime = -10;

    CodeEditor() { multiline = true; }

    float fontSize() const { return module ? module->fontSize : 10.5f; }
    float lineH() const { return std::round(fontSize() * 1.3f); }
    float gutterW() const { return charW * 3.f + 8.f; }
    float statusH() const { return lineH() + 4.f; }
    int visibleLines() const { return std::max(1, (int)((box.size.y - statusH() - 4.f) / lineH())); }
    int visibleCols() const { return std::max(1, (int)((box.size.x - gutterW() - 6.f) / charW)); }

    // ---- text geometry (byte offsets; columns count UTF-8 codepoints)
    int lineStart(int pos) const {
        while (pos > 0 && text[pos - 1] != '\n') pos--;
        return pos;
    }
    int lineEnd(int pos) const {
        while (pos < (int)text.size() && text[pos] != '\n') pos++;
        return pos;
    }
    int colOf(int pos) const {
        int s = lineStart(pos), col = 0;
        for (int i = s; i < pos; i++)
            if (((unsigned char)text[i] & 0xC0) != 0x80) col++;
        return col;
    }
    int lineOf(int pos) const {
        int l = 0;
        for (int i = 0; i < pos && i < (int)text.size(); i++)
            if (text[i] == '\n') l++;
        return l;
    }
    int startOfLine(int line) const {
        int pos = 0;
        for (int l = 0; l < line; l++) {
            int e = lineEnd(pos);
            if (e >= (int)text.size()) return (int)text.size() + 1;  // past the end
            pos = e + 1;
        }
        return pos;
    }
    int posAt(int line, int col) const {
        int s = startOfLine(line);
        if (s > (int)text.size()) return (int)text.size();
        int e = lineEnd(s), c = 0, i = s;
        while (i < e && c < col) {
            i++;
            while (i < e && ((unsigned char)text[i] & 0xC0) == 0x80) i++;
            c++;
        }
        return i;
    }
    int lineCount() const { return lineOf((int)text.size()) + 1; }

    int getTextPosition(math::Vec mousePos) override {
        int line = scrollLine + (int)std::floor((mousePos.y - 3.f) / lineH());
        line = clamp(line, 0, lineCount() - 1);
        int col = scrollCol + (int)std::round((mousePos.x - gutterW()) / charW);
        return posAt(line, std::max(col, 0));
    }

    void ensureCursorVisible() {
        int line = lineOf(cursor), col = colOf(cursor);
        int vis = visibleLines();
        if (line < scrollLine) scrollLine = line;
        if (line >= scrollLine + vis) scrollLine = line - vis + 1;
        int vc = visibleCols();
        if (col < scrollCol) scrollCol = std::max(0, col - 4);
        if (col >= scrollCol + vc) scrollCol = col - vc + 4;
    }

    // ---- editing

    void changed() {
        ChangeEvent e;
        onChange(e);
    }

    void onChange(const ChangeEvent& e) override {
        if (!module) return;
        module->setCode(text);
        dirty = true;
        lastEdit = system::getTime();
    }

    void pushUndo(const Snapshot& s) {
        undo.push_back(s);
        if (undo.size() > 500) undo.erase(undo.begin());
        redo.clear();
    }

    void restore(std::vector<Snapshot>& from, std::vector<Snapshot>& to) {
        if (from.empty()) return;
        to.push_back({text, cursor, selection});
        Snapshot s = from.back();
        from.pop_back();
        text = s.text;
        cursor = clamp(s.cursor, 0, (int)text.size());
        selection = clamp(s.selection, 0, (int)text.size());
        changed();
        ensureCursorVisible();
    }

    void replaceRange(int a, int b, const std::string& s) {
        text.replace(a, b - a, s);
    }

    // Indents (or unindents) the lines touched by the selection.
    void indentLines(bool out) {
        int a = std::min(cursor, selection), b = std::max(cursor, selection);
        int s = lineStart(a);
        int endLine = lineOf(b > a && b == lineStart(b) ? b - 1 : b);
        int startLine = lineOf(s);
        int newA = a, newB = b;
        for (int l = endLine; l >= startLine; l--) {
            int ls = startOfLine(l);
            if (!out) {
                text.insert(ls, "  ");
                if (ls <= newA) newA += 2;
                newB += 2;
            } else {
                int n = 0;
                while (n < 2 && ls + n < (int)text.size() && text[ls + n] == ' ') n++;
                text.erase(ls, n);
                if (ls < newA) newA -= std::min(n, newA - ls);
                newB -= n;
            }
        }
        if (cursor <= selection) {
            cursor = newA;
            selection = newB;
        } else {
            selection = newA;
            cursor = newB;
        }
        cursor = clamp(cursor, 0, (int)text.size());
        selection = clamp(selection, 0, (int)text.size());
        changed();
    }

    void toggleComment() {
        int a = std::min(cursor, selection), b = std::max(cursor, selection);
        int startLine = lineOf(a);
        int endLine = lineOf(b > a && b == lineStart(b) ? b - 1 : b);
        bool allCommented = true;
        for (int l = startLine; l <= endLine; l++) {
            int ls = startOfLine(l), le = lineEnd(ls), i = ls;
            while (i < le && text[i] == ' ') i++;
            if (i < le && text.compare(i, 2, "//") != 0) allCommented = false;
        }
        for (int l = endLine; l >= startLine; l--) {
            int ls = startOfLine(l), le = lineEnd(ls), i = ls;
            while (i < le && text[i] == ' ') i++;
            if (allCommented) {
                if (text.compare(i, 3, "// ") == 0)
                    text.erase(i, 3);
                else if (text.compare(i, 2, "//") == 0)
                    text.erase(i, 2);
            } else {
                text.insert(i, "// ");
            }
        }
        cursor = selection = clamp(cursor, 0, (int)text.size());
        changed();
    }

    void moveVertical(int dl, bool shift) {
        int line = lineOf(cursor), col = colOf(cursor);
        int nl = clamp(line + dl, 0, lineCount() - 1);
        cursor = (dl < 0 && line == 0) ? 0 : (dl > 0 && line == lineCount() - 1) ? (int)text.size() : posAt(nl, col);
        if (!shift) selection = cursor;
    }

    void onSelectText(const SelectTextEvent& e) override {
        Snapshot before{text, cursor, selection};
        double now = system::getTime();
        bool coalesce = now - lastTypeTime < 1.0 && e.codepoint != ' ' && cursor == selection;
        if (e.codepoint == '}') {
            // dedent a closing brace typed on a blank line
            int ls = lineStart(cursor);
            bool blank = cursor == selection && cursor - ls >= 2;
            for (int i = ls; i < cursor && blank; i++) blank = text[i] == ' ';
            if (blank) {
                text.erase(cursor - 2, 2);
                cursor = selection = cursor - 2;
            }
        }
        TextField::onSelectText(e);
        if (text != before.text) {
            if (!coalesce) pushUndo(before);
            lastTypeTime = now;
            ensureCursorVisible();
        }
    }

    void onSelectKey(const SelectKeyEvent& e) override {
        if (e.action != GLFW_PRESS && e.action != GLFW_REPEAT) {
            TextField::onSelectKey(e);
            return;
        }
        Snapshot before{text, cursor, selection};
        int mods = e.mods & RACK_MOD_MASK;
        bool shift = mods & GLFW_MOD_SHIFT;
        bool handled = true;
        if (e.keyName == "z" && (mods == RACK_MOD_CTRL)) {
            restore(undo, redo);
            e.consume(this);
            return;
        }
        if ((e.keyName == "z" && mods == (RACK_MOD_CTRL | GLFW_MOD_SHIFT)) || (e.keyName == "y" && mods == RACK_MOD_CTRL)) {
            restore(redo, undo);
            e.consume(this);
            return;
        }
        if ((e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER) && mods == RACK_MOD_CTRL) {
            if (module) {
                module->compileCurrent(true);
                dirty = false;
            }
        } else if ((e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER) && (mods & ~GLFW_MOD_SHIFT) == 0) {
            int ls = lineStart(cursor), i = ls;
            while (i < cursor && text[i] == ' ') i++;
            std::string indent(i - ls, ' ');
            int j = std::min(cursor, selection) - 1;
            while (j >= ls && text[j] == ' ') j--;
            if (j >= ls && text[j] == '{') indent += "  ";
            insertText("\n" + indent);
        } else if (e.key == GLFW_KEY_TAB && mods == 0) {
            if (cursor != selection && lineOf(cursor) != lineOf(selection))
                indentLines(false);
            else
                insertText(std::string(2 - colOf(cursor) % 2, ' '));
        } else if (e.key == GLFW_KEY_TAB && mods == GLFW_MOD_SHIFT) {
            indentLines(true);
        } else if (e.keyName == "/" && mods == RACK_MOD_CTRL) {
            toggleComment();
        } else if (e.key == GLFW_KEY_UP && (mods & ~GLFW_MOD_SHIFT) == 0) {
            moveVertical(-1, shift);
        } else if (e.key == GLFW_KEY_DOWN && (mods & ~GLFW_MOD_SHIFT) == 0) {
            moveVertical(1, shift);
        } else if (e.key == GLFW_KEY_PAGE_UP && (mods & ~GLFW_MOD_SHIFT) == 0) {
            moveVertical(-visibleLines(), shift);
        } else if (e.key == GLFW_KEY_PAGE_DOWN && (mods & ~GLFW_MOD_SHIFT) == 0) {
            moveVertical(visibleLines(), shift);
        } else if (e.key == GLFW_KEY_HOME && (mods & ~GLFW_MOD_SHIFT) == 0) {
            int ls = lineStart(cursor), i = ls;
            while (i < (int)text.size() && text[i] == ' ') i++;
            cursor = (cursor == i) ? ls : i;  // smart home: first non-blank, then column 0
            if (!shift) selection = cursor;
        } else if (e.key == GLFW_KEY_END && (mods & ~GLFW_MOD_SHIFT) == 0) {
            cursor = lineEnd(cursor);
            if (!shift) selection = cursor;
        } else if ((e.key == GLFW_KEY_HOME || e.key == GLFW_KEY_END) && (mods & RACK_MOD_CTRL)) {
            cursor = e.key == GLFW_KEY_HOME ? 0 : (int)text.size();
            if (!shift) selection = cursor;
        } else if (e.key == GLFW_KEY_ESCAPE) {
            APP->event->setSelectedWidget(nullptr);
        } else {
            handled = false;
        }
        if (handled)
            e.consume(this);
        else
            TextField::onSelectKey(e);
        if (text != before.text) pushUndo(before);
        if (text != before.text || cursor != before.cursor) ensureCursorVisible();
    }

    void onButton(const ButtonEvent& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && (e.mods & GLFW_MOD_SHIFT)) {
            // shift-click extends the selection
            int keep = selection;
            TextField::onButton(e);
            selection = keep;
            return;
        }
        TextField::onButton(e);
    }

    void onDoubleClick(const DoubleClickEvent& e) override {
        // select the word under the cursor
        int a = cursor, b = cursor;
        while (a > 0 && isWordChar(text[a - 1])) a--;
        while (b < (int)text.size() && isWordChar(text[b])) b++;
        selection = a;
        cursor = b;
        e.consume(this);
    }

    void onHoverScroll(const HoverScrollEvent& e) override {
        e.consume(this);
#if defined ARCH_MAC
        const float notch = 10.f;
#else
        const float notch = 50.f;
#endif
        scrollAccum += e.scrollDelta.y;
        int steps = (int)(scrollAccum / notch * 3);
        if (steps != 0) {
            scrollAccum -= steps * notch / 3;
            scrollLine = clamp(scrollLine - steps, 0, std::max(0, lineCount() - 1));
        }
        if (e.scrollDelta.x != 0) scrollCol = std::max(0, scrollCol - (int)(e.scrollDelta.x / notch * 4));
    }

    void step() override {
        TextField::step();
        if (!module) return;
        module->collectTrash();
        if (seenVersion != module->codeVersion) {
            seenVersion = module->codeVersion;
            text = module->entry().code;
            cursor = selection = 0;
            scrollLine = scrollCol = 0;
            undo.clear();
            redo.clear();
            dirty = false;
        }
        if (dirty && module->liveCompile && system::getTime() - lastEdit > 0.25) {
            dirty = false;
            module->compileCurrent(true);
        }
    }

    // ---- drawing

    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
        nvgFillColor(args.vg, nvgRGB(0x14, 0x15, 0x18));
        nvgFill(args.vg);
        nvgStrokeWidth(args.vg, 1.f);
        nvgStrokeColor(args.vg, APP->event->selectedWidget == this ? nvgRGB(0xe0, 0x90, 0x1f) : nvgRGB(0x40, 0x40, 0x44));
        nvgStroke(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) {
            Widget::drawLayer(args, layer);
            return;
        }
        std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (!font) return;
        NVGcontext* vg = args.vg;
        nvgSave(vg);
        nvgIntersectScissor(vg, 1, 1, box.size.x - 2, box.size.y - 2);
        nvgFontFaceId(vg, font->handle);
        nvgFontSize(vg, fontSize());
        nvgTextLetterSpacing(vg, 0.f);
        float bounds[4];
        nvgTextBounds(vg, 0, 0, "0000000000", nullptr, bounds);
        charW = (bounds[2] - bounds[0]) / 10.f;
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

        const float lh = lineH(), gw = gutterW();
        const int vis = visibleLines();
        const bool focused = APP->event->selectedWidget == this;
        const int selA = std::min(cursor, selection), selB = std::max(cursor, selection);
        const int errLine = (module && !module->compileOk) ? module->error.line - 1 : -1;

        // gutter
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, gw - 3, box.size.y - statusH());
        nvgFillColor(vg, nvgRGB(0x1b, 0x1c, 0x20));
        nvgFill(vg);

        // Walk all lines (block comments need the state from the top).
        int pos = 0, line = 0;
        bool inBlock = false;
        const int n = (int)text.size();
        while (pos <= n) {
            int le = lineEnd(pos);
            int row = line - scrollLine;
            bool visible = row >= 0 && row < vis;
            float y = 3.f + row * lh + lh / 2;
            if (visible) {
                if (line == errLine) {
                    nvgBeginPath(vg);
                    nvgRect(vg, gw - 3, y - lh / 2, box.size.x, lh);
                    nvgFillColor(vg, nvgRGBA(0xd0, 0x30, 0x30, 0x50));
                    nvgFill(vg);
                }
                // selection
                if (selA != selB && selA <= le && selB >= pos) {
                    int a = std::max(selA, pos), b = std::min(selB, le);
                    float xa = gw + (colOf(a) - scrollCol) * charW;
                    float xb = gw + (colOf(b) - scrollCol) * charW + (selB > le ? charW * 0.5f : 0);
                    nvgBeginPath(vg);
                    nvgRect(vg, std::max(xa, gw - 2), y - lh / 2, std::max(xb - std::max(xa, gw - 2), 0.f), lh);
                    nvgFillColor(vg, nvgRGBA(0x40, 0x70, 0xc0, 0x80));
                    nvgFill(vg);
                }
                // line number
                nvgFillColor(vg, line == errLine ? nvgRGB(0xff, 0x60, 0x60) : nvgRGB(0x60, 0x62, 0x6a));
                nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                std::string num = std::to_string(line + 1);
                nvgText(vg, gw - 6, y, num.c_str(), nullptr);
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            }
            // tokens (always scanned to track block comments)
            int i = pos;
            int col = 0;
            auto put = [&](int a, int b, NVGcolor c) {
                int cols = 0;
                for (int k = a; k < b; k++)
                    if (((unsigned char)text[k] & 0xC0) != 0x80) cols++;
                if (visible && b > a) {
                    float x = gw + (col - scrollCol) * charW;
                    if (x + cols * charW > gw - 2 && x < box.size.x) {
                        nvgFillColor(vg, c);
                        nvgText(vg, x, y, text.c_str() + a, text.c_str() + b);
                    }
                }
                col += cols;
            };
            const NVGcolor cText = nvgRGB(0xd8, 0xd8, 0xd0), cComment = nvgRGB(0x6a, 0x74, 0x6a),
                           cNum = nvgRGB(0x7f, 0xd0, 0xe0), cKey = nvgRGB(0xd0, 0x8a, 0xf0),
                           cBuiltin = nvgRGB(0xff, 0xb0, 0x30), cDirective = nvgRGB(0xe0, 0x90, 0x1f),
                           cFn = nvgRGB(0x9a, 0xc8, 0x7a);
            while (i < le) {
                if (inBlock) {
                    int j = i;
                    while (j < le && !(text[j] == '*' && j + 1 < le && text[j + 1] == '/')) j++;
                    if (j < le) {
                        j += 2;
                        inBlock = false;
                    }
                    put(i, j, cComment);
                    i = j;
                    continue;
                }
                char c = text[i];
                if (c == '/' && i + 1 < le && text[i + 1] == '/') {
                    put(i, le, cComment);
                    i = le;
                } else if (c == '/' && i + 1 < le && text[i + 1] == '*') {
                    inBlock = true;
                    put(i, i + 2, cComment);
                    i += 2;
                } else if (c == '#') {
                    put(i, le, cDirective);
                    i = le;
                } else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < le && std::isdigit((unsigned char)text[i + 1]))) {
                    int j = i;
                    while (j < le && (std::isalnum((unsigned char)text[j]) || (text[j] == '.' && !(j + 1 < le && text[j + 1] == '.'))))
                        j++;
                    put(i, j, cNum);
                    i = j;
                } else if (isWordChar(c)) {
                    int j = i;
                    while (j < le && isWordChar(text[j])) j++;
                    std::string w = text.substr(i, j - i);
                    NVGcolor col2 = cText;
                    if (inList(w, kKeywords, sizeof kKeywords / sizeof *kKeywords))
                        col2 = cKey;
                    else if (inList(w, kBuiltins, sizeof kBuiltins / sizeof *kBuiltins))
                        col2 = cBuiltin;
                    else if (j < le && text[j] == '(')
                        col2 = cFn;
                    put(i, j, col2);
                    i = j;
                } else {
                    int j = i + 1;
                    while (j < le && ((unsigned char)text[j] & 0xC0) == 0x80) j++;
                    put(i, j, cText);
                    i = j;
                }
            }
            // cursor
            if (visible && focused && cursor >= pos && cursor <= le) {
                float blink = std::fmod((float)system::getTime(), 1.f);
                if (blink < 0.6f) {
                    float x = gw + (colOf(cursor) - scrollCol) * charW;
                    nvgBeginPath(vg);
                    nvgRect(vg, x - 0.5f, y - lh / 2 + 1, 1.5f, lh - 2);
                    nvgFillColor(vg, nvgRGB(0xff, 0xc0, 0x40));
                    nvgFill(vg);
                }
            }
            line++;
            if (le >= n) break;
            pos = le + 1;
            if (line - scrollLine >= vis && !inBlock) {
                // nothing below is visible; block comment state no longer matters
                break;
            }
        }

        // status bar
        float sy = box.size.y - statusH();
        nvgBeginPath(vg);
        nvgRect(vg, 0, sy, box.size.x, statusH());
        nvgFillColor(vg, nvgRGB(0x1b, 0x1c, 0x20));
        nvgFill(vg);
        std::string status;
        NVGcolor sc = nvgRGB(0x80, 0xc0, 0x80);
        if (!module) {
            status = "fscript editor";
        } else if (!module->compileOk) {
            status = string::f("line %d:%d  %s", module->error.line, module->error.col, module->error.msg.c_str());
            sc = nvgRGB(0xff, 0x70, 0x70);
        } else if (dirty) {
            status = module->liveCompile ? "editing..." : "modified - Ctrl+Enter to compile";
            sc = nvgRGB(0xc0, 0xc0, 0x80);
        } else {
            status = string::f("ok  %d ops  x%d", module->instructionCount, module->oversample);
        }
        if (module) {
            std::string pos2 = string::f("%d:%d", lineOf(cursor) + 1, colOf(cursor) + 1);
            nvgFillColor(vg, nvgRGB(0x70, 0x72, 0x7a));
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgText(vg, box.size.x - 6, sy + statusH() / 2, pos2.c_str(), nullptr);
        }
        nvgFillColor(vg, sc);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, 6, sy + statusH() / 2, status.c_str(), nullptr);

        // scroll indicator
        int total = lineCount();
        if (total > vis) {
            float h = box.size.y - statusH() - 4;
            float th = std::max(10.f, h * vis / total);
            float ty = 2 + (h - th) * scrollLine / std::max(1, total - vis);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, box.size.x - 4, ty, 2.5f, th, 1.f);
            nvgFillColor(vg, nvgRGBA(0x80, 0x80, 0x88, 0x80));
            nvgFill(vg);
        }
        nvgRestore(vg);
        Widget::drawLayer(args, layer);
    }
};

// =================================================================================================
// Model selector: "<  name  >" in the title band. Arrows step, the name opens the list.

static void modelMenu(FilterLab* module);

struct ModelDisplay : widget::OpaqueWidget {
    FilterLab* module = nullptr;
    float scrollAccum = 0.f;

    void onButton(const ButtonEvent& e) override {
        OpaqueWidget::onButton(e);
        if (!module || e.action != GLFW_PRESS) return;
        if (e.button != GLFW_MOUSE_BUTTON_LEFT && e.button != GLFW_MOUSE_BUTTON_RIGHT) return;
        e.consume(this);
        float arrow = box.size.y;
        if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.pos.x < arrow)
            module->select(module->current - 1);
        else if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.pos.x > box.size.x - arrow)
            module->select(module->current + 1);
        else
            modelMenu(module);
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
            module->select(module->current - steps);
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
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
            if (font) {
                std::string name = "TPT SVF";
                std::string count;
                if (module) {
                    name = module->entry().name;
                    if (module->isModified(module->current)) name += " *";
                    if (module->entry().factory < 0) name = "[" + name + "]";
                    count = string::f("%d/%d", module->current + 1, (int)module->entries.size());
                }
                float cy = box.size.y / 2;
                nvgFontFaceId(args.vg, font->handle);
                nvgFontSize(args.vg, 12.f);
                nvgFillColor(args.vg, nvgRGB(0xff, 0xb0, 0x30));
                nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgText(args.vg, box.size.x / 2, cy, name.c_str(), nullptr);
                nvgFillColor(args.vg, nvgRGB(0xe0, 0x90, 0x20));
                nvgText(args.vg, box.size.y / 2, cy, "<", nullptr);
                nvgText(args.vg, box.size.x - box.size.y / 2, cy, ">", nullptr);
                nvgFontSize(args.vg, 9.f);
                nvgFillColor(args.vg, nvgRGB(0x80, 0x60, 0x20));
                nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nvgText(args.vg, box.size.x - box.size.y - 2, cy, count.c_str(), nullptr);
            }
        }
        Widget::drawLayer(args, layer);
    }
};

// Script-defined labels next to the knobs and jacks.
struct DynamicLabels : widget::Widget {
    FilterLab* module = nullptr;
    std::vector<Vec> knobPos;  // mm, left edge, baseline
    Vec jackPos[4];            // in, in2, out, out2 (mm, centre)

    void draw(const DrawArgs& args) override {
        std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
        std::shared_ptr<window::Font> bold = APP->window->loadFont(asset::system("res/fonts/DejaVuSans-Bold.ttf"));
        if (!font || !bold) return;
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        for (int i = 0; i < 4; i++) {
            Vec p = mm2px(knobPos[i]);
            nvgFontFaceId(args.vg, bold->handle);
            nvgFontSize(args.vg, 8.f);
            nvgFillColor(args.vg, kPanelDark);
            std::string id = string::f("P%d", i + 1);
            nvgText(args.vg, p.x, p.y, id.c_str(), nullptr);
            std::string label = module ? module->labels[i] : "";
            if (!label.empty()) {
                nvgFontFaceId(args.vg, font->handle);
                nvgFontSize(args.vg, 7.5f);
                nvgFillColor(args.vg, nvgRGB(0x50, 0x50, 0x54));
                nvgText(args.vg, p.x + 13.f, p.y, label.substr(0, 22).c_str(), nullptr);
            }
        }
        const char* fallback[4] = {"IN", "IN2", "OUT", "OUT2"};
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(args.vg, bold->handle);
        for (int i = 0; i < 4; i++) {
            std::string s = fallback[i];
            if (module) s = i < 2 ? module->inLabels[i] : module->outLabels[i - 2];
            if (s.size() > 9) s = s.substr(0, 9);
            nvgFontSize(args.vg, s.size() > 5 ? 6.f : 7.5f);
            nvgFillColor(args.vg, i < 2 ? kPanelDark : kPanelLight);
            Vec p = mm2px(jackPos[i]);
            nvgText(args.vg, p.x, p.y, s.c_str(), nullptr);
        }
        Widget::draw(args);
    }
};

static std::string userDir() {
    std::string dir = asset::user("FilterLab");
    system::createDirectories(dir);
    return dir;
}

static void loadFile(FilterLab* module) {
    osdialog_filters* filters = osdialog_filters_parse("fscript:fs,txt");
    char* path = osdialog_file(OSDIALOG_OPEN, userDir().c_str(), nullptr, filters);
    osdialog_filters_free(filters);
    if (!path) return;
    std::string p = path;
    std::free(path);
    std::vector<uint8_t> data = system::readFile(p);
    std::string code(data.begin(), data.end());
    module->addUser(system::getStem(p), code);
}

static void saveFile(FilterLab* module) {
    osdialog_filters* filters = osdialog_filters_parse("fscript:fs");
    std::string name = module->entry().name;
    for (char& c : name)
        if (!isWordChar(c) && c != '-') c = '_';
    name += ".fs";
    char* path = osdialog_file(OSDIALOG_SAVE, userDir().c_str(), name.c_str(), filters);
    osdialog_filters_free(filters);
    if (!path) return;
    std::string p = path;
    std::free(path);
    if (system::getExtension(p).empty()) p += ".fs";
    const std::string& code = module->entry().code;
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return;
    std::fwrite(code.data(), 1, code.size(), f);
    std::fclose(f);
}

static void addModelItems(ui::Menu* menu, FilterLab* module) {
    for (int i = 0; i < (int)module->entries.size(); i++) {
        if (i == filterlab::kPresetCount) menu->addChild(new MenuSeparator);
        const Entry& e = module->entries[i];
        std::string name = e.name + (module->isModified(i) ? " *" : "");
        menu->addChild(createCheckMenuItem(name, "", [=]() { return module->current == i; }, [=]() { module->select(i); }));
    }
}

static void modelMenu(FilterLab* module) {
    ui::Menu* menu = createMenu();
    menu->addChild(createMenuLabel("Filter model"));
    addModelItems(menu, module);
}

struct FilterLabWidget : ModuleWidget {
    FilterLabWidget(FilterLab* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/FilterLab.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        PanelLabels* labels = new PanelLabels;
        labels->box = Rect(Vec(0, 0), box.size);
        labels->labels = {
            {Vec(28.f, 6.2f), "FILTER LAB", 13.f, true, kPanelLight},
            {Vec(28.f, 11.2f), "write your own filters", 6.5f, false, kPanelGrey},
            {Vec(209.f, 90.0f), "ERR", 6.f, false, kPanelDark},
            {Vec(209.f, 101.0f), "RESET", 6.f, false, kPanelDark},
        };
        addChild(labels);

        ModelDisplay* display = createWidget<ModelDisplay>(mm2px(Vec(56.f, 2.6f)));
        display->box.size = mm2px(Vec(160.f - 56.f, 8.6f));
        display->module = module;
        addChild(display);

        CodeEditor* editor = createWidget<CodeEditor>(mm2px(Vec(3.f, 14.5f)));
        editor->box.size = mm2px(Vec(167.f, 109.5f));
        editor->module = module;
        if (!module) editor->text = filterlab::kPresets[0].code;
        addChild(editor);

        const float xk = 181.f, xt = 197.f, xj = 211.f;
        DynamicLabels* dyn = new DynamicLabels;
        dyn->box = Rect(Vec(0, 0), box.size);
        dyn->module = module;
        for (int i = 0; i < 4; i++) {
            float y = 27.f + 17.f * i;
            dyn->knobPos.push_back(Vec(174.f, y - 8.f));
            addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(xk, y)), module, FilterLab::P1_PARAM + i));
            addParam(createParamCentered<Trimpot>(mm2px(Vec(xt, y)), module, FilterLab::ATT1_PARAM + i));
            addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xj, y)), module, FilterLab::CV1_INPUT + i));
        }
        dyn->jackPos[0] = Vec(xk, 90.f);
        dyn->jackPos[1] = Vec(xt, 90.f);
        dyn->jackPos[2] = Vec(xk, 109.6f);
        dyn->jackPos[3] = Vec(xt, 109.6f);
        addChild(dyn);

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xk, 97.f)), module, FilterLab::IN_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xt, 97.f)), module, FilterLab::IN2_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xk, 116.f)), module, FilterLab::OUT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xt, 116.f)), module, FilterLab::OUT2_OUTPUT));
        addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(209.f, 94.5f)), module, FilterLab::ERROR_LIGHT));
        addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(209.f, 105.5f)), module, FilterLab::RESET_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        FilterLab* module = dynamic_cast<FilterLab*>(this->module);
        if (!module) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createSubmenuItem("Filter model", "", [=](Menu* sub) { addModelItems(sub, module); }));
        menu->addChild(createMenuItem("New model from template", "", [=]() {
            module->addUser("User " + std::to_string(module->entries.size() - filterlab::kPresetCount + 1),
                            filterlab::kPresets[filterlab::kPresetCount - 1].code);
        }));
        menu->addChild(createMenuItem("Duplicate as new model", "", [=]() {
            std::string code = module->entry().code;
            std::string name = module->entry().name + " copy";
            // rename the copy in its #name line so the two stay apart
            size_t p = code.find("#name ");
            if (p != std::string::npos) {
                size_t e = code.find('\n', p);
                code.replace(p, e == std::string::npos ? std::string::npos : e - p, "#name " + name);
            }
            module->addUser(name, code);
        }));
        if (module->isModified(module->current))
            menu->addChild(createMenuItem("Revert to factory code", "", [=]() {
                module->replaceCode(filterlab::kPresets[module->entry().factory].code);
            }));
        if (module->entry().factory < 0)
            menu->addChild(createMenuItem("Delete this model", "", [=]() { module->removeCurrent(); }));
        menu->addChild(createMenuItem("Load model from file...", "", [=]() { loadFile(module); }));
        menu->addChild(createMenuItem("Save model to file...", "", [=]() { saveFile(module); }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem("Reset filter state", "", [=]() { module->resetRequest = true; }));
        menu->addChild(createIndexSubmenuItem(
            "Oversampling", {"Off", "2x", "4x", "8x"},
            [=]() { return (size_t)(module->oversample == 8 ? 3 : module->oversample / 2); },
            [=](size_t i) {
                module->oversample = 1 << i;
                module->osRequest = module->oversample;
                module->compileCurrent(false);
            }));
        menu->addChild(createBoolPtrMenuItem("Compile as you type", "", &module->liveCompile));
        menu->addChild(createIndexSubmenuItem(
            "Editor font size", {"Small", "Medium", "Large", "Extra large"},
            [=]() {
                float s = module->fontSize;
                return (size_t)(s < 10 ? 0 : s < 11.5f ? 1 : s < 13 ? 2 : 3);
            },
            [=](size_t i) {
                const float sizes[] = {9.f, 10.5f, 12.f, 14.f};
                module->fontSize = sizes[i];
            }));
        menu->addChild(createMenuLabel("Editor: Ctrl+Enter compile, Ctrl+Z/Y undo/redo, Ctrl+/ comment"));
    }
};

Model* modelFilterLab = createModel<FilterLab, FilterLabWidget>("FilterLab");
