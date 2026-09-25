# Kettle Linux's own frame generation: an implicit Vulkan layer (VK_LAYER_KETTLE_framegen) that
# interpolates frames on the game's own device, no Lossless Scaling needed. Sources live next to
# this PKGBUILD. Off unless KETTLE_FG=1, which the Frame Generation plugin sets per game.
# Proton ARM64 games (x86 code via FEX inside Wine) use the native aarch64 Vulkan loader, so
# this one layer covers them and native arm64 games.
pkgname=kettle-framegen
pkgver=0.1.0
pkgrel=4
pkgdesc='Kettle Linux frame generation Vulkan layer'
arch=(aarch64)
license=(BSD-3-Clause)
depends=(glibc vulkan-icd-loader)
makedepends=(gcc make glslang vulkan-headers)

build() {
  make -C "$startdir" clean
  make -C "$startdir" CC=gcc CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS"
}

package() {
  install -Dm755 "$startdir/build/libVkLayer_kettle_framegen.so" -t "$pkgdir/usr/lib"
  install -Dm644 "$startdir/VkLayer_kettle_framegen.json" -t "$pkgdir/usr/share/vulkan/implicit_layer.d"
  install -Dm644 "$startdir/LICENSE" -t "$pkgdir/usr/share/licenses/$pkgname"
}
