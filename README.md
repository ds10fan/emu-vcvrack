# emu-vcvrack: E-mu chips as VCV Rack modules

A VCV Rack 2 plugin with four modules: the H-chip filter, the G-chip sample interpolator as a
sampler, a small drive, and a 7-track CV/gate recorder. **H-Chip Filter** is the digital filter section of E-mu's
patent US 5,170,369 (Rossum, "Dynamic digital IIR audio filter"), the filter block of the
Emulator IV's H-chip, run cycle by cycle, and programmed with the filter recipes of the EOS
firmware. Thirty of the E4's filter types are available: the stock lowpass, highpass, bandpass,
swept EQ, phaser, flanger and vocal types, and the OB-Xa and Fuzz types.

The DSP is a C++ port of a Rust cycle-level emulator of the patent and is bit-exact against it
(`test/`). The filter recipes (`dsp/emu_filters.cpp`: the decompiled `computeFrames` of EOS
3.20e and the OB-Xa / Fuzz reference) are used only to turn (filter type, Q) into the ten control words of a voice.

## What is modelled

`dsp/hchip.hpp`, `dsp/hchip.cpp`:

- FIG. 6 datapath: adder, four muxes, two accumulators, delay memory latches, multiplier input
  latches, run from the reservation table as microcode (nine control words per section, twelve
  with the gain rows);
- FIG. 7 block floating point multiplier as a three-latch pipeline;
- FIG. 8 coefficient decoder with the difference-coded first coefficient and the code table
  recovered on the chip;
- the voice: three sections in series from the E4's ten words, cutoff and flags
  (as measured on the E4): normalised and unnormalised pole
  sections, zero sections, the gain word.

Widths are the emulator's defaults: 24-bit wrapping accumulator and input, 16-bit saturating
delay memory, 16-bit saturating multiplier operand, 16-bit coefficient mantissa, floor-truncated
product. The module's context menu offers the
alternatives the patent leaves open: a 17- or 18-bit multiplier operand, round-to-nearest
products, a saturating accumulator.

**Not ported:** the interpolator of FIGs. 9 to 12, which moves the interpolating variable x
toward a destination over 32 to 4096 samples. Here the cutoff control is turned straight into x:
a cutoff of c (0..255, continuous) gives x = c / 255 × 65535, so a cutoff byte lands exactly on
the chip's c × 0x0101 and values in between are the chip's own fine steps. The codes are
re-interpolated whenever x changes, as the coefficient math unit would.

**Not modelled:** the voice gain compensation the firmware applies outside the filter (the
`ret` value of the recipes), the sample path before and after the filter, and the morph filter
types (Dual EQ Morph, 2EQ+Lowpass Morph, 2EQMorph+Exprssn, Peak/Shelf Morph), which take six
more parameters than the module has controls for.

## The module

| Control | Function |
|---|---|
| TYPE | filter type: click the display (left or right button) for the list of types, step with the < and > buttons (wrapping around) or scroll over the display; also in the context menu. The display shows the E4 menu name |
| CUTOFF, CV, attenuverter | cutoff 0..255 as on the E4, with 32 steps per volt of CV (about one octave on the lowpass display law) |
| Q, CV, attenuverter | the second parameter, 0..127 (Q, Gain, Resonance, Body Size or Fuzz depending on the type), 10 V of CV for the full range |
| DRIVE | input level into the chip, −24 to +24 dB, compensated at the output; 5 V peak is 16-bit full scale at 0 dB |
| IN, OUT | audio, polyphonic (one chip voice per channel) |
| CLIP | lights when the delay memory, the multiplier operand or the accumulator saturated |

The display's second line is what the E4 shows for the two parameters: the frequency of the
display law (`fil_freq`), the gain in dB for the EQ types, or the raw value. The frequency is
scaled by the engine sample rate over 48 kHz: the chip runs at the engine rate and its
coefficients do not know the clock, so at 96 kHz the same words sit one octave higher.

## DS10 Drive

A second, small module: a hard clipper whose drive also sets the dry/wet mix,

