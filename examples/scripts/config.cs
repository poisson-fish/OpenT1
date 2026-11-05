// Original OpenT1 example. Load this before the functions that consume it.
$Example::maximum = 25;
$Example::enabled = true;

function Example::clamp(%amount)
{
    if (!$Example::enabled || %amount < 0)
        return 0;
    if (%amount > $Example::maximum)
        return $Example::maximum;
    return %amount;
}

return Example::clamp(100);
