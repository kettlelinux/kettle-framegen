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
