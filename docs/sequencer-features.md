# Sequencer conditions and beat repeat

## Trig conditions

The sequencer page's **cond** step field edits the selected step. Conditions
are evaluated against that track's completed pattern cycles, starting at cycle
zero when playback starts or the pattern changes:

| Value | Trigger rule |
|-------|--------------|
| Always | Every cycle |
| 2nd / 3rd / 4th | Every 2nd, 3rd or 4th cycle |
| 1:2 / 1:3 / 1:4 | First cycle of each 2-, 3- or 4-cycle group |
| n1s | Every cycle after the first |

Step probability remains independent and is applied together with the
condition. Existing flam timing remains the ratchet control (x2, x3, x4, x6,
x8); each step also retains its two existing automation parameter locks.

## Sequencer beat repeat

Set the repeat rate in the **pattern settings** page (16th, 8th, quarter or
half note). In performance mode, hold the eighth performance pad (the
otherwise unused eighth roll pad) to repeat the current segment; release it
to resume at the position the sequencer would naturally have reached. The
repeat holds the condition-cycle context for the loop, while the hidden natural
position and cycle counters continue advancing. Stopping the sequencer or
switching patterns also releases the repeat. This loops sequencer steps and is
independent of the audio freeze effect.

The existing song mode already stores and edits a 64-entry pattern chain.
Additional per-track ratchet controls, more than two parameter locks per step,
and undo/bar-range editing are deferred to avoid changing the established
seven-byte step format and broadening this storage migration.

## Hardware validation checklist

Hardware checks are pending; do not treat host tests as audio verification.

- Load a legacy `.pat` and confirm every step remains unconditional and sounds
  as before; save it as V04, reload it, and check all condition values.
- Exercise each condition across at least four pattern cycles, with probability
  both disabled and below 100%, and verify independent track-cycle behavior.
- Set each beat-repeat rate, hold/release the eighth performance pad while
  running, and verify natural-position resynchronization. Also test stop,
  pattern switch, external clock and repeat near track wrap.
- Run `tools/lxr_status.py` during ordinary playback and repeat use; record
  CPU peak and xruns, and listen for dropouts.
- Verify the new V04 `.pat` and `tmp.pat` data survives save, power-cycle and
  reload. Confirm kit `.snd`/`tmp.snd` sizes and the `EngV01` trailer remain
  unchanged.
