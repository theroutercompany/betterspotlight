{
  description = "BetterSpotlight reproducible developer and CI environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachSystem [ "aarch64-darwin" "x86_64-darwin" ] (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };

        devShell = import ./nix/devshell.nix { inherit pkgs; };
      in
      {
        devShells.default = devShell;
        checks = import ./nix/checks.nix { inherit pkgs; };

        apps = {
          configure = flake-utils.lib.mkApp {
            drv = pkgs.writeShellApplication {
              name = "bs-configure";
              runtimeInputs = [ pkgs.bash ];
              text = ''
                exec ${./scripts/ci/configure.sh} "$@"
              '';
            };
          };

          build = flake-utils.lib.mkApp {
            drv = pkgs.writeShellApplication {
              name = "bs-build";
              runtimeInputs = [ pkgs.bash ];
              text = ''
                exec ${./scripts/ci/build.sh} "$@"
              '';
            };
          };

          test = flake-utils.lib.mkApp {
            drv = pkgs.writeShellApplication {
              name = "bs-test";
              runtimeInputs = [ pkgs.bash ];
              text = ''
                exec ${./scripts/ci/test.sh} "$@"
              '';
            };
          };

          coverage = flake-utils.lib.mkApp {
            drv = pkgs.writeShellApplication {
              name = "bs-coverage";
              runtimeInputs = [ pkgs.bash ];
              text = ''
                exec ${./scripts/ci/coverage.sh} "$@"
              '';
            };
          };
        };
      }
    );
}
