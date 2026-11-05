# Scripting reference

OpenT1 implements the legacy Tribes console language used by `.cs` files. These
files are **not C#**, JavaScript, or a promise of compatibility with every later
TorqueScript feature. Installation scripts supply many game functions on top of
the engine's native commands. The [command index](Command-index.md) links native
registrations; the [examples](Scripting-examples.md) provide runnable starting points.

## Statements and values

Terminate assignments/calls/returns with `;`, and group statements with `{ }`.
`//` introduces a line comment; `/* ... */` introduces a block comment.
Values cross console/native boundaries as strings, with legacy numeric/boolean
coercion for expressions and command arguments.

```text
// A local calculation and a persistent console global.
%points = 3;
$Example::score = %points * 2;
echo("score: " @ $Example::score);
```

Use double quotes for strings and `@` or `$+` for concatenation. Parenthesize
arithmetic within concatenations for clarity. The language supports decimal,
hexadecimal and floating literals. Integer arithmetic uses 32-bit legacy rules;
decimal integer literals wrap to signed 32-bit values by default. Printed numeric
results use six significant digits. Avoid using floating results as exact IDs.

| Syntax | Meaning |
| --- | --- |
| `%local` | Function-local variable; top-level locals belong to the root scope |
| `$Example::value` | Global variable in the current console |
| `$Example::score[%name]` | Indexed variable; useful for keyed values |
| `+ - * / %` | Arithmetic |
| `== != < <= > >=` | Comparisons using legacy coercion rules |
| `! && \|\|` | Logical operators; `&&`/`\|\|` short-circuit |
| `& \| ^ ~ << >>` | Integer bit operations |
| `= += -= *= /= %=`, `++ --` | Assignments and increments |
| `@`, `$+` | String concatenation |

Missing variables read as empty strings. Assigning the empty string removes an
ordinary variable. Name lookup is ASCII case-insensitive. Function calls create
isolated local scopes; a called function does not inherit the caller's `%locals`.
`$` globals persist in the console, but do not replicate to another machine or
automatically persist across application restarts.

Comparison behavior depends on expression coercion. For an explicit,
case-sensitive lexical string comparison in the game, use
`String::Compare(%left, %right)` from the file plugin; it returns `-1`, `0`, or `1`.
Do not assume C++/JavaScript truth or equality behavior for arbitrary strings.

## Functions and control flow

```text
function Example::bounded(%amount)
{
    if (%amount < 0)
        return 0;
    else if (%amount > 25)
        return 25;
    return %amount;
}

for (%i = 0; %i < 3; %i++)
    echo(Example::bounded(%i));
```

The parser supports `if/else`, `while`, `for`, `break`, `continue`, and `return`.
Use these supported constructs instead of assuming a later-language feature
such as `package`, `Parent::`, or `switch` is implemented.

Function names can use a namespace (`Example::bounded`). Later functions/native
registrations with the same case-insensitive name replace the old entry. Keep
initialization order explicit. Arguments and results are console values; validate
values before passing them to native commands that expect object IDs or vectors.

## Loading, evaluation and output

| Call | Contract |
| --- | --- |
| `exec("file.cs")` | Load through current resource paths/archives; `.cs` can be omitted. Returns `True` on successful load, not the file's final return expression |
| `eval(%source)` | Evaluate source in the current scope; use sparingly and avoid untrusted constructed code |
| `echo(%text, ...)` | Write concatenated arguments to console output |
| `trace()` | Print the current script call chain |
| `cls()` | Clear the in-memory console log |
| `floor(%value)` | Legacy truncation toward zero, including negative values |
| `sqrt(%value)` | Square-root utility |

A missing/empty file or parse failure makes `exec` return `False`. Parse failures
execute no partial script. Unknown commands log a message and return `False`.
Successful parsing does not guarantee a game command can succeed: the relevant
world, object, datablock or connection may not exist yet.

`$Console::logMode = 1;` appends to `console.log` while opening/closing each line;
mode `2` keeps it open, and mode `0` disables file logging. The file root defaults
to the working directory. `export`/`exportFunctions` can write selected variables
or functions to a caller-supplied file. Scripts and native commands are not a
filesystem sandbox; run scripts you trust.

## Datablocks and objects

Legacy datablock declarations describe shared game data such as armor, weapons
and projectiles. The game adapter must recognize the declared class and its
fields. For example, the declaration shape is:

```text
PlayerData ExampleArmor
{
    // Supply the fields/assets required by the PlayerData implementation.
};
```

This illustrates syntax only; an empty armor definition is not a usable player.
Use your installation's working definitions as the compatibility reference and
check fields against native datablock code before adding a new definition.

`instant` declarations create simulation objects through the active data manager:

```text
instant SimGroup "ExampleGroup"
{
};
```

Objects belong to a simulation manager/world. Field reads/assignments use
`%object.field` and can include indices. Native `newObject`, `deleteObject`, group
and lookup commands provide additional lifecycle operations. Inspect their
implementations for exact signatures; ownership and cleanup matter across
mission changes. The standalone console's default data manager is inert, so
parsing an object declaration alone does not create game-world behavior.

## Game commands, callbacks and networking

Commands are installed by engine plugins, often only for a suitable server/client
context. Useful families include `Client::`, `Player::`, `GameBase::`, GUI/editor
commands, file/string helpers and simulation object commands. Start from the
[native index](Command-index.md) and follow the registration's argument checks.
This table highlights a few verified contracts:

| Call | Meaning |
| --- | --- |
| `Client::sendMessage(%client, %type, %text [, %sender])` | Server-side message delivery to a client; missing/disconnected targets can be silently ignored |
| `GameBase::getPosition(%object)` | Obtain a game object's position through its native binding |
| `remoteEval(%peer, %command, ...)` | Dispatch a remote console event; the receiving function is named `remote` followed by the command, with the sender supplied first |

An engine callback runs only where native code or a loaded script explicitly
invokes it. A function named `Example::onAward` is just a function until your mod
calls it. For existing callbacks, trace `console.execute`/`isFunction` call sites
and the installation scripts to establish the argument order, world, invocation
time, return contract and override behavior.

Delayed work (`schedule`) depends on the simulation event loop. It will not
advance merely by evaluating a standalone console script. Remote callbacks must
validate the sender, permissions, object ownership and bounds before changing
authoritative state. A client-supplied score or object ID is not authoritative.

## Limits and native integration

Native command arguments include the command name in `argv[0]`; script authors
do not pass that name as an extra argument. The runtime accepts up to 511
arguments and field lists up to 127 values, while individual commands may impose
smaller limits (`remoteEval`, for example). Parser structural limits remain in
force. Runtime step/value/depth budgets are opt-in for hosts, and native callbacks
must bound their own work. Runtime errors unwind execution but retain earlier
side effects.

For the C++ API, ownership, reentrancy, execution limits and custom data-manager
contracts, see
[`docs/script-runtime.md`](https://github.com/poisson-fish/OpenT1/blob/main/docs/script-runtime.md).
