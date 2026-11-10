# Vigil

![Vigil watching five jobs](docs/screenshot.png)

Vigil watches long jobs and sends a desktop notification when one stalls,
finishes, fails or ends. You (or an agent) start a training run, an export
or a loop, walk away, and hear about it when something changes, instead of
coming back to ask "is it stuck?".

It watches four kinds of job:

| Kind | Started with | Stalled when |
|---|---|---|
| A local command | `vigil run --name N -- COMMAND...` | its log is quiet for `--stall` minutes (20) |
| A log on an ssh host | `vigil add ssh --name N --host H --log PATH [--pid PID]` | the log is quiet for `--stall` minutes (20) |
| An EC2 instance | `vigil add ec2 --name N --instance ID [--region R]` | never; it ends when the instance stops |
| A branch a loop pushes to | `vigil add git --name N --repo OWNER/REPO --branch B` | no new commit for `--stall` minutes (180) |

A user timer runs `vigil check` every five minutes, so jobs are watched with
the window closed. The window checks once a minute while open, and shows how
long the `aws login` session has left when
[`aws-session`](https://github.com/TheMridul/dotfiles) is on the PATH, because
hosts behind SSM and every AWS check stop working when it ends.

## Install

Vigil is built for [Omarchy](https://omarchy.org) (Arch Linux and Hyprland)
and follows its theme, rounding and language live. `bin/install` builds and
installs it as an Arch package.

```sh
bin/install
systemctl --user enable --now vigil.timer
vigil skill install    # teaches coding agents to use it
```

Vigil then shows up in the app launcher (Super + Space).

## Commands

Every command takes `--json`, exits 0 or 1, and writes errors to stderr.

```sh
vigil run --name "stage 3" --stall 30 -- python3 train.py --stage 3
vigil add git --name "agent loop" --repo me/lab --branch agent-loop
vigil check --json     # probe everything now and notify
vigil list --json      # last known state, no probing
vigil remove ID        # stop watching; the job keeps running
```

`vigil run` starts the command in its own session, so it survives the
terminal or agent that started it, and records the exit code beside its log
in `~/.local/state/vigil/logs/`.

## Shortcuts

| Key | Action |
|---|---|
| Space | Check every job now |
| J / K or Down / Up | Select the next or previous job |
| Enter | Follow the selected job's log in a terminal (or open its page) |
| X / Delete | Stop watching the selected job |
| Ctrl+Z / Ctrl+Shift+Z | Undo or redo a removal |
| ? | Show every shortcut |
| Q | Quit |

## Build

```sh
bin/build    # build/vigil
bin/test     # Qt Test, offscreen
```

Needs `qmake6`, a C++17 compiler and Qt 6 (base, declarative, wayland).
