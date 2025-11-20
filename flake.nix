{
  description = "A server that accepts and executes bash commands";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    {
      overlays = {
        default = self.overlays.remote-bash-server;
        remote-bash-server = (final: prev: {
          remote-bash-server = self.packages.${prev.system}.default;
        });
      };
    } // flake-utils.lib.eachDefaultSystem (system:
      let pkgs = nixpkgs.legacyPackages.${system};
      in {
        packages.default = pkgs.clangStdenv.mkDerivation {
          pname = "remote-bash-server";
          version = "0.0.0";
          src = ./server.c;
          dontUnpack = true;
          buildPhase = ''
            clang $src -O3 -o remote-bash-server -DBASH_PATH='"${pkgs.bash}/bin/bash"'
          '';
          installPhase = ''
            mkdir -p $out/bin
            cp remote-bash-server $out/bin/remote-bash-server
          '';
        };
      });
}
