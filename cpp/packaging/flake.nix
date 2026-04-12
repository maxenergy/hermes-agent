{
  description = "Hermes Agent C++ backend";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.05";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "hermes-cpp";
          version = "0.0.1";
          src = ../.;
          nativeBuildInputs = with pkgs; [ cmake ];
          buildInputs = with pkgs; [ sqlite nlohmann_json yaml-cpp openssl ];
          cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];
        };
      });
    };
}
