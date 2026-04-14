// Skills Guard — regex-based security scanner.  See the header for a full
// description of the trust model and install policy.  This file is a
// function-level port of tools/skills_guard.py with the LLM-audit path
// removed (the C++ surface calls the LLM lazily via a different module).
#include "hermes/skills/skills_guard.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>

namespace hermes::skills::guard {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Trust configuration
// ---------------------------------------------------------------------------

namespace {

const std::unordered_set<std::string>& trusted_repos() {
    static const std::unordered_set<std::string> repos = {
        "openai/skills", "anthropics/skills",
    };
    return repos;
}

// Install policy matrix: trust_level x verdict -> "allow" | "block" | "ask".
// Mirrors INSTALL_POLICY in Python.
std::string policy_decision(const std::string& trust, const std::string& verdict) {
    // Row order: {safe, caution, dangerous}
    struct Row { const char* safe; const char* caution; const char* dangerous; };
    Row row;
    if (trust == "builtin") {
        row = {"allow", "allow", "allow"};
    } else if (trust == "trusted") {
        row = {"allow", "allow", "block"};
    } else if (trust == "agent-created") {
        row = {"allow", "allow", "ask"};
    } else {  // community or unknown
        row = {"allow", "block", "block"};
    }
    if (verdict == "safe") return row.safe;
    if (verdict == "caution") return row.caution;
    return row.dangerous;
}

// Quick lowercase helper — used by severity/category comparisons.
std::string to_upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string right_pad(std::string s, std::size_t n) {
    if (s.size() < n) s.append(n - s.size(), ' ');
    return s;
}

std::string iso8601_utc_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - secs).count();
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setw(6) << std::setfill('0') << us
        << "+00:00";
    return oss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Threat pattern table
// ---------------------------------------------------------------------------

namespace {

struct PatternRow {
    const char* regex;
    const char* id;
    const char* severity;
    const char* category;
    const char* description;
};

// NOTE: Patterns use std::regex with ECMAScript grammar + case-insensitive
// matching (to mirror Python's re.IGNORECASE).  Any pattern that fails to
// compile is logged and skipped rather than crashing the scanner.
const std::vector<PatternRow>& threat_patterns() {
    static const std::vector<PatternRow> patterns = {
        // Exfiltration: shell commands leaking secrets
        {R"(curl\s+[^\n]*\$\{?\w*(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|API))",
         "env_exfil_curl", "critical", "exfiltration",
         "curl command interpolating secret environment variable"},
        {R"(wget\s+[^\n]*\$\{?\w*(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|API))",
         "env_exfil_wget", "critical", "exfiltration",
         "wget command interpolating secret environment variable"},
        {R"(fetch\s*\([^\n]*\$\{?\w*(KEY|TOKEN|SECRET|PASSWORD|API))",
         "env_exfil_fetch", "critical", "exfiltration",
         "fetch() call interpolating secret environment variable"},
        {R"(httpx?\.(get|post|put|patch)\s*\([^\n]*(KEY|TOKEN|SECRET|PASSWORD))",
         "env_exfil_httpx", "critical", "exfiltration",
         "HTTP library call with secret variable"},
        {R"(requests\.(get|post|put|patch)\s*\([^\n]*(KEY|TOKEN|SECRET|PASSWORD))",
         "env_exfil_requests", "critical", "exfiltration",
         "requests library call with secret variable"},
        // Exfiltration: reading credential stores
        {R"(base64[^\n]*env)", "encoded_exfil", "high", "exfiltration",
         "base64 encoding combined with environment access"},
        {R"(\$HOME/\.ssh|~/\.ssh)", "ssh_dir_access", "high", "exfiltration",
         "references user SSH directory"},
        {R"(\$HOME/\.aws|~/\.aws)", "aws_dir_access", "high", "exfiltration",
         "references user AWS credentials directory"},
        {R"(\$HOME/\.gnupg|~/\.gnupg)", "gpg_dir_access", "high", "exfiltration",
         "references user GPG keyring"},
        {R"(\$HOME/\.kube|~/\.kube)", "kube_dir_access", "high", "exfiltration",
         "references Kubernetes config directory"},
        {R"(\$HOME/\.docker|~/\.docker)", "docker_dir_access", "high", "exfiltration",
         "references Docker config (may contain registry creds)"},
        {R"(\$HOME/\.hermes/\.env|~/\.hermes/\.env)", "hermes_env_access", "critical", "exfiltration",
         "directly references Hermes secrets file"},
        {R"(cat\s+[^\n]*(\.env|credentials|\.netrc|\.pgpass|\.npmrc|\.pypirc))",
         "read_secrets_file", "critical", "exfiltration",
         "reads known secrets file"},
        // Exfiltration: programmatic env access
        {R"(printenv|env\s*\|)", "dump_all_env", "high", "exfiltration",
         "dumps all environment variables"},
        {R"(os\.environ\b(?!\s*\.get\s*\(\s*["']PATH))",
         "python_os_environ", "high", "exfiltration",
         "accesses os.environ (potential env dump)"},
        {R"(os\.getenv\s*\(\s*[^\)]*(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL))",
         "python_getenv_secret", "critical", "exfiltration",
         "reads secret via os.getenv()"},
        {R"(process\.env\[)", "node_process_env", "high", "exfiltration",
         "accesses process.env (Node.js environment)"},
        {R"(ENV\[.*(KEY|TOKEN|SECRET|PASSWORD))", "ruby_env_secret", "critical", "exfiltration",
         "reads secret via Ruby ENV[]"},
        // Exfiltration: DNS and staging
        {R"(\b(dig|nslookup|host)\s+[^\n]*\$)", "dns_exfil", "critical", "exfiltration",
         "DNS lookup with variable interpolation (possible DNS exfiltration)"},
        {R"(>\s*/tmp/[^\s]*\s*&&\s*(curl|wget|nc|python))", "tmp_staging", "critical", "exfiltration",
         "writes to /tmp then exfiltrates"},
        // Markdown image/link exfil
        {R"(!\[.*\]\(https?://[^\)]*\$\{?)", "md_image_exfil", "high", "exfiltration",
         "markdown image URL with variable interpolation (image-based exfil)"},
        {R"(\[.*\]\(https?://[^\)]*\$\{?)", "md_link_exfil", "high", "exfiltration",
         "markdown link with variable interpolation"},
        // Prompt injection
        {R"(ignore\s+(\w+\s+)*(previous|all|above|prior)\s+instructions)",
         "prompt_injection_ignore", "critical", "injection",
         "prompt injection: ignore previous instructions"},
        {R"(you\s+are\s+(\w+\s+)*now\s+)", "role_hijack", "high", "injection",
         "attempts to override the agent's role"},
        {R"(do\s+not\s+(\w+\s+)*tell\s+(\w+\s+)*the\s+user)",
         "deception_hide", "critical", "injection",
         "instructs agent to hide information from user"},
        {R"(system\s+prompt\s+override)", "sys_prompt_override", "critical", "injection",
         "attempts to override the system prompt"},
        {R"(pretend\s+(\w+\s+)*(you\s+are|to\s+be)\s+)", "role_pretend", "high", "injection",
         "attempts to make the agent assume a different identity"},
        {R"(disregard\s+(\w+\s+)*(your|all|any)\s+(\w+\s+)*(instructions|rules|guidelines))",
         "disregard_rules", "critical", "injection",
         "instructs agent to disregard its rules"},
        {R"(output\s+(\w+\s+)*(system|initial)\s+prompt)",
         "leak_system_prompt", "high", "injection",
         "attempts to extract the system prompt"},
        {R"((when|if)\s+no\s*one\s+is\s+(watching|looking))",
         "conditional_deception", "high", "injection",
         "conditional instruction to behave differently when unobserved"},
        {R"(act\s+as\s+(if|though)\s+(\w+\s+)*you\s+(\w+\s+)*(have\s+no|don't\s+have)\s+(\w+\s+)*(restrictions|limits|rules))",
         "bypass_restrictions", "critical", "injection",
         "instructs agent to act without restrictions"},
        {R"(translate\s+.*\s+into\s+.*\s+and\s+(execute|run|eval))",
         "translate_execute", "critical", "injection",
         "translate-then-execute evasion technique"},
        {R"(<!--[^>]*(ignore|override|system|secret|hidden)[^>]*-->)",
         "html_comment_injection", "high", "injection",
         "hidden instructions in HTML comments"},
        {R"(<\s*div\s+style\s*=\s*["'][\s\S]*?display\s*:\s*none)",
         "hidden_div", "high", "injection",
         "hidden HTML div (invisible instructions)"},
        // Destructive
        {R"(rm\s+-rf\s+/)", "destructive_root_rm", "critical", "destructive",
         "recursive delete from root"},
        {R"(rm\s+(-[^\s]*)?r.*\$HOME|\brmdir\s+.*\$HOME)",
         "destructive_home_rm", "critical", "destructive",
         "recursive delete targeting home directory"},
        {R"(chmod\s+777)", "insecure_perms", "medium", "destructive",
         "sets world-writable permissions"},
        {R"(>\s*/etc/)", "system_overwrite", "critical", "destructive",
         "overwrites system configuration file"},
        {R"(\bmkfs\b)", "format_filesystem", "critical", "destructive",
         "formats a filesystem"},
        {R"(\bdd\s+.*if=.*of=/dev/)", "disk_overwrite", "critical", "destructive",
         "raw disk write operation"},
        {R"(shutil\.rmtree\s*\(\s*["'/])", "python_rmtree", "high", "destructive",
         "Python rmtree on absolute or root-relative path"},
        {R"(truncate\s+-s\s*0\s+/)", "truncate_system", "critical", "destructive",
         "truncates system file to zero bytes"},
        // Persistence
        {R"(\bcrontab\b)", "persistence_cron", "medium", "persistence",
         "modifies cron jobs"},
        {R"(\.(bashrc|zshrc|profile|bash_profile|bash_login|zprofile|zlogin)\b)",
         "shell_rc_mod", "medium", "persistence",
         "references shell startup file"},
        {R"(authorized_keys)", "ssh_backdoor", "critical", "persistence",
         "modifies SSH authorized keys"},
        {R"(ssh-keygen)", "ssh_keygen", "medium", "persistence",
         "generates SSH keys"},
        {R"(systemd.*\.service|systemctl\s+(enable|start))",
         "systemd_service", "medium", "persistence",
         "references or enables systemd service"},
        {R"(/etc/init\.d/)", "init_script", "medium", "persistence",
         "references init.d startup script"},
        {R"(launchctl\s+load|LaunchAgents|LaunchDaemons)",
         "macos_launchd", "medium", "persistence",
         "macOS launch agent/daemon persistence"},
        {R"(/etc/sudoers|visudo)", "sudoers_mod", "critical", "persistence",
         "modifies sudoers (privilege escalation)"},
        {R"(git\s+config\s+--global\s+)", "git_config_global", "medium", "persistence",
         "modifies global git configuration"},
        // Network
        {R"(\bnc\s+-[lp]|ncat\s+-[lp]|\bsocat\b)", "reverse_shell", "critical", "network",
         "potential reverse shell listener"},
        {R"(\bngrok\b|\blocaltunnel\b|\bserveo\b|\bcloudflared\b)",
         "tunnel_service", "high", "network",
         "uses tunneling service for external access"},
        {R"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}:\d{2,5})",
         "hardcoded_ip_port", "medium", "network",
         "hardcoded IP address with port"},
        {R"(0\.0\.0\.0:\d+|INADDR_ANY)", "bind_all_interfaces", "high", "network",
         "binds to all network interfaces"},
        {R"(/bin/(ba)?sh\s+-i\s+.*>/dev/tcp/)", "bash_reverse_shell", "critical", "network",
         "bash interactive reverse shell via /dev/tcp"},
        {R"(python[23]?\s+-c\s+["']import\s+socket)", "python_socket_oneliner", "critical", "network",
         "Python one-liner socket connection (likely reverse shell)"},
        {R"(socket\.connect\s*\(\s*\()", "python_socket_connect", "high", "network",
         "Python socket connect to arbitrary host"},
        {R"(webhook\.site|requestbin\.com|pipedream\.net|hookbin\.com)",
         "exfil_service", "high", "network",
         "references known data exfiltration/webhook testing service"},
        {R"(pastebin\.com|hastebin\.com|ghostbin\.)", "paste_service", "medium", "network",
         "references paste service (possible data staging)"},
        // Obfuscation
        {R"(base64\s+(-d|--decode)\s*\|)", "base64_decode_pipe", "high", "obfuscation",
         "base64 decodes and pipes to execution"},
        {R"(\\x[0-9a-fA-F]{2}.*\\x[0-9a-fA-F]{2}.*\\x[0-9a-fA-F]{2})",
         "hex_encoded_string", "medium", "obfuscation",
         "hex-encoded string (possible obfuscation)"},
        {R"(\beval\s*\(\s*["'])", "eval_string", "high", "obfuscation",
         "eval() with string argument"},
        {R"(\bexec\s*\(\s*["'])", "exec_string", "high", "obfuscation",
         "exec() with string argument"},
        {R"(echo\s+[^\n]*\|\s*(bash|sh|python|perl|ruby|node))",
         "echo_pipe_exec", "critical", "obfuscation",
         "echo piped to interpreter for execution"},
        {R"(compile\s*\(\s*[^\)]+,\s*["'].*["']\s*,\s*["']exec["']\s*\))",
         "python_compile_exec", "high", "obfuscation",
         "Python compile() with exec mode"},
        {R"(getattr\s*\(\s*__builtins__)", "python_getattr_builtins", "high", "obfuscation",
         "dynamic access to Python builtins (evasion technique)"},
        {R"(__import__\s*\(\s*["']os["']\s*\))", "python_import_os", "high", "obfuscation",
         "dynamic import of os module"},
        {R"(codecs\.decode\s*\(\s*["'])", "python_codecs_decode", "medium", "obfuscation",
         "codecs.decode (possible ROT13 or encoding obfuscation)"},
        {R"(String\.fromCharCode|charCodeAt)", "js_char_code", "medium", "obfuscation",
         "JavaScript character code construction (possible obfuscation)"},
        {R"(atob\s*\(|btoa\s*\()", "js_base64", "medium", "obfuscation",
         "JavaScript base64 encode/decode"},
        {R"(\[::-1\])", "string_reversal", "low", "obfuscation",
         "string reversal (possible obfuscated payload)"},
        {R"(chr\s*\(\s*\d+\s*\)\s*\+\s*chr\s*\(\s*\d+)",
         "chr_building", "high", "obfuscation",
         "building string from chr() calls (obfuscation)"},
        {R"(\\u[0-9a-fA-F]{4}.*\\u[0-9a-fA-F]{4}.*\\u[0-9a-fA-F]{4})",
         "unicode_escape_chain", "medium", "obfuscation",
         "chain of unicode escapes (possible obfuscation)"},
        // Execution
        {R"(subprocess\.(run|call|Popen|check_output)\s*\()",
         "python_subprocess", "medium", "execution",
         "Python subprocess execution"},
        {R"(os\.system\s*\()", "python_os_system", "high", "execution",
         "os.system() — unguarded shell execution"},
        {R"(os\.popen\s*\()", "python_os_popen", "high", "execution",
         "os.popen() — shell pipe execution"},
        {R"(child_process\.(exec|spawn|fork)\s*\()",
         "node_child_process", "high", "execution",
         "Node.js child_process execution"},
        {R"(Runtime\.getRuntime\(\)\.exec\()", "java_runtime_exec", "high", "execution",
         "Java Runtime.exec() — shell execution"},
        {R"(`[^`]*\$\([^)]+\)[^`]*`)", "backtick_subshell", "medium", "execution",
         "backtick string with command substitution"},
        // Path traversal
        {R"(\.\./\.\./\.\.)", "path_traversal_deep", "high", "traversal",
         "deep relative path traversal (3+ levels up)"},
        {R"(\.\./\.\.)", "path_traversal", "medium", "traversal",
         "relative path traversal (2+ levels up)"},
        {R"(/etc/passwd|/etc/shadow)", "system_passwd_access", "critical", "traversal",
         "references system password files"},
        {R"(/proc/self|/proc/\d+/)", "proc_access", "high", "traversal",
         "references /proc filesystem (process introspection)"},
        {R"(/dev/shm/)", "dev_shm", "medium", "traversal",
         "references shared memory (common staging area)"},
        // Crypto mining
        {R"(xmrig|stratum\+tcp|monero|coinhive|cryptonight)",
         "crypto_mining", "critical", "mining",
         "cryptocurrency mining reference"},
        {R"(hashrate|nonce.*difficulty)", "mining_indicators", "medium", "mining",
         "possible cryptocurrency mining indicators"},
        // Supply chain
        {R"(curl\s+[^\n]*\|\s*(ba)?sh)", "curl_pipe_shell", "critical", "supply_chain",
         "curl piped to shell (download-and-execute)"},
        {R"(wget\s+[^\n]*-O\s*-\s*\|\s*(ba)?sh)", "wget_pipe_shell", "critical", "supply_chain",
         "wget piped to shell (download-and-execute)"},
        {R"(curl\s+[^\n]*\|\s*python)", "curl_pipe_python", "critical", "supply_chain",
         "curl piped to Python interpreter"},
        {R"(#\s*///\s*script.*dependencies)", "pep723_inline_deps", "medium", "supply_chain",
         "PEP 723 inline script metadata with dependencies (verify pinning)"},
        {R"(pip\s+install\s+(?!-r\s)(?!.*==))", "unpinned_pip_install", "medium", "supply_chain",
         "pip install without version pinning"},
        {R"(npm\s+install\s+(?!.*@\d))", "unpinned_npm_install", "medium", "supply_chain",
         "npm install without version pinning"},
        {R"(uv\s+run\s+)", "uv_run", "medium", "supply_chain",
         "uv run (may auto-install unpinned dependencies)"},
        {R"((curl|wget|httpx?\.get|requests\.get|fetch)\s*[\(]?\s*["']https?://)",
         "remote_fetch", "medium", "supply_chain",
         "fetches remote resource at runtime"},
        {R"(git\s+clone\s+)", "git_clone", "medium", "supply_chain",
         "clones a git repository at runtime"},
        {R"(docker\s+pull\s+)", "docker_pull", "medium", "supply_chain",
         "pulls a Docker image at runtime"},
        // Privilege escalation
        {R"(^allowed-tools\s*:)", "allowed_tools_field", "high", "privilege_escalation",
         "skill declares allowed-tools (pre-approves tool access)"},
        {R"(\bsudo\b)", "sudo_usage", "high", "privilege_escalation",
         "uses sudo (privilege escalation)"},
        {R"(setuid|setgid|cap_setuid)", "setuid_setgid", "critical", "privilege_escalation",
         "setuid/setgid (privilege escalation mechanism)"},
        {R"(NOPASSWD)", "nopasswd_sudo", "critical", "privilege_escalation",
         "NOPASSWD sudoers entry (passwordless privilege escalation)"},
        {R"(chmod\s+[u+]?s)", "suid_bit", "critical", "privilege_escalation",
         "sets SUID/SGID bit on a file"},
        // Agent config persistence
        {R"(AGENTS\.md|CLAUDE\.md|\.cursorrules|\.clinerules)",
         "agent_config_mod", "critical", "persistence",
         "references agent config files (could persist malicious instructions across sessions)"},
        {R"(\.hermes/config\.yaml|\.hermes/SOUL\.md)",
         "hermes_config_mod", "critical", "persistence",
         "references Hermes configuration files directly"},
        {R"(\.claude/settings|\.codex/config)", "other_agent_config", "high", "persistence",
         "references other agent configuration files"},
        // Credential exposure
        {R"((api[_-]?key|token|secret|password)\s*[=:]\s*["'][A-Za-z0-9+/=_-]{20,})",
         "hardcoded_secret", "critical", "credential_exposure",
         "possible hardcoded API key, token, or secret"},
        {R"(-----BEGIN\s+(RSA\s+)?PRIVATE\s+KEY-----)",
         "embedded_private_key", "critical", "credential_exposure",
         "embedded private key"},
        {R"(ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{80,})",
         "github_token_leaked", "critical", "credential_exposure",
         "GitHub personal access token in skill content"},
        {R"(sk-[A-Za-z0-9]{20,})", "openai_key_leaked", "critical", "credential_exposure",
         "possible OpenAI API key in skill content"},
        {R"(sk-ant-[A-Za-z0-9_-]{90,})", "anthropic_key_leaked", "critical", "credential_exposure",
         "possible Anthropic API key in skill content"},
        {R"(AKIA[0-9A-Z]{16})", "aws_access_key_leaked", "critical", "credential_exposure",
         "AWS access key ID in skill content"},
        // Jailbreak
        {R"(\bDAN\s+mode\b|Do\s+Anything\s+Now)", "jailbreak_dan", "critical", "injection",
         "DAN (Do Anything Now) jailbreak attempt"},
        {R"(\bdeveloper\s+mode\b.*\benabled?\b)", "jailbreak_dev_mode", "critical", "injection",
         "developer mode jailbreak attempt"},
        {R"(hypothetical\s+scenario.*(ignore|bypass|override))",
         "hypothetical_bypass", "high", "injection",
         "hypothetical scenario used to bypass restrictions"},
        {R"(for\s+educational\s+purposes?\s+only)",
         "educational_pretext", "medium", "injection",
         "educational pretext often used to justify harmful content"},
        {R"((respond|answer|reply)\s+without\s+(\w+\s+)*(restrictions|limitations|filters|safety))",
         "remove_filters", "critical", "injection",
         "instructs agent to respond without safety filters"},
        {R"(you\s+have\s+been\s+(\w+\s+)*(updated|upgraded|patched)\s+to)",
         "fake_update", "high", "injection",
         "fake update/patch announcement (social engineering)"},
        {R"(new\s+policy|updated\s+guidelines|revised\s+instructions)",
         "fake_policy", "medium", "injection",
         "claims new policy/guidelines (may be social engineering)"},
        // Context window exfil
        {R"((include|output|print|send|share)\s+(\w+\s+)*(conversation|chat\s+history|previous\s+messages|context))",
         "context_exfil", "high", "exfiltration",
         "instructs agent to output/share conversation history"},
        {R"((send|post|upload|transmit)\s+.*\s+(to|at)\s+https?://)",
         "send_to_url", "high", "exfiltration",
         "instructs agent to send data to a URL"},
    };
    return patterns;
}

