//! How each kind of job is checked.
//!
//! A remote kind is one command (ssh, aws or gh) whose output `evaluate` turns
//! into a state; a local job is read straight from /proc and its own files.
//! Everything here is pure apart from the files a local job reads, so the
//! tests drive it with canned output.

use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
use std::time::UNIX_EPOCH;

use serde_json::Value;

use crate::store::Job;

pub struct Command {
    pub program: String,
    pub args: Vec<String>,
}

pub fn shell_quote(text: &str) -> String {
    format!("'{}'", text.replace('\'', r"'\''"))
}

/// A path for a remote shell, keeping a leading ~/ working.
pub fn remote_path(path: &str) -> String {
    if path == "~" {
        "\"$HOME\"".into()
    } else if let Some(rest) = path.strip_prefix("~/") {
        format!("\"$HOME\"/{}", shell_quote(rest))
    } else {
        shell_quote(path)
    }
}

/// The last meaningful line of some output: after the last carriage return
/// (progress bars), without colour codes, cut to 200 characters.
pub fn last_line(text: &[u8]) -> String {
    let text = String::from_utf8_lossy(text);
    for line in text.split('\n').rev() {
        let segment = line.split('\r').rfind(|s| !s.is_empty()).unwrap_or("");
        let cleaned = strip_ansi(segment);
        let trimmed = cleaned.trim();
        if !trimmed.is_empty() {
            return trimmed.chars().take(200).collect();
        }
    }
    String::new()
}

/// Drops CSI escape sequences such as colours and cursor moves.
fn strip_ansi(text: &str) -> String {
    let chars: Vec<char> = text.chars().collect();
    let mut out = String::with_capacity(text.len());
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '\x1b' && chars.get(i + 1) == Some(&'[') {
            let mut j = i + 2;
            while j < chars.len() && (chars[j].is_ascii_digit() || chars[j] == ';' || chars[j] == '?') {
                j += 1;
            }
            while j < chars.len() && (' '..='/').contains(&chars[j]) {
                j += 1;
            }
            if j < chars.len() && ('@'..='~').contains(&chars[j]) {
                i = j + 1;
                continue;
            }
        }
        out.push(chars[i]);
        i += 1;
    }
    out
}

pub fn error_text(err: &[u8], code: i32) -> String {
    let text = String::from_utf8_lossy(err);
    // Hosts behind SSM and every aws call fail this way once `aws login` has
    // run out; say what to do rather than echo the CLI's noise.
    if text.to_lowercase().contains("expired") || text.contains("aws login") || text.contains("ExpiredToken") {
        return "AWS session ended; run aws login".into();
    }
    let line = last_line(err);
    if line.is_empty() { format!("Exited with code {code}") } else { line }
}

/// The kernel's start time of a process, so a reused PID is not mistaken for
/// the job. Empty when the process is gone.
pub fn start_ticks(pid: i64) -> String {
    if pid <= 0 {
        return String::new();
    }
    let Ok(stat) = fs::read_to_string(format!("/proc/{pid}/stat")) else {
        return String::new();
    };
    // The command name is in parentheses and may hold spaces, so count fields
    // from the last ')'. Start time is field 22, the 20th after it.
    let Some(close) = stat.rfind(')') else { return String::new() };
    stat.get(close + 2..)
        .and_then(|rest| rest.split(' ').nth(19))
        .unwrap_or("")
        .to_string()
}

