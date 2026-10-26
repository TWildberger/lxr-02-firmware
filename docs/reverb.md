# Plate reverb (FX bus)

The FX bus has a sixth effect type, **`rev`**: a stereo plate reverb (figure-8
tank after J. Dattorro, *Effect Design Part 1*, JAES 1997). Upstream v1.7
contains an unfinished reverb that is compiled out (`USE_REVERB 0`). This
firmware replaces it with a new implementation in
`Core/Src/Soundengine/DSPAudio/FX/Reverb.c`.

## Using it

Like every FX type, the reverb is an insert on the FX bus. A voice is sent to it
by setting the voice's output routing to the FX bus. The FX page (**FX** button)
switches to the reverb sub-page when you select `rev`:

| Slot | Label | Parameter | Range |
|------|-------|-----------|-------|
| 1 | `typ` | FX type (`PAR_FX_TYPE`) | `rev` = 5 |
| 2 | `out` | FX output routing (`PAR_FX_ROUTING`) | stereo or a single output as for the other FX. Mono outputs get the L+R wet mix |
| 3 | `d/w` | Dry/wet (`PAR_FX_DRY_WET`) | 0 … 100 % |
| 4 | `typ` | Reverb type (`PAR_FX_REVERB_TYPE`) | `off`, `Plt`, `Eco` (see below) |
| 5 | `pre` | Pre-delay (`PAR_FX_P1`) | 1 … 46 ms |
| 6 | `dec` | Decay (`PAR_FX_P2`) | RT60 ≈ 0.3 s … 19 s (exponential, ≈2.4 s at the centre) |
| 7 | `ton` | Tone (`PAR_FX_P3`) | centre = neutral. Lower = darker (input low-pass at 1.2 kHz is blended in, more damping in the tank). Higher = brighter (input high-pass at 500 Hz is blended in, less damping) |
| 8 | `ref` | Early reflections (`PAR_FX_P4`) | 0 = none |

**Reverb type**

* `off`: the FX bus passes through dry, exactly like FX type `off`. **This is the
  default.** Set it to `Plt` or `Eco` to hear the reverb.
* `Plt`: the tank runs at the full sample rate.
* `Eco`: the tank runs at half the sample rate, using about half the CPU. The wet
  signal is limited to ≈11 kHz, and all delays are twice as long, so the space
  sounds larger and less dense. The decay time is compensated.

Changing the type, or selecting the reverb again after another FX type,
starts from silence. There is no stale tail.

All parameters are ordinary kit parameters. They are stored in `.snd`, sent as
NRPN (FX type 106, dry/wet 108, P1–P4 = 109–112, reverb type 116, same as
before) and are available over SysEx (`PARAM_SET`, `KIT_DUMP`) and as LFO and
velocity targets (P1–P4).

## Compatibility

No parameter was added, so `.snd`, `glo.cfg`, `params.json` ids and NRPN numbers are
unchanged. Only two ranges grew: FX type max 4 → 5 (`rev`), and reverb type
max 0 → 2 (`off`/`Plt`/`Eco`).

* Kits from stock firmware have FX type 0–4, so the reverb is not used.
* Some kits saved by an older firmware have **FX type 5** (one factory kit set
  was reported to contain it). Stock v1.7 plays them dry because FX type 5 is
  not handled there. Their reverb type byte is 0, which is `off` here, so they
  still play dry. Only a kit that you save with reverb type `Plt`/`Eco` uses
  the reverb. A scan of the 16 factory kits on a device and of 386 `.snd` files
  from SD backups found no kit with a reverb type byte other than 0.
* Kits saved with the reverb and then loaded on stock v1.7 play the FX bus dry.
  The LCD then shows an unknown FX type name.

## Resources

* **Memory:** the reverb needs 20 480 `__fp16` samples (40 KB). It uses the start
  of the shared 64 KB `FX_BUFFER` in DTCM, which the delay also uses. No new
  buffer was added. When the FX type changes to one that keeps audio in
  `FX_BUFFER` (delay, reverb) and another FX left data there, the buffer is
  cleared first with a word-wise clear (≈16 k cycles, once). Only one of these
  FX types can be active at a time.
* **CPU:** see [performance.md](performance.md#plate-reverb). The reverb's time
  is counted in the `fx` bucket of the CPU meter.
* **Code:** the reverb runs from flash (I-cache). It adds no ITCM code. The
  FX parameter setters moved out of ITCM, so ITCM use went *down* 408 bytes.

## Design notes

* Signal path: (L+R) → tone tilt → pre-delay (with 4 early-reflection taps) → two
  parallel pairs of input allpass diffusers → the two halves of the tank. Each
  half is: modulated allpass (±8 samples, 0.8 Hz quadrature LFO) → delay →
  one-pole damping → ×feedback → allpass → delay → ×feedback → into the other
  half. 7 output taps per side give a decorrelated stereo output (L/R
  correlation ≈ 0 in the host test).
* Feedback is `10^(−0.75·T_loop/RT60)` (4 gain stages per loop) and is capped
  at 0.98. The tank signal is clamped to ±4 so the fp16 lines can never hold
  inf/NaN. That would otherwise also poison the delay, which shares the
  buffer.
* The upstream code had several bugs. One allpass was processed twice and another
  never. Mono routing wrote to both outputs. The tone extremes muted the reverb,
  and nothing limited the feedback. The upstream code also switched the
  snare, hi-hat and cymbal filters to a cheaper SVF while the reverb was
  active. That would change the sound of the voices, so it is disabled here
  (`REVERB_CHEAP_VOICE_FILTERS 0` in `FxBus.h`).
* Host tests: `tests/host/test_reverb.c` checks that `off` is bit-exact with FX
  `off`, the dry path, mono routing, measured RT60 against the parameter,
  stereo decorrelation, wet level, stability at maximum settings with
  full-scale input, clear, and type changes.
