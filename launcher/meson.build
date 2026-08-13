# The launcher is its own project on purpose.
#
# It is 64-bit and the game is -m32: there is no lib32-gtk4, so these cannot be
# one binary even if we wanted them to be. They talk by running each other --
# see docs/adr/0017-launcher-is-a-separate-program.md.
#
# It is also GNOME-specific, and that is fine: a launcher is a platform shell,
# not the app. The game stays SDL-only and runs perfectly well with no launcher
# at all.
project('frlg-launcher', ['c', 'vala'],
  version: '0.1.0',
  meson_version: '>= 1.0.0',
  # warning_level stays low on purpose: what the compiler sees is Vala's
  # generated C, and warning about unused variables in generated code is noise
  # nobody can act on.
  default_options: ['warning_level=1'],
)

gnome = import('gnome')

gtk = dependency('gtk4', version: '>= 4.10')
adw = dependency('libadwaita-1', version: '>= 1.4')
glib = dependency('glib-2.0')
gio = dependency('gio-2.0')

blueprint = find_program('blueprint-compiler')

subdir('data')
subdir('src')
