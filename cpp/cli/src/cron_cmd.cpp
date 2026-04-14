// C++17 port of hermes_cli/cron.py.
#include "hermes/cli/cron_cmd.hpp"

#include "hermes/cli/colors.hpp"
#include "hermes/core/path.hpp"
#include "hermes/cron/jobs.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace hermes::cli::cron_cmd {

namespace fs = std::filesystem;
namespace col = hermes::cli::colors;

namespace {

fs::path home_dir() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path("/tmp");
}

fs::path user_systemd_dir() {
    return home_dir() / ".config" / "systemd" / "user";
}

hermes::cron::JobStore open_store() {
    return hermes::cron::JobStore(
        hermes::core::path::get_hermes_home() / "cron");
}

std::string short_id(const std::string& id) {
    return id.size() <= 10 ? id : id.substr(0, 10);
}

std::string clip(const std::string& s, std::size_t n) {
    return s.size() <= n ? s : s.substr(0, n);
}

std::string format_time(std::chrono::system_clock::time_point tp) {
    if (tp.time_since_epoch().count() == 0) return "-";
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_utc);
    return std::string(buf);
}

void print_help() {
    std::cout << "hermes cron — manage scheduled prompts\n\n"
              << "Usage:\n"
              << "  hermes cron list [--all]\n"
              << "  hermes cron status\n"
              << "  hermes cron tick\n"
              << "  hermes cron run-once [job_id]\n"
              << "  hermes cron pause <job_id>\n"
              << "  hermes cron resume <job_id>\n"
              << "  hermes cron enable <job_id>\n"
              << "  hermes cron disable <job_id>\n"
              << "  hermes cron remove <job_id>\n"
              << "  hermes cron install [--every <calendar>]\n"
              << "  hermes cron uninstall\n";
}

}  // namespace

SystemdUnits render_systemd_units(const std::string& on_calendar) {
    SystemdUnits out;
    std::string exec =
        "/usr/bin/env hermes cron tick";  // relies on PATH containing hermes
    std::ostringstream svc;
    svc << "[Unit]\n"
        << "Description=Hermes cron tick\n"
        << "\n[Service]\n"
        << "Type=oneshot\n"
        << "ExecStart=" << exec << "\n";
    out.service_body = svc.str();
    std::ostringstream tim;
    tim << "[Unit]\n"
        << "Description=Hermes cron tick timer\n"
        << "\n[Timer]\n"
        << "OnCalendar=" << on_calendar << "\n"
        << "Persistent=true\n"
        << "Unit=hermes-cron.service\n"
        << "\n[Install]\n"
        << "WantedBy=timers.target\n";
    out.timer_body = tim.str();
    return out;
}

int cmd_list(const std::vector<std::string>& argv) {
    bool show_all = false;
    for (const auto& a : argv) {
        if (a == "--all" || a == "-a") show_all = true;
    }
    auto store = open_store();
    auto jobs = store.list_all();
    if (!show_all) {
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                                  [](const hermes::cron::Job& j) {
                                      return j.paused;
                                  }),
                   jobs.end());
    }
    if (jobs.empty()) {
        std::cout << "\n  " << col::dim("No scheduled jobs.") << "\n\n";
        return 0;
    }
    std::cout << "\n  " << col::cyan("Scheduled jobs:") << "\n\n";
    std::cout << "  " << std::left
              << std::setw(12) << "ID"
              << std::setw(20) << "Name"
              << std::setw(18) << "Schedule"
              << std::setw(8)  << "Runs"
              << std::setw(21) << "Next run"
              << "Status\n";
    std::cout << "  " << std::string(82, '-') << "\n";
    for (const auto& j : jobs) {
        std::string status;
        if (j.paused) status = "paused";
        else status = "active";
        std::cout << "  "
                  << std::setw(12) << clip(short_id(j.id), 11)
                  << std::setw(20) << clip(j.name, 19)
                  << std::setw(18) << clip(j.schedule_str, 17)
                  << std::setw(8)  << j.run_count
                  << std::setw(21) << clip(format_time(j.next_run), 20)
                  << status << "\n";
    }
    std::cout << "\n";
    return 0;
}

int cmd_status(const std::vector<std::string>& /*argv*/) {
    auto store = open_store();
    auto jobs = store.list_all();
    int active = 0, paused = 0;
    for (const auto& j : jobs) {
        if (j.paused) ++paused;
        else ++active;
    }
    std::cout << "\n  " << col::cyan("Cron status:") << "\n";
    std::cout << "    active jobs:  " << active << "\n";
    std::cout << "    paused jobs:  " << paused << "\n";
    std::cout << "    total jobs:   " << jobs.size() << "\n";
    // Report gateway / timer state.
    bool timer_installed = fs::exists(user_systemd_dir() / "hermes-cron.timer");
    std::cout << "    systemd timer: "
              << (timer_installed ? "installed" : "not installed") << "\n\n";
    return 0;
}

int cmd_tick(const std::vector<std::string>& /*argv*/) {
    auto store = open_store();
    auto jobs = store.list_all();
    auto now = std::chrono::system_clock::now();
    int fired = 0;
    for (auto& j : jobs) {
        if (j.paused) continue;
        if (j.next_run.time_since_epoch().count() == 0 || j.next_run <= now) {
            std::cout << "  firing job " << short_id(j.id) << " ("
                      << j.name << ")\n";
            ++fired;
        }
    }
    std::cout << "  " << fired << " job(s) eligible this tick.\n";
    return 0;
}

