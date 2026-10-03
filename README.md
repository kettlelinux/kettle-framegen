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
4. marks what stayed the same in both frames (HUDs, black bars), so the scene's motion
   neither moves it nor samples from it,
5. fills spare swapchain images with frames interpolated along that motion, taking background
   that a moving object uncovers from the frame where it shows,
6. presents those, then the game's frame. FIFO paces them.

Where the motion is lost, because the camera turns faster than the search reaches (about 160
pixels per frame at 3440x1440), a generated frame shows the nearer real frame again: in the
regions whose blocks all match poorly, and in the whole frame once enough of its blocks are
unmatched (from 15%, fully at 30%), as at a scene cut. That is briefly uneven, but cleaner than
a patchwork of half-fitting vectors.

Swapchains get up to 2 extra images for the generated frames, so the highest multiplier is 3x.
Beyond that, too few frames are real: below about 50 rendered fps, generated frames show more
artifacts than they smooth.

## Building

You need a C compiler, `make`, `glslangValidator` (glslang) and the Vulkan headers. The
shaders are compiled to SPIR-V and embedded in the library.

```sh
make
sudo make install            # PREFIX=/usr by default; DESTDIR and LIBDIR work as usual
```

This installs `libVkLayer_kettle_framegen.so` and its manifest in
`$PREFIX/share/vulkan/implicit_layer.d`.

### 32-bit games

32-bit games, including older ones that run through Proton, load 32-bit Vulkan layers, so on
x86_64 they also need a 32-bit copy of the layer. Building it needs a multilib compiler
(`lib32-gcc-libs` on Arch, `gcc-multilib` on Debian and Ubuntu):

```sh
make lib32
sudo make install-lib32      # LIB32DIR=$PREFIX/lib32 by default
```

This installs the library in `$LIB32DIR` and a second manifest,
`VkLayer_kettle_framegen_32.json`, for a layer named `VK_LAYER_KETTLE_framegen_32`. The Vulkan
loader picks layers by name, so the two copies need different names. Each game then loads
the copy that matches its own architecture. `make install-lib32` takes the same `PREFIX`,
`DESTDIR`, `CC` and `CFLAGS` as `make install`, and `CFLAGS32` (default `-m32`) holds the
32-bit flags.

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
19 of 19 cases passed
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
CASES=test/cases/<name>`. For a case with a truth, set `burst = 8` with `dump`: the next 8 real
frames in a row are saved, and every other one is the truth for a frame generated between its
neighbours (`0.ppm`, `1.ppm`, ... from frames 0, 2, 4, ... and `truth/1-1.ppm`, ... from 1, 3,
...). The motion between them is twice the game's, so they are harder than the game itself.

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
| `multiplier` | `2` | Frames shown per rendered frame, 1 to 3, or `auto` (below). `1` turns generation off. |
| `refresh` | from the display | Display refresh rate in Hz, for `auto` (below). |
| `mode` | `motion` | `blend` mixes frames without motion estimation. |
| `flow_scale` | `0.5` | Resolution of the motion estimate as a fraction of the frame, 0.1 to 1. |
| `fifo` | `true` | Force FIFO presentation, also for games that pick a present mode per frame (DXVK, vkd3d-proton). Read when the swapchain is created. |
| `preserve_images` | `false` | Add no extra swapchain images. Read when the swapchain is created. |
| `stats` | `false` | Log GPU time per stage every 2 seconds. |
| `latency` | `normal` | `low` holds the game until its previous frame is on screen (below). |
| `dump` | unset | Directory to save the next generated frames to, for debugging. |
| `burst` | `0` | With `dump`: save this many real frames in a row instead (up to 16). |

With FIFO, frames are shown evenly only when the game renders fast enough to fill every refresh
with the multiplier: a 45 fps game at 2x on a 120 Hz display shows each generated frame for one
refresh and each rendered one for nearly two, which judders. `multiplier = auto` picks the
fewest frames per rendered one that fill every refresh (3x at 70 fps on 180 Hz), and FIFO then
holds the game at refresh / multiplier (60 fps here). Whether the game could fill it with one
less doesn't show while it is held, so the layer tries one less now and then: it keeps it if
the display still fills, and otherwise goes back (a second or two of judder) and doubles the
wait before the next try, up to 5 minutes. A raise that falls well short of the frames it
should show (the swapchain can run short of images) is undone and not retried for 10 seconds,
doubling while it keeps failing. A game slower than refresh / 3 (60 fps on 180 Hz) can't fill
every refresh even at 3x. The log shows each change.

Where the GPU's driver has `VK_EXT_present_timing` (RADV; Turnip from the Adreno 750 of the
Snapdragon 8 Gen 3), the display tells `auto` its refresh rate and the log shows it. Elsewhere
(Adreno 740 and older, PanVK) set `refresh`: without it, the layer takes the display as full
when the game waits for it and measures the refresh rate there, but a GPU-bound game also waits
short of full, so `auto` can settle too low; with a frame limiter between the layer and the
display, it measures the limit. A `refresh` setting always wins.

Generated frames between two real ones need the later one rendered, so the game always runs a
frame ahead of the display. Any further and its frames wait in the swapchain's queue, which is
input latency. With `latency = low`, after each of the game's frames the layer waits until the
previous one is on screen, so the game starts its next frame that much later and closer to
being shown. It needs `VK_KHR_present_wait` (RADV, Turnip and PanVK have it; the layer turns it
on). Many games already keep their own queue short (vkd3d-proton and DXVK do), and at a full
display the layer's own wait for spare images does too, so `stats` logs how often the previous
frame wasn't shown yet: near 0%, `low` changes nothing.

The layer logs to stderr with the prefix `VK_LAYER_KETTLE_framegen:`.

While a game runs, `$XDG_RUNTIME_DIR/kettle-framegen/<pid>` holds the multiplier in use as
`multiplier=<n>` (the one `auto` picked, or the setting; `1` while generation is off), for tools
that show it. The layer rewrites it when the multiplier changes and removes it when the game exits
normally. It is the game's own runtime directory: in a container such as Steam's pressure-vessel
(Proton) that is not the host's, so read it as `/proc/<pid>/root/$XDG_RUNTIME_DIR/...`, with the
game's `XDG_RUNTIME_DIR`.

## License

BSD 3-Clause, see [LICENSE](LICENSE).
