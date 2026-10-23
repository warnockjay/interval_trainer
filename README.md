# interval_trainer
a simple C script to help me train pitch intervals. It plays a starting pitch, tells you which interval to find, lets you sing or imagine the answer, then plays the real note so you can check yourself. Every trial is saved to a JSON file so you can track your progress.

It is written in C and uses the [miniaudio](https://github.com/mackron/miniaudio) library for sound, so there is nothing to install besides a C compiler.

## Modes

**1. Intervals.** The program shows an interval and a direction (for example, "ascending m3"), plays the starting pitch for 1 second, and waits. Try to sing or imagine the second pitch, press Enter to hear it, then tell the program whether you were right.

**2. Three-note sequences.** The program shows two intervals and plays the starting pitch. Interval 1 is the distance from note 1 to note 2, and interval 2 is the distance from note 1 to note 3. Press Enter to hear notes 2 and 3, then say whether you were right.

Details:

- Intervals used: m2, M2, m3, M3, P4, P5, P8, each ascending or descending.
- All pitches stay between C3 and C5 (C4 is middle C). Starting pitches can be any semitone in that range, such as C#3 or F#4.
- Each note lasts 1 second.
- After each trial you can press Enter for another, `m` to change mode, or `q` to quit.

## Log format

The log is a JSON array with one object per trial:

```json
{
  "timestamp": "2026-10-22T19:03:11Z",
  "mode": "interval",
  "initial_pitch": "C#3",
  "initial_midi": 49,
  "intervals": [
    {"name": "m3", "direction": "ascending"}
  ],
  "correct": true
}
```

- `timestamp` is in UTC and marks when the trial began.
- `mode` is `"interval"` or `"sequence"`.
- `initial_pitch` is the starting note name, and `initial_midi` is the same note as a MIDI number.
- `intervals` has one entry in interval mode and two in sequence mode. In sequence mode, both intervals are measured from the starting pitch.
- `correct` is your own yes/no answer.
