// Hermes Agent — JetBrains service that owns the `hermes acp` child process.
//
// Spawns the binary, speaks the Agent Client Protocol over stdio using
// LSP-style Content-Length framing, and exposes a small API to the rest of
// the plugin (start, stop, initialize, authenticate, call).
//
// This is scaffolding: wiring into UI (HermesToolWindowFactory) and message
// listeners (async notifications) is left as a TODO.
package com.hermes.agent

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.components.Service
import com.intellij.openapi.diagnostic.Logger
import kotlinx.coroutines.CompletableDeferred
import java.io.BufferedInputStream
import java.io.OutputStream
import java.nio.charset.StandardCharsets
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger

@Service(Service.Level.APP)
class HermesAgentService {
    private val log = Logger.getInstance(HermesAgentService::class.java)

    @Volatile private var process: Process? = null
    @Volatile private var stdin: OutputStream? = null
    @Volatile private var readerThread: Thread? = null

    private val nextId = AtomicInteger(1)
    private val pending = ConcurrentHashMap<Int, CompletableDeferred<Map<String, Any?>>>()

    data class Config(
        val executable: String = "hermes",
        val args: List<String> = listOf("acp"),
        val authMethod: String = "api-key",
        val apiKeyEnvVar: String = "ANTHROPIC_API_KEY",
    )

    @Synchronized
    fun start(config: Config = Config()) {
        if (process != null) return
        log.info("starting hermes acp: ${config.executable} ${config.args.joinToString(" ")}")
        val pb = ProcessBuilder(listOf(config.executable) + config.args)
            .redirectErrorStream(false)
        val p = pb.start()
        process = p
        stdin = p.outputStream
        startReader(BufferedInputStream(p.inputStream))

        // Initialize + authenticate off the EDT.
        ApplicationManager.getApplication().executeOnPooledThread {
            try {
                call("initialize", mapOf(
                    "protocol_version" to 1,
                    "client_info" to mapOf("name" to "jetbrains-hermes", "version" to "0.1.0"),
                ))
                val params = mutableMapOf<String, Any>()
                if (config.authMethod == "api-key") {
                    System.getenv(config.apiKeyEnvVar)?.let { params["api_key"] = it }
                }
                call("authenticate", mapOf("method_id" to config.authMethod, "params" to params))
            } catch (t: Throwable) {
                log.warn("initialize/authenticate failed", t)
            }
        }
    }

    @Synchronized
    fun stop() {
        process?.destroy()
        process = null
        stdin = null
        readerThread?.interrupt()
        readerThread = null
        pending.values.forEach { it.cancel() }
        pending.clear()
    }

    /** Blocking JSON-RPC call; returns the response envelope. */
    fun call(method: String, params: Map<String, Any?>): Map<String, Any?> {
        val id = nextId.getAndIncrement()
        val deferred = CompletableDeferred<Map<String, Any?>>()
        pending[id] = deferred
        val body = encodeJson(mapOf(
            "jsonrpc" to "2.0",
            "id" to id,
            "method" to method,
            "params" to params,
        ))
        val payload = body.toByteArray(StandardCharsets.UTF_8)
        val header = "Content-Length: ${payload.size}\r\n\r\n".toByteArray(StandardCharsets.US_ASCII)
        val out = stdin ?: throw IllegalStateException("hermes acp not running")
        synchronized(out) {
            out.write(header)
            out.write(payload)
            out.flush()
        }
        // TODO: replace polling loop with coroutine await.
        var tries = 0
        while (!deferred.isCompleted && tries < 600) {
            Thread.sleep(50)
            tries++
        }
        @Suppress("UNCHECKED_CAST")
        return if (deferred.isCompleted) deferred.getCompleted() else throw RuntimeException("timeout: $method")
    }

    private fun startReader(input: BufferedInputStream) {
        val t = Thread({
            try {
                while (!Thread.currentThread().isInterrupted) {
                    val header = readHeader(input) ?: break
                    val len = HEADER_RE.find(header)?.groupValues?.get(1)?.toInt() ?: continue
                    val body = ByteArray(len)
                    var off = 0
                    while (off < len) {
                        val n = input.read(body, off, len - off)
                        if (n < 0) return@Thread
                        off += n
                    }
                    handleMessage(String(body, StandardCharsets.UTF_8))
                }
            } catch (t: Throwable) {
                log.info("hermes acp reader exited: ${t.message}")
            }
        }, "hermes-acp-reader")
        t.isDaemon = true
        t.start()
        readerThread = t
    }

    private fun readHeader(input: BufferedInputStream): String? {
        val sb = StringBuilder()
        var prev = 0
        while (true) {
            val b = input.read()
            if (b < 0) return null
            sb.append(b.toChar())
            // Terminator is \r\n\r\n.
            if (sb.length >= 4 &&
                sb[sb.length - 4] == '\r' && sb[sb.length - 3] == '\n' &&
                sb[sb.length - 2] == '\r' && sb[sb.length - 1] == '\n') {
                return sb.toString()
            }
            prev = b
            if (sb.length > 4096) return null  // sanity cap
        }
    }

    private fun handleMessage(body: String) {
        // Minimal parser: pull out "id" and "result". Real plugin should
        // depend on an actual JSON library — deliberately avoided here to
        // keep this scaffold free of external deps. TODO: use kotlinx.serialization.
        val idMatch = Regex("""\"id\"\s*:\s*(\d+)""").find(body) ?: return
        val id = idMatch.groupValues[1].toInt()
        val d = pending.remove(id) ?: return
        d.complete(mapOf("raw" to body))
    }

    private fun encodeJson(obj: Any?): String = when (obj) {
        null -> "null"
        is Boolean -> obj.toString()
        is Number -> obj.toString()
        is String -> "\"" + obj.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n") + "\""
        is Map<*, *> -> obj.entries.joinToString(",", "{", "}") {
            encodeJson(it.key.toString()) + ":" + encodeJson(it.value)
        }
        is Iterable<*> -> obj.joinToString(",", "[", "]") { encodeJson(it) }
        else -> encodeJson(obj.toString())
    }

    companion object {
        private val HEADER_RE = Regex("Content-Length:\\s*(\\d+)", RegexOption.IGNORE_CASE)
    }
}
