blueprints = custom_target('blueprints',
  input: files('window.blp'),
  output: '.',
  command: [blueprint, 'batch-compile', '@OUTPUT@', '@CURRENT_SOURCE_DIR@', '@INPUT@'],
)

resources = gnome.compile_resources('frlg-resources',
  'frlg.gresource.xml',
  dependencies: blueprints,
  source_dir: meson.current_build_dir(),
)
