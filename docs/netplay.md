# Multi-machine netplay

Several computers on the same network, one game between them. There is
nothing to configure: no IP address to type, no host and client to decide in
advance, no port to forward, and no firewall rule to add.

## Playing

1. Open MANX on every computer **without a ROM on the command line** - the
   plain launcher, not a direct game launch. Discovery only runs from the
   launcher, because that is the only place a game has not been chosen yet.
2. Choose **Network Play**. Each machine lists the others it can see, by
   name, within a couple of seconds.
3. On the machine that wants to drive, pick a machine to ask - or **Ask
   Everyone**. That machine becomes the host.
4. Everyone asked gets an invitation on screen, wherever they are in the
   launcher, and answers **Allow** or **Deny**. Nothing starts on anybody's
   machine until they have agreed.
5. The host then picks how to play and which game, and every machine that
   accepted starts it together.

Only games installed on **every** machine taking part are offered, so a pick
can never leave one of them with nothing to load.

## The lobby screen

The lobby is one screen for every stage of getting a game going, drawn as
cards so it reads from across a room and drives from a pad. It redraws
itself whenever anything on the network changes rather than waiting for a
keypress.

- **Searching** - while nothing has answered, it says so and keeps looking.
  Once it has tried every address on every network this machine is on, it
  stops saying "searching" and lists what to check instead (see below).
- **Invitation** - when another machine asks, the lobby shows who and offers
  **Allow** (join their game; they choose what everyone plays) or **Deny**
  (stay on this machine).
- **Host** - the machine that asked shows every machine's state: *Not
  asked*, *Asked - no answer yet*, *In as Player N*, *Accepted - no place
  left*, or *Declined*. Once two or more are in, the host picks how to play
  and the game.
- **Player N** - a machine that accepted but is not the host waits while the
  host chooses; it starts the game automatically.
- **Declined** - if nobody accepted, the lobby says so and offers to ask
  again or pick a different machine. Nothing was started anywhere.

Two controls are always available, wherever the negotiation has got to:

- **Do Not Disturb** - flips this machine's presence so others see it as
  *Not accepting invitations* and cannot ask it. Useful when a machine is
  being used for something else but still wants to be on the network. The
  lobby also shows a machine that is mid-game as *In a game* rather than
  letting you send an invitation into it.
- **Machine Logs** - every machine relays what it prints to every other,
  continuously, over the same link that found them. Pick a machine to read
  its console output from here. The lines a cabinet printed just before it
  stopped are already on the other machine by the time it goes quiet, which
  is exactly the output that is otherwise lost.

## How many machines

Up to eight, but how many actually play depends on what you choose:

- **Arcade System Link** is the original cabinet-to-cabinet network the
  boards themselves implement - a ring of up to eight. Every machine that
  accepted joins it.
- **Take Turns** and **Play Together** are MANX's own lockstep netplay,
  which shares one emulated board between two players. That is two machines
  by nature; the host plus the first machine to accept take part.

The host assigns each machine its player number from the roster, so with
more than two machines each one starts as the cabinet it was actually
assigned rather than everybody assuming they are Player 2.

## Leaving a session

Closing the game on any one machine brings them all back to the lobby
together. The host withdrawing, or a cabinet dropping out, dissolves the
session and every machine sees it end and returns to the lobby rather than
being left stranded on a black screen.

## What it does under the covers

Every machine transmits on UDP 35109 while in the launcher, then on
35112 upwards once a game is running - one port per cabinet. Every packet is
sent three ways at once:

- to the loopback address, so two MANX instances on one computer link up
  exactly as two computers do;
- to the broadcast address of each network interface, which is the whole of
  discovery on a network that is not filtering anything;
- to each address on the subnet in turn, paced so a /24 is covered in about
  a second.

That last one is what makes the zero-setup promise hold. A desktop firewall
in its default state drops an arriving broadcast, so two machines relying on
broadcast alone both transmit and both stay deaf. Every one of those
firewalls does track outbound conversations, though, and lets the reply to
one back in. When both machines sweep the subnet, each one's own outbound
packet is what opens the door for the other's - they meet in the middle,
with no rule added anywhere.

Once a machine answers, the sweep stops and the link becomes a plain unicast
conversation with each machine found. The two native cabinet-link transports
- the Model 2 comm ring and the System 22 C139 board - do exactly the same
thing, so System Link across real machines works without a firewall rule
either. The launcher also hands the address it
found to the game it starts (`MANX_INPUT_PEER_HOST`), so the running
cabinet continues the conversation the launcher opened rather than starting
a fresh search.

Netplay itself is lockstep: both machines simulate the whole board from the
same inputs, and no video is sent between them. A board that keeps operator
data reports a hash of it before the first frame, so two machines with
different EEPROM settings are told they disagree instead of quietly
drifting apart.

## Platform notes

- **Linux** - works as shipped. Nothing to allow in `ufw` or `firewalld`.
- **Windows** - works as shipped. Windows may show its own
  "Allow MANX to communicate on these networks?" prompt the first time;
  answer yes on every computer. Ticking *Private networks* is enough.
- **Android** - the same code, and the same result: there is no inbound
  firewall to negotiate. Client isolation on the Wi-Fi access point is the
  one thing that will defeat it, since that blocks devices from addressing
  each other at all - see below.

## When it does not connect

The launcher stops saying "searching" once it has tried every address on
every network this machine is on, and says what to check instead. In order
of likelihood:

1. **MANX is not open on the other computer**, or it is open on a game
   rather than the launcher.
2. **The two are not on the same network.** A machine on Wi-Fi and a
   machine on a guest SSID are on different subnets and cannot reach each
   other whatever MANX does.
3. **Client isolation ("AP isolation") is on** at the access point. This
   stops wireless devices addressing each other and is the one failure MANX
   cannot work around. Turn it off, or put both machines on Ethernet.
4. **A firewall is denying outbound traffic too.** Rare on a desktop, but
   it defeats the return-path trick, because there is then no outbound
   conversation to attach the reply to.

`tools/lobby_probe.py` speaks the discovery protocol and can be run on
either machine, with or without MANX running:

```bash
python3 tools/lobby_probe.py              # search and answer, like MANX does
python3 tools/lobby_probe.py --listen-only # show what arrives, transmit nothing
```

It prints `PEER <address>` for every MANX or probe it hears from, which
separates "the network is not carrying our packets" from "MANX is not
running over there".

## Setting up a test machine

To push builds to another machine and read its logs back over SSH, run the
setup script once on it. It installs and starts the SSH server, opens port
22 in the firewall if one is active, and installs the development machine's
public key so builds can be pushed and logs read without a password prompt.
It is idempotent - a second run changes nothing.

```bash
# Linux / macOS
bash tools/setup-machine.sh

# Windows (run in an Administrator PowerShell)
powershell -ExecutionPolicy Bypass -File tools\setup-machine.ps1
```

Each prints the `user@address` lines the machine is reachable at. The
private half of the key never leaves the machine that made it; only the
public half is written to `~/.ssh/authorized_keys` (or
`administrators_authorized_keys` on Windows).
