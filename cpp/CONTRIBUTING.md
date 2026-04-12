# Contributing to Hermes C++ Backend

## Setup
```bash
cmake --preset default && cmake --build build/default -j && ctest --test-dir build/default
```

## Code Style
- C++17, `-Wall -Wextra -Wpedantic -Werror`
- clang-format (LLVM base, 4-space indent, 100 col)
- No comments unless the WHY is non-obvious

## Adding a Tool
1. Create `cpp/tools/src/<tool>.cpp` + `cpp/tools/include/hermes/tools/<tool>.hpp`
2. Register with `ToolRegistry::instance().register_tool({...})`
3. Add to `cpp/tools/CMakeLists.txt`
4. Write tests in `cpp/tools/tests/test_<tool>.cpp`
5. All handlers return JSON strings

## Adding a Platform Adapter
1. Create `cpp/gateway/platforms/<platform>.hpp/.cpp`
2. Inherit `BasePlatformAdapter`
3. Register in `adapter_factory.cpp`
4. Add to `cpp/gateway/CMakeLists.txt`

## Key Invariants
- Never hardcode `~/.hermes` — use `get_hermes_home()`
- Prompt cache: never mutate past messages except via ContextCompressor
- Tool handlers MUST return JSON strings
- All state paths via `get_hermes_home()` for profile safety
