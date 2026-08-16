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
fs = import('fs')

# There is no release to tell testing builds apart by, so the version carries
# the commit it was built from: 0.1.0-be9ed29-dev. A build handed to someone
# that cannot say which commit it is is a bug report nobody can act on.
#
# Read from git where there is a git to read, and from a stamped file where
# there is not -- which is every Flatpak build, because the manifest copies this
# directory on its own and .git is above it, outside what the sandbox can see.
# tools/package_flatpak.sh writes the stamp.
build_id = ''
build_date = ''

git = find_program('git', required: false)
if git.found()
  rev = run_command(git, '-C', meson.project_source_root(), 'rev-parse',
                    '--short', 'HEAD', check: false)
  if rev.returncode() == 0
    build_id = rev.stdout().strip()
    # A bundle labelled with a commit it does not match is the failure this is
    # meant to prevent, so an uncommitted tree says so.
    dirty = run_command(git, '-C', meson.project_source_root(), 'status',
                        '--porcelain', check: false)
    if dirty.returncode() == 0 and dirty.stdout().strip() != ''
      build_id += '-dirty'
    endif
    when = run_command(git, '-C', meson.project_source_root(), 'log', '-1',
                       '--format=%cs', check: false)
    if when.returncode() == 0
      build_date = when.stdout().strip()
    endif
  endif
endif

if build_id == '' and fs.exists('build-id.txt')
  stamp = fs.read('build-id.txt').strip().split('\n')
  build_id = stamp[0].strip()
  if stamp.length() > 1
    build_date = stamp[1].strip()
  endif
endif

if build_id == ''
  full_version = meson.project_version() + '-dev'
else
  full_version = meson.project_version() + '-' + build_id + '-dev'
endif

# AppStream will not take a release without one.
if build_date == ''
  build_date = '1970-01-01'
endif

message('version: ' + full_version + ' (' + build_date + ')')

gtk = dependency('gtk4', version: '>= 4.10')
adw = dependency('libadwaita-1', version: '>= 1.4')
glib = dependency('glib-2.0')
gio = dependency('gio-2.0')

blueprint = find_program('blueprint-compiler')

subdir('data')
subdir('src')
