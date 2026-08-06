// lpr - clean ALPR camera client entry point.
//
// Usage:
//   lpr --nats <url> --token <tok> [--settings-subject <subj>]   # production: bootstrap over NATS
//   lpr --settings <file.json>                                   # offline: bootstrap from a file
//
// Production flow: connect to NATS (sending the token), subscribe to the settings subject,
// and the backend pushes LPR + camera settings -> models load (backend per settings) ->
// cameras are created by link type -> plate detection runs. Offline mode loads a settings
// JSON file directly (same bootstrap), useful without a broker.
#include "lpr/app/Application.h"
#include "lpr/Log.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

namespace {
std::atomic<bool>* g_running = nullptr;
void onSignal(int) { if (g_running) *g_running = false; }

std::string argValue(int argc, char** argv, const std::string& key, const std::string& def = "") {
    for (int i = 1; i + 1 < argc; ++i) if (key == argv[i]) return argv[i + 1];
    return def;
}
bool hasArg(int argc, char** argv, const std::string& key) {
    for (int i = 1; i < argc; ++i) if (key == argv[i]) return true;
    return false;
}
std::string env(const char* name, const std::string& def = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : def;
}
// Directory that contains this executable (so cert/key/ca can sit next to lpr.exe
// regardless of the working directory).
std::string exeDir() {
    try {
#ifdef _WIN32
        char buf[MAX_PATH];
        DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            return std::filesystem::path(std::string(buf, n)).parent_path().string();
#else
        char buf[PATH_MAX];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = '\0'; return std::filesystem::path(buf).parent_path().string(); }
#endif
    } catch (...) {}
    return ".";
}

// Full path to this executable (for a self re-exec on the restart command).
std::string exePath() {
    try {
#ifdef _WIN32
        char buf[MAX_PATH];
        DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) return std::string(buf, n);
#else
        char buf[PATH_MAX];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = '\0'; return std::string(buf); }
#endif
    } catch (...) {}
    return "";
}

// Restart THIS process with the same arguments — the C++ side of the "restart the LPR"
// API. Called only after the pipeline has been stopped (app.stop()) so NATS subs, cameras
// and workers are already torn down. On success this never returns (POSIX execv) / exits
// (Windows). On failure it returns and main() falls through to a normal exit, letting an
// external supervisor (systemd/NSSM/docker restart:always) bring the reader back instead.
void relaunchSelf(char** argv) {
    const std::string path = exePath();
    if (path.empty()) { LOGE() << "lpr: cannot resolve exe path for restart"; return; }
    LOGW() << "lpr: re-launching '" << path << "'";
#ifdef _WIN32
    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    // Reuse the exact original command line so all flags/paths carry over.
    std::string cmdline = ::GetCommandLineA();
    if (::CreateProcessA(path.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                         0, nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hProcess); ::CloseHandle(pi.hThread);
        std::exit(0);                          // parent exits; the fresh process took over
    }
    LOGE() << "lpr: CreateProcess failed (" << ::GetLastError() << "); not restarting";
#else
    ::execv(path.c_str(), argv);               // replaces the image on success
    LOGE() << "lpr: execv failed (" << errno << "); not restarting";
#endif
}
} // namespace

