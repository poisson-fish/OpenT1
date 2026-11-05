# Script examples

These scripts are original project examples covered by the repository license.
They require only the console runtime, with no game data or live server. Copy a
script into an active script search directory in your own mod, then evaluate
`exec("score.cs");`, `exec("config.cs");`, or `exec("callback.cs");` in the console.
See [the scripting guide](https://github.com/poisson-fish/OpenT1/wiki/Scripting-examples).

`score.cs` demonstrates namespaces, globals indexed by player name, local
arguments, validation and return values. Its final expression returns `8`.
`config.cs` demonstrates configurable policy and returns `25`.
`callback.cs` defines and invokes its own hook and returns `Alice:3`.
`exec` itself returns `True` on a successful load; that is separate from these
top-level return values, which the native regression test inspects directly.

These examples do not replace game callbacks, install a mission, or replicate
their globals between server and client. Rename the Example namespace before
using the pattern in a distributed mod.
