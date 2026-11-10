---
name: deadbeat
description: Register long-running jobs (training runs, exports, builds, remote scripts, EC2 instances, agent loops) with deadbeat so the user is notified when they stall, finish or fail, and read their state instead of polling. Use whenever you start or hand back a job that will outlive your turn, or are asked "is it stuck" or "check status".
---

# Deadbeat

Deadbeat watches jobs and sends a desktop notification when one stalls (its log
or branch goes quiet), finishes, fails or ends. A timer runs `deadbeat check`
every five minutes, so jobs are watched even when no agent is running. On Omarchy, the bar panel shows them too.

## Starting a job you will not wait on

Instead of `nohup`, `&` or a background shell, run it through deadbeat:

    deadbeat run --name "stage 3 training" --stall 30 -- python3 -m lab.train --stage 3

It runs in its own session, keeps its output in a log, and records the exit
code. `--stall` is how many minutes of silent log mean stuck (default 20).

## Watching something already running

    deadbeat add ssh --name "labels export" --host build-box --log ~/job.log --pid 4242
    deadbeat add ec2 --name "loop box" --instance i-0123456789abcdef0 --region us-east-1
    deadbeat add git --name "agent loop" --repo OWNER/REPO --branch agent-loop --stall 240

A git job is stalled when the branch gets no new commit for `--stall` minutes
(default 180), which suits loops that push once per iteration.

## Reading state

    deadbeat check --json   # probe every job now; prints a JSON array
    deadbeat list --json    # last known state, no probing

Each job has `id`, `name`, `kind`, `state` (`running`, `stalled`, `done`,
`failed`, `ended`, `unknown`), `detail` (latest log line, commit or error),
`activity` (last sign of life, epoch seconds) and `source`. `unknown` means
the check itself failed; `detail` says why, often an expired `aws login`.

Report what deadbeat says rather than re-deriving it. When you are done with a
job, `deadbeat remove ID` stops watching it (the job itself keeps running).