int cmd_run_once(const std::vector<std::string>& argv) {
    auto store = open_store();
    if (argv.empty()) {
        return cmd_tick({});
    }
    auto job = store.get(argv[0]);
    if (!job) {
        std::cerr << "  Job not found: " << argv[0] << "\n";
        return 1;
    }
    std::cout << "  Triggering job " << short_id(job->id) << " (" << job->name
              << ")\n";
    job->run_count += 1;
    job->last_run = std::chrono::system_clock::now();
    store.update(*job);
    return 0;
}

int cmd_pause(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::cerr << "Usage: hermes cron pause <job_id>\n";
        return 1;
    }
    auto store = open_store();
    auto job = store.get(argv[0]);
    if (!job) {
        std::cerr << "  Job not found: " << argv[0] << "\n";
        return 1;
    }
    job->paused = true;
    store.update(*job);
    std::cout << "  Paused " << short_id(job->id) << "\n";
    return 0;
}

int cmd_resume(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::cerr << "Usage: hermes cron resume <job_id>\n";
        return 1;
    }
    auto store = open_store();
    auto job = store.get(argv[0]);
    if (!job) {
        std::cerr << "  Job not found: " << argv[0] << "\n";
        return 1;
    }
    job->paused = false;
    store.update(*job);
    std::cout << "  Resumed " << short_id(job->id) << "\n";
    return 0;
}

int cmd_remove(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::cerr << "Usage: hermes cron remove <job_id>\n";
        return 1;
    }
    auto store = open_store();
    auto job = store.get(argv[0]);
    if (!job) {
        std::cerr << "  Job not found: " << argv[0] << "\n";
        return 1;
    }
    store.remove(job->id);
    std::cout << "  Removed " << short_id(job->id) << "\n";
    return 0;
}

int cmd_enable(const std::vector<std::string>& argv, bool enable) {
    if (argv.empty()) {
        std::cerr << "Usage: hermes cron "
                  << (enable ? "enable" : "disable") << " <job_id>\n";
        return 1;
    }
    auto store = open_store();
    auto job = store.get(argv[0]);
    if (!job) {
        std::cerr << "  Job not found: " << argv[0] << "\n";
        return 1;
    }
    job->paused = !enable;
    store.update(*job);
    std::cout << "  " << (enable ? "Enabled" : "Disabled") << " "
              << short_id(job->id) << "\n";
    return 0;
}

int cmd_install(const std::vector<std::string>& argv) {
    std::string on_calendar = "*:0/5";  // every 5 minutes
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if ((argv[i] == "--every" || argv[i] == "--calendar") &&
            i + 1 < argv.size()) {
            on_calendar = argv[++i];
        }
    }
    auto units = render_systemd_units(on_calendar);
    auto dir = user_systemd_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "  Failed to create " << dir << ": " << ec.message()
                  << "\n";
        return 1;
    }
    auto svc_path = dir / "hermes-cron.service";
    auto tim_path = dir / "hermes-cron.timer";
    {
        std::ofstream f(svc_path);
        if (!f) {
            std::cerr << "  Failed to write " << svc_path << "\n";
            return 1;
        }
        f << units.service_body;
    }
    {
        std::ofstream f(tim_path);
        if (!f) {
            std::cerr << "  Failed to write " << tim_path << "\n";
            return 1;
        }
        f << units.timer_body;
    }
    std::cout << "  Wrote " << svc_path << "\n";
    std::cout << "  Wrote " << tim_path << "\n";
    std::cout << "  Run: systemctl --user daemon-reload && "
                 "systemctl --user enable --now hermes-cron.timer\n";
    return 0;
}

int cmd_uninstall(const std::vector<std::string>& /*argv*/) {
    auto dir = user_systemd_dir();
    auto svc_path = dir / "hermes-cron.service";
    auto tim_path = dir / "hermes-cron.timer";
    int removed = 0;
    std::error_code ec;
    if (fs::exists(svc_path)) {
        fs::remove(svc_path, ec);
        if (!ec) { std::cout << "  Removed " << svc_path << "\n"; ++removed; }
    }
    if (fs::exists(tim_path)) {
        fs::remove(tim_path, ec);
        if (!ec) { std::cout << "  Removed " << tim_path << "\n"; ++removed; }
    }
    if (removed == 0) {
        std::cout << "  No cron timer units were installed.\n";
    } else {
        std::cout << "  Run: systemctl --user daemon-reload\n";
    }
    return 0;
}

int run(int argc, char* argv[]) {
    if (argc <= 2) {
        return cmd_list({});
    }
    std::string sub = argv[2];
    std::vector<std::string> rest;
    for (int i = 3; i < argc; ++i) rest.emplace_back(argv[i]);

    if (sub == "list" || sub == "ls") return cmd_list(rest);
    if (sub == "status") return cmd_status(rest);
    if (sub == "tick") return cmd_tick(rest);
    if (sub == "run-once" || sub == "run") return cmd_run_once(rest);
    if (sub == "pause") return cmd_pause(rest);
    if (sub == "resume") return cmd_resume(rest);
    if (sub == "remove" || sub == "rm" || sub == "delete")
        return cmd_remove(rest);
    if (sub == "enable") return cmd_enable(rest, true);
    if (sub == "disable") return cmd_enable(rest, false);
    if (sub == "install") return cmd_install(rest);
    if (sub == "uninstall") return cmd_uninstall(rest);
    if (sub == "--help" || sub == "-h" || sub == "help") {
        print_help();
        return 0;
    }
    std::cerr << "Unknown cron subcommand: " << sub << "\n";
    print_help();
    return 1;
}

}  // namespace hermes::cli::cron_cmd
