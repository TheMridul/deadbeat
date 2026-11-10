//! Checking every job at once, and the notifications that come out of it.

use std::process::{Command, Stdio};
use std::thread;

use crate::probe;
use crate::store::{Job, Store};

const ALERTS: [&str; 4] = ["stalled", "done", "failed", "ended"];

/// Probes every job: local ones directly, remote ones in parallel, each with
/// a 60-second limit. Returns the jobs with their new states.
pub fn probe_all(mut jobs: Vec<Job>, now: i64) -> Vec<Job> {
    let handles: Vec<_> = jobs
        .iter()
        .enumerate()
        .filter_map(|(index, job)| probe::command(job).map(|command| (index, command)))
        .map(|(index, command)| (index, thread::spawn(move || run(command))))
        .collect();
    for job in jobs.iter_mut().filter(|job| job.kind == "local") {
        probe::evaluate_local(job, now);
    }
    for (index, handle) in handles {
        let (code, out, err) = handle.join().unwrap_or((255, Vec::new(), b"probe crashed".to_vec()));
        probe::evaluate(&mut jobs[index], code, &out, &err, crate::now());
    }
    jobs
}

fn run(command: probe::Command) -> (i32, Vec<u8>, Vec<u8>) {
    // timeout(1) kills a hung ssh or API call and exits 124; nothing here may
    // prompt, because nobody is there to answer.
    let output = Command::new("timeout")
        .arg("60")
        .arg(&command.program)
        .args(&command.args)
        // gh hangs on a locked keyring; with no D-Bus it reads hosts.yml.
        .env("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent")
        .env("GH_PROMPT_DISABLED", "1")
        .env("AWS_PAGER", "")
        .stdin(Stdio::null())
        .output();
    match output {
        Ok(output) => {
            let code = output.status.code().unwrap_or(255);
            let err = match code {
                124 => b"Timed out after 60 seconds".to_vec(),
                127 => format!("{} not found", command.program).into_bytes(),
                _ => output.stderr,
            };
            (code, output.stdout, err)
        }
        Err(e) => (127, Vec::new(), format!("cannot run timeout: {e}").into_bytes()),
    }
}

/// Copies the results onto the stored jobs (which may have changed while the
/// probes ran) and returns the jobs that need a notification: the ones that
/// just stalled, finished, failed or ended.
pub fn merge(stored: &mut [Job], results: &[Job]) -> Vec<Job> {
    let mut to_notify = Vec::new();
    for job in stored.iter_mut() {
        let Some(result) = results.iter().find(|r| r.id == job.id) else { continue };
        job.state = result.state.clone();
        job.detail = result.detail.clone();
        if result.activity > 0 {
            job.activity = result.activity;
        }
        job.checked = result.checked;
        if ALERTS.contains(&job.state.as_str()) && job.notified != job.state {
            job.notified = job.state.clone();
            to_notify.push(job.clone());
        } else if job.state == "running" {
            // Back to running, so the next stall gets its own notification.
            job.notified.clear();
        }
    }
    to_notify
}

/// Merges the results under the store's lock, then notifies. Every check
/// finishes here, so a job is announced once even when two checks overlap.
pub fn record(store: &Store, results: &[Job]) -> Result<(), String> {
    let to_notify = store.update(|stored| merge(stored, results))?;
    for job in &to_notify {
        notify(job);
    }
    Ok(())
}

pub fn title(job: &Job) -> String {
    match job.state.as_str() {
        "stalled" => format!("{} has stalled", job.name),
        "done" => format!("{} is done", job.name),
        "failed" => format!("{} failed", job.name),
        _ => format!("{} has ended", job.name),
    }
}

fn notify(job: &Job) {
    let urgency = if job.state == "failed" || job.state == "stalled" { "critical" } else { "normal" };
    // An AppImage parent (T3 Code, for one) leaks its LD_LIBRARY_PATH, and
    // notify-send then loads the wrong libnotify and dies.
    let _ = Command::new("notify-send")
        .env_remove("LD_LIBRARY_PATH")
        .args(["-a", "Deadbeat", "-i", "deadbeat", "-u", urgency, &title(job), &job.detail])
        .stdin(Stdio::null())
        .status();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn notifications_go_out_once_per_state() {
        let stored = Job { id: "a".into(), kind: "ssh".into(), state: "running".into(), ..Job::default() };
        let mut jobs = vec![stored.clone()];
        let mut result = stored;

        result.state = "stalled".into();
        assert_eq!(merge(&mut jobs, &[result.clone()]).len(), 1);
        assert_eq!(merge(&mut jobs, &[result.clone()]).len(), 0);

        result.state = "running".into();
        assert_eq!(merge(&mut jobs, &[result.clone()]).len(), 0);
        result.state = "stalled".into();
        assert_eq!(merge(&mut jobs, &[result.clone()]).len(), 1);

        result.state = "unknown".into();
        assert_eq!(merge(&mut jobs, &[result.clone()]).len(), 0);
        result.state = "failed".into();
        assert_eq!(merge(&mut jobs, &[result.clone()]).len(), 1);

        // A job removed while the probes ran is not brought back.
        let mut empty: Vec<Job> = Vec::new();
        assert_eq!(merge(&mut empty, &[result]).len(), 0);
        assert!(empty.is_empty());
    }

    #[test]
    fn a_missing_program_is_named() {
        let (code, _, err) = run(probe::Command { program: "deadbeat-no-such-tool".into(), args: vec![] });
        assert_eq!(code, 127);
        assert_eq!(String::from_utf8_lossy(&err), "deadbeat-no-such-tool not found");
    }
}
