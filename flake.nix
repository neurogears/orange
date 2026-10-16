{
  description = "Orange – multi-camera capture, streaming and recording GUI for Emergent cameras";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    nixgl = {
      url = "github:nix-community/nixGL";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, nixgl }:
    (flake-utils.lib.eachSystem [ "x86_64-linux" ] (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config = {
            # The CUDA toolkit and the NVIDIA driver packages are unfree.
            allowUnfree = true;
          };
        };

        cuda = pkgs.cudaPackages;

        # nixGL bridges the Nix store / host GPU driver gap on non-NixOS.
        # Its auto-detection parses /proc/driver/nvidia/version with a regex
        # that only matches the proprietary kernel module format
        # ("... Kernel Module  595.71.05  ..."); the open kernel module,
        # default for driver >= 560 (e.g. Ubuntu 24.04), reports
        # "... Open Kernel Module for x86_64  595.71.05  ..." instead, so
        # detection fails and nixGLDefault silently falls back to the
        # Mesa-only wrapper, which provides no libcuda.  Detect the version
        # here with a regex that accepts both formats and pass it to nixGL
        # explicitly.  Reading /proc keeps the with-nixgl output impure.
        hostNvidiaVersionFile = pkgs.runCommand "impure-host-nvidia-version" {
          # Force a re-read on every evaluation so the wrapper tracks host
          # driver upgrades instead of reusing a stale cached version.
          time = builtins.currentTime;
          preferLocalBuild = true;
          allowSubstitutes = false;
        } "cp /proc/driver/nvidia/version $out 2>/dev/null || touch $out";

        hostNvidiaVersion =
          let
            data = builtins.readFile hostNvidiaVersionFile;
            m = builtins.match ".*Kernel Module( for [A-Za-z0-9_]+)? +([0-9][0-9.]*) .*" data;
          in if m == null then null else builtins.elemAt m 1;

        # Import nixGL as a plain Nix expression rather than via its flake
        # outputs so the detected version can be passed in.  Passing our pkgs
        # also means its allowUnfree covers the NVIDIA driver package nixGL
        # builds, so NIXPKGS_ALLOW_UNFREE=1 is not needed at install time.
        nixglHost = import nixgl {
          inherit pkgs;
          nvidiaVersion = hostNvidiaVersion;
        };

        # ---------------------------------------------------------------------------
        # Emergent eSDK – proprietary, installed on the host by the vendor
        # installer, and deeply coupled to host state: Rivermax licensing,
        # Mellanox/OFED user-space libraries, NIC drivers, and (for GPUDirect)
        # the nvidia_peermem kernel module.  It is therefore consumed as an
        # impure host path, not vendored into the store:
        #
        #   * Build time: the SDK tree is copied into the sandbox (via
        #     builtins.path) purely so headers resolve and the linker can see
        #     the Emergent libraries.  This requires building with --impure.
        #   * Run time: the binaries reference the real host install at
        #     /opt/EVT/eSDK/lib via DT_RPATH (see postFixup below), so the
        #     SDK runs exactly as the vendor installer set it up.
        #
        # Override the host location with ORANGE_ESDK_PATH if the SDK is
        # installed elsewhere.
        # ---------------------------------------------------------------------------
        esdkHostPath =
          let env = builtins.getEnv "ORANGE_ESDK_PATH";
          in if env != "" then env else "/opt/EVT/eSDK";
        esdk = builtins.path { path = /. + esdkHostPath; name = "evt-esdk"; };

        # Host library directories appended to the runtime search path so the
        # eSDK's own dependencies (librivermax, libibverbs, libmlx5, ...)
        # resolve from the host OFED/Rivermax installation.  They come last,
        # so they can never shadow a library the Nix store provides.
        esdkRuntimeLibDirs = [
          "${esdkHostPath}/lib"
          "/usr/lib/x86_64-linux-gnu"
          "/lib/x86_64-linux-gnu"
        ];

        # Directories the host loader searches via /etc/ld.so.conf.d — the
        # vendor installers register the DOCA/Rivermax/DPDK and CUDA-shim
        # locations there (e.g. /opt/mellanox/doca/lib/x86_64-linux-gnu,
        # needed by the GPUDirect path), and the Nix loader does not read
        # ld.so.cache.  Parsed at evaluation time (impure, like the
        # driver-version probe) and baked into the binaries' DT_RPATH after
        # the Nix store paths; a change to the host loader configuration
        # therefore needs a rebuild to be picked up.
        hostLdConfDirs =
          let
            confDir = "/etc/ld.so.conf.d";
            dirLines = text:
              builtins.filter (l: l != null)
                (map
                  (l:
                    let m = builtins.match "[[:space:]]*(/[^#[:space:]]*).*" l;
                    in if m == null then null else builtins.head m)
                  (pkgs.lib.splitString "\n" text));
          in
          if builtins.pathExists confDir then
            pkgs.lib.unique (builtins.concatMap
              (f: dirLines (builtins.readFile (confDir + "/" + f)))
              (builtins.attrNames
                (pkgs.lib.filterAttrs (_name: t: t == "regular" || t == "symlink")
                  (builtins.readDir confDir))))
          else [];

        # ---------------------------------------------------------------------------
        # FFmpeg – used purely as an MP4 muxer; NVENC is driven directly
        # through the vendored src/NvEncoder classes, not through ffmpeg.
        # The stock nixpkgs build therefore suffices.  The split dev/lib outputs are joined into one prefix so the CMake
        # DIR_FFMPEG cache variable points at include/ and lib/ together.
        # ---------------------------------------------------------------------------
        ffmpeg = pkgs.ffmpeg;
        ffmpegPrefix = pkgs.symlinkJoin {
          name = "ffmpeg-joined";
          paths = [ ffmpeg.dev ffmpeg.lib ];
        };

        # ---------------------------------------------------------------------------
        # Git submodules – src = self (flake archive) omits submodule content
        # because Nix uses git-archive under the hood.  The submodule repos
        # are all public, so each is fetched independently and copied into
        # the source tree via postUnpack.  Revs must match the .gitmodules
        # pins; refresh hashes with nix/update-submodule-hashes.sh.
        # ---------------------------------------------------------------------------
        imgui = pkgs.fetchFromGitHub {
          owner = "ocornut"; repo = "imgui";
          rev   = "3fec562da1d5af5f5c35e236e3e843ef0eadbcf3";
          hash  = "sha256-jKCfCUr+v0bUZnc1t3GUL/vb4ywDz4nor+WWF2brofI=";
        };
        implot = pkgs.fetchFromGitHub {
          owner = "epezent"; repo = "implot";
          rev   = "f156599faefe316f7dd20fe6c783bf87c8bb6fd9";
          hash  = "sha256-+aYeQww8kql54tybGKi1LKbEQHm7RG1tetIBeiiXqE0=";
        };
        imguiFileDialog = pkgs.fetchFromGitHub {
          owner = "aiekick"; repo = "ImGuiFileDialog";
          rev   = "517a4a8b9c4a4f662f811e1f9f0e976fac546def";
          hash  = "sha256-TojBmpY7YJgd0Q6n7Dv6u6C4s6G8FMi83zAVeB5ykuQ=";
        };
        iconFontHeaders = pkgs.fetchFromGitHub {
          owner = "juliettef"; repo = "IconFontCppHeaders";
          rev   = "7d6ff1f4ba51e7a2b142be39457768abece1549c";
          hash  = "sha256-Yo9aWkNzbYc8EIcskhmWBax23Fz0eb+xNDE3J26lP70=";
        };

        orangePkg = pkgs.stdenv.mkDerivation {
          pname   = "orange";
          version = "unstable-${self.lastModifiedDate or "dirty"}";

          src = self;

          # Populate the git submodule directories that src = self leaves empty.
          postUnpack = ''
            cp -r --no-preserve=mode ${imgui}/.           "$sourceRoot/third_party/imgui/"
            cp -r --no-preserve=mode ${implot}/.          "$sourceRoot/third_party/implot/"
            cp -r --no-preserve=mode ${imguiFileDialog}/. "$sourceRoot/third_party/ImGuiFileDialog/"
            cp -r --no-preserve=mode ${iconFontHeaders}/. "$sourceRoot/third_party/IconFontCppHeaders/"
          '';

          # libnvidia-encode is a GPU driver library, not part of the CUDA
          # toolkit, and absent from the Nix sandbox at build time.  Create a
          # stub shared lib exporting the two NVENC entry points the vendored
          # NvEncoder classes call so the linker can resolve them.  The real
          # driver-provided library is found at runtime through the DT_RPATH
          # set in postFixup; the stub is never installed.
          #
          # The eSDK libraries have transitive dependencies of their own: the
          # GenICam runtime (libGenApi/libGCBase), Emergent's ffmpeg, libtiff.
          # -L does not apply to resolving a shared library's DT_NEEDED
          # entries, so pass -rpath-link for every library directory inside
          # the SDK tree, and --allow-shlib-undefined for anything the vendor
          # resolves only at runtime from the host.
          preBuild = ''
            cat > nvidia_encode_stub.c << 'EOF'
            void NvEncodeAPICreateInstance()          {}
            void NvEncodeAPIGetMaxSupportedVersion()  {}
            EOF
            cc -shared -fPIC -Wl,-soname,libnvidia-encode.so.1 -o libnvidia-encode.so nvidia_encode_stub.c
            NIX_LDFLAGS="-L$(pwd) $NIX_LDFLAGS"
            # The SDK ships parallel RHEL builds of some libraries under the
            # same sonames (e.g. gpu-direct/rhel); exclude them so the Ubuntu
            # variants are the only candidates.
            for d in $(find ${esdk} \( -type f -o -type l \) -name '*.so*' | xargs -rn1 dirname | sort -u | grep -v '/rhel'); do
              NIX_LDFLAGS="-rpath-link $d $NIX_LDFLAGS"
            done
            NIX_LDFLAGS="--allow-shlib-undefined $NIX_LDFLAGS"
            export NIX_LDFLAGS
          '';

          nativeBuildInputs = with pkgs; [
            cmake
            pkg-config
            cuda.cuda_nvcc
            patchelf
          ];

          buildInputs = with pkgs; [
            # CUDA toolkit (cudart, cuda_driver, NPP, NVTX)
            cuda.cudatoolkit
            cuda.cuda_cudart
            cuda.cuda_nvtx
            cuda.libnpp

            # FFmpeg (MP4 muxer only)
            ffmpeg

            # OpenGL / windowing
            glfw
            glew
            libGL
            mesa  # provides libEGL / OpenGL for the build sandbox

            # ZeroMQ: sync-pulse server (src/sync_pulse.h)
            zeromq
          ];

          cmakeFlags = [
            # Point the build at Nix store paths instead of ~/nvidia/… and
            # the host eSDK (build-time sandbox copy; runtime uses the host
            # install through the DT_RPATH set in postFixup).
            "-DDIR_FFMPEG=${ffmpegPrefix}"
            "-DDIR_ESDK=${esdk}"

            # Broad CUDA arch coverage: Turing through Hopper.
            "-DCMAKE_CUDA_ARCHITECTURES=75;80;86;89;90"
          ];

          installPhase = ''
            runHook preInstall

            # App dir: GUI binary, fonts (found next to the executable),
            # example camera configs, the sudo askpass helper and the
            # revision this package was built from.
            mkdir -p $out/opt/orange $out/bin
            cp orange $out/opt/orange/orange
            cp -r ${self}/fonts $out/opt/orange/fonts
            mkdir -p $out/opt/orange/examples
            cp ${self}/config/*.json* $out/opt/orange/examples/
            install -m 755 ${self}/packaging/orange-askpass.sh $out/opt/orange/orange-askpass
            echo "${self.shortRev or self.dirtyShortRev or "unknown"}" > $out/opt/orange/VERSION

            # bin/orange is the launcher (packaging/orange-launcher.sh): it
            # logs each run to ~/orange_data/logs, self-elevates with sudo
            # (camera capture needs raw NIC access) passing an explicit
            # environment whitelist — the GUI/session vars, the nixGL driver
            # vars when launched through the with-nixgl output, and the eSDK
            # debug switch — and runs orange from the app dir.
            substitute ${self}/packaging/orange-launcher.sh $out/bin/orange \
              --replace-fail "@zenity@" "${pkgs.zenity}/bin/zenity"
            chmod +x $out/bin/orange

            # Desktop entry and icon (found through ~/.nix-profile/share).
            install -Dm644 ${self}/icon.png $out/share/icons/hicolor/512x512/apps/orange.png
            mkdir -p $out/share/applications
            substitute ${self}/packaging/orange.desktop $out/share/applications/orange.desktop \
              --replace-fail "@bindir@" "$out/bin"

            runHook postInstall
          '';

          # Rewrite the runtime search path as a forced DT_RPATH (not
          # RUNPATH): Nix store paths first, then the NixOS driver dir, then
          # the host eSDK and system library dirs.  DT_RPATH is inherited
          # when the Emergent libraries (which carry no RUNPATH) resolve
          # their own dependencies, so librivermax/libibverbs/libmlx5 and the
          # driver's libcuda/libnvidia-encode fall through to the host dirs,
          # while Nix-provided libraries always win the search order.
          # Registered as the last postFixup hook: the CUDA
          # removeStubsFromRunpath hook also rewrites the search path with
          # plain patchelf, which converts a forced DT_RPATH back into a
          # DT_RUNPATH — and RUNPATH is not inherited when the Emergent
          # libraries resolve their own dependencies.  Running last keeps the
          # forced DT_RPATH in the final binaries.
          preFixup = ''
            orangeForceHostRpath() {
              # Mirror every library directory inside the SDK tree back to
              # its host location so the GenICam runtime and Emergent's
              # bundled libraries resolve wherever the vendor installer put
              # them.
              # /rhel is excluded: the SDK ships parallel RHEL builds of some
              # libraries under the same sonames (e.g. gpu-direct/rhel), and
              # whichever directory appears first in the RPATH would claim
              # the soname for the whole process.
              esdkHostDirs=$(find ${esdk} \( -type f -o -type l \) -name '*.so*' | xargs -rn1 dirname | sort -u | grep -v '/rhel' | sed 's|^${esdk}|${esdkHostPath}|' | tr '\n' ':')
              # libtiff comes first: the Emergent libraries need libtiff.so.6,
              # and whichever copy loads first claims the soname for the whole
              # process.  The Nix libtiff carries the cumulative symbol-version
              # set, so it also satisfies any Nix-built library that loads it;
              # the host's older copy satisfies only the Emergent side.
              hostDirs="${pkgs.lib.getLib pkgs.libtiff}/lib:/run/opengl-driver/lib:''${esdkHostDirs}${pkgs.lib.concatStringsSep ":" (pkgs.lib.unique (esdkRuntimeLibDirs ++ hostLdConfDirs))}"
              exe=$out/opt/orange/orange
              patchelf --force-rpath --set-rpath "$(patchelf --print-rpath $exe):$hostDirs" $exe
            }
            postFixupHooks+=(orangeForceHostRpath)
          '';

          meta = with pkgs.lib; {
            description = "Multi-camera capture, streaming and recording GUI for Emergent cameras";
            homepage    = "https://github.com/moments-behavior/orange";
            license     = licenses.mit;
            platforms   = [ "x86_64-linux" ];
            maintainers = [];
          };
        };

      in {
        # -------------------------------------------------------------------------
        # Packages.  All package builds read the host eSDK, so they must be
        # run with --impure (see NIX.md).
        # -------------------------------------------------------------------------
        packages.default = orangePkg;

        # Non-NixOS users need nixGL to bridge the Nix store / host driver
        # gap for OpenGL vendor discovery.  The host NVIDIA driver version is
        # read from /proc at evaluation time:
        #   nix profile install --impure .#orange-with-nixgl
        # The baked-in driver version must match the host; rerun
        # `nix profile upgrade --impure` after a host driver update.
        packages.orange-with-nixgl =
          if hostNvidiaVersion == null then
            throw "Cannot detect NVIDIA driver version from /proc/driver/nvidia/version; nixGL wrapper cannot be built.  If you are on NixOS, use .#default instead of .#orange-with-nixgl.  If you are on non-NixOS, ensure the NVIDIA driver is installed and the kernel module is loaded."
          else
            pkgs.runCommand "orange-with-nixgl" {} ''
              mkdir -p $out/bin $out/share/applications
              cat > $out/bin/orange <<EOF
#!${pkgs.runtimeShell}
exec ${nixglHost.nixGLCommon nixglHost.nixGLNvidia}/bin/nixGL ${orangePkg}/bin/orange "\$@"
EOF
              chmod +x $out/bin/orange
              # The same desktop entry, launching through nixGL.
              ln -s ${orangePkg}/share/icons $out/share/icons
              substitute ${self}/packaging/orange.desktop $out/share/applications/orange.desktop \
                --replace-fail "@bindir@" "$out/bin"
            '';

        # -------------------------------------------------------------------------
        # Development shell – drops you into an environment where you can run
        # cmake -S . -B build && cmake --build build -j  without extra setup.
        # Reads the host eSDK, so enter it with: nix develop --impure
        # -------------------------------------------------------------------------
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            pkg-config
            patchelf
            cuda.cuda_nvcc
            cuda.cudatoolkit
            cuda.cuda_cudart
            cuda.cuda_nvtx
            cuda.libnpp
            ffmpeg
            glfw
            glew
            libGL
            mesa
            zeromq
          ];

          shellHook = ''
            export DIR_FFMPEG="${ffmpegPrefix}"
            export DIR_ESDK="${esdk}"
            echo "Orange dev shell ready."
            echo "  Build: cmake -S . -B build -DDIR_FFMPEG=$DIR_FFMPEG -DDIR_ESDK=$DIR_ESDK && cmake --build build -j"
          '';
        };
      }
    )) // {
      # -------------------------------------------------------------------------
      # NixOS module – system-agnostic, lives outside eachSystem.
      # Add to your configuration.nix imports:
      #   imports = [ orange.nixosModules.default ];
      #   programs.orange.enable = true;
      # -------------------------------------------------------------------------
      nixosModules.default = { config, lib, pkgs, ... }:
        let cfg = config.programs.orange; in {
          options.programs.orange.enable =
            lib.mkEnableOption "Orange multi-camera capture GUI";

          config = lib.mkIf cfg.enable {
            hardware.nvidia.enable    = lib.mkDefault true;
            hardware.opengl.enable    = lib.mkDefault true;
            environment.systemPackages =
              [ self.packages.x86_64-linux.default ];
          };
        };
    };
}
