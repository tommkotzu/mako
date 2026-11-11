# Home Manager module: programs.mako-files
# (services.mako is Home Manager's notification daemon, a different program.)
self:
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.programs.mako-files;
  inherit (lib)
    mkEnableOption
    mkIf
    mkOption
    types
    ;
in
{
  options.programs.mako-files = {
    enable = mkEnableOption "Mako, a fast, hotkey-first fork of GNOME Files";

    package = mkOption {
      type = types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "mako.packages.\${system}.default";
      description = "The Mako package.";
    };

    defaultFileManager = mkOption {
      type = types.bool;
      default = true;
      description = "Open folders with Mako (xdg-mime default for inode/directory).";
    };

    fileChooser = mkOption {
      type = types.bool;
      default = true;
      description = ''
        Use Mako as the open/save dialog for apps that go through the desktop
        portal. Needs xdg-desktop-portal, which GNOME and most desktops run.
      '';
    };

    settings = mkOption {
      type = types.attrsOf types.anything;
      default = { };
      example = lib.literalExpression ''
        {
          folder-color-scheme = "vibrant";
          bottom-bar = true;
          inspector-side = "right";
          sidebar-hidden-places = [ "music" "network" ];
        }
      '';
      description = ''
        Mako's settings (dconf path org/gnome/nautilus/flier), written on
        every switch. See data/org.gnome.nautilus.flier.gschema.xml for every key.
      '';
    };

    bookmarks = mkOption {
      type = types.listOf types.str;
      default = [ ];
      example = [ "file:///home/me/Projects Projects" ];
      description = ''
        Sidebar bookmarks ("URI [label]" per line). Setting this makes the
        bookmarks file read-only, so bookmarks can no longer be added in Mako.
      '';
    };

    recursiveSearch = mkOption {
      type = types.nullOr (
        types.enum [
          "always"
          "local-only"
          "never"
        ]
      );
      default = null;
      description = ''
        Override the recursive search policy. null leaves the default
        (local-only) untouched. "always" searches all subfolders;
        "local-only" limits recursion to local filesystems; "never"
        searches only the current folder. Only written when set.
      '';
    };
  };

  config = mkIf cfg.enable {
    home.packages = [ cfg.package ];

    # D-Bus activation and the FileManager1 service for a user install.
    xdg.dataFile."dbus-1/services/org.gnome.Nautilus.Devel.service".source =
      "${cfg.package}/share/dbus-1/services/org.gnome.Nautilus.Devel.service";
    xdg.dataFile."dbus-1/services/org.freedesktop.FileManager1.service" = mkIf cfg.defaultFileManager {
      source = "${cfg.package}/share/dbus-1/services/org.freedesktop.FileManager1.service";
    };

    xdg.mimeApps = mkIf cfg.defaultFileManager {
      enable = true;
      defaultApplications."inode/directory" = cfg.package.desktopId;
    };

    xdg.portal = mkIf cfg.fileChooser {
      enable = true;
      extraPortals = [ cfg.package ];
      config.common."org.freedesktop.impl.portal.FileChooser" = lib.mkDefault [
        "mako"
        "gnome"
        "gtk"
      ];
      config.gnome."org.freedesktop.impl.portal.FileChooser" = lib.mkDefault [
        "mako"
        "gnome"
      ];
    };

    dconf.settings = lib.mkMerge [
      (lib.mkIf (cfg.settings != { }) {
        "org/gnome/nautilus/flier" = cfg.settings;
      })
      (lib.mkIf (cfg.recursiveSearch != null) {
        "org/gnome/nautilus/preferences" = {
          recursive-search = cfg.recursiveSearch;
        };
      })
    ];

    gtk.gtk3.bookmarks = mkIf (cfg.bookmarks != [ ]) cfg.bookmarks;
  };
}
