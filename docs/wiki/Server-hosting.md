# Server hosting

The normal OpenT1 executable also runs a dedicated server. It still requires the
installation's game data and startup scripts; `--no-audio` prevents playback-device
initialization, and `--headless` prevents window/video/GPU initialization.

```powershell
.\opent1.exe --assets "C:\Games\Tribes" -dedicated Raindance --headless --no-audio
```

```sh
./opent1 --assets /srv/tribes -mod ExampleMod -dedicated ExampleMission --headless --no-audio
```

The installation's `console.cs` handles `-mod` and `-dedicated`. Put the mission
immediately after `-dedicated`; placing an option there can make that option be
interpreted as the mission name. Use the actual mission identifier supported by
the installation, without assuming the examples exist in a custom mod.

## Preferences and ports

Use the installation/mod's normal server-preference file for hostname, player
limit, port and public-hosting behavior. `$Server::Port` supplies the final UDP
game port after startup scripts load preferences. OpenT1 fails startup if it
cannot bind that port. A port in use, an invalid interface or firewall rule can
prevent hosting even when loading the mission succeeds.

Allow the selected UDP port through the host firewall/router as appropriate for
your network. Discovery/master-server configuration is separate from the game
transport. Verify a connection from another machine; a local host/join test alone
does not establish internet reachability or interoperability with vanilla clients.

## Master-server announcements

Hosted and dedicated servers send announcements when both
`$Server::HostPublicGame` and `$Server::Heartbeats` are true (both default to true).
Setting either preference to false stops announcements on the next network tick;
it does not stop the game listener or its ping/game-info replies.

`$Server::numMasters` selects the number of configured entries, from zero to 64,
in `$Server::MasterAddressN0`, `$Server::MasterAddressN1`, and so on. Zero disables
announcements. Each entry accepts an IPv4 address or hostname with an optional
`IP:` prefix and port; an omitted port defaults to 28000. The browser's
`display-name<TAB>address` form is also accepted. For example, in your own
server-preference file:

```text
$Server::HostPublicGame = true;
$Server::Heartbeats = true;
$Server::numMasters = 1;
$Server::MasterAddressN0 = "My master\tIP:master.example.org:28000";
```

Use an actual master you intend to announce to. OpenT1 does not replace or add to
the installation's master list. Empty or malformed entries are skipped, and
duplicate configured endpoints are announced to once. Hostnames resolve asynchronously;
failed lookups are retried after 120 seconds without blocking the game loop.

The first announcement uses the final bound game socket after startup preferences
have loaded. Further announcements use that same socket every 120 seconds.
Changing the enabled state or master destinations restarts announcements; unchanged
preferences preserve the interval. Recreating a server cancels its old DNS work
and starts the new listener's announcement lifecycle.

Loopback UDP regression tests cover announcement timing, preference changes,
hostname lookup, server recreation, source ports and ping/game-info replies. These
checks do not verify registration with a public master or discovery by an
independent internet client. Verify those separately with your chosen master,
firewall/router configuration and game installation.

## Console and operation

Headless/dedicated startup streams logs to stdout. Commands entered on stdin are
evaluated on the simulation thread between frames. Include the script semicolon:

```text
echo($Server::Port);
echo($Server::HostName);
quit();
```

Redirect stdout/stderr for a service, give it a writable working directory, and
configure graceful stop/restart in your service manager. EOF stops stdin reading
without stopping the server. Test mission changes, reconnects and restart recovery
with your actual mod. Keep preferences and saves backed up before upgrades.

## Remote console

The optional Telnet console starts without a listener. A positive `$TelnetPort`
starts one; zero or `-1` stops it. `$TelnetBindAddress` defaults to `127.0.0.1`, and
authentication requires a nonempty `$TelnetPassword`. It executes host console
commands and does not encrypt traffic. Prefer local administration or an encrypted
tunnel; keep passwords and private administrative endpoints out of logs and bug
reports. This service is separate from the UDP game port.

For complete options and network behavior, see
[runtime](https://github.com/poisson-fish/OpenT1/blob/main/docs/runtime.md) and
[simulation networking](https://github.com/poisson-fish/OpenT1/blob/main/docs/simulation-networking.md).
