# emu-vcvrack: the E-mu H-chip filter as a VCV Rack module

A VCV Rack 2 plugin with two modules. **H-Chip Filter** is the digital filter section of E-mu's
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
| TYPE | filter type (snap knob; also in the context menu); the display shows the E4 menu name |
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
src/widgets.hpp              runtime panel labels shared by the modules
res/HChipFilter.svg, DS10Drive.svg   the panel graphics (labels are drawn at runtime)
dsp/hchip.hpp, hchip.cpp     the chip model
dsp/ds10_drive.hpp           the drive formula
dsp/emu_filters.hpp, .cpp    the EOS filter recipes
test/hchip_test.cpp          DSP tests; test/expected/vectors.txt the reference vectors
test/ds10_test.cpp           drive formula test
test/smoke/smoke.cpp         headless module test
```
