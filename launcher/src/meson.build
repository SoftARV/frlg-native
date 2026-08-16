sources = files(
  'main.vala',
  'application.vala',
  'window.vala',
  'game.vala',
  'profiles.vala',
  'settings.vala',
)

# So the About dialog names the same build the package does.
config_vala = configure_file(
  input: 'config.vala.in',
  output: 'config.vala',
  configuration: {'VERSION': full_version},
)

executable('frlg-launcher',
  sources, config_vala, resources,
  dependencies: [gtk, adw, glib, gio],
  # _() reaches gi18n-lib.h, which insists on knowing the catalogue's name even
  # when there are no translations yet.
  c_args: ['-DGETTEXT_PACKAGE="frlg-launcher"'],
  install: true,
)
