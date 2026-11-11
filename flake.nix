{
  description = "Mako: a fast, hotkey-first fork of GNOME Files (Nautilus)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      lib = nixpkgs.lib;
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # The source without the README media and promo tooling.
      src = lib.fileset.toSource {
        root = ./.;
        fileset = lib.fileset.difference ./. (
          lib.fileset.unions [
            (lib.fileset.maybeMissing ./docs/media)
            (lib.fileset.maybeMissing ./tools)
            (lib.fileset.maybeMissing ./_build)
            ./flake.nix
            (lib.fileset.maybeMissing ./flake.lock)
            ./nix
          ]
        );
      };
      version = "50.2.2-mako-${self.shortRev or self.dirtyShortRev or "dev"}";
      build = pkgs: pkgs.callPackage ./nix/package.nix { inherit src version; };
    in
    {
      packages = forAllSystems (pkgs: rec {
        mako-files = build pkgs;
        default = mako-files;
      });

      overlays.default = final: prev: { mako-files = build final; };

      nixosModules.default = import ./nix/nixos-module.nix self;
      homeModules.default = import ./nix/hm-module.nix self;

      apps = forAllSystems (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.default}/bin/nautilus";
          meta = {
            description = "Mako file manager";
            mainProgram = "nautilus";
          };
        };
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
          packages = [ pkgs.blueprint-compiler ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
