# Hermes Agent C++ Architecture

This document describes the C++17 backend architecture of Hermes Agent using
Mermaid diagrams. All components listed below live under `cpp/` and are built
by the top-level `cpp/CMakeLists.txt`.

## Component Overview

```mermaid
graph TB
    CLI[hermes_cpp CLI] --> Agent[AIAgent]
    Gateway[Gateway Runner] --> Agent
    MCPServer[MCP Server] --> Agent

    Agent --> LLM[LLM Clients]
    Agent --> Tools[Tool Registry]
    Agent --> Context[Context Compressor]
    Agent --> Memory[Memory Manager]

    Tools --> |49 tools| ToolImpls[file/web/browser/...]
    LLM --> |OpenAI/Anthropic/OpenRouter| HttpTransport[CurlTransport]
    Gateway --> |18 adapters| Platforms[Telegram/Discord/Slack/...]

    Agent --> SessionDB[(SessionDB FTS5)]
    Gateway --> SessionStore[(Session Store)]
```

## Agent Loop Sequence

```mermaid
sequenceDiagram
    User->>Agent: message
    Agent->>PromptBuilder: build system prompt
    PromptBuilder-->>Agent: prompt
    loop until no tool_calls or budget exhausted
        Agent->>LLM: complete(messages)
        LLM-->>Agent: response
        alt has tool_calls
            Agent->>Tools: dispatch(name, args)
            Tools-->>Agent: result
        end
    end
    Agent->>SessionDB: save messages
    Agent-->>User: final response
```

## Tool Dispatch

```mermaid
flowchart LR
    Agent -->|tool_call| Registry
    Registry -->|check check_fn| Handler
    Handler --> Dispatch{Dispatch}
    Dispatch -->|local| LocalExec[LocalEnvironment]
    Dispatch -->|HTTP| CurlTransport
    Dispatch -->|browser| CdpBackend
    Dispatch -->|MCP| McpTransport
    Handler -->|JSON result| Registry
    Registry -->|truncate if > max| Agent
```

## Gateway Architecture

```mermaid
graph TB
    Runner[Gateway Runner] --> Poller[Adapter Pollers]
    Poller --> Telegram
    Poller --> Discord
    Poller --> Slack
    Poller --> Matrix
    Poller --> Other[... 14 more]

    Telegram --> Normalizer[Message Normalizer]
    Discord --> Normalizer
    Slack --> Normalizer
    Matrix --> Normalizer
    Other --> Normalizer

    Normalizer --> Router[Session Router]
    Router --> Store[(SessionStore)]
    Router --> Agent[AIAgent]
    Agent --> Hooks[Gateway Hooks]
    Hooks --> Pairing[Pairing Manager]
    Hooks --> Replies[Reply Dispatcher]
    Replies --> Telegram
    Replies --> Discord
    Replies --> Slack
```

## Memory and Context Flow

```mermaid
flowchart TD
    UserTurn[User Turn] --> History[Message History]
    History --> Budget{Token Budget?}
    Budget -->|over| Compressor[Context Compressor]
    Compressor --> Summary[Summary Block]
    Summary --> History
    Budget -->|under| Prompt[Prompt Builder]
    History --> Prompt
    MemStore[(MemoryStore)] --> MemRecall[Memory Recall]
    MemRecall --> Prompt
    Prompt --> LLM
    LLM --> ToolLoop[Tool Loop]
    ToolLoop --> MemWrite[Memory Writer]
    MemWrite --> MemStore
```

## Build Dependency Graph

```mermaid
graph LR
    core --> approval
    core --> config
    core --> profile
    core --> state
    core --> auth
    config --> profile
    core --> external
    external --> llm
    external --> tools
    core --> environments
    llm --> agent
    tools --> agent
    state --> agent
    environments --> agent
    skills --> agent
    agent --> cli
    agent --> gateway
    agent --> mcp_server
    agent --> batch
    agent --> acp
    cron --> gateway
    plugins --> agent
```

See also `module-dependency.md` for a text listing of each library target and
its direct CMake link dependencies.
