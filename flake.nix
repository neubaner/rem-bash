{
  description = "A server that accepts and executes bash commands";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    {
      overlays = {
        default = self.overlays.rem-bash;
        rem-bash =
          (final: prev: { rem-bash = self.packages.${prev.system}.default; });
      };
    } // flake-utils.lib.eachDefaultSystem (system:
      let pkgs = nixpkgs.legacyPackages.${system};
      in {
        packages.default = pkgs.clangStdenv.mkDerivation {
          pname = "rem-bash";
          version = "0.0.1";
          src = ./server.c;
          dontUnpack = true;
          buildPhase = ''
            clang $src -O3 -o rem-bash -DBASH_PATH='"${pkgs.bash}/bin/bash"'
          '';
          installPhase = ''
            mkdir -p $out/bin
            cp rem-bash $out/bin/rem-bash
          '';
        };
      });
}
