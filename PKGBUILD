# Maintainer: (you)
# Local build: run `makepkg -si` from the repository root.
pkgname=mattexplorer
pkgver=0.0.19
pkgrel=1
pkgdesc="Fast, dependency-free file manager for Wayland (Omarchy)"
arch=('x86_64')
url="https://github.com/CHANGEME/mattexplorer"
license=('MIT')
depends=('glibc')
optdepends=('sudo: change the permissions or owner of files you do not own, as root'
            'udisks2: mount, unmount and eject drives from the sidebar'
            'xdg-utils: xdg-open for files no application claims'
            'shared-mime-info: file types from names (present on every desktop)')
makedepends=('cmake' 'ninja' 'clang')
source=()
sha256sums=()

build() {
  cmake -S "$startdir" -B "$srcdir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build "$srcdir/build"
}

check() {
  cmake --build "$srcdir/build" --target check
}

package() {
  DESTDIR="$pkgdir" cmake --install "$srcdir/build"
}