// Compiled form of the table — built lazily on first scan.
struct CompiledPattern {
    std::regex re;
    const PatternRow* row;
};

const std::vector<CompiledPattern>& compiled_patterns() {
    static const auto compiled = [] {
        std::vector<CompiledPattern> out;
        out.reserve(threat_patterns().size());
        for (const auto& row : threat_patterns()) {
            try {
                out.push_back({std::regex(row.regex,
                                          std::regex::ECMAScript | std::regex::icase),
                               &row});
            } catch (const std::regex_error&) {
                // Skip unsupported patterns rather than crashing.
            }
        }
        return out;
    }();
    return compiled;
}

const std::unordered_set<std::string>& scannable_extensions() {
    static const std::unordered_set<std::string> exts = {
        ".md", ".txt", ".py", ".sh", ".bash", ".js", ".ts", ".rb",
        ".yaml", ".yml", ".json", ".toml", ".cfg", ".ini", ".conf",
        ".html", ".css", ".xml", ".tex", ".r", ".jl", ".pl", ".php",
    };
    return exts;
}

const std::unordered_set<std::string>& suspicious_binary_extensions() {
    static const std::unordered_set<std::string> exts = {
        ".exe", ".dll", ".so", ".dylib", ".bin", ".dat", ".com",
        ".msi", ".dmg", ".app", ".deb", ".rpm",
    };
    return exts;
}

