# Script runtime

Link `opent1_console` and include `<opent1/console/console.hpp>`. Each `Console`
owns its scopes, command registry, function ASTs, tags and log. Independent
instances can run concurrently. Use external locking around a shared console
and any resource manager shared between consoles;
same-thread reentrant calls are supported. A supplied `ResourceManager` and
references captured by variable bindings must outlive their use by the console.
Coordinate filesystem outputs separately when instances use the same paths.

## Host integration

```cpp
#include <opent1/console/console.hpp>
#include <opent1/core/res_manager.hpp>
#include <array>

opent1::ResourceManager resources;
const std::array roots{std::filesystem::path("base/scripts")};
if (!resources.setSearchPaths(roots)) { /* report missing script roots */ }
opent1::Console console(&resources);

opent1::Int32 score = 0;
console.bindVariable("Game::score", score);
console.setOutput([](std::string_view line) { /* send to engine console */ });
console.addCommand(100, "Game::award",
    [](opent1::Console& c, opent1::Int32 id,
       std::span<const std::string> argv) -> std::optional<std::string> {
        // argv[0] is the registered command name. Validate engine arguments here.
        if (id != 100 || argv.size() != 2) { return "False"; }
        c.setVariable("Game::score", argv[1]);
        return "True";
    });

auto result = console.evaluate(R"(
    function award(%amount) { return Game::award(%amount); }
    award(10);
    return $Game::score;
)", "startup.cs");
// result.value == "10", score == 10
for (const auto& diagnostic : result.diagnostics) {
    // diagnostic.format() includes source, line and column.
}
auto loaded = console.executeFile("startup"); // supplies .cs when needed
const std::array<std::string, 1> arguments{"20"};
auto called = console.execute("award", arguments);
```

## Scopes, callbacks, and objects

`eval` executes in the current scope. Function calls create isolated local `%`
scopes; `$` variables are global. Top-level `%` values persist in the root scope.
Missing variables read as empty strings. Empty assignments remove ordinary
variables; registered bindings remain. Function definitions and native commands
replace each other by ASCII-insensitive name. Functions retain their parsed
program, including after their loading script returns or is replaced.

Callbacks receive stable owned strings, including the receiver as the first
argument after the command name for `dot_op_<method>`. Returning `nullopt` gives
`False` to script callers and `True` to direct host callers, preserving legacy.
Custom getter/setter bindings can adapt additional engine data types and change
notifications. Names and string values stop at NUL. Host operations that exceed
the value limit throw `std::length_error`; execution failures return diagnostics.

Install an owned `ConsoleDataManager` with `setDataManager` to connect datablock
declarations, `instant` objects and field access/assignment to the engine. The
default adapter is inert. The runtime balances object begin/end calls on failure.
It preserves legacy's unused second field index and reversed index selection for
`object.field[a][b]`. Values in field assignments arrive in declaration order.

## Script loading and execution limits

`executeFile` reads through the resource manager, including mounted archives.
`setScriptLoader` supplies an alternate loader returning optional source text.
Successful loads return `True` regardless of a script's return expression;
missing/empty files return `False` and log the filename. Parse failures return
`False` with diagnostics and execute no partial script. Unknown commands log a
message and return `False`. Check both `ExecutionResult::ok()` and `.value` when
the calling command uses boolean success strings.

`ExecutionLimits` controls parsing, steps, values, arguments and depth. Execution
steps, value bytes and recursion depth are unmetered by default, matching the
legacy interpreter. Hosts can opt into finite limits, shared across nested
callbacks, eval and exec. Native stack headroom is checked before recursive
execution, and malformed cyclic AST nodes are rejected independently of budgets.
Parser structural limits remain in force. Commands allow 511 arguments and field
lists allow 127 values. A failed invocation unwinds frames and object declarations;
mutations already completed before a runtime error remain. Subsequent invocations
receive a fresh budget and empty result. Native callbacks must bound their own
work; the interpreter cannot interrupt a callback that does not return.

The runtime keeps the legacy numeric/string coercions, comparison rules,
six-significant-digit numbers, postfix results, top-level flow behavior, and
truncating `floor`. It defines integer edge cases without C++ overflow or invalid
shifts. Evaluation state and string results belong to the console instance.
The console regression cases verify script results and engine events.

## Output and filesystem access

`setOutput` installs an output sink. `subscribeOutput` adds a subscription whose
returned token controls its lifetime; it is independent of the main sink.
`$Console::logBufferEnabled` controls the in-memory log and
`$Console::printLevel` controls output filtering. The built-in `cls` command
clears the in-memory log.

File logging is implemented by `opent1_console` itself. `$Console::logMode = 0`
disables it, mode `1` opens and closes `console.log` for each line, and mode `2`
keeps the file open. Both enabled modes append in the process working directory.
The `export` and `exportFunctions` built-ins can also write caller-supplied
filesystem paths; without a filename they print the selected entries.

Execution limits bound interpreter work, not filesystem permissions or native
command capabilities. Loading scripts through a resource manager does not
sandbox the commands they execute. Hosts must choose the commands and bindings
they expose. Platform quit/debug commands and interactive completion are supplied
by engine/UI adapters. The game's console and Telnet service can execute these
host commands; see [runtime services](runtime.md#network-and-console-services).
