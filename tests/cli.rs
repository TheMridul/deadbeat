//! Drives the real binary against a scratch state directory.

use std::path::Path;
use std::process::Command;
use std::time::{Duration, Instant};

use serde_json::Value;

fn deadbeat(state: &Path, args: &[&str]) -> (bool, Value, String) {
    let output = Command::new(env!("CARGO_BIN_EXE_deadbeat"))
        .args(args)
        .env("DEADBEAT_STATE_DIR", state)
        // Keep the test from showing desktop notifications.
        .env("PATH", "/usr/bin:/bin")
        .env("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent")
        .output()
        .unwrap();
    let stdout = String::from_utf8_lossy(&output.stdout).to_string();
    let json = serde_json::from_str(&stdout).unwrap_or(Value::Null);
    (output.status.success(), json, String::from_utf8_lossy(&output.stderr).to_string())
}

#[test]
fn a_run_job_goes_from_running_to_done_and_can_be_removed() {
    let dir = tempfile::tempdir().unwrap();
    let (ok, job, _) = deadbeat(dir.path(), &["run", "--name", "greet", "--json", "--", "sh", "-c", "echo hello there"]);
    assert!(ok);
    let id = job["id"].as_str().unwrap().to_string();
    assert_eq!(job["state"], "running");

    let deadline = Instant::now() + Duration::from_secs(10);
    let state = loop {
        let (_, jobs, _) = deadbeat(dir.path(), &["check", "--json"]);
        let state = jobs[0]["state"].as_str().unwrap().to_string();
        if state != "running" || Instant::now() > deadline {
            break jobs[0].clone();
        }
        std::thread::sleep(Duration::from_millis(100));
    };
    assert_eq!(state["state"], "done");
    assert_eq!(state["detail"], "hello there");
    assert_eq!(state["notified"], "done");
    assert_eq!(state["source"], "sh -c 'echo hello there'");

    let (ok, removed, _) = deadbeat(dir.path(), &["remove", &id, "--json"]);
    assert!(ok);
    assert_eq!(removed["removed"], id.as_str());
    let (_, jobs, _) = deadbeat(dir.path(), &["list", "--json"]);
    assert_eq!(jobs, Value::Array(vec![]));
}

#[test]
fn bad_input_fails_with_a_message() {
    let dir = tempfile::tempdir().unwrap();
    let (ok, _, err) = deadbeat(dir.path(), &["add", "git", "--name", "x", "--repo", "noslash"]);
    assert!(!ok);
    assert_eq!(err.trim(), "deadbeat: add git needs --repo OWNER/REPO");
    let (ok, _, err) = deadbeat(dir.path(), &["remove", "ffffff"]);
    assert!(!ok);
    assert_eq!(err.trim(), "deadbeat: no job ffffff");
}

#[test]
fn the_skill_installs_where_asked() {
    let dir = tempfile::tempdir().unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_deadbeat"))
        .args(["skill", "install"])
        .env("DEADBEAT_SKILL_DIR", dir.path())
        .output()
        .unwrap();
    assert!(output.status.success());
    let skill = std::fs::read_to_string(dir.path().join("deadbeat/SKILL.md")).unwrap();
    assert!(skill.starts_with("---\nname: deadbeat"));
}

#[test]
fn a_kaggle_kernel_that_completes_fetches_its_output() {
    let dir = tempfile::tempdir().unwrap();
    let state = dir.path().join("state");
    let out = dir.path().join("out");
    // Stands in for the kaggle CLI: the kernel is complete, and "output"
    // writes a file where it was asked to.
    let cli = dir.path().join("kaggle");
    std::fs::write(
        &cli,
        concat!(
            "#!/bin/sh\n",
            "case \"$2\" in\n",
            "  status) echo \"$3 has status \\\"KernelWorkerStatus.COMPLETE\\\"\" ;;\n",
            "  output) echo fetched > \"$5/result.txt\" ;;\n",
            "esac\n",
        ),
    )
    .unwrap();
    std::fs::set_permissions(&cli, std::os::unix::fs::PermissionsExt::from_mode(0o755)).unwrap();

    let bad_slug = ["add", "kaggle", "--name", "k", "--kernel", "me/train;rm"];
    let (ok, _, err) = deadbeat(&state, &bad_slug);
    assert!(!ok);
    assert!(err.contains("--kernel OWNER/SLUG"), "{err}");
    let no_cli = ["add", "kaggle", "--name", "k", "--kernel", "me/train", "--kaggle", "/nonexistent/kaggle"];
    let (ok, _, err) = deadbeat(&state, &no_cli);
    assert!(!ok);
    assert_eq!(err.trim(), "deadbeat: no kaggle CLI at /nonexistent/kaggle");

    let (cli, out_dir) = (cli.to_str().unwrap(), out.to_str().unwrap());
    let add = ["add", "kaggle", "--name", "train", "--kernel", "me/train", "--kaggle", cli, "--output", out_dir, "--json"];
    let (ok, job, err) = deadbeat(&state, &add);
    assert!(ok, "{err}");
    assert_eq!(job["source"], "me/train");

    let (_, jobs, _) = deadbeat(&state, &["check", "--json"]);
    assert_eq!(jobs[0]["state"], "done");
    assert_eq!(jobs[0]["notified"], "done");
    // The download is a job of its own, started in the output directory.
    assert_eq!(jobs[1]["name"], "train output");
    let deadline = Instant::now() + Duration::from_secs(10);
    while !out.join("result.txt").exists() && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(100));
    }
    assert_eq!(std::fs::read_to_string(out.join("result.txt")).unwrap(), "fetched\n");

    // A second check announces nothing new and starts no second download.
    let (_, jobs, _) = deadbeat(&state, &["check", "--json"]);
    assert_eq!(jobs.as_array().unwrap().len(), 2);
}
