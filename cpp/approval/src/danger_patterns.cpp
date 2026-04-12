#include "hermes/approval/danger_patterns.hpp"

namespace hermes::approval {

namespace {

// Build the canonical pattern table. Patterns are ported from
// tools/approval.py DANGEROUS_PATTERNS plus a handful of extras called
// out in the Phase 6 spec (mkfs/dd to /dev, sudo bash<<, certificate
// trust manipulation, iptables -F, crontab -r, shred, passwd, ssh-add).
//
// Severity:
//   3 = high   — irreversible system damage / credential exfil / data loss
//   2 = medium — significant footprint, may be recoverable
//   1 = low    — informational only (no entries at this tier yet)
//
// Categories: filesystem | network | system | database | shell
std::vector<DangerPattern> build_table() {
    std::vector<DangerPattern> p;
    p.reserve(64);

    auto add = [&](std::string key, std::string regex, std::string category,
                   std::string description, int severity = 3) {
        p.push_back({std::move(key), std::move(regex), std::move(category),
                     std::move(description), severity});
    };

    // ---- Recursive deletes ------------------------------------------------
    add("rm_root",
        R"(\brm\s+(-[^\s]*\s+)*/)",
        "filesystem", "delete in root path");
    add("rm_recursive_short",
        R"(\brm\s+-[^\s]*r)",
        "filesystem", "recursive delete");
    add("rm_recursive_long",
        R"(\brm\s+--recursive\b)",
        "filesystem", "recursive delete (long flag)");
    add("rm_rf_root_explicit",
        R"(\brm\s+.*-[rR]f?\s+/)",
        "filesystem", "rm -rf rooted at /");

    // ---- chmod / chown ----------------------------------------------------
    add("chmod_world_writable",
        R"(\bchmod\s+(-[^\s]*\s+)*(777|666|o\+[rwx]*w|a\+[rwx]*w)\b)",
        "filesystem", "world/other-writable permissions");
    add("chmod_recursive_world_writable",
        R"(\bchmod\s+--recursive\b.*(777|666|o\+[rwx]*w|a\+[rwx]*w))",
        "filesystem", "recursive world/other-writable (long flag)");
    add("chown_recursive_root_short",
        R"(\bchown\s+(-[^\s]*)?R\s+root)",
        "filesystem", "recursive chown to root");
    add("chown_recursive_root_long",
        R"(\bchown\s+--recursive\b.*root)",
        "filesystem", "recursive chown to root (long flag)");

    // ---- Filesystem creation / disk overwrite -----------------------------
    add("mkfs_any",
        R"(\bmkfs(\.[a-z0-9]+)?\b)",
        "filesystem", "format filesystem");
    add("dd_if",
        R"(\bdd\s+.*if=)",
        "filesystem", "disk copy");
    add("dd_to_disk",
        R"(\bdd\s+.*of=/dev/[sn]d)",
        "filesystem", "dd write to physical disk");
    add("redirect_to_block_device",
        R"(>\s*/dev/sd)",
        "filesystem", "write to block device");

    // ---- SQL destructive --------------------------------------------------
    add("sql_drop",
        R"(\bdrop\s+(table|database)\b)",
        "database", "SQL DROP");
    add("sql_delete_no_where",
        R"(\bdelete\s+from\b(?!.*\bwhere\b))",
        "database", "SQL DELETE without WHERE");
    add("sql_truncate",
        R"(\btruncate\s+(table)?\s*\w)",
        "database", "SQL TRUNCATE");

    // ---- /etc clobber -----------------------------------------------------
    add("redirect_to_etc",
        R"(>\s*/etc/)",
        "system", "overwrite system config");
    add("redirect_to_etc_passwd",
        R"(>\s*/etc/(passwd|shadow|sudoers|hostname))",
        "system", "overwrite critical /etc file");
    add("append_to_etc",
        R"(>>\s*/etc/)",
        "system", "append to system config");
    add("tee_to_etc",
        R"(\btee\b\s+(-a?\s+)?/etc/)",
        "system", "tee into /etc");

    // ---- Service control --------------------------------------------------
    add("systemctl_stop_disable",
        R"(\bsystemctl\s+(stop|disable|mask)\b)",
        "system", "stop/disable system service");
    add("systemctl_disable_critical",
        R"(\bsystemctl\s+(disable|mask|stop)\s+(sshd|networking|ssh|systemd-networkd))",
        "system", "disable critical system service");

    // ---- Process killing --------------------------------------------------
    add("kill_all_processes",
        R"(\bkill\s+-9\s+-1\b)",
        "system", "kill all processes");
    add("pkill_force",
        R"(\bpkill\s+-9\b)",
        "system", "force kill processes");
    add("killall_any",
        R"(\bkillall\b)",
        "system", "killall command");

    // ---- Fork bomb --------------------------------------------------------
    add("fork_bomb",
        R"(:\(\)\s*\{\s*:\s*\|\s*:\s*&\s*\}\s*;\s*:)",
        "shell", "fork bomb");

    // ---- Shell -c injection ----------------------------------------------
    add("shell_dash_c",
        R"(\b(bash|sh|zsh|ksh)\s+-[^\s]*c(\s+|$))",
        "shell", "shell command via -c/-lc flag");
    add("interpreter_dash_e_or_c",
        R"(\b(python[23]?|perl|ruby|node)\s+-[ec]\s+)",
        "shell", "script execution via -e/-c flag");
    add("interpreter_heredoc",
        R"(\b(python[23]?|perl|ruby|node)\s+<<)",
        "shell", "script execution via heredoc");
    add("sudo_bash_heredoc",
        R"(\bsudo\s+(bash|sh)\s*<<)",
        "shell", "sudo shell with heredoc");

    // ---- Pipe-to-shell ----------------------------------------------------
    add("curl_pipe_shell",
        R"(\b(curl|wget)\b.*\|\s*(ba)?sh\b)",
        "network", "pipe remote content to shell");
    add("shell_proc_subst_remote",
        R"(\b(bash|sh|zsh|ksh)\s+<\s*<?\s*\(\s*(curl|wget)\b)",
        "network", "execute remote script via process substitution");

    // ---- xargs / find -----------------------------------------------------
    add("xargs_rm",
        R"(\bxargs\s+.*\brm\b)",
        "filesystem", "xargs with rm");
    add("find_exec_rm",
        R"(\bfind\b.*-exec\s+(/\S*/)?rm\b)",
        "filesystem", "find -exec rm");
    add("find_delete",
        R"(\bfind\b.*-delete\b)",
        "filesystem", "find -delete");

    // ---- Self-termination protection -------------------------------------
    add("kill_hermes_named",
        R"(\b(pkill|killall)\b.*\b(hermes|gateway|cli\.py)\b)",
        "system", "kill hermes/gateway process (self-termination)");
    add("kill_pgrep_subst_paren",
        R"(\bkill\b.*\$\(\s*pgrep\b)",
        "system", "kill process via pgrep expansion (self-termination)");
    add("kill_pgrep_subst_backtick",
        R"(\bkill\b.*`\s*pgrep\b)",
        "system", "kill process via backtick pgrep expansion (self-termination)");

    // ---- Copy/move/edit into /etc ----------------------------------------
    add("cp_into_etc",
        R"(\b(cp|mv|install)\b.*\s/etc/)",
        "system", "copy/move file into /etc/");
    add("sed_inplace_etc_short",
        R"(\bsed\s+-[^\s]*i.*\s/etc/)",
        "system", "in-place edit of system config");
    add("sed_inplace_etc_long",
        R"(\bsed\s+--in-place\b.*\s/etc/)",
        "system", "in-place edit of system config (long flag)");

    // ---- Git destructive --------------------------------------------------
    add("git_reset_hard",
        R"(\bgit\s+reset\s+--hard\b)",
        "filesystem", "git reset --hard (destroys uncommitted changes)");
    add("git_push_force_long",
        R"(\bgit\s+push\b.*--force\b)",
        "network", "git force push (rewrites remote history)");
    add("git_push_force_short",
        R"(\bgit\s+push\b.*\s-f\b)",
        "network", "git force push short flag");
    add("git_clean_force",
        R"(\bgit\s+clean\s+-[^\s]*f)",
        "filesystem", "git clean -f (deletes untracked files)");
    add("git_branch_delete_force",
        R"(\bgit\s+branch\s+-D\b)",
        "filesystem", "git branch force delete");

    // ---- chmod +x ; ./script ---------------------------------------------
    add("chmod_x_exec",
        R"(\bchmod\s+\+x\b.*[;&|]+\s*\./)",
        "shell", "chmod +x followed by immediate execution");

    // ---- Networking nukes -------------------------------------------------
    add("iptables_flush",
        R"(\biptables\s+-F\b)",
        "network", "iptables flush");
    add("nftables_flush",
        R"(\bnft\s+flush\s+ruleset\b)",
        "network", "nftables flush ruleset");

    // ---- Cron / scheduling -----------------------------------------------
    add("crontab_remove",
        R"(\bcrontab\s+-r\b)",
        "system", "crontab -r (remove all cron jobs)");

    // ---- Data shredding --------------------------------------------------
    add("shred_files",
        R"(\bshred\b)",
        "filesystem", "shred command (irreversible overwrite)");

    // ---- Account / credential manipulation -------------------------------
    add("passwd_change",
        R"(\bpasswd\s+\w)",
        "system", "passwd update for arbitrary user");
    add("ssh_add_remove_all",
        R"(\bssh-add\s+-D\b)",
        "system", "remove all SSH identities");
    add("update_ca_trust",
        R"(\b(update-ca-trust|update-ca-certificates)\b)",
        "system", "modify trusted certificate store");
    add("trust_anchor_add",
        R"(\btrust\s+anchor\b)",
        "system", "p11-kit trust anchor manipulation");

    // ---- Gateway protection ----------------------------------------------
    add("gateway_run_outside_systemd",
        R"(\bgateway\s+run\b.*(&\s*$|&\s*;|\bdisown\b|\bsetsid\b))",
        "system", "start gateway outside systemd");
    add("nohup_gateway_run",
        R"(\bnohup\b.*\bgateway\s+run\b)",
        "system", "nohup gateway run (bypasses systemd)");

    return p;
}

}  // namespace

const std::vector<DangerPattern>& danger_patterns() {
    static const std::vector<DangerPattern> table = build_table();
    return table;
}

std::optional<DangerPattern> find_pattern(std::string_view key) {
    for (const auto& p : danger_patterns()) {
        if (p.key == key) return p;
    }
    return std::nullopt;
}

}  // namespace hermes::approval
