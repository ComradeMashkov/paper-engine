# Audio banks, bindings and acoustic zones

`Paper::Audio` accepts editable `paper.audio` TOML banks (`.pabank`, version 1).
`parseAudioBank`, `writeAudioBank`, `loadAudioBank` and `validateAudioBank` validate
unknown keys, IDs, references, ranges and capacities. Definition edits require
no recompilation. `EngineConfig::audioBankFile` opts into a bank relative to the
asset root; existing `sounds` configuration stays supported. The bank is selected
at engine construction, or replaced explicitly through `Audio::openBank`.

```toml
format = "paper.audio"
version = 1
[bank]
[[bank.sounds]]
id = "motor.hum"
file = "machines/hum"
bus = "ambience"
variants = 2
loop = true
gain = 0.5
[[bank.sources]]
id = "motor"
sound = "motor.hum"
node = "machine"
offset = [0, 0.5, 0]
range = 12
[[bank.zones]]
id = "workshop"
center = [0, 2, 0]
half = [4, 2, 4]
rotation = [0, 0, 0, 1]
priority = 1
fade = 1
wet = 0.5
feedback = 0.4
damping = 0.3
ambience = "motor.hum"
```

Supply `audio/machines/hum-1.wav` and `hum-2.wav` under the asset root. Strict
loading checks canonical paths, every variant and finite PCM before replacing a
live stream. Failed definition/WAV validation leaves the old stream intact.
Device-open failures are reported through the existing disabled-audio behavior.
Files cap at 32 MiB, decoded sounds at 60 seconds and the bank at 256 MiB PCM.
Sources cap at 4096, zones at 256, sounds at 256, variants at 64.

The editor's File → Edit Audio Bank opens a source editor with Undo/Redo,
validation of every WAV, and atomic Save. Comments and no-op bytes remain intact;
external changes prevent overwriting and require reopening. Invalid definitions
can be repaired before Save. Closing unsaved edits offers Save/Discard/Cancel.
This editor changes bank content; it does not audition sounds or run the game.

`AudioBindings` owns an immutable bank and bounded loop slots. Authored sources
are bound initially. `bind(instance, source, node)` creates another instance with
an explicit node override; an empty node uses world-space offsets. Supply a
resolver returning current scene-node `MeshTransform`s. Offsets follow the full
transform. A missing node silences its loop; `event` returns no placement. Events
are resolved on demand, loops in each `frame`. Unbinding clears a slot on the
next frame. The instance ID remains stable while the host manages its lifetime.

Call `AudioBindings::frame(scene, resolver, dt)`, then `Audio::apply(frame)`.
The complete scene/loop snapshot is submitted under one SDL stream lock.
Positions, banks and strings are prepared on the host thread. The PCM callback
performs no file access, allocation or scene traversal.

Acoustic boxes support full quaternion orientation. Boundary fade uses inward
distance in metres. The highest active priority wins; overlapping zones of that
priority blend by normalized weights. Outside zones the original global echo
profile applies. Wet/feedback/damping transition with a 0.2 second response.
Ambience has separate stable loop slots and fades with zone weight. All looping
sources and zone ambience together must fit 16 slots; exhaustion fails explicitly
and never evicts a source. Feedback caps at 0.95 and damping at 0.99 to keep tails
stable. Acoustic barriers continue using the shared oriented box ray test.
