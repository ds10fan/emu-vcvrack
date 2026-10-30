// Widgets shared by the modules.
#pragma once
#include "plugin.hpp"

#include <vector>

// Panel labels drawn at runtime (the panel SVGs hold only the graphics; nanosvg renders no text).
struct PanelLabels : widget::Widget {
    struct Label {
        Vec pos;  // mm, centre
        const char* text;
        float size;
        bool bold;
        NVGcolor color;
    };
    std::vector<Label> labels;

    void draw(const DrawArgs& args) override {
        std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
        std::shared_ptr<window::Font> bold = APP->window->loadFont(asset::system("res/fonts/DejaVuSans-Bold.ttf"));
        if (!font) return;
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        for (const Label& l : labels) {
            nvgFontFaceId(args.vg, (l.bold && bold) ? bold->handle : font->handle);
            nvgFontSize(args.vg, l.size);
            nvgFillColor(args.vg, l.color);
            Vec p = mm2px(l.pos);
            nvgText(args.vg, p.x, p.y, l.text, nullptr);
        }
        Widget::draw(args);
    }
};

const NVGcolor kPanelDark = nvgRGB(0x20, 0x20, 0x24);
const NVGcolor kPanelLight = nvgRGB(0xf0, 0xf0, 0xf0);
const NVGcolor kPanelGrey = nvgRGB(0xb0, 0xb0, 0xb0);
