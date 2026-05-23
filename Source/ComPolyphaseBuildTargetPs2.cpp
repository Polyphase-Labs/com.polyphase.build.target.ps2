// MSVC SDL deprecation suppression — every CRT call is bounds-checked.
#define _CRT_SECURE_NO_WARNINGS

/**
 * @file ComPolyphaseBuildTargetPs2.cpp
 * @brief PS2 (PS2SDK + gsKit) build-target addon for Polyphase Engine.
 *
 * Adds a "Sony PS2 (PS2SDK + gsKit)" entry to the editor's Build Profile
 * dropdown.
 *
 * Toolchain expectations on the host:
 *
 *   Linux / macOS — native:
 *     - PS2DEV env var pointing at the ps2dev install prefix.
 *       Canonical layout: $PS2DEV/ee/bin/mips64r5900el-ps2-elf-gcc and
 *       $PS2DEV/ps2sdk/samples/Makefile.eeglobal_cpp.
 *     - mkisofs (or genisoimage) on PATH when ps2.makeIso=1.
 *
 *   Windows — native (default, ps2dev DOES ship Windows binaries since v2.0.0):
 *     - PS2DEV env var pointing at the extracted ps2dev-windows-latest tree
 *       (e.g. C:\ps2dev). Cross-compiler binaries live under %PS2DEV%\ee\bin.
 *     - `make` (or `mingw32-make`) on PATH — typically supplied by MSYS2 or
 *       Git Bash. The ps2dev Windows bundle assumes a POSIX-style shell.
 *     - mkisofs (cdrtools / MSYS2 `pacman -S mkisofs`) on PATH when ISO output
 *       is enabled.
 *
 *   Windows — opt-in WSL fallback (Target Options: "Use WSL"):
 *     - `wsl` available on PATH (Windows 10 2004+ / Windows 11).
 *     - A WSL distro with ps2dev installed inside it. Pre-translates
 *       Windows paths to /mnt/<drive>/... so the bash body never sees
 *       backslashes.
 *
 *   Optional everywhere:
 *     - PCSX2 (pcsx2-qt.exe / pcsx2-qt) for emulator launch. PS2_EMULATOR env
 *       var overrides the executable name; ps2.pcsx2Path profile option takes
 *       precedence over both.
 *
 * Project expectations:
 *   - The PS2 build's makefile (`Makefile_PS2`) ships INSIDE this addon, at
 *     `Packages/com.polyphase.build.target.ps2/Makefile_PS2`. Projects don't
 *     have to add anything themselves; the addon's GetCompileCommand passes
 *     the addon-relative path to `make -f`.
 *
 * Licensing isolation: the engine binary never links against PS2SDK. Every
 * mips64r5900el-ps2-elf / ps2sdk / gsKit reference lives only in this addon
 * and the addon-shipped Runtime/PS2/ tree (which is compiled INTO the user's
 * PS2 ELF, not into the engine's editor binary).
 *
 * Maintainer: Polyphase Engine team.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"
#include "imgui.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <cctype>

static PolyphaseEngineAPI* sEngineAPI = nullptr;

#if EDITOR
namespace
{
    // ----- Helpers ----------------------------------------------------------

    std::string GetEnvOrEmpty(const char* name)
    {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    }

    bool FileExists(const std::string& path)
    {
        if (path.empty()) return false;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f) { std::fclose(f); return true; }
        return false;
    }

    // ----- Per-profile option keys -----------------------------------------

    constexpr const char* kTitleKey         = "ps2.title";          // ISO volume id + boot banner
    constexpr const char* kDiscIdKey        = "ps2.discId";         // 8.3 uppercase, e.g. "POLY0001"
    constexpr const char* kRegionKey        = "ps2.region";         // "NTSC" | "PAL"
    constexpr const char* kMakefileKey      = "ps2.makefile";       // bare filename inside addon root, or absolute override
    constexpr const char* kJobsKey          = "ps2.jobs";           // make -j parallelism
    constexpr const char* kUseWslKey        = "ps2.useWsl";         // Windows-only: "0" = native (default), "1" = route through WSL
    constexpr const char* kWslDistroKey     = "ps2.wslDistro";      // Windows-only WSL distro override (honoured when useWsl=1)
    constexpr const char* kPs2DevPathKey    = "ps2.ps2devPath";     // Override $PS2DEV as seen by build shell
    constexpr const char* kMakeIsoKey       = "ps2.makeIso";        // "0" = bare ELF only, "1" = also run mkisofs
    constexpr const char* kPcsx2PathKey     = "ps2.pcsx2Path";      // Override PCSX2 binary

    constexpr const char* kTitleDefault     = "Polyphase Game";
    constexpr const char* kDiscIdDefault    = "POLY0001";          // 8.3-safe; SYSTEM.CNF BOOT2 reference
    constexpr const char* kRegionDefault    = "NTSC";
    constexpr const char* kMakefileDefault  = "Makefile_PS2";
    constexpr const char* kJobsDefault      = "4";
    // Default WSL on Windows. ps2dev's native Windows binaries depend on
    // an MSYS2 runtime that isn't on most users' PATH — they fail to launch
    // with STATUS_INVALID_IMAGE_FORMAT and cmd /C eats the error silently,
    // making "build succeeded but no ELF" the typical native-Windows
    // experience. WSL ps2dev install is reliable. Users with a working
    // native install can untick "Use WSL" in Target Options.
#if defined(_WIN32)
    constexpr const char* kUseWslDefault    = "1";
#else
    constexpr const char* kUseWslDefault    = "0";
#endif
    constexpr const char* kMakeIsoDefault   = "0";
    constexpr const char* kPcsx2Default     = "pcsx2-qt.exe";       // POSIX hosts override via PS2_EMULATOR

    // Decide whether the build shell on this host should route through WSL.
    // POSIX hosts always answer false. Windows hosts default TRUE (native
    // ps2dev's bundled toolchain isn't reliable without MSYS2 runtime on
    // PATH); users with a working native install untick "Use WSL" in
    // Target Options. When the profile setting isn't present yet (fresh
    // profile), we fall back to kUseWslDefault rather than defaulting to
    // false — otherwise the first build always picks native.
    bool UseWsl(const PolyphaseBuildContext* ctx)
    {
#if defined(_WIN32)
        if (ctx == nullptr || ctx->GetProfileSetting == nullptr) return (kUseWslDefault[0] == '1');
        char buf[8] = {0};
        if (ctx->GetProfileSetting(kUseWslKey, buf, sizeof(buf)) == 0 || buf[0] == '\0')
        {
            // No profile entry yet — honour the platform-level default.
            return (kUseWslDefault[0] == '1');
        }
        return buf[0] == '1';
#else
        (void)ctx;
        return false;
#endif
    }

#if defined(_WIN32)
    // Translate a Windows absolute path into its default WSL2 mount-point:
    //   C:\Foo\Bar  ->  /mnt/c/Foo/Bar
    std::string WinToWslPath(const std::string& winPath)
    {
        if (winPath.empty()) return winPath;
        if (winPath[0] == '/') return winPath;
        if (winPath.size() >= 2 && winPath[1] == ':')
        {
            std::string out = "/mnt/";
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(winPath[0])));
            for (size_t i = 2; i < winPath.size(); ++i)
            {
                out += (winPath[i] == '\\') ? '/' : winPath[i];
            }
            return out;
        }
        std::string out = winPath;
        for (char& c : out) if (c == '\\') c = '/';
        return out;
    }
#endif

    // Render a host path for embedding inside the build shell. When WSL is in
    // play (Windows + ps2.useWsl=1), pre-translate to /mnt/<drive>/... so the
    // bash body never sees backslashes. Otherwise quote-as-is.
    std::string ShellPath(const PolyphaseBuildContext* ctx, const std::string& path)
    {
#if defined(_WIN32)
        if (UseWsl(ctx)) return std::string("'") + WinToWslPath(path) + "'";
        // Native Windows path: still need to quote for the shell. Use double-
        // quotes so backslashes pass through unchanged.
        return std::string("\"") + path + "\"";
#else
        (void)ctx;
        return std::string("'") + path + "'";
#endif
    }

    // Emit "wsl [-d <distro>]" prefix when ps2.useWsl=1 on Windows; empty otherwise.
    std::string WslPrefix(const PolyphaseBuildContext* ctx)
    {
#if defined(_WIN32)
        if (!UseWsl(ctx)) return std::string();
        std::string distro;
        if (ctx != nullptr && ctx->GetProfileSetting != nullptr)
        {
            char buf[128] = {0};
            if (ctx->GetProfileSetting(kWslDistroKey, buf, sizeof(buf)) != 0 && buf[0] != '\0')
            {
                distro = buf;
            }
        }
        if (distro.empty()) return std::string("wsl ");
        return std::string("wsl -d ") + distro + " ";
#else
        (void)ctx;
        return std::string();
#endif
    }

    // Wrap a bash command for the host:
    //   Windows + useWsl=1: `wsl [-d distro] bash -lc "<body>"`
    //   POSIX:              `bash -lc "<body>"`
    //   Windows native:     body is emitted as a plain cmd.exe command (no wrap).
    //                       Callers must build the body in a cmd-compatible way.
    std::string WrapShell(const PolyphaseBuildContext* ctx, const std::string& body)
    {
#if defined(_WIN32)
        if (UseWsl(ctx)) return WslPrefix(ctx) + "bash -lc \"" + body + "\"";
        // Native Windows: caller-supplied body is already a cmd.exe command line.
        return body;
#else
        (void)ctx;
        return std::string("bash -lc \"") + body + "\"";
#endif
    }

    // Shell prelude that auto-detects PS2DEV from the same fallback ladder
    // Makefile_PS2 uses, then prepends $PS2DEV/{bin,ee/bin,ps2sdk/bin} to PATH.
    // Used by mkisofs/PostPackage on POSIX + WSL paths where the build shell
    // doesn't source ~/.bashrc.
    std::string Ps2DevAutoDetectPrelude()
    {
        return
            "for d in /usr/local/ps2dev /opt/ps2dev \\\"\\$HOME/ps2dev\\\" "
            "/mnt/c/ps2dev /mnt/d/ps2dev; do "
            "if [ -f \\\"\\$d/ps2sdk/samples/Makefile.eeglobal_cpp\\\" ]; then "
            "export PS2DEV=\\\"\\$d\\\"; break; fi; done && "
            "export PATH=\\\"\\$PS2DEV/bin:\\$PS2DEV/ee/bin:\\$PS2DEV/ps2sdk/bin:\\$PATH\\\"";
    }

    std::string ReadOption(const PolyphaseBuildContext* ctx, const char* key, const char* fallback)
    {
        if (ctx == nullptr || ctx->GetProfileSetting == nullptr) return fallback ? fallback : "";
        char buf[512] = {0};
        if (ctx->GetProfileSetting(key, buf, sizeof(buf)) == 0 || buf[0] == '\0')
        {
            return fallback ? fallback : "";
        }
        return std::string(buf);
    }

    // Uppercase a string and truncate to 8 chars (8.3-safe disc ID).
    std::string MakeDiscIdSafe(const std::string& src)
    {
        std::string out;
        for (char c : src)
        {
            if (out.size() >= 8) break;
            if (std::isalnum(static_cast<unsigned char>(c)))
            {
                out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            else if (c == '_' || c == '-')
            {
                out += c;
            }
        }
        if (out.empty()) out = kDiscIdDefault;
        return out;
    }

    // ----- Build-target callbacks ------------------------------------------

    // Native Windows probe: look for mips64r5900el-ps2-elf-gcc.exe under
    // %PS2DEV%\ee\bin or one of the canonical install dirs.
    bool ProbeNativeWindowsPs2Dev(std::string& outPs2Dev, std::string& outReason)
    {
        const char* envPs2 = std::getenv("PS2DEV");
        const std::string candidates[] = {
            envPs2 ? std::string(envPs2) : std::string(),
            "C:\\ps2dev",
            "D:\\ps2dev",
            GetEnvOrEmpty("USERPROFILE") + "\\ps2dev"
        };
        for (const auto& c : candidates)
        {
            if (c.empty()) continue;
            const std::string gcc = c + "\\ee\\bin\\mips64r5900el-ps2-elf-gcc.exe";
            const std::string sdkSentinel = c + "\\ps2sdk\\samples\\Makefile.eeglobal_cpp";
            if (FileExists(gcc) && FileExists(sdkSentinel))
            {
                outPs2Dev = c;
                return true;
            }
        }
        outReason =
            "PS2 toolchain not found. Install ps2dev for Windows from "
            "https://github.com/ps2dev/ps2dev/releases (download "
            "ps2dev-windows-latest.tar.gz, extract to e.g. C:\\ps2dev, "
            "set PS2DEV env var to point there). For WSL-based builds, "
            "enable 'Use WSL' in the Target Options and install ps2dev "
            "inside your WSL distro.";
        return false;
    }

    int32_t Ps2_Validate(char* outReason, size_t cap)
    {
        // ImGui can call this many times per second when hovering the target;
        // BuildTargetRegistry caches the verdict per target. We still keep the
        // probe cheap: file existence checks on Windows, a single `system()`
        // call when WSL or POSIX. Callers that need finer-grained caching
        // (per-frame in DrawProfileOptions) should already debounce upstream.
#if defined(_WIN32)
        // The Validate call doesn't have a build context, so we can't read
        // ps2.useWsl directly. Probe both paths; succeed if either works.
        std::string ps2dev;
        std::string nativeReason;
        if (ProbeNativeWindowsPs2Dev(ps2dev, nativeReason))
        {
            return 1;
        }

        // Native missing — try WSL as the fallback. Build a probe that
        // accepts either of:
        //   (1) PS2DEV exported in WSL ~/.bashrc pointing at an install with
        //       samples/Makefile.eeglobal_cpp
        //   (2) ps2sdk available under one of the candidate paths even without
        //       PS2DEV set
        char probe[1024];
        std::snprintf(probe, sizeof(probe),
            "wsl bash -lc \"command -v mips64r5900el-ps2-elf-gcc >/dev/null 2>&1 "
            "|| [ -f /usr/local/ps2dev/ps2sdk/samples/Makefile.eeglobal_cpp ] "
            "|| [ -f /opt/ps2dev/ps2sdk/samples/Makefile.eeglobal_cpp ]\"");
        const int rc = std::system(probe);
        if (rc != 0)
        {
            std::snprintf(outReason, cap, "%s", nativeReason.c_str());
            return 0;
        }
        return 1;
#else
        const std::string ps2dev = GetEnvOrEmpty("PS2DEV");
        if (ps2dev.empty())
        {
            std::snprintf(outReason, cap,
                "PS2DEV env var not set. Install ps2dev "
                "(https://github.com/ps2dev/ps2dev) and export PS2DEV in your "
                "shell rc — e.g. `export PS2DEV=/usr/local/ps2dev`.");
            return 0;
        }
        if (!FileExists(ps2dev + "/ee/bin/mips64r5900el-ps2-elf-gcc"))
        {
            std::snprintf(outReason, cap,
                "PS2DEV='%s' but mips64r5900el-ps2-elf-gcc not found under "
                "ee/bin/. Re-run the ps2dev installer or fix PS2DEV.",
                ps2dev.c_str());
            return 0;
        }
        if (!FileExists(ps2dev + "/ps2sdk/samples/Makefile.eeglobal_cpp"))
        {
            std::snprintf(outReason, cap,
                "PS2DEV='%s' is missing ps2sdk/samples/Makefile.eeglobal_cpp. "
                "Install ps2sdk on top of the toolchain.", ps2dev.c_str());
            return 0;
        }
        return 1;
#endif
    }

    int32_t Ps2_GetCompileCommand(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr) return 0;

        const std::string makefileOpt = ReadOption(ctx, kMakefileKey, kMakefileDefault);
        const std::string ps2devOpt   = ReadOption(ctx, kPs2DevPathKey, "");
        const std::string jobsOpt     = ReadOption(ctx, kJobsKey, kJobsDefault);

        int jobs = 0;
        for (char c : jobsOpt) { if (c < '0' || c > '9') { jobs = 0; break; } jobs = jobs * 10 + (c - '0'); }
        if (jobs < 1 || jobs > 64) jobs = 4;

        const bool isAbsolute =
            !makefileOpt.empty() &&
            (makefileOpt[0] == '/' ||
             (makefileOpt.size() >= 2 && makefileOpt[1] == ':'));
        const std::string makefilePath = isAbsolute
            ? makefileOpt
            : (std::string(ctx->projectDir) +
               "/Packages/com.polyphase.build.target.ps2/" + makefileOpt);

        const std::string intermediateDir =
            std::string(ctx->projectDir) + "/Intermediate/PS2";

        char jobsArg[16];
        std::snprintf(jobsArg, sizeof(jobsArg), " -j%d", jobs);

        const bool useWsl = UseWsl(ctx);

#if defined(_WIN32)
        if (!useWsl)
        {
            // Native Windows path. ps2dev bundles its own Unix-style tools
            // (cygwin or mingw-style); user must have `make` (or `mingw32-make`)
            // and the cross-compiler on PATH. We build a cmd.exe-style command
            // line — no bash wrap.
            //
            // ps2dev's native Windows installer extracts to a flat layout with
            // `bin\make.exe` and `ee\bin\<toolchain>.exe`. We pre-prepend those
            // dirs to PATH for this command via `set "PATH=...;%PATH%"` so
            // make's recursive invocations find ee-gcc even if the user only
            // set PS2DEV without touching PATH.
            std::string ps2devResolved = ps2devOpt;
            if (ps2devResolved.empty())
            {
                const char* env = std::getenv("PS2DEV");
                if (env) ps2devResolved = env;
            }
            if (ps2devResolved.empty())
            {
                std::string reason;
                ProbeNativeWindowsPs2Dev(ps2devResolved, reason);
            }

            std::string cmd;
            cmd += "cmd /C \"";
            if (!ps2devResolved.empty())
            {
                cmd += "set \"PS2DEV=" + ps2devResolved + "\" && ";
                cmd += "set \"PATH=" + ps2devResolved + "\\bin;" +
                       ps2devResolved + "\\ee\\bin;" +
                       ps2devResolved + "\\ps2sdk\\bin;%PATH%\" && ";
            }
            cmd += "if not exist \"" + intermediateDir + "\" mkdir \"" + intermediateDir + "\" && ";
            if (ctx->forceRebuild)
            {
                cmd += "del /Q \"" + intermediateDir + "\\*.o\" \"" + intermediateDir +
                       "\\*.d\" \"" + intermediateDir + "\\*.elf\" 2>nul & ";
                cmd += "del /Q \"" + std::string(ctx->projectDir) + "\\Build\\PS2\\*.elf\" 2>nul & ";
            }
            // Use `make` first; if that fails to launch fall back to `mingw32-make`.
            // We can't easily detect-and-pick within a single cmd line, so just
            // use `make` and trust the ps2dev native bundle has it.
            cmd += "make -C \"" + intermediateDir + "\"";
            cmd += " -f \"" + makefilePath + "\"";
            cmd += " PROJECT_ROOT=\"" + std::string(ctx->projectDir) + "\"";
            if (!ps2devResolved.empty())
            {
                cmd += " PS2DEV=\"" + ps2devResolved + "\"";
            }
            if (ctx->engineDir != nullptr && ctx->engineDir[0] != '\0')
            {
                cmd += " POLYPHASE_PATH=\"" + std::string(ctx->engineDir) + "\"";
            }
            cmd += jobsArg;
            cmd += "\"";

            std::snprintf(outCmd, cap, "%s", cmd.c_str());
            return 1;
        }
#endif

        // WSL or POSIX path — single bash body wrapped via WrapShell.
        std::string makePs2Dev;
        if (!ps2devOpt.empty())
        {
            makePs2Dev = " PS2DEV='" + ps2devOpt + "'";
        }

        std::string makePolyphasePath;
        if (ctx->engineDir != nullptr && ctx->engineDir[0] != '\0')
        {
            makePolyphasePath = " POLYPHASE_PATH=" + ShellPath(ctx, ctx->engineDir);
        }

        const std::string mkIntermediate =
            "mkdir -p " + ShellPath(ctx, intermediateDir) + " && ";

        std::string cleanPrefix;
        if (ctx->forceRebuild)
        {
            cleanPrefix =
                "(cd " + ShellPath(ctx, intermediateDir) +
                " && rm -f *.o *.d *.elf 2>/dev/null; true) && " +
                "(rm -f " + ShellPath(ctx, std::string(ctx->projectDir) + "/Build/PS2") +
                "/*.elf 2>/dev/null; true) && ";
        }

        const std::string makeProjectRoot =
            " PROJECT_ROOT=" + ShellPath(ctx, ctx->projectDir);

        const std::string body =
            mkIntermediate +
            cleanPrefix +
            "make -C " + ShellPath(ctx, intermediateDir) +
            " -f " + ShellPath(ctx, makefilePath) +
            makeProjectRoot + makePs2Dev + makePolyphasePath + jobsArg;

        (void)useWsl;
        std::snprintf(outCmd, cap, "%s", WrapShell(ctx, body).c_str());
        return 1;
    }

    int32_t Ps2_GetCompiledBinaryPath(const PolyphaseBuildContext* ctx, char* outPath, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr || ctx->projectName == nullptr) return 0;
        std::snprintf(outPath, cap, "%s/Build/PS2/%s.elf",
                      ctx->projectDir, ctx->projectName);
        return 1;
    }

    // Force PS2-specific keys in a packaged Config.ini:
    //   - WindowWidth/WindowHeight match the GS video mode (NTSC 640x448,
    //     PAL 640x512). Project Config.ini carries the desktop dimensions
    //     (typically 1280x720); without this rewrite the engine's
    //     ReadEngineConfig would override OctPreInitialize's PS2 defaults.
    //   - UseAssetRegistry=1: PS2 is an addon-platform (basePlatform=Linux);
    //     Engine.cpp's console-override only enumerates Android/GameCube/
    //     Wii/N3DS so PS2 doesn't get the auto-enable. Without the registry,
    //     asset discovery walks directories via SYS_OpenDirectory (stubbed in
    //     Phase 0-2) and finds nothing. Setting it in Config.ini means
    //     ReadEngineConfig will load the right value regardless of when
    //     OctPreInitialize fires.
    void ForcePs2WindowSizeInConfig(const std::string& configPath, const std::string& region)
    {
        const int width  = 640;
        const int height = (region == "PAL") ? 512 : 448;

        std::ifstream in(configPath);
        if (!in.is_open()) return;

        std::ostringstream out;
        std::string line;
        bool sawW = false, sawH = false, sawAR = false;
        while (std::getline(in, line))
        {
            if (line.rfind("WindowWidth=", 0) == 0)  { out << "WindowWidth="  << width  << "\n"; sawW = true; }
            else if (line.rfind("WindowHeight=", 0) == 0) { out << "WindowHeight=" << height << "\n"; sawH = true; }
            else if (line.rfind("UseAssetRegistry=", 0) == 0) { out << "UseAssetRegistry=1\n"; sawAR = true; }
            else { out << line << "\n"; }
        }
        in.close();

        if (!sawW)  out << "WindowWidth="  << width  << "\n";
        if (!sawH)  out << "WindowHeight=" << height << "\n";
        if (!sawAR) out << "UseAssetRegistry=1\n";

        std::ofstream o(configPath, std::ios::trunc);
        if (o.is_open()) o << out.str();
    }

    // Copy a file with simple binary IO. Used to produce <DISCID>.ELF
    // alongside the bare <projectName>.elf so SYSTEM.CNF's BOOT2 line
    // matches an uppercase 8.3 filename on the ISO9660 catalogue.
    bool CopyFileSimple(const std::string& src, const std::string& dst)
    {
        std::ifstream in(src, std::ios::binary);
        if (!in.is_open()) return false;
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << in.rdbuf();
        return out.good();
    }

    // Write SYSTEM.CNF in the packaged output directory. Format per
    // psdevwiki: BOOT2 / VER / VMODE keys, CRLF or LF both work, file MUST
    // be uppercase SYSTEM.CNF in the ISO root, and the BOOT2 ELF reference
    // is `cdrom0:\<DISCID>.ELF;1` (backslash, uppercase, `;1` version suffix).
    bool WriteSystemCnf(const std::string& path, const std::string& discId, const std::string& region)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << "BOOT2 = cdrom0:\\" << discId << ".ELF;1\r\n";
        out << "VER = 1.00\r\n";
        out << "VMODE = " << ((region == "PAL") ? "PAL" : "NTSC") << "\r\n";
        return out.good();
    }

    int32_t Ps2_PostPackage(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        const std::string title    = ReadOption(ctx, kTitleKey, kTitleDefault);
        const std::string discIdRaw = ReadOption(ctx, kDiscIdKey, kDiscIdDefault);
        const std::string discId   = MakeDiscIdSafe(discIdRaw);
        const std::string region   = ReadOption(ctx, kRegionKey, kRegionDefault);
        const std::string makeIso  = ReadOption(ctx, kMakeIsoKey, kMakeIsoDefault);

        // Diagnostic: print every option PostPackage saw, especially
        // makeIso. If makeIso != "1" here even though the user checked
        // the box, the build profile didn't persist the change.
        if (ctx->WriteOutputLine)
        {
            char dbg[256];
            std::snprintf(dbg, sizeof(dbg),
                "[PS2 PostPackage] title='%s' discId='%s' region='%s' makeIso='%s'",
                title.c_str(), discId.c_str(), region.c_str(), makeIso.c_str());
            ctx->WriteOutputLine(dbg);
        }

        const std::string outDir   = ctx->packageOutputDir;
        const std::string elfPath  = outDir + "/" + ctx->projectName + ".elf";
        const std::string discElf  = outDir + "/" + discId + ".ELF";
        const std::string sysCnf   = outDir + "/SYSTEM.CNF";

        // (1) Rewrite Config.ini in both packaged copies.
        ForcePs2WindowSizeInConfig(outDir + "/Config.ini", region);
        ForcePs2WindowSizeInConfig(outDir + "/" + std::string(ctx->projectName) + "/Config.ini", region);
        if (ctx->Log != nullptr)
        {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                "Patched WindowWidth/Height for region=%s in packaged Config.ini",
                region.c_str());
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, msg);
        }

        // (2) Write SYSTEM.CNF.
        if (!WriteSystemCnf(sysCnf, discId, region))
        {
            if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_ERROR, "Failed to write SYSTEM.CNF");
            return 0;
        }

        // (3) Copy <projectName>.elf to <DISCID>.ELF (uppercase, 8.3-safe).
        //     The bare ELF stays for PCSX2 -elf direct boot; the uppercase
        //     copy is what SYSTEM.CNF references for BIOS / ISO boot.
        if (FileExists(elfPath))
        {
            if (!CopyFileSimple(elfPath, discElf))
            {
                if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_ERROR, "Failed to copy ELF to DISCID.ELF");
                return 0;
            }
        }
        else
        {
            if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_WARNING,
                "Compiled ELF not found at expected path; SYSTEM.CNF written but BOOT2 target is missing.");
        }

        // (4) Optionally wrap into a bootable ISO.
        if (makeIso == "1")
        {
            // Build the title string safely-escaped for the shell. Inside a
            // bash body we close, escape, and re-open single quotes for any
            // literal '. For cmd.exe (native Windows) we strip embedded
            // double-quotes — mkisofs accepts a simple title and any
            // exotic chars break ISO9660 anyway.
            auto quoteForBash = [](const std::string& s) {
                std::string out = "'";
                for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
                out += '\'';
                return out;
            };

            const std::string isoOut = outDir + "/" + ctx->projectName + ".iso";

#if defined(_WIN32)
            if (!UseWsl(ctx))
            {
                // Native Windows: shell out to mkisofs via cmd. User must have
                // mkisofs (cdrtools / MSYS2) on PATH.
                std::string sanitizedTitle = title;
                for (char& c : sanitizedTitle) if (c == '"') c = ' ';
                std::string cmd = "cmd /C \"cd /D \"" + outDir + "\" && mkisofs -quiet -V \"" +
                    sanitizedTitle + "\" -sysid \"PLAYSTATION\" -l -iso-level 1 -A \"POLYPHASE\" -o \"" +
                    isoOut + "\" .\"";
                if (ctx->WriteOutputLine) ctx->WriteOutputLine(cmd.c_str());
                const int rc = std::system(cmd.c_str());
                if (rc != 0)
                {
                    if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_ERROR,
                        "mkisofs failed. Install cdrtools or use MSYS2 (`pacman -S mkisofs`), "
                        "or enable 'Use WSL' to run mkisofs from inside WSL.");
                    return 0;
                }
                if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_DEBUG,
                    (std::string("ISO written: ") + isoOut).c_str());
                return 1;
            }
#endif

            // POSIX or Windows+WSL — wrap mkisofs in the auto-detect prelude
            // so PS2DEV-bundled mkisofs is found.
            const std::string body = Ps2DevAutoDetectPrelude() + " && cd " + ShellPath(ctx, outDir) +
                " && mkisofs -quiet -V " + quoteForBash(title) +
                " -sysid 'PLAYSTATION' -l -iso-level 1 -A 'POLYPHASE' -o " +
                ShellPath(ctx, isoOut) + " .";
            const std::string cmd = WrapShell(ctx, body);
            if (ctx->WriteOutputLine) ctx->WriteOutputLine(cmd.c_str());
            const int rc = std::system(cmd.c_str());
            if (rc != 0)
            {
                if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_ERROR,
                    "mkisofs failed. Install genisoimage (`apt install genisoimage`) inside your "
                    "build shell, or on macOS `brew install cdrtools`.");
                return 0;
            }
            if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_DEBUG,
                (std::string("ISO written: ") + isoOut).c_str());
        }

        if (ctx->Log)
        {
            char ok[512];
            std::snprintf(ok, sizeof(ok),
                "PS2 package complete: %s (region=%s, discId=%s%s)",
                outDir.c_str(), region.c_str(), discId.c_str(),
                (makeIso == "1") ? ", ISO emitted" : ", bare ELF only");
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, ok);
        }
        return 1;
    }

    int32_t Ps2_RunInEmulator(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr) return 0;

        const std::string override_ = GetEnvOrEmpty("PS2_EMULATOR");
        const std::string pcsx2Opt  = ReadOption(ctx, kPcsx2PathKey, "");
        const std::string exe = !override_.empty() ? override_
                              : !pcsx2Opt.empty()  ? pcsx2Opt
                              : std::string(kPcsx2Default);

        const std::string makeIso = ReadOption(ctx, kMakeIsoKey, kMakeIsoDefault);

        // -batch makes PCSX2 exit when the window closes (clean dev loop).
        // -elf skips BIOS for fastest iteration. -fastboot is meaningful only
        // for ISO boot; combine with the path to the .iso.
        if (makeIso == "1")
        {
            std::snprintf(outCmd, cap,
                "\"%s\" -batch -fastboot \"%s/%s.iso\"",
                exe.c_str(), ctx->packageOutputDir, ctx->projectName);
        }
        else
        {
            std::snprintf(outCmd, cap,
                "\"%s\" -batch -elf \"%s/%s.elf\"",
                exe.c_str(), ctx->packageOutputDir, ctx->projectName);
        }
        return 1;
    }

    void Ps2_DrawProfileOptions(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->SetProfileSetting == nullptr) return;

        // ----- Title -------------------------------------------------------
        {
            std::string current = ReadOption(ctx, kTitleKey, kTitleDefault);
            char buf[128] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Title", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kTitleKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ISO volume id / boot banner string. Free-form, but ISO9660 prefers ASCII.");
        }

        // ----- Disc ID -----------------------------------------------------
        {
            std::string current = ReadOption(ctx, kDiscIdKey, kDiscIdDefault);
            char buf[16] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Disc ID", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kDiscIdKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("8.3-uppercase ID used for SYSTEM.CNF BOOT2 (e.g. POLY0001). "
                                  "Auto-uppercased and truncated to 8 chars when emitting.");
        }

        // ----- Region ------------------------------------------------------
        {
            static const char* kRegions[] = { "NTSC", "PAL" };
            std::string current = ReadOption(ctx, kRegionKey, kRegionDefault);
            int idx = (current == "PAL") ? 1 : 0;
            if (ImGui::Combo("Region", &idx, kRegions, IM_ARRAYSIZE(kRegions)))
            {
                ctx->SetProfileSetting(kRegionKey, kRegions[idx]);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Selects SYSTEM.CNF VMODE and the packaged Config.ini WindowHeight (NTSC: 640x448, PAL: 640x512).");
        }

        // ----- Make ISO ----------------------------------------------------
        {
            std::string current = ReadOption(ctx, kMakeIsoKey, kMakeIsoDefault);
            bool make = (current == "1");
            if (ImGui::Checkbox("Make ISO (mkisofs)", &make))
            {
                ctx->SetProfileSetting(kMakeIsoKey, make ? "1" : "0");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off (default): emit a bare ELF for fast iteration via PCSX2 -elf.\n"
                                  "On: also run mkisofs to produce a bootable .iso. Required for FreeMcBoot "
                                  "or burning to DVD-R. Needs mkisofs / genisoimage on PATH in the build shell.");
        }

#if defined(_WIN32)
        // ----- Use WSL toggle (Windows only) -------------------------------
        {
            std::string current = ReadOption(ctx, kUseWslKey, kUseWslDefault);
            bool useWsl = (current == "1");
            if (ImGui::Checkbox("Use WSL", &useWsl))
            {
                ctx->SetProfileSetting(kUseWslKey, useWsl ? "1" : "0");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off (default): use native Windows ps2dev (download "
                                  "ps2dev-windows-latest.tar.gz from https://github.com/ps2dev/ps2dev/releases).\n"
                                  "On: route all toolchain calls through WSL2. Useful if your ps2dev install lives "
                                  "inside WSL or if you want mkisofs from genisoimage.");
        }

        // ----- WSL distro override (only meaningful when useWsl=1) ---------
        {
            std::string current = ReadOption(ctx, kWslDistroKey, "");
            char buf[64] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("WSL Distro", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kWslDistroKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Passed to `wsl -d <name>` when 'Use WSL' is on. Empty = default distro.");
        }
#endif

        // ----- PS2DEV path -------------------------------------------------
        {
            std::string current = ReadOption(ctx, kPs2DevPathKey, "");
            char buf[256] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("PS2DEV path", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kPs2DevPathKey, buf);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Absolute path to your ps2dev install as seen by the build shell.\n"
                    "  Native Windows: e.g. C:\\ps2dev\n"
                    "  WSL: e.g. /usr/local/ps2dev or /mnt/c/ps2dev\n"
                    "  POSIX: e.g. /usr/local/ps2dev or $HOME/ps2dev\n"
                    "Leave empty to use $PS2DEV / the makefile's auto-detect fallback.");
            }
        }

        // ----- Makefile path -----------------------------------------------
        {
            std::string current = ReadOption(ctx, kMakefileKey, kMakefileDefault);
            char buf[256] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Makefile", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kMakefileKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Bare filename resolves inside the addon (default: Makefile_PS2). "
                                  "Absolute paths override.");
        }

        // ----- Parallel jobs ----------------------------------------------
        {
            std::string current = ReadOption(ctx, kJobsKey, kJobsDefault);
            int jobs = std::atoi(current.c_str());
            if (jobs < 1 || jobs > 64) jobs = 4;
            if (ImGui::SliderInt("Parallel Jobs", &jobs, 1, 32))
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%d", jobs);
                ctx->SetProfileSetting(kJobsKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("`make -j<N>`. Each engine TU peaks at ~1-2 GB during ee-gcc compile.\n"
                                  "16 GB host → 4-6 jobs. 32 GB → 8-12. Default 4.");
        }

        // ----- PCSX2 path --------------------------------------------------
        {
            std::string current = ReadOption(ctx, kPcsx2PathKey, "");
            char buf[256] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("PCSX2 binary", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kPcsx2PathKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Override the emulator binary. Default: pcsx2-qt.exe (Windows) / pcsx2-qt (POSIX).\n"
                                  "Env var PS2_EMULATOR takes precedence over this field.");
        }

        ImGui::Spacing();
#if defined(_WIN32)
        ImGui::TextDisabled("Default: native Windows ps2dev (https://github.com/ps2dev/ps2dev/releases).");
        ImGui::TextDisabled("Toggle 'Use WSL' to route toolchain calls through your WSL distro.");
#else
        ImGui::TextDisabled("Requires ps2dev ($PS2DEV) and mkisofs (when ISO output enabled) on PATH.");
#endif
        ImGui::TextDisabled("Phase 0-2: ELF boots in PCSX2 + gsKit clears screen. Phases 3-5 (libpad, audsrv, widgets, Lua) TBD.");
    }

    // Canonical descriptor. Strings are deep-copied by the registry; this
    // static instance just needs to outlive the RegisterBuildTarget call.
    static PolyphaseBuildTargetDesc gPs2Target{};
}
#endif // EDITOR

// ----- Plugin lifecycle -----------------------------------------------------

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    if (api) api->LogDebug("com.polyphase.build.target.ps2 loaded.");
    return 0;
}

static void OnUnload()
{
    if (sEngineAPI) sEngineAPI->LogDebug("com.polyphase.build.target.ps2 unloaded.");
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* /*nodeFactory*/) {}
static void RegisterScriptFuncs(lua_State* L) { (void)L; }

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    if (hooks == nullptr) return;

    if (hooks->RegisterBuildTarget == nullptr)
    {
        if (sEngineAPI)
        {
            sEngineAPI->LogWarning("com.polyphase.build.target.ps2: this engine "
                                   "build predates the build-target API (need plugin "
                                   "apiVersion >= 4). Target not registered.");
        }
        return;
    }

    gPs2Target = {};
    gPs2Target.apiVersion            = POLYPHASE_BUILD_TARGET_API_VERSION;
    gPs2Target.targetId              = "homebrew.ps2";
    gPs2Target.displayName           = "Sony PS2 (PS2SDK + gsKit)";
    gPs2Target.iconText              = "";
    gPs2Target.category              = "Retro Consoles";
    gPs2Target.basePlatform          = 1; /* Platform::Linux — Unix-like cook + ELF */
    gPs2Target.binaryExtension       = ".elf";
    gPs2Target.requiresDocker        = 0;
    gPs2Target.supportsRunOnDevice   = 0; /* ps2link arrives in Phase 3+ */
    gPs2Target.supportsEmulator      = 1;
    gPs2Target.Validate              = &Ps2_Validate;
    gPs2Target.PreCook               = nullptr;
    gPs2Target.CookAsset             = nullptr;
    gPs2Target.GetCompileCommand     = &Ps2_GetCompileCommand;
    gPs2Target.GetCompiledBinaryPath = &Ps2_GetCompiledBinaryPath;
    gPs2Target.PostPackage           = &Ps2_PostPackage;
    gPs2Target.RunOnDevice           = nullptr;
    gPs2Target.RunInEmulator         = &Ps2_RunInEmulator;
    gPs2Target.DrawProfileOptions    = &Ps2_DrawProfileOptions;
    gPs2Target.SerializeProfileOptions   = nullptr;
    gPs2Target.DeserializeProfileOptions = nullptr;

    // Variant 2: point the engine at the addon-shipped platform extension
    // headers. The path is interpreted relative to the addon's root (the
    // directory containing package.json). ActionManager writes
    // Generated/PolyphasePlatform_*.h bridges that #include the addon's
    // SystemTypes_Platform.h / InputTypes_Platform.h / AudioTypes_Platform.h /
    // NetworkTypes_Platform.h. The Makefile_PS2 sets -DPOLYPHASE_PLATFORM_ADDON=1
    // and -I<Generated/> so the engine's fork headers pick up the addon-
    // provided typedefs.
    gPs2Target.platformExtensionDir = "Runtime/PS2";

    hooks->RegisterBuildTarget(hookId, &gPs2Target);
}
#endif

extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    if (desc == nullptr) return 1;
    desc->apiVersion          = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName          = "com.polyphase.build.target.ps2";
    desc->pluginVersion       = "1.0.0";
    desc->OnLoad              = OnLoad;
    desc->OnUnload            = OnUnload;
    desc->Tick                = nullptr;
    desc->TickEditor          = nullptr;
    desc->RegisterTypes       = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI    = RegisterEditorUI;
#else
    desc->RegisterEditorUI    = nullptr;
#endif
    desc->OnEditorPreInit     = nullptr;
    desc->OnEditorReady       = nullptr;
    return 0;
}
