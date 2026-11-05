// Original OpenT1 example. Run with exec("score.cs");
// Globals belong to this console; these scores are not network replication.
$Example::score["Alice"] = 0;

function Example::award(%name, %points)
{
    if (%points < 0)
        return $Example::score[%name];

    $Example::score[%name] += %points;
    echo(%name @ " score: " @ $Example::score[%name]);
    return $Example::score[%name];
}

Example::award("Alice", 5);
return Example::award("Alice", 3);