// Invisible/bidi characters as UTF-8 byte sequences.  Detection scans the
// raw UTF-8 text for these sequences rather than decoding to code points.
struct InvisibleChar {
    const char* bytes;   // UTF-8 encoding
    std::uint32_t code;  // code point
    const char* name;
};
const std::vector<InvisibleChar>& invisible_chars() {
    static const std::vector<InvisibleChar> chars = {
        {"\xE2\x80\x8B", 0x200B, "zero-width space"},
        {"\xE2\x80\x8C", 0x200C, "zero-width non-joiner"},
        {"\xE2\x80\x8D", 0x200D, "zero-width joiner"},
        {"\xE2\x81\xA0", 0x2060, "word joiner"},
        {"\xE2\x81\xA2", 0x2062, "invisible times"},
        {"\xE2\x81\xA3", 0x2063, "invisible separator"},
        {"\xE2\x81\xA4", 0x2064, "invisible plus"},
        {"\xEF\xBB\xBF", 0xFEFF, "BOM/zero-width no-break space"},
        {"\xE2\x80\xAA", 0x202A, "LTR embedding"},
        {"\xE2\x80\xAB", 0x202B, "RTL embedding"},
        {"\xE2\x80\xAC", 0x202C, "pop directional"},
        {"\xE2\x80\xAD", 0x202D, "LTR override"},
        {"\xE2\x80\xAE", 0x202E, "RTL override"},
        {"\xE2\x81\xA6", 0x2066, "LTR isolate"},
        {"\xE2\x81\xA7", 0x2067, "RTL isolate"},
        {"\xE2\x81\xA8", 0x2068, "first strong isolate"},
        {"\xE2\x81\xA9", 0x2069, "pop directional isolate"},
    };
    return chars;
}