```
out = mix(in, clamp(in * drive * 4, -1, 1), drive),   mix(a, b, t) = a + (b - a) t
```

per sample (`dsp/ds10_drive.hpp`), with 5 V as the 1.0 of the formula. DRIVE knob 0..100 %,
CV input with attenuverter (10 V for the full range), drive clamped to 0..1, polyphonic.

## G-Chip Sampler

A WAV sample player whose resampler is the interpolator of US 5,111,727 (Rossum, "Digital
sampling instrument for digital audio data"), the E-mu G-chip of the Emax II / Emulator III
generation, implemented from a specification of that interpolator and nothing else:

- one 223-point linear-phase FIR prototype at 32 sub-phases; the symmetric half (112 words plus
  a zero at address 112) is the coefficient ROM, Table 1 of the patent (`dsp/gchip_proto.inc`,
  generated from the spec's list);
- seven taps per output sample on `x[i-2 .. i+4]`, coefficient addresses `|(-80 + 32 n) - f_MS|`,
  each blended toward the next ROM word by the fine phase, normalised to a sum of 1;
- a phase accumulator with 32 fraction bits per voice.

`test/gchip_test.cpp` checks the table's checksums and symmetry, the seven coefficient vectors
and the resampling vector of the spec's section 7 to 1e-9, the sum and symmetry invariants over
4096 phases, and an alias floor of −77 dB for a sine shifted up by 2^(1/3) (the spec's figure for
an implementation without the fine blend is −43 dB). The output point of the interpolator is
`x(i + f + 0.5)`: at phase 0 the taps sit half a sample either side of the centre, as the spec's
section 5 says and its resampling vector confirms.

| Control | Function |
|---|---|
| display | the waveform with start (green) and loop (blue) markers and the play heads; click it to load a WAV. A WAV dropped on the module loads too |
| TUNE, FINE | ±24 semitones, ±100 cents, added to the V/OCT input; 0 V plays the file at its own rate |
| START | where a gate starts playback, as a fraction of the file |
| L.START, L.END, LOOP | loop points as fractions of the file (set from the file's `smpl` chunk when it has one) and the loop mode: OFF plays once, GATE loops while the gate is high and then plays on to the end, ON loops until the next gate |
| V/OCT, GATE | polyphonic; one voice per channel, retriggered on each rising edge |
| OUT, EOC | audio (16-bit full scale is 5 V) and a 1 ms trigger when a voice runs off the end |

Context menu: "Fixed-point datapath" switches to the spec's scalar form with 16-bit coefficients
(unity 2^14), an 8-bit blend weight and a 32-bit accumulator, the widths section 8 of the spec
measures as the knee; the default computes the same taps in double precision. Both paths play
the same 16-bit words: files are converted to 16-bit mono on load (channels averaged), and the
file's sample rate against the engine rate is part of the pitch ratio. A loop is read with
wrapped addresses, which stands in for the replicated guard samples the spec asks for, so loops
need no prepared files. The file path is stored in the patch, not the audio.

Not in the spec and not modelled: the filter family of section 12 (only one impulse response is
published), the volume and mix stages, the four-bank memory timing.

## minty-seq

A 7-track CV/gate step recorder (`dsp/mintyseq.hpp`, `src/MintySeq.cpp`). Each track is 4 bars
of sixteenths (64 steps) in two patterns, A and B; ten sets hold seven tracks each. One track is
selected; what arrives at the CV and GATE inputs is recorded onto its current pattern, quantised
to the nearest sixteenth (a key pressed in the second half of a step lands on the next one), with
the key's length as ties (at least one step; a key held longer than the loop fills it). A note
written over earlier notes replaces what it covers and cuts the tail of a note it interrupts.
The selected track also plays the live input through while a key is held, so you hear what you
record.

| Control | Function |
|---|---|
| BPM | the internal clock, 30 to 300, default 127; ignored while CLOCK is connected |
| CLOCK, RESET | external clock, one pulse per sixteenth, and a reset to bar 1 (the next pulse plays step 1); the period is measured so recording still quantises |
| PLAY | start (from bar 1) / stop; stopping also stops recording and closes the gates |
| REC | record on the selected track; starts the transport if it is stopped |
| CLEAR | clear the selected track's current pattern |
| SEL (per track) | select the track; the lane shows the pattern and the play head, click it to select too |
| A/B (per track) | which pattern the track plays, switched at once; lit for B |
| SET < > | one of ten sets of seven tracks; the A/B choice is part of the set |
| CV, GATE in | what to record |
| PLAY, REC, CLEAR in | triggers for the same three buttons, for MIDI or CV control |
| CV, GATE out × 7 | one pair per track; a new note right after another gets a 1 ms gate gap |
| CLICK out | a metronome click (audio): white noise through a 2 kHz high-pass with an exponential decay, on every beat, louder and longer on the first beat of a bar |

Keyboard: while the mouse is over the module or after clicking it, Space starts and stops, Enter
records, Shift+Backspace (or Shift+Delete) clears, Up/Down select the track, Tab switches the
selected track's A/B, Left/Right change the set. These keys are outside Rack's computer-keyboard
MIDI map (letters, digits, brackets, punctuation), so that keyboard keeps playing notes, and
outside Rack's own module shortcuts (a plain Backspace still deletes the module). Everything is
saved in the patch: the sets, the A/B choices, the selected set and track. The transport starts
stopped.

## Build

```
# the Rack SDK, once (https://vcvrack.com/downloads)
unzip Rack-SDK-2.6.4-lin-x64.zip -d sdk/
make -j8            # plugin.so
make dist           # dist/EmuHChip-2.0.0-lin-x64.vcvplugin
make install        # copies it into ~/.local/share/Rack2/plugins-lin-x64/
```

`RACK_DIR` selects another SDK location.

## Tests

```
make -C test check    # dsp/hchip.cpp against test/expected/vectors.txt, the recipes, and the measured responses
make -C test ds10     # the DS10 drive formula
make -C test gchip    # the G-chip interpolator against the spec's vectors, the voice, the WAV reader
make -C test minty    # the sequencer core: quantised recording, ties, playback, clocks
make -C test smoke    # the Rack module headless against the SDK's libRack.so (16 channels, every type, cost)
```

`test/expected/vectors.txt` holds fifteen cases (stock types at byte and arbitrary cutoffs,
alternative widths, flags 0x00, 0x40 and 0xC0) of 3000 samples each, written by the Rust
emulator; the C++ port must reproduce every sample and every saturation counter. The response
checks reproduce the emulator's measured responses (2 and 6 pole lowpass, 4th
order highpass at cutoff 128, Q 64).

## Layout

```
plugin.json, Makefile        the Rack plugin
src/plugin.cpp, plugin.hpp   plugin entry
src/HChipFilter.cpp          the filter module: voices, controls, display, context menu
src/DS10Drive.cpp            the drive module
src/GChip.cpp                the sampler module: loading, voices, waveform display
src/MintySeq.cpp             the recorder module: I/O, buttons, keys, displays, patch storage
src/widgets.hpp              runtime panel labels shared by the modules
res/*.svg                    the panel graphics (labels are drawn at runtime)
dsp/hchip.hpp, hchip.cpp     the chip model
dsp/ds10_drive.hpp           the drive formula
dsp/gchip.hpp, gchip.cpp     the G-chip interpolator and playback voice; gchip_proto.inc is Table 1
dsp/wav.hpp, wav.cpp         RIFF/WAVE reader (PCM 8/16/24/32, float, smpl loop)
dsp/mintyseq.hpp             the recorder core: sets, patterns, quantised recording, clocks
dsp/emu_filters.hpp, .cpp    the EOS filter recipes
test/hchip_test.cpp          DSP tests; test/expected/vectors.txt the reference vectors
test/ds10_test.cpp           drive formula test
test/gchip_test.cpp          interpolator, voice and WAV reader tests
test/mintyseq_test.cpp       recorder core tests
test/smoke/smoke.cpp         headless module test
```
