# Scripting examples

The repository ships three original, complete examples in
[`examples/scripts`](https://github.com/poisson-fish/OpenT1/tree/main/examples/scripts).
Each is executed by the native console regression suite on every CI platform.
They need no external game assets. Their namespace is deliberately separate from
installation-defined game callbacks.

## Load an example in your mod

Copy `score.cs` into a directory already selected by your mod's resource search
path, then run:

```text
exec("score.cs");
Example::award("Alice", 2);
echo($Example::score["Alice"]);
```

The file starts Alice at zero and awards five plus three points. The extra call
above brings her score to ten. Re-executing the file resets this example's initial
score. `exec` returns `True` when it loads successfully; the function returns the
new score. These scores are console-local example data, not networked player state.

The important function is:

```text
function Example::award(%name, %points)
{
    if (%points < 0)
        return $Example::score[%name];

    $Example::score[%name] += %points;
    echo(%name @ " score: " @ $Example::score[%name]);
    return $Example::score[%name];
}
```

This demonstrates local parameters, a global indexed by name, validation,
concatenation and an explicit return. A real game mod should key authoritative
state using validated game identities and handle reconnect/mission cleanup.

## Separate configuration from behavior

`config.cs` defines `$Example::maximum` and `$Example::enabled`, then defines
`Example::clamp(%amount)`. It returns zero for negative/disabled requests and caps
positive requests at the configured maximum. Try:

```text
exec("config.cs");
echo(Example::clamp(100));  // 25
$Example::maximum = 10;
echo(Example::clamp(100));  // 10
$Example::enabled = false;
echo(Example::clamp(5));    // 0
```

Load configuration before code that consumes it. A `$` prefix gives console-wide
lifetime; persistence to disk still requires your installation's configuration
save path or explicit export.

## Define and invoke a hook

`callback.cs` defines `Example::onAward` and calls it from `Example::grant` after
updating an example counter. Its final result is `Alice:3`. The engine does not
invoke this hook automatically. This is a pattern for a mod-owned extension point,
not a replacement for the game's existing award/spawn/death callbacks.

When adapting the pattern to real callbacks, locate the actual invocation and
preserve its arguments and return semantics. Avoid loading a replacement that
silently removes initialization performed by the original function.

## Verify changes

For source contributors, configure/build normally and run `ctest --preset
x64-release -R console --output-on-failure` (use the corresponding Linux/macOS
preset on those hosts). The `console/documented_examples` case loads the exact
shipped `.cs` files, checks their results, and exercises negative/disabled inputs.
Add game integration coverage when an example begins using world objects,
networking, assets or timed callbacks; the pure console test cannot establish
those behaviors.