std::string strip(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, e - b + 1);
}

std::string hex4(std::uint32_t c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04X", c);
    return buf;
}

}  // namespace

std::size_t threat_pattern_count() {
    return threat_patterns().size();
}

// ---------------------------------------------------------------------------
// Trust level resolution
// ---------------------------------------------------------------------------

std::string resolve_trust_level(std::string_view source_in) {
    std::string source(source_in);
    const char* prefixes[] = {"skills-sh/", "skills.sh/", "skils-sh/", "skils.sh/"};
    for (const auto* prefix : prefixes) {
        std::string p(prefix);
        if (source.rfind(p, 0) == 0) {
            source.erase(0, p.size());
            break;
        }
    }
    if (source == "agent-created") return "agent-created";
    if (source == "official" || source.rfind("official/", 0) == 0) return "builtin";
    for (const auto& t : trusted_repos()) {
        if (source == t || source.rfind(t, 0) == 0) return "trusted";
    }
    return "community";
}

// ---------------------------------------------------------------------------
// Verdict
// ---------------------------------------------------------------------------

std::string determine_verdict(const std::vector<Finding>& findings) {
    if (findings.empty()) return "safe";
    bool has_critical = false;
    bool has_high = false;
    for (const auto& f : findings) {
        if (f.severity == "critical") has_critical = true;
        else if (f.severity == "high") has_high = true;
    }
    if (has_critical) return "dangerous";
    (void)has_high;  // verdict "caution" covers both high-and-only and medium-only
    return "caution";
}

