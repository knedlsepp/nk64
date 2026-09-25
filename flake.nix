{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    self.submodules = true;
  };
  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        crossSystem = {
          config = "mips-linux-gnu"; # prefix expected by scripts in tools/
          system = "mips64-elf";
          gcc.arch = "vr4300";
          gcc.tune = "vr4300";
          gcc.abi = "32";
        };
        pkgs = import nixpkgs { inherit system; };
        pkgsCross = import nixpkgs { inherit system crossSystem; };
        baseRomUS = pkgs.requireFile {
          name = "mk64.us.z64";
          message = ''
            ==== MISSING BASE ROM =======================================================

            Please rename your ROM to mk64.us.z64 and add it to the Nix store using
                nix-store --add-fixed sha256 mk64.us.z64
            then rerun nix-shell.
          '';
          sha256 = "1nm52yxbcgzq7k9fr6j77q7c7lvfh4r7svl5nbn3409zss6m7f6n";
        };
        baseRomEU = pkgs.requireFile {
          name = "mk64.eu.v11.z64";
          message = ''
            ==== MISSING BASE ROM =======================================================

            Please rename your ROM to mk64.eu.v11.z64 and add it to the Nix store using
                nix-store --add-fixed sha256 mk64.eu.v11.z64
            then rerun nix-shell.
          '';
          sha256 = "0vffw5v41bi1p0zjw2cc0bhlj7ccv6v83cna00i53sf5pdl2m7cd";
        };
        # Torch's CMake uses FetchContent, which can't reach the network in the sandbox
        torchDeps = {
          libgfxd = pkgs.fetchFromGitHub {
            owner = "glankk";
            repo = "libgfxd";
            rev = "96fd3b849f38b3a7c7b7f3ff03c5921d328e6cdf";
            hash = "sha256-dedZuV0BxU6goT+rPvrofYqTz9pTA/f6eQcsvpDWdvQ=";
          };
          yaml-cpp = pkgs.fetchFromGitHub {
            owner = "jbeder";
            repo = "yaml-cpp";
            rev = "2f86d13775d119edbb69af52e5f566fd65c6953b";
            hash = "sha256-GtUTbEaRR3+GfVkt3t8EsqBHVffVKOl8urtQTaHozIo=";
          };
          spdlog = pkgs.fetchFromGitHub {
            owner = "gabime";
            repo = "spdlog";
            rev = "7e635fca68d014934b4af8a1cf874f63989352b7";
            hash = "sha256-cxTaOuLXHRU8xMz9gluYz0a93O0ez2xOxbloyc1m1ns=";
          };
          tinyxml2 = pkgs.fetchFromGitHub {
            owner = "leethomason";
            repo = "tinyxml2";
            rev = "10.0.0";
            hash = "sha256-9xrpPFMxkAecg3hMHzzThuy0iDt970Iqhxs57Od+g2g=";
          };
        };
      in {
        devShells.default = pkgsCross.mkShell {
          name = "devshell";
          packages = with pkgs; [
            ninja # needed for ninja -t compdb in run, as n2 doesn't support it
            n2 # same as ninja, but with prettier output
            zlib
            libyaml
            python3
            python3Packages.virtualenv
            ccache
            git
            iconv
            pkgsCross.gcc # for n64crc
            pkgs.gcc
            cmake
            ares
          ];
          shellHook = ''
            cp ${baseRomUS} ./baserom.us.z64
            cp ${baseRomEU} ./baserom.eu.v11.z64
          '';
        };

        packages.default = pkgs.stdenvNoCC.mkDerivation {
          name = "mk64-rom";
          src = self;
          nativeBuildInputs = with pkgs; [
            ninja
            n2
            zlib
            libyaml
            python3
            python3Packages.virtualenv
            ccache
            git
            iconv
            pkgsCross.gcc
            gcc
            cmake
            gnumake
            which
            file
            patchelf
          ];
          dontUseCmakeConfigure = true;
          buildPhase = ''
            # Copy base ROMs
            cp ${baseRomUS} ./baserom.us.z64
            cp ${baseRomEU} ./baserom.eu.v11.z64
            
            # The prebuilt IDO binaries expect /lib64/ld-linux-x86-64.so.2, which the sandbox lacks
            for f in tools/ido-recomp/linux/*; do
              if file "$f" | grep -q 'x86-64.*interpreter'; then
                patchelf --set-interpreter "$(cat ${pkgs.stdenv.cc}/nix-support/dynamic-linker)" \
                  --set-rpath ${pkgs.lib.makeLibraryPath [ pkgs.glibc ]} "$f"
              fi
            done

            # Pre-seed Torch's CMake cache with the pre-fetched dependencies
            cmake -S tools/torch -B tools/torch/cmake-build-release -G Ninja \
              -DCMAKE_BUILD_TYPE=Release \
              -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
              -DFETCHCONTENT_SOURCE_DIR_LIBGFXD=${torchDeps.libgfxd} \
              -DFETCHCONTENT_SOURCE_DIR_YAML-CPP=${torchDeps.yaml-cpp} \
              -DFETCHCONTENT_SOURCE_DIR_SPDLOG=${torchDeps.spdlog} \
              -DFETCHCONTENT_SOURCE_DIR_TINYXML2=${torchDeps.tinyxml2}

            # Build tools
            make -C tools -j$(nproc)
            
            # Extract assets (Torch's config.yml only knows the US ROM)
            make assets VERSION=us -j$(nproc)
            
            # Build ROM
            make VERSION=us -j$(nproc)
            make VERSION=eu.v11 -j$(nproc)
          '';
          installPhase = ''
            mkdir -p $out
            cp build/eu.v11/mk64.eu.v11.z64 $out/mk64.eu.v11.z64
          '';
        };
      }
    );
}
