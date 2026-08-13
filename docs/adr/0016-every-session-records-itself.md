# ADR 0016 — Every session records itself

**Status:** accepted

## Context

Nine of the bugs fixed so far were found by playing, not by reading, and each was reachable only once
the thing before it worked. That makes a tester's session the project's most productive input, and a
session nobody can replay close to worthless: without the inputs and the state they started from,
what arrives is "it crashed near the Pokémon Center", which is a search rather than a fix.

The port could already produce everything needed. `FRLG_INPUT_RECORD` writes the trace, `FRLG_SAV`
names the save, and the terminal carries the log. The README asks for all three, and adds the rule
that makes them work together:

> **Before each session, copy your save**: a trace only replays against the save the run *started*
> with, and playing changes it.

That instruction is the problem. It has to be followed **before** anything goes wrong, by someone who
has no reason yet to think anything will. Miguel forgot it once and lost a sixteen-minute recording to
a stray key; a friend testing from the README will forget it more often than that. Every report that
arrives with a trace and the *post-crash* save is a report that cannot be replayed at all, and it looks
complete, which is worse than looking broken.

Opt-in recording has the same shape: the runs that matter are the ones nobody expected to matter.

## Decision

**Every run collects its own session, by default.** A directory under
`$XDG_DATA_HOME/frlg-native/sessions/` named for the moment the run started, holding `input.trace`
recorded live, `start.sav` copied before the game can touch it, and `session.log` with both output
streams. On a fault, `crash.txt` joins them and the four are packed into `report.zip`.

`FRLG_NO_RECORD=1` switches it off. A launcher will expose it as a setting when there is a launcher;
until then the flag is what the golden and audio harnesses use, because a capture is not a play
session.

Recording is on rather than off because the three files have to exist *before* anything goes wrong,
and no instruction issued in advance survives contact with a tester who is there to play.

## Consequences

The save snapshot is the load-bearing part, and it is why this cannot be a flag a careful person sets:
the copy has to be taken at launch, and by the time anyone knows they wanted it, the game has already
written over it.

Disk grows by roughly the save's size per run — 128 KB, plus a log. Five runs are kept and older ones
pruned, so a tester who plays fifty times still has the last five.

**The bundle contains the player's save**, which is their game and whatever name they chose. It is
never sent anywhere on its own: the port writes a file, says what is in it, and the player decides
whether to attach it. There is deliberately no upload button — that would turn a local tool into a
service, with an endpoint, a privacy policy and an authentication story, to obtain data a file
attachment already delivers.

Sessions record during development builds of a game that is not finished. When the port ships to
people who are not testing it, the default is worth revisiting — at that point a launcher exists and
this becomes a setting with a sensible default rather than a build-wide decision.