/// The command that checks a remote job. None for a local job.
pub fn command(job: &Job) -> Option<Command> {
    let owned = |items: &[&str]| items.iter().map(|s| s.to_string()).collect::<Vec<_>>();
    match job.kind.as_str() {
        "ssh" => {
            let pid = job.number("pid");
            let pid_check = if pid > 0 {
                format!("if kill -0 {pid} 2>/dev/null; then echo alive; else echo dead; fi")
            } else {
                "echo -".into()
            };
            // One line each: the remote clock, the log's mtime (-1 when
            // missing), whether the process lives, then the log's last lines.
            let script = format!(
                "f={}; date +%s; if [ -e \"$f\" ]; then stat -c %Y \"$f\"; else echo -1; fi; {pid_check}; \
                 [ -e \"$f\" ] && tail -n 3 \"$f\"; true",
                remote_path(job.text("log"))
            );
            // No shared connection: with ControlPersist the proxy runs in a
            // background master, and an SSM host's "session has expired"
            // error from aws never reaches us, leaving only "Connection closed".
            let mut args = owned(&["-o", "BatchMode=yes", "-o", "ConnectTimeout=30"]);
            args.push(job.text("host").into());
            args.push(script);
            Some(Command { program: "ssh".into(), args })
        }
        "ec2" => {
            let mut args = owned(&["ec2", "describe-instances", "--instance-ids"]);
            args.push(job.text("instance").into());
            args.extend(owned(&[
                "--output",
                "json",
                "--query",
                "Reservations[0].Instances[0].{state:State.Name,launch:LaunchTime,\
                 lifecycle:InstanceLifecycle,reason:StateTransitionReason}",
            ]));
            if job.spec.contains_key("region") {
                args.push("--region".into());
                args.push(job.text("region").into());
            }
            Some(Command { program: "aws".into(), args })
        }
        "git" => Some(Command {
            program: "gh".into(),
            args: vec![
                "api".into(),
                format!("repos/{}/commits/{}", job.text("repo"), job.text("branch")),
                "--jq".into(),
                r#"[.commit.committer.date, (.commit.message|split("\n")[0])]|@tsv"#.into(),
            ],
        }),
        _ => None,
    }
}

fn parse_iso(text: &str) -> i64 {
    chrono::DateTime::parse_from_rfc3339(text.trim()).map(|t| t.timestamp()).unwrap_or(0)
}

fn mark_activity(job: &mut Job, activity: i64, now: i64) {
    job.activity = activity;
    let quiet = activity > 0 && now - activity > job.stall_minutes * 60;
    job.state = if quiet { "stalled" } else { "running" }.into();
}

fn set(job: &mut Job, state: &str, detail: impl Into<String>) {
    job.state = state.into();
    job.detail = detail.into();
}

pub fn evaluate(job: &mut Job, code: i32, out: &[u8], err: &[u8], now: i64) {
    job.checked = now;
    if job.kind == "ec2" && String::from_utf8_lossy(err).contains("InvalidInstanceID.NotFound") {
        return set(job, "ended", "Instance no longer exists");
    }
    if code != 0 {
        // A failed check says nothing about the job itself, so keep its last
        // known activity and only report why the check failed.
        return set(job, "unknown", error_text(err, code));
    }

    match job.kind.as_str() {
        "ssh" => {
            let text = String::from_utf8_lossy(out);
            let lines: Vec<&str> = text.split('\n').collect();
            let host = job.text("host").to_string();
            if lines.len() < 3 {
                return set(job, "unknown", format!("Unexpected reply from {host}"));
            }
            let remote_now: i64 = lines[0].trim().parse().unwrap_or(0);
            let mtime: i64 = lines[1].trim().parse().unwrap_or(-1);
            if mtime < 0 {
                return set(job, "unknown", format!("Log not found on {host}"));
            }
            job.detail = last_line(lines[3..].join("\n").as_bytes());
            // Measure the log's age on the remote clock, then place it on
            // ours, so a skewed remote clock does not look like a stall.
            let activity = now - (remote_now - mtime);
            if lines[2].trim() == "dead" {
                job.activity = activity;
                job.state = "ended".into();
                if job.detail.is_empty() {
                    job.detail = "Process ended".into();
                }
                return;
            }
            mark_activity(job, activity, now);
        }
        "ec2" => {
            let reply: Value = serde_json::from_slice(out).unwrap_or(Value::Null);
            let field = |key: &str| reply.get(key).and_then(Value::as_str).unwrap_or("").to_string();
            let state = field("state");
            job.activity = parse_iso(&field("launch"));
            if state == "running" || state == "pending" {
                let spot = field("lifecycle") == "spot";
                set(job, "running", if spot { "Running (spot)" } else { "Running" });
            } else {
                let reason = field("reason");
                let detail = if reason.is_empty() { state.clone() } else { format!("{state}: {reason}") };
                set(job, if state.is_empty() { "unknown" } else { "ended" }, detail);
            }
        }
        "git" => {
            let text = String::from_utf8_lossy(out);
            let line = text.trim();
            let (date, message) = line.split_once('\t').unwrap_or((line, ""));
            let date = parse_iso(date);
            if date == 0 {
                return set(job, "unknown", "Unexpected reply from GitHub");
            }
            job.detail = message.chars().take(200).collect();
            mark_activity(job, date, now);
        }
        _ => {}
    }
}

