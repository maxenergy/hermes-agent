// Hermes Agent VS Code extension — ACP client.
//
// This extension spawns `hermes acp` as a child process and speaks the Agent
// Client Protocol over stdio. It's deliberately minimal: the heavy lifting
// happens in the hermes binary, and we just shuttle JSON-RPC messages.
//
// Compile with:   npx tsc -p .
//
// Not published to the marketplace yet — see README.
import * as cp from "child_process";
import * as vscode from "vscode";

// A trivial JSON-RPC framer for ACP over stdio using Content-Length headers
// (same framing as LSP).
class JsonRpcTransport {
    private buffer = Buffer.alloc(0);
    private nextId = 1;
    private pending = new Map<number, (res: any) => void>();

    constructor(private proc: cp.ChildProcess) {
        proc.stdout?.on("data", (chunk: Buffer) => {
            this.buffer = Buffer.concat([this.buffer, chunk]);
            this.drain();
        });
    }

    private drain(): void {
        while (true) {
            const headerEnd = this.buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0) return;
            const header = this.buffer.slice(0, headerEnd).toString("utf8");
            const m = /Content-Length:\s*(\d+)/i.exec(header);
            if (!m) {
                this.buffer = this.buffer.slice(headerEnd + 4);
                continue;
            }
            const len = parseInt(m[1], 10);
            const bodyStart = headerEnd + 4;
            if (this.buffer.length < bodyStart + len) return;
            const body = this.buffer.slice(bodyStart, bodyStart + len).toString("utf8");
            this.buffer = this.buffer.slice(bodyStart + len);
            try {
                const msg = JSON.parse(body);
                if (typeof msg.id === "number" && this.pending.has(msg.id)) {
                    const resolve = this.pending.get(msg.id)!;
                    this.pending.delete(msg.id);
                    resolve(msg);
                }
            } catch (err) {
                console.error("hermes acp: bad JSON", err);
            }
        }
    }

    call(method: string, params: any = {}): Promise<any> {
        return new Promise((resolve, reject) => {
            const id = this.nextId++;
            this.pending.set(id, resolve);
            const payload = JSON.stringify({ jsonrpc: "2.0", id, method, params });
            const header = `Content-Length: ${Buffer.byteLength(payload, "utf8")}\r\n\r\n`;
            if (!this.proc.stdin?.write(header + payload)) {
                this.pending.delete(id);
                reject(new Error("failed to write to hermes acp stdin"));
            }
        });
    }
}

let transport: JsonRpcTransport | undefined;
let child: cp.ChildProcess | undefined;

async function startHermes(context: vscode.ExtensionContext): Promise<void> {
    if (child) return;
    const cfg = vscode.workspace.getConfiguration("hermes");
    const exe = cfg.get<string>("executable", "hermes");
    const args = cfg.get<string[]>("acpArgs", ["acp"]);

    const env = { ...process.env };
    // The binary will auto-detect ANTHROPIC_API_KEY etc., so no extra work here.

    child = cp.spawn(exe, args, {
        stdio: ["pipe", "pipe", "pipe"],
        env,
    });
    child.on("exit", (code) => {
        vscode.window.showWarningMessage(`hermes acp exited (code=${code})`);
        child = undefined;
        transport = undefined;
    });
    child.stderr?.on("data", (buf: Buffer) => {
        console.log("[hermes acp]", buf.toString("utf8"));
    });

    transport = new JsonRpcTransport(child);

    // 1. initialize
    const init = await transport.call("initialize", {
        protocol_version: 1,
        client_info: { name: "vscode-hermes", version: "0.1.0" },
    });
    console.log("hermes initialize:", init);

    // 2. authenticate (per configured method).
    const method = cfg.get<string>("authMethod", "api-key");
    const envVar = cfg.get<string>("apiKeyEnvVar", "ANTHROPIC_API_KEY");
    const params: any = {};
    if (method === "api-key") {
        const k = process.env[envVar];
        if (k) params.api_key = k;
    }
    const authResp = await transport.call("authenticate", { method_id: method, params });
    console.log("hermes authenticate:", authResp);

    vscode.window.showInformationMessage("Hermes Agent started.");
}

async function stopHermes(): Promise<void> {
    if (!child) return;
    child.kill();
    child = undefined;
    transport = undefined;
    vscode.window.showInformationMessage("Hermes Agent stopped.");
}

export function activate(context: vscode.ExtensionContext): void {
    context.subscriptions.push(
        vscode.commands.registerCommand("hermes.start", () => startHermes(context)),
        vscode.commands.registerCommand("hermes.stop", () => stopHermes()),
        vscode.commands.registerCommand("hermes.authenticate", async () => {
            if (!transport) {
                vscode.window.showWarningMessage("Run 'Hermes: Start Agent' first.");
                return;
            }
            const method = await vscode.window.showQuickPick(["api-key", "oauth"], {
                placeHolder: "Select ACP auth method",
            });
            if (!method) return;
            const token = await vscode.window.showInputBox({
                prompt: method === "api-key" ? "API key" : "OAuth access_token",
                password: true,
            });
            if (!token) return;
            const params = method === "api-key" ? { api_key: token } : { access_token: token };
            const r = await transport.call("authenticate", { method_id: method, params });
            vscode.window.showInformationMessage(`hermes authenticate: ${JSON.stringify(r)}`);
        }),
    );
}

export function deactivate(): Thenable<void> | undefined {
    return stopHermes();
}
