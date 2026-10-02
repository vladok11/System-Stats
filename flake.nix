{
  description = "System-Stats monitoring utility";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }: let
    system = "x86_64-linux";
    pkgs = nixpkgs.legacyPackages.${system};
  in {
    packages.${system}.default = pkgs.stdenv.mkDerivation {
      pname = "system_stats";
      version = "1.0.0";

      src = ./.;

      buildPhase = ''
        gcc -Wall -Wextra -O2 system_stats.c -o system_stats
      '';

      installPhase = ''
        mkdir -p $out/bin
        cp system_stats $out/bin/
      '';
    };
  };
}