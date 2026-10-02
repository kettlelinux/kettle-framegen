# kettle-framegen

Frame generation for Vulkan games, as an implicit Vulkan layer (`VK_LAYER_KETTLE_framegen`).
It works with native Vulkan games and with Proton's DXVK and vkd3d-proton. It was written for
[Kettle Linux](https://github.com/kettlelinux/kettlelinux) on Snapdragon handhelds, but it is
plain C and Vulkan with nothing tied to Kettle.

Everything runs on the game's own `VkDevice` and present queue. There is no second device and
no image is shared between devices. On each `vkQueuePresentKHR` the layer:

1. copies the presented image into a two-frame history,
2. builds a luma pyramid of it,
3. estimates motion between the previous frame and this one, coarse to fine,
4. fills spare swapchain images with frames interpolated along that motion,
5. presents those, then the game's frame. FIFO paces them.

Swapchains get up to 3 extra images for the generated frames, so the highest multiplier is 4x.

## Building

You need a C compiler, `make`, `glslangValidator` (glslang) and the Vulkan headers. The
shaders are compiled to SPIR-V and embedded in the library.

```sh
make
sudo make install            # PREFIX=/usr by default; DESTDIR and LIBDIR work as usual
```

This installs `libVkLayer_kettle_framegen.so` and its manifest in
`$PREFIX/share/vulkan/implicit_layer.d`.

## Testing

`make test` runs the cases in `test/cases` through the shaders on a headless Vulkan device and
compares the generated frames with stored references. It needs the Vulkan loader and a driver;
without a GPU, Mesa's lavapipe works (CI uses it). `FGTEST_DEVICE=<n>` picks another device.

```
$ make test
build/fgtest  test/cases/blend test/cases/cut ...
device: llvmpipe (LLVM 21.1.8, 256 bits)
...
object          1-1   cut  0.7%  truth  32.19 dB  ref  63.97 dB   0.00% off  ok
...
10 of 10 cases passed
```

Per generated frame: the share of blocks no vector matched (above 30% counts as a scene cut),
the PSNR against the true in-between frame, and the PSNR and share of pixels off by more than
24 against the reference.

Each case is a directory of frames (`0.ppm`, `1.ppm`, ...), an optional `case.conf`
(`multiplier`, `flow_scale`, `mode`, and the tolerances `min_psnr` and `max_bad`), the
references in `ref/` and, for the synthetic cases, the true in-between frames in `truth/`. The
PSNR against the truth measures quality; the references catch changes. Drivers round
differently, so a frame passes when it is close to its reference, not identical: see
[test/fgtest.c](test/fgtest.c).

After a change that is meant to alter the output, look at the new frames (`make test
FGTEST_FLAGS="-o out"` writes them, with difference images for the failures), check that the
truth PSNRs didn't drop, then `make test-update` to replace the references. `CASES=...` limits
either to some cases.

The synthetic cases come from `test/make-cases.py`. To add a case from a game, set `dump` (see
below) and copy the first and last frame of the dump to a new case as `0.ppm` and `1.ppm`, with
`multiplier` set to the number of frames the dump holds minus one; then `make test-update
CASES=test/cases/<name>`.

## Using it

The layer stays off unless the game's environment has `KETTLE_FG=1`. On Steam, set it per game
in the launch options:

```
KETTLE_FG=1 %command%
```

`DISABLE_KETTLE_FG=1` forces it off.

## Settings

Settings come from a per-game file of `key = value` lines (`#` at the start of a line or after
whitespace starts a comment). The layer rereads the file while the game runs, so changes apply
without a restart, except the two marked below. The file is the first of:

- `$KETTLE_FG_CONFIG`
- `$XDG_CONFIG_HOME/kettle-framegen/<SteamAppId>.conf`
- `~/.config/kettle-framegen/<SteamAppId>.conf`

Without a `SteamAppId`, `default` is used as the name. A `KETTLE_FG_<KEY>` variable (for
example `KETTLE_FG_MULTIPLIER=3`) overrides the file.

| Key | Default | Meaning |
| --- | --- | --- |
| `multiplier` | `2` | Frames shown per rendered frame, 1 to 4. `1` turns generation off. |
| `mode` | `motion` | `blend` mixes frames without motion estimation. |
| `flow_scale` | `0.5` | Resolution of the motion estimate as a fraction of the frame, 0.1 to 1. |
| `fifo` | `true` | Force FIFO presentation. Read when the swapchain is created. |
| `preserve_images` | `false` | Add no extra swapchain images. Read when the swapchain is created. |
| `stats` | `false` | Log GPU time per stage every 2 seconds. |
| `dump` | unset | Directory to save the next generated frames to, for debugging. |

The layer logs to stderr with the prefix `VK_LAYER_KETTLE_framegen:`.

## License

BSD 3-Clause, see [LICENSE](LICENSE).
