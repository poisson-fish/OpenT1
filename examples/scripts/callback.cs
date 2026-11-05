// Original OpenT1 example. A small hook owned by this example.
// The engine does not invoke Example::onAward automatically.
function Example::onAward(%name, %amount)
{
    $Example::lastAward = %name @ ":" @ %amount;
}

function Example::grant(%name, %amount)
{
    for (%i = 0; %i < %amount; %i++)
        $Example::total[%name]++;
    Example::onAward(%name, %amount);
    return $Example::lastAward;
}

return Example::grant("Alice", 3);
