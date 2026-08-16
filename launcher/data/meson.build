# One named output per blueprint, not a directory.
#
# `batch-compile` into '.' looks tidier and quietly breaks incremental builds:
# Meson cannot depend on a directory, so glib-compile-resources never sees that
# window.ui changed, and the binary keeps an embedded copy of the UI from
# whenever the resource last happened to rebuild. The symptom is edits to the
# .blp having no effect at all -- a menu item that is in the file, in the
# compiled .ui, and not in the program.
window_ui = custom_target('window.ui',
  input: files('window.blp'),
  output: 'window.ui',
  command: [blueprint, 'compile', '--output', '@OUTPUT@', '@INPUT@'],
)

resources = gnome.compile_resources('frlg-resources',
  'frlg.gresource.xml',
  dependencies: [window_ui],
  source_dir: meson.current_build_dir(),
)

# Desktop integration. Needed by any packaged build: without the .desktop file
# there is no entry to launch, and without the icon the entry has no face. The
# names are the application id because that is what the shell matches on.
app_id = 'io.github.softarv.frlg'

install_data(app_id + '.desktop',
  install_dir: get_option('datadir') / 'applications',
)

# Generated, because the version in it is the one thing that tells two testing
# builds apart -- see the top-level meson.build.
metainfo = configure_file(
  input: app_id + '.metainfo.xml.in',
  output: app_id + '.metainfo.xml',
  configuration: {'VERSION': full_version, 'DATE': build_date},
)

install_data(metainfo,
  install_dir: get_option('datadir') / 'metainfo',
)

install_data('icons' / (app_id + '.svg'),
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps',
)