SeverityCounts count_severities(const std::vector<Finding>& findings) {
    SeverityCounts c;
    for (const auto& f : findings) {
        if (f.severity == "critical") ++c.critical;
        else if (f.severity == "high") ++c.high;
        else if (f.severity == "medium") ++c.medium;
        else if (f.severity == "low") ++c.low;
    }
    return c;
}

// ---------------------------------------------------------------------------
// File scanning
// ---------------------------------------------------------------------------

std::vector<Finding> scan_file(const fs::path& file_path, std::string_view rel_path_in) {
    std::vector<Finding> findings;
    std::string rel_path = rel_path_in.empty()
        ? file_path.filename().string()
        : std::string(rel_path_in);

    // Only scan recognised text extensions (SKILL.md gets a free pass).
    std::string ext = file_path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto& sc = scannable_extensions();
    if (sc.find(ext) == sc.end() && file_path.filename() != "SKILL.md") {
        return findings;
    }

    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) return findings;
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    // Split content into lines.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : content) {
            if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        lines.push_back(std::move(cur));
    }

    // Regex pass — one finding per (pattern_id, line).
    std::set<std::pair<std::string, int>> seen;
    for (const auto& cp : compiled_patterns()) {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            int lineno = static_cast<int>(i) + 1;
            std::pair<std::string, int> key{cp.row->id, lineno};
            if (seen.count(key)) continue;
            std::smatch m;
            if (std::regex_search(lines[i], m, cp.re)) {
                seen.insert(key);
                std::string matched = strip(lines[i]);
                if (matched.size() > 120) matched = matched.substr(0, 117) + "...";
                Finding f;
                f.pattern_id = cp.row->id;
                f.severity = cp.row->severity;
                f.category = cp.row->category;
                f.file = rel_path;
                f.line = lineno;
                f.match = std::move(matched);
                f.description = cp.row->description;
                findings.push_back(std::move(f));
            }
        }
    }

    // Invisible unicode pass — one finding per line.
    for (std::size_t i = 0; i < lines.size(); ++i) {
        int lineno = static_cast<int>(i) + 1;
        for (const auto& inv : invisible_chars()) {
            if (lines[i].find(inv.bytes) != std::string::npos) {
                Finding f;
                f.pattern_id = "invisible_unicode";
                f.severity = "high";
                f.category = "injection";
                f.file = rel_path;
                f.line = lineno;
                f.match = "U+" + hex4(inv.code) + " (" + inv.name + ")";
                f.description = std::string("invisible unicode character ")
                              + inv.name
                              + " (possible text hiding/injection)";
                findings.push_back(std::move(f));
                break;
            }
        }
    }

    return findings;
}

