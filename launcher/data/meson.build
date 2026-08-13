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
