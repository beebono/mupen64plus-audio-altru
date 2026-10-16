# Options

Config section `Audio-Altru` in `mupen64plus.cfg`. When the section does not exist yet and `Audio-SDL` does, these keys are copied from it on the first
start: `DEFAULT_FREQUENCY`, `SWAP_CHANNELS`, `PRIMARY_BUFFER_SIZE`, `PRIMARY_BUFFER_TARGET`, `SECONDARY_BUFFER_SIZE`, `RESAMPLE`, `VOLUME_ADJUST`, `VOLUME_DEFAULT`, `AUDIO_SYNC`.
Keys given with `--set Audio-Altru[...]` before the start are kept. `Audio-SDL` is only read.

| key | default | meaning |
|---|---|---|
| `DEFAULT_FREQUENCY` | 33600 | rate used until the game sets one |
| `SWAP_CHANNELS` | False | swap left and right |
| `PRIMARY_BUFFER_SIZE` | 16384 | size of the N64 sample buffer, in output samples |
| `PRIMARY_BUFFER_TARGET` | 2048 | fill target, in output samples |
| `SECONDARY_BUFFER_SIZE` | 1024 | SDL device buffer, in output samples |
| `RESAMPLE` | trivial | resampler |
| `VOLUME_ADJUST`, `VOLUME_DEFAULT` | 5, 80 | volume step, start volume |
| `AUDIO_SYNC` | True | delay the core to the audio clock |