// ---------------------------------------------------------------------------
// Structural checks
// ---------------------------------------------------------------------------

std::vector<Finding> check_structure(const fs::path& skill_dir) {
    std::vector<Finding> findings;
    std::error_code ec;
    if (!fs::is_directory(skill_dir, ec)) return findings;

    int file_count = 0;
    std::uintmax_t total_size = 0;
    auto base_canonical = fs::weakly_canonical(skill_dir, ec);

    for (auto it = fs::recursive_directory_iterator(
             skill_dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        std::error_code lec;
        bool is_sym = fs::is_symlink(entry.path(), lec);
        bool is_reg = fs::is_regular_file(entry.path(), lec);
        if (!is_sym && !is_reg) continue;

        std::string rel;
        try {
            rel = fs::relative(entry.path(), skill_dir).string();
        } catch (...) { rel = entry.path().filename().string(); }

        ++file_count;

        if (is_sym) {
            std::error_code reec;
            auto resolved = fs::read_symlink(entry.path(), reec);
            if (!reec) {
                // Resolve relative symlink targets.
                auto absolute = resolved.is_absolute()
                    ? resolved
                    : entry.path().parent_path() / resolved;
                auto canon = fs::weakly_canonical(absolute, reec);
                std::error_code cmp_ec;
                auto base_str = base_canonical.string();
                auto canon_str = canon.string();
                bool escapes = canon_str.rfind(base_str, 0) != 0;
                if (escapes) {
                    Finding f;
                    f.pattern_id = "symlink_escape";
                    f.severity = "critical";
                    f.category = "traversal";
                    f.file = rel;
                    f.line = 0;
                    f.match = std::string("symlink -> ") + canon_str;
                    f.description = "symlink points outside the skill directory";
                    findings.push_back(std::move(f));
                }
            } else {
                Finding f;
                f.pattern_id = "broken_symlink";
                f.severity = "medium";
                f.category = "traversal";
                f.file = rel;
                f.line = 0;
                f.match = "broken symlink";
                f.description = "broken or circular symlink";
                findings.push_back(std::move(f));
            }
            continue;
        }

        std::error_code sz_ec;
        auto size = fs::file_size(entry.path(), sz_ec);
        if (sz_ec) continue;
        total_size += size;

        if (size > static_cast<std::uintmax_t>(kMaxSingleFileKB) * 1024) {
            std::ostringstream oss;
            oss << (size / 1024) << "KB";
            Finding f;
            f.pattern_id = "oversized_file";
            f.severity = "medium";
            f.category = "structural";
            f.file = rel;
            f.line = 0;
            f.match = oss.str();
            std::ostringstream dm;
            dm << "file is " << (size / 1024) << "KB (limit: " << kMaxSingleFileKB << "KB)";
            f.description = dm.str();
            findings.push_back(std::move(f));
        }

        std::string ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto& sus = suspicious_binary_extensions();
        if (sus.find(ext) != sus.end()) {
            Finding f;
            f.pattern_id = "binary_file";
            f.severity = "critical";
            f.category = "structural";
            f.file = rel;
            f.line = 0;
            f.match = "binary: " + ext;
            f.description = "binary/executable file (" + ext + ") should not be in a skill";
            findings.push_back(std::move(f));
        }

        // Executable bit on non-script files.
        std::error_code perm_ec;
        auto status = fs::status(entry.path(), perm_ec);
        bool is_script_ext = (ext == ".sh" || ext == ".bash" || ext == ".py"
                              || ext == ".rb" || ext == ".pl");
        if (!perm_ec && !is_script_ext) {
            auto perms = status.permissions();
            auto any_exec = (perms & (fs::perms::owner_exec | fs::perms::group_exec
                                      | fs::perms::others_exec));
            if (any_exec != fs::perms::none) {
                Finding f;
                f.pattern_id = "unexpected_executable";
                f.severity = "medium";
                f.category = "structural";
                f.file = rel;
                f.line = 0;
                f.match = "executable bit set";
                f.description = "file has executable permission but is not a recognized script type";
                findings.push_back(std::move(f));
            }
        }
    }

    if (file_count > kMaxFileCount) {
        std::ostringstream oss;
        oss << file_count << " files";
        Finding f;
        f.pattern_id = "too_many_files";
        f.severity = "medium";
        f.category = "structural";
        f.file = "(directory)";
        f.line = 0;
        f.match = oss.str();
        std::ostringstream dm;
        dm << "skill has " << file_count << " files (limit: " << kMaxFileCount << ")";
        f.description = dm.str();
        findings.push_back(std::move(f));
    }

    if (total_size > static_cast<std::uintmax_t>(kMaxTotalSizeKB) * 1024) {
        std::ostringstream oss;
        oss << (total_size / 1024) << "KB total";
        Finding f;
        f.pattern_id = "oversized_skill";
        f.severity = "high";
        f.category = "structural";
        f.file = "(directory)";
        f.line = 0;
        f.match = oss.str();
        std::ostringstream dm;
        dm << "skill is " << (total_size / 1024) << "KB total (limit: "
           << kMaxTotalSizeKB << "KB)";
        f.description = dm.str();
        findings.push_back(std::move(f));
    }

    return findings;
}

// ---------------------------------------------------------------------------
// Top-level scan
// ---------------------------------------------------------------------------

namespace {

std::string build_summary(const std::string& name, const std::string& /*source*/,
                          const std::string& /*trust*/, const std::string& verdict,
                          const std::vector<Finding>& findings) {
    if (findings.empty()) {
        return name + ": clean scan, no threats detected";
    }
    std::set<std::string> cats;
    for (const auto& f : findings) cats.insert(f.category);
    std::ostringstream oss;
    oss << name << ": " << verdict << " — " << findings.size() << " finding(s) in ";
    bool first = true;
    for (const auto& c : cats) {
        if (!first) oss << ", ";
        oss << c;
        first = false;
    }
    return oss.str();
}

}  // namespace

ScanResult scan_skill(const fs::path& skill_path, std::string_view source) {
    ScanResult result;
    result.skill_name = skill_path.filename().string();
    result.source = std::string(source);
    result.trust_level = resolve_trust_level(source);

    std::error_code ec;
    if (fs::is_directory(skill_path, ec)) {
        auto sf = check_structure(skill_path);
        result.findings.insert(result.findings.end(), sf.begin(), sf.end());

        for (auto it = fs::recursive_directory_iterator(
                 skill_path, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code lec;
            if (fs::is_regular_file(it->path(), lec)) {
                std::string rel;
                try { rel = fs::relative(it->path(), skill_path).string(); }
                catch (...) { rel = it->path().filename().string(); }
                auto ff = scan_file(it->path(), rel);
                result.findings.insert(result.findings.end(), ff.begin(), ff.end());
            }
        }
    } else if (fs::is_regular_file(skill_path, ec)) {
        auto ff = scan_file(skill_path, skill_path.filename().string());
        result.findings.insert(result.findings.end(), ff.begin(), ff.end());
    }

    result.verdict = determine_verdict(result.findings);
    result.scanned_at = iso8601_utc_now();
    result.summary = build_summary(result.skill_name, result.source,
                                    result.trust_level, result.verdict,
                                    result.findings);
    return result;
}

// ---------------------------------------------------------------------------
// Install policy
// ---------------------------------------------------------------------------

InstallVerdict should_allow_install(const ScanResult& result, bool force) {
    InstallVerdict v;
    auto decision = policy_decision(result.trust_level, result.verdict);
    auto findings_count = std::to_string(result.findings.size());

    if (decision == "allow") {
        v.decision = InstallDecision::Allow;
        v.reason = "Allowed (" + result.trust_level + " source, "
                   + result.verdict + " verdict)";
        return v;
    }
    if (force) {
        v.decision = InstallDecision::Allow;
        v.reason = "Force-installed despite " + result.verdict + " verdict ("
                   + findings_count + " findings)";
        return v;
    }
    if (decision == "ask") {
        v.decision = InstallDecision::NeedsConfirmation;
        v.reason = "Requires confirmation (" + result.trust_level
                   + " source + " + result.verdict + " verdict, "
                   + findings_count + " findings)";
        return v;
    }
    v.decision = InstallDecision::Block;
    v.reason = "Blocked (" + result.trust_level + " source + "
               + result.verdict + " verdict, " + findings_count
               + " findings). Use --force to override.";
    return v;
}

// ---------------------------------------------------------------------------
// Human-readable report
// ---------------------------------------------------------------------------

std::string format_scan_report(const ScanResult& result) {
    std::ostringstream oss;
    oss << "Scan: " << result.skill_name
        << " (" << result.source << "/" << result.trust_level << ")  "
        << "Verdict: " << to_upper(result.verdict);

    if (!result.findings.empty()) {
        // Sort: critical, high, medium, low.
        auto sorted = result.findings;
        auto severity_rank = [](const std::string& s) {
            if (s == "critical") return 0;
            if (s == "high") return 1;
            if (s == "medium") return 2;
            if (s == "low") return 3;
            return 4;
        };
        std::stable_sort(sorted.begin(), sorted.end(),
                         [&](const Finding& a, const Finding& b) {
                             return severity_rank(a.severity)
                                  < severity_rank(b.severity);
                         });
        oss << "\n";
        for (const auto& f : sorted) {
            std::string loc = f.file + ":" + std::to_string(f.line);
            std::string match = f.match.size() > 60 ? f.match.substr(0, 60) : f.match;
            oss << "  " << right_pad(to_upper(f.severity), 8) << " "
                << right_pad(f.category, 14) << " "
                << right_pad(loc, 30) << " \"" << match << "\"\n";
        }
    } else {
        oss << "\n";
    }

    auto iv = should_allow_install(result);
    std::string status;
    switch (iv.decision) {
        case InstallDecision::Allow: status = "ALLOWED"; break;
        case InstallDecision::NeedsConfirmation: status = "NEEDS CONFIRMATION"; break;
        case InstallDecision::Block: status = "BLOCKED"; break;
    }
    oss << "Decision: " << status << " — " << iv.reason;
    return oss.str();
}

// ---------------------------------------------------------------------------
// Content hash
// ---------------------------------------------------------------------------

namespace {

// Minimal SHA-256 implementation — avoids pulling in openssl for a single
// call site.  Based on the RFC 6234 reference; operates on byte spans.
struct Sha256 {
    std::uint32_t h[8];
    std::uint64_t len_bits = 0;
    std::uint8_t buf[64];
    std::size_t buf_len = 0;

    Sha256() {
        static const std::uint32_t iv[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
        };
        std::copy(std::begin(iv), std::end(iv), std::begin(h));
    }

    static std::uint32_t rotr(std::uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    void compress(const std::uint8_t block[64]) {
        static const std::uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
        };
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(block[i * 4]) << 24)
                 | (std::uint32_t(block[i * 4 + 1]) << 16)
                 | (std::uint32_t(block[i * 4 + 2]) << 8)
                 |  std::uint32_t(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto a = h[0], b = h[1], c = h[2], d = h[3];
        auto e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            auto S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            auto ch = (e & f) ^ (~e & g);
            auto t1 = hh + S1 + ch + k[i] + w[i];
            auto S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            auto mj = (a & b) ^ (a & c) ^ (b & c);
            auto t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const std::uint8_t* data, std::size_t n) {
        len_bits += std::uint64_t(n) * 8;
        while (n > 0) {
            auto take = std::min(n, std::size_t(64 - buf_len));
            std::memcpy(buf + buf_len, data, take);
            buf_len += take;
            data += take;
            n -= take;
            if (buf_len == 64) {
                compress(buf);
                buf_len = 0;
            }
        }
    }

    std::string finalize() {
        // Append 0x80 then pad with zeros.
        buf[buf_len++] = 0x80;
        if (buf_len > 56) {
            std::memset(buf + buf_len, 0, 64 - buf_len);
            compress(buf);
            buf_len = 0;
        }
        std::memset(buf + buf_len, 0, 56 - buf_len);
        // Length in bits, big-endian 64-bit.
        for (int i = 0; i < 8; ++i) {
            buf[56 + i] = static_cast<std::uint8_t>(len_bits >> (56 - 8 * i));
        }
        compress(buf);

        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (int i = 0; i < 8; ++i) oss << std::setw(8) << h[i];
        return oss.str();
    }
};

}  // namespace

std::string content_hash(const fs::path& skill_path) {
    Sha256 s;
    std::error_code ec;
    auto feed = [&](const fs::path& file) {
        std::ifstream ifs(file, std::ios::binary);
        if (!ifs) return;
        char buf[8192];
        while (ifs) {
            ifs.read(buf, sizeof(buf));
            auto got = ifs.gcount();
            if (got > 0) {
                s.update(reinterpret_cast<std::uint8_t*>(buf),
                          static_cast<std::size_t>(got));
            }
        }
    };
    if (fs::is_directory(skill_path, ec)) {
        std::vector<fs::path> files;
        for (auto it = fs::recursive_directory_iterator(
                 skill_path, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code lec;
            if (fs::is_regular_file(it->path(), lec)) files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) feed(f);
    } else if (fs::is_regular_file(skill_path, ec)) {
        feed(skill_path);
    }
    auto hex = s.finalize();
    return "sha256:" + hex.substr(0, 16);
}

}  // namespace hermes::skills::guard
