# NixOS module: programs.mako-files
# (The name "mako" is already taken in nixpkgs by the notification daemon.)
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
      description = ''
        Open folders with Mako, and let it answer "show in folder" requests
        from other apps (org.freedesktop.FileManager1).
      '';
    };

    fileChooser = mkOption {
      type = types.bool;
      default = true;
      description = ''
        Use Mako as the open/save dialog for apps that go through the desktop
        portal (Flatpaks, Firefox, Chromium and others).
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
        Defaults for Mako's own settings (dconf path org/gnome/nautilus/flier).
        Users can still change them in Preferences. See
        data/org.gnome.nautilus.flier.gschema.xml for every key.
      '';
    };

    recursiveSearch = mkOption {
      type = types.enum [
        "always"
        "local-only"
        "never"
      ];
      default = "always";
      description = ''
        Whether to search subfolders recursively. "always" searches all
        subfolders (like ripgrep); "local-only" limits recursion to local
        filesystems; "never" searches only the current folder.
      '';
    };
  };

  config = mkIf cfg.enable {
    # High priority so Mako's FileManager1 service and `nautilus` command win
    # over GNOME Files when both are installed.
    environment.systemPackages = [
      (if cfg.defaultFileManager then lib.hiPrio cfg.package else cfg.package)
    ];
    services.dbus.packages = [ cfg.package ];

    # Trash, network places and mounts.
    services.gvfs.enable = lib.mkDefault true;
    programs.dconf.enable = true;

    xdg.mime.defaultApplications = mkIf cfg.defaultFileManager {
      "inode/directory" = cfg.package.desktopId;
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

    programs.dconf.profiles.user.databases =
      lib.optionals (cfg.settings != { }) [
        { settings."org/gnome/nautilus/flier" = cfg.settings; }
      ]
      ++ [
        {
          settings."org/gnome/nautilus/preferences" = {
            recursive-search = cfg.recursiveSearch;
          };
        }
      ];
  };
}
