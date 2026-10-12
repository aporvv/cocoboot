{
  description = "Cocoboot - PalmOS 5 boot loader for ARM Linux, with external DTB support";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      # prc-tools is a 32-bit-era i686-targeted autotools tree; only
      # x86_64-linux is tested (that is also what upstream CI builds).
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};

      # ------------------------------------------------------------------
      # Palm OS SDK 5r3 (headers + m68k glue libraries).  Archived by
      # jichu4n after the original Palm downloads disappeared.
      # ------------------------------------------------------------------
      palm-os-sdk = pkgs.stdenv.mkDerivation {
        pname = "palm-os-sdk";
        version = "5r3";
        src = pkgs.fetchFromGitHub {
          owner = "jichu4n";
          repo = "palm-os-sdk";
          rev = "1fa22066ca0f8b74949c14dd1d626294145d1c09";
          hash = "sha256-sj5MmNSO+Pr3ipIbOhmUC4uYWdoCvcKj4tzguoJ0lCw=";
        };
        dontBuild = true;
        installPhase = ''
          mkdir -p $out
          cp -r sdk-5r3 $out/sdk-5r3
        '';
      };

      # ------------------------------------------------------------------
      # prc-tools 2.3 (m68k-palmos-gcc / arm-palmos-gcc / build-prc /
      # obj-res / ...), patched to build on modern Linux.  The build
      # replicates upstream's tools/build.sh, with two changes:
      #   - everything lands inside $out (no /usr, no /opt/palmdev)
      #   - the SDK is registered with palmdev-prep so the compilers
      #     pick up PalmOS.h and -lPalmOSGlue automatically
      # NOTE: keep `make -j1`; this old tree has parallel-build races
      # (m68k-palmos/libm reliably breaks with -j2).
      # ------------------------------------------------------------------
      prc-tools-remix = pkgs.stdenv.mkDerivation {
        pname = "prc-tools-remix";
        version = "2.3-unstable-2025-12-01";
        src = pkgs.fetchFromGitHub {
          owner = "jichu4n";
          repo = "prc-tools-remix";
          rev = "db9a91d746bac4c5921e3786c39b5dc3eca7b83c";
          hash = "sha256-Z+KCOAOUsDxj4AzVgO2jhc56kp+t8cm3G87X326fsXQ=";
        };

        nativeBuildInputs = with pkgs; [
          autoconf
          automake
          libtool
          flex
          bison
          texinfo
          gperf
          perl
        ];
        buildInputs = [ pkgs.ncurses ];

        # gcc/binutils 2.x-era sources do not survive modern hardening
        hardeningDisable = [ "all" ];
        dontConfigure = true;
        dontStrip = true;
        # CRITICAL: stdenv would replace the tree's config.sub/config.guess
        # with gnulib versions.  prc-tools ships a *patched* config.sub
        # that canonicalises m68k-palmos -> m68k-palmos-coff (the target
        # name gcc's configure knows); the stock one yields
        # m68k-unknown-palmos and the gcc sub-configure bails with
        # "Configuration m68k-unknown-palmos not supported".
        dontUpdateAutotoolsGnuConfigScripts = true;

        buildPhase = ''
          runHook preBuild

          # palmdev must exist (and contain the SDK) before configure
          mkdir -p $out/palmdev
          cp -r ${palm-os-sdk}/sdk-5r3 $out/palmdev/

          mkdir build && cd build
          CC="$CC -std=gnu89" \
          CFLAGS="-w -O1 -fcommon -fno-strict-aliasing" \
          ../prc-tools-2.3/configure \
            --enable-targets=m68k-palmos,arm-palmos \
            --enable-languages=c,c++ \
            --disable-nls \
            --build=i686-linux-gnu \
            --host=i686-linux-gnu \
            --prefix=$out \
            --infodir=$out/share/info \
            --mandir=$out/share/man \
            --with-palmdev-prefix=$out/palmdev

          CC="$CC -std=gnu89" \
          CFLAGS="-w -O1 -fcommon -fno-strict-aliasing" \
          make -j1

          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          cd "$NIX_BUILD_TOP/$sourceRoot/build"
          make MAKEINFO=true install
          # Teach the compilers where the SDK lives (writes gcc specs
          # files inside $out/lib/gcc-lib/*-palmos-coff/*/specs)
          $out/bin/palmdev-prep -d sdk-5r3
          runHook postInstall
        '';
      };

      # ------------------------------------------------------------------
      # PilRC - the PalmOS resource compiler (compiles cocoboot.rcp).
      # The repo does not ship a generated ./configure, so bootstrap
      # autotools first.
      # ------------------------------------------------------------------
      pilrc = pkgs.stdenv.mkDerivation {
        pname = "pilrc";
        version = "3.2-unstable-2020";
        src = pkgs.fetchFromGitHub {
          owner = "jichu4n";
          repo = "pilrc";
          rev = "981f88186a1aee57c4c19adc5c497016f01a4743";
          hash = "sha256-gdFY83rXLkym9IM1C6PvkCRS+z3rAz7M98oQx9qgiNw=";
        };
        nativeBuildInputs = with pkgs; [ autoconf automake libtool ];
        # modern nixpkgs gcc defaults to C23, where `bool` is a keyword
        # and pilrc's `typedef ... bool;` no longer compiles
        env.NIX_CFLAGS_COMPILE = "-std=gnu11";
        configurePhase = ''
          cd unix
          aclocal
          autoconf
          automake --add-missing --copy
          ./configure --prefix=$out
        '';
        buildPhase = "make";
        installPhase = "make install";
      };

      # ------------------------------------------------------------------
      # Cocoboot itself.  builtins.path (rather than ./. directly) so
      # that new/untracked files are included without `git add`.
      # ------------------------------------------------------------------
      cocoboot = pkgs.stdenv.mkDerivation {
        pname = "cocoboot-dtb";
        version = "0.6.2";
        src = builtins.path {
          name = "cocoboot-src";
          path = ./.;
          filter = name: type:
            !(builtins.elem (baseNameOf name)
              [ ".git" "cocoboot.prc" "result" ]);
        };

        nativeBuildInputs = [ prc-tools-remix pilrc pkgs.python3 ];

        buildPhase = ''
          # /usr/bin/env does not exist inside the build sandbox
          patchShebangs tools/
          # empty images/ -> an "iTbl" resource with no builtin images
          mkdir -p images
          make
        '';

        installPhase = ''
          mkdir -p $out/share/cocoboot $out/bin
          cp cocoboot.prc $out/share/cocoboot/
          cp cocoboot.conf.example $out/share/cocoboot/
          cat > $out/bin/cocoboot-prc <<EOS
          #!${pkgs.runtimeShell}
          echo "cocoboot.prc: $out/share/cocoboot/cocoboot.prc"
          echo "Copy it into Palm/Launcher on your SD card, or install via:"
          echo "  pilot-xfer -p /dev/ttyUSBn -i $out/share/cocoboot/cocoboot.prc"
          EOS
          chmod +x $out/bin/cocoboot-prc
        '';
      };

    in
    {
      packages.${system} = {
        default = cocoboot;
        inherit cocoboot prc-tools-remix pilrc palm-os-sdk;
      };

      # Shell with the full PalmOS cross toolchain on PATH, so you can
      # just run `make` yourself.
      devShells.${system}.default = pkgs.mkShell {
        packages = [ prc-tools-remix pilrc pkgs.python3 ];
      };
    };
}
