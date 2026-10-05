# Audio banks, bindings and acoustic zones

`Paper::AudioCore` provides definitions, bank parsing/writing, bindings, zones and
the PCM mixer without SDL or Qt. Use it for content validation and headless game
logic. `Paper::Audio` adds WAV decoding and the SDL device facade and continues
to include all core functionality for existing consumers.

`Paper::Audio` accepts editable `paper.audio` TOML banks (`.pabank`, version 2).
Version 1 remains readable with spatial source defaults; writers emit version 2.
`parseAudioBank`, `writeAudioBank`, `loadAudioBank` and `validateAudioBank` validate
unknown keys, IDs, references, ranges and capacities. Definition edits require
no recompilation. `EngineConfig::audioBankFile` opts into a bank relative to the
asset root; existing `sounds` configuration stays supported. The bank is selected
at engine construction. `Audio::replaceBank` and `Engine::replaceAudioBank`
prepare and decode a candidate before swapping it under the live stream lock.
They preserve the device, mute and bus levels; a rejected candidate retains the
working bank. Already queued PCM can finish within device latency. `openBank`
remains available when opening a new device is intended.

```toml
format = "paper.audio"
version = 2
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
spatial = true
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

File → New Audio Bank / Edit Audio Bank and double-clicking `.pabank` files open
a nonmodal editor. Structure provides sound/source/zone trees and batched property
forms. Source bindings offer IDs from the current project; Save rejects missing
node references. Choose WAV selects the first `stem-1.wav` inside `audio/`.
Audition Selected validates every WAV and opens an explicit dry preview for a sound,
source or zone ambience; Stop, applied edits, close and Play release its stream. Nothing plays
automatically when opening a bank. Definitions do not change the running game.

Source and structured edits share Undo/Redo, including across Save. No-op saves
preserve exact bytes; source edits retain comments. Structured changes canonicalize
the bank into one Undo step. Save validates every WAV and uses atomic replacement;
external changes prevent overwriting and require reopening. Invalid definitions
can be repaired in Source. Pending properties apply on Save or selection change;
invalid edits stay in the form. Play includes the open bank's validated draft in
its isolated asset copy. Closing unsaved edits offers Save/Discard/Cancel.

Scene view displays blue oriented acoustic boxes, pink source markers and range
circles while editing. Closing restores the saved bank overlay. Hide Audio Overlays
clears it. Sources follow the selected scene's full node transforms; zones are
world-space bank data. The example bank includes a short synthesized chime.

`AudioBindings` owns an immutable bank and bounded loop slots. Authored sources
are bound initially. `bind(instance, source, node)` creates another instance with
an explicit node override; an empty node uses world-space offsets. Supply a
resolver returning current scene-node `MeshTransform`s. Offsets follow the full
transform. A missing node silences its loop; `event` returns no placement. Events
are resolved on demand, loops in each `frame`. Unbinding clears a slot on the
next frame. The instance ID remains stable while the host manages its lifetime.

Set `spatial = false` for a nonspatial bed. Source offsets still follow their
bound node, while the mixer omits distance, panning and barrier attenuation.
Version 1 banks reject an explicit `spatial` property rather than reinterpreting it.

Call `AudioBindings::frame(scene, resolver, dt)`, then `Audio::apply(frame)` or
`Engine::audioFrame(frame)`.
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