fn tail(path: &str, bytes: u64) -> Vec<u8> {
    let mut out = Vec::new();
    if let Ok(mut file) = File::open(path) {
        let size = file.metadata().map(|m| m.len()).unwrap_or(0);
        if size > bytes {
            let _ = file.seek(SeekFrom::Start(size - bytes));
        }
        let _ = file.read_to_end(&mut out);
    }
    out
}

pub fn mtime(path: impl AsRef<Path>) -> Option<i64> {
    let modified = fs::metadata(path).ok()?.modified().ok()?;
    Some(modified.duration_since(UNIX_EPOCH).ok()?.as_secs() as i64)
}

pub fn evaluate_local(job: &mut Job, now: i64) {
    job.checked = now;
    let log = job.text("log").to_string();
    let exit_path = job.text("exit").to_string();
    job.detail = last_line(&tail(&log, 4096));

    if let Some(code) = fs::read_to_string(&exit_path).ok().and_then(|t| t.trim().parse::<i32>().ok()) {
        job.activity = mtime(&exit_path).unwrap_or(now);
        if code == 0 {
            job.state = "done".into();
        } else {
            job.state = "failed".into();
            job.detail = if job.detail.is_empty() {
                format!("Exit code {code}")
            } else {
                format!("Exit code {code}: {}", job.detail)
            };
        }
        return;
    }

    let ticks = job.text("ticks");
    if !ticks.is_empty() && start_ticks(job.number("pid")) == ticks {
        let activity = mtime(&log).unwrap_or(job.created);
        mark_activity(job, activity, now);
        return;
    }
    // The wrapper shell writes the exit code, so its absence means the job
    // was killed from outside, or the machine went down under it.
    set(job, "failed", "Stopped without an exit code");
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::time::{Duration, SystemTime};

    fn now() -> i64 {
        SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() as i64
    }

    fn job(kind: &str, spec: Value) -> Job {
        Job { kind: kind.into(), spec: spec.as_object().unwrap().clone(), ..Job::default() }
    }

    fn local(dir: &Path) -> Job {
        job(
            "local",
            json!({"log": dir.join("job.log"), "exit": dir.join("job.exit"), "pid": 0, "ticks": ""}),
        )
    }

    fn ssh() -> Job {
        job("ssh", json!({"host": "box", "log": "~/run.log"}))
    }

    #[test]
    fn local_job_reads_its_exit_code() {
        let dir = tempfile::tempdir().unwrap();
        let mut job = local(dir.path());
        fs::write(dir.path().join("job.log"), "step 1\nstep 2\n").unwrap();
        fs::write(dir.path().join("job.exit"), "0\n").unwrap();
        evaluate_local(&mut job, now());
        assert_eq!((job.state.as_str(), job.detail.as_str()), ("done", "step 2"));

        fs::write(dir.path().join("job.exit"), "3\n").unwrap();
        evaluate_local(&mut job, now());
        assert_eq!((job.state.as_str(), job.detail.as_str()), ("failed", "Exit code 3: step 2"));
    }

    #[test]
    fn local_job_stalls_when_its_log_goes_quiet() {
        let dir = tempfile::tempdir().unwrap();
        let mut job = local(dir.path());
        let pid = std::process::id() as i64;
        job.spec.insert("pid".into(), json!(pid));
        job.spec.insert("ticks".into(), json!(start_ticks(pid)));
        let log = dir.path().join("job.log");
        fs::write(&log, "epoch 4\n").unwrap();
        evaluate_local(&mut job, now());
        assert_eq!(job.state, "running");

        let old = SystemTime::now() - Duration::from_secs(30 * 60);
        File::options().write(true).open(&log).unwrap().set_modified(old).unwrap();
        evaluate_local(&mut job, now());
        assert_eq!(job.state, "stalled");
    }

    #[test]
    fn local_job_with_a_reused_pid_counts_as_killed() {
        let dir = tempfile::tempdir().unwrap();
        let mut job = local(dir.path());
        job.spec.insert("pid".into(), json!(std::process::id()));
        job.spec.insert("ticks".into(), json!("1"));
        evaluate_local(&mut job, now());
        assert_eq!((job.state.as_str(), job.detail.as_str()), ("failed", "Stopped without an exit code"));
    }

    #[test]
    fn ssh_reply_is_measured_on_the_remote_clock() {
        let mut job = ssh();
        // The remote clock is far behind ours; the log is 10 minutes old there.
        evaluate(&mut job, 0, b"1000\n400\nalive\nepoch 3 loss 0.21\n", b"", 5000);
        assert_eq!((job.state.as_str(), job.activity), ("running", 4400));
        assert_eq!(job.detail, "epoch 3 loss 0.21");

        job.stall_minutes = 5;
        evaluate(&mut job, 0, b"1000\n400\nalive\n", b"", 5000);
        assert_eq!(job.state, "stalled");

        evaluate(&mut job, 0, b"1000\n990\ndead\ndone\n", b"", 5000);
        assert_eq!(job.state, "ended");

        evaluate(&mut job, 0, b"1000\n-1\n-\n", b"", 5000);
        assert_eq!(job.state, "unknown");
    }

    #[test]
    fn an_expired_aws_login_is_named() {
        let mut job = ssh();
        job.activity = 42;
        let err = b"kex_exchange_identification\nYour session has expired. Please reauthenticate using 'aws login'.\n";
        evaluate(&mut job, 255, b"", err, 5000);
        assert_eq!((job.state.as_str(), job.detail.as_str()), ("unknown", "AWS session ended; run aws login"));
        assert_eq!(job.activity, 42);
    }

    #[test]
    fn ssh_script_runs_in_a_real_shell() {
        // Run the probe's remote script locally, with HOME pointing at a temp
        // directory, so its quoting and output format are tested for real.
        let dir = tempfile::tempdir().unwrap();
        fs::write(dir.path().join("odd 'name'.log"), "first\nloss 0.5\r loss 0.4\n").unwrap();
        let mut job = job("ssh", json!({"host": "box", "log": "~/odd 'name'.log", "pid": std::process::id()}));
        let command = command(&job).unwrap();
        assert_eq!(command.program, "ssh");
        let output = std::process::Command::new("sh")
            .args(["-c", command.args.last().unwrap()])
            .env("HOME", dir.path())
            .output()
            .unwrap();
        evaluate(&mut job, output.status.code().unwrap(), &output.stdout, &output.stderr, now());
        assert_eq!((job.state.as_str(), job.detail.as_str()), ("running", "loss 0.4"));
    }

    #[test]
    fn ec2_states() {
        let mut job = job("ec2", json!({"instance": "i-0123456789abcdef0"}));
        let running = br#"{"state":"running","launch":"2026-11-09T02:00:00+00:00","lifecycle":"spot","reason":""}"#;
        evaluate(&mut job, 0, running, b"", now());
        assert_eq!((job.state.as_str(), job.detail.as_str()), ("running", "Running (spot)"));
        assert!(job.activity > 0);

        evaluate(&mut job, 0, br#"{"state":"terminated","reason":"Service initiated (spot)"}"#, b"", now());
        assert_eq!((job.state.as_str(), job.detail.as_str()), ("ended", "terminated: Service initiated (spot)"));

        evaluate(&mut job, 254, b"", b"An error occurred (InvalidInstanceID.NotFound) when calling", now());
        assert_eq!(job.state, "ended");
    }

    #[test]
    fn git_branch_stalls_without_commits() {
        let mut job = job("git", json!({"repo": "o/r", "branch": "b"}));
        job.stall_minutes = 180;
        let commit = parse_iso("2026-11-09T07:14:28Z");
        let reply = b"2026-11-09T07:14:28Z\tMerge pull request #2\n";
        evaluate(&mut job, 0, reply, b"", commit + 3600);
        assert_eq!((job.state.as_str(), job.detail.as_str()), ("running", "Merge pull request #2"));
        evaluate(&mut job, 0, reply, b"", commit + 4 * 3600);
        assert_eq!(job.state, "stalled");
    }

    #[test]
    fn last_line_cleans_progress_output() {
        assert_eq!(last_line(b"a\n\x1b[32m 50%\r 100% done\x1b[0m\n\n"), "100% done");
        assert_eq!(last_line(b""), "");
    }

    #[test]
    fn remote_paths_are_quoted() {
        assert_eq!(remote_path("~/a b'c"), r#""$HOME"/'a b'\''c'"#);
        assert_eq!(remote_path("/var/log/x"), "'/var/log/x'");
    }
}