int main(int argc, char** argv) {
    using namespace lpr;

    // File logging: new file daily, 10 MB roll, purge logs older than retention (default 30d).
    // Override dir with LOG_DIR, retention with LOG_RETENTION_DAYS; --log-console also echoes.
    const std::string logDir = env("LOG_DIR");
    Logger::instance().init("lpr", logDir);
    if (!env("LOG_RETENTION_DAYS").empty())
        Logger::instance().setRetentionDays(std::atoi(env("LOG_RETENTION_DAYS").c_str()));
    if (hasArg(argc, argv, "--log-console")) Logger::instance().setConsole(true);

    Application::Options opts;

    // NATS connection from the environment (as in the original), CLI overrides env.
    //   NATS_SERVER_URL, AUTH_TOKEN, NATS_USER, NATS_PASSWORD
    const std::string envUrl = env("NATS_SERVER_URL");
    opts.natsUrl      = argValue(argc, argv, "--nats", envUrl.empty() ? opts.natsUrl : envUrl);
    opts.natsToken    = argValue(argc, argv, "--token", env("AUTH_TOKEN"));
    opts.natsUser     = argValue(argc, argv, "--user", env("NATS_USER"));
    {
        std::string pass = env("NATS_PASS");                  // original env name
        if (pass.empty()) pass = env("NATS_PASSWORD");        // also accepted
        opts.natsPassword = argValue(argc, argv, "--password", pass);
    }
    // Mutual TLS. Precedence: --cert/--key/--ca flag > env > the file sitting next to the
    // exe (client-cert.pem / client-key.pem / ca.pem, the original names). Drop the .pem
    // files beside lpr.exe once and they're picked up every run, no flags or env needed.
    {
        const std::filesystem::path dir = exeDir();
        auto tlsFile = [&](const char* flag, const char* envName, const char* fname) -> std::string {
            std::string v = argValue(argc, argv, flag, env(envName));
            if (!v.empty()) return v;                          // explicit flag/env wins
            std::error_code ec;
            std::filesystem::path p = dir / fname;
            if (std::filesystem::exists(p, ec)) return p.string();   // default: next to the exe
            return "";                                         // absent -> skip (no mutual TLS for it)
        };
        opts.natsCertFile = tlsFile("--cert", "NATS_CERT_FILE", "client-cert.pem");
        opts.natsKeyFile  = tlsFile("--key",  "NATS_KEY_FILE",  "client-key.pem");
        opts.natsCaFile   = tlsFile("--ca",   "NATS_CA_FILE",   "ca.pem");
        LOGI() << "NATS TLS files: cert='" << opts.natsCertFile << "' key='" << opts.natsKeyFile
               << "' ca='" << opts.natsCaFile << "'";
    }
    // TLS handshake-first: on via --tls-first or NATS_TLS_FIRST, but --no-tls-first ALWAYS wins
    // (a definitive override for a stale/persistent NATS_TLS_FIRST env var).
    opts.natsTlsFirst = (hasArg(argc, argv, "--tls-first") || !env("NATS_TLS_FIRST").empty())
                        && !hasArg(argc, argv, "--no-tls-first");
    if (!env("NATS_TLS_HOSTNAME").empty())
        opts.natsExpectedHostname = env("NATS_TLS_HOSTNAME");
    if (!argValue(argc, argv, "--tls-hostname").empty())
        opts.natsExpectedHostname = argValue(argc, argv, "--tls-hostname");
    // Use NATS if a URL was provided by env or CLI.
    opts.useNats = hasArg(argc, argv, "--nats") || !envUrl.empty();
    if (!argValue(argc, argv, "--settings-subject").empty())
        opts.settingsRequestSubject = argValue(argc, argv, "--settings-subject");

    const std::string settingsFile = argValue(argc, argv, "--settings");
    opts.configPath = argValue(argc, argv, "--config", env("LPR_CONFIG"));

    Application app(std::move(opts));

    std::atomic<bool> running{true};
    std::atomic<bool> restartRequested{false};
    g_running = &running;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // Backend "restart the LPR" command (reset_lpr on command.<clientId>): end the run
    // loop and remember that this was a restart, not a shutdown, so we re-exec below.
    app.setRestartHandler([&running, &restartRequested] {
        restartRequested = true;
        running = false;
    });

    if (!settingsFile.empty()) {
        // Offline: load settings JSON and bootstrap directly.
        std::ifstream f(settingsFile);
        if (!f) { std::cerr << "cannot open settings file: " << settingsFile << "\n"; return 1; }
        nlohmann::json body;
        try { f >> body; }
        catch (const std::exception& e) { std::cerr << "bad settings JSON: " << e.what() << "\n"; return 1; }
        app.bootstrapFromJson(body);
    } else {
        if (!app.start(running)) {           // retries NATS connect until up or Ctrl-C
            std::cerr << "stopped before a connection was established\n";
            return running ? 1 : 0;
        }
    }

    LOGI() << "lpr: running (Ctrl-C to stop)";
    while (running) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    LOGI() << "lpr: shutting down";
    app.stop();

    // A reset_lpr command asked for a restart: the pipeline is now stopped, so relaunch
    // this process with the same arguments. relaunchSelf doesn't return on success.
    if (restartRequested) relaunchSelf(argv);

    return 0;
}
