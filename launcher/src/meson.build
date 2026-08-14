sources = files(
  'main.vala',
  'application.vala',
  'window.vala',
  'game.vala',
  'profiles.vala',
)

executable('frlg-launcher',
  sources, resources,
  dependencies: [gtk, adw, glib, gio],
  # _() reaches gi18n-lib.h, which insists on knowing the catalogue's name even
  # when there are no translations yet.
  c_args: ['-DGETTEXT_PACKAGE="frlg-launcher"'],
  install: true,
)
