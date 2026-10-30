#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
    pluginInstance = p;
    p->addModel(modelHChipFilter);
    p->addModel(modelDS10Drive);
    p->addModel(modelGChip);
    p->addModel(modelMintySeq);
    p->addModel(modelMidiVerb);
}
