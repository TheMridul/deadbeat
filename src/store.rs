//! The watched jobs and the one JSON file they live in.
//!
//! The timer's `deadbeat check`, agents' `deadbeat add` and the bar plugin all
//! touch jobs.json, so every change goes through `Store::update`, which holds
//! an flock across the read and an atomic save.

use std::collections::hash_map::RandomState;
use std::fs::{self, File, OpenOptions, TryLockError};
use std::hash::{BuildHasher, Hasher};
use std::path::{Path, PathBuf};
use std::thread;
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

/// One watched job. `kind` is "local", "ssh", "ec2" or "git", and `spec` holds
/// what the probe for that kind needs. The fields after `stall_minutes` are the
/// result of the latest check.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Job {
    pub id: String,
    pub name: String,
    pub kind: String,
    pub spec: Map<String, Value>,
    pub created: i64,
    pub stall_minutes: i64,
    /// running, stalled, done, failed, ended or unknown
    pub state: String,
    /// the latest log line, commit, instance state or error
    pub detail: String,
    /// last sign of life, in epoch seconds
    pub activity: i64,
    pub checked: i64,
    /// the state a notification last went out for
    pub notified: String,
}

impl Default for Job {
    fn default() -> Self {
        Job {
            id: String::new(),
            name: String::new(),
            kind: String::new(),
            spec: Map::new(),
            created: 0,
            stall_minutes: 20,
            state: "unknown".into(),
            detail: String::new(),
            activity: 0,
            checked: 0,
            notified: String::new(),
        }
    }
}

impl Job {
    pub fn text(&self, key: &str) -> &str {
        self.spec.get(key).and_then(Value::as_str).unwrap_or("")
    }

    pub fn number(&self, key: &str) -> i64 {
        self.spec.get(key).and_then(Value::as_i64).unwrap_or(0)
    }

    /// What the job watches, in one short line: a command, host:log, an instance.
    pub fn source(&self) -> String {
        match self.kind.as_str() {
            "local" => self.text("command").to_string(),
            "ssh" => format!("{}:{}", self.text("host"), self.text("log")),
            "ec2" => self.text("instance").to_string(),
            "git" => format!("{}@{}", self.text("repo"), self.text("branch")),
            other => other.to_string(),
        }
    }
}

#[derive(Serialize, Deserialize, Default)]
struct File_ {
    #[serde(default)]
    jobs: Vec<Job>,
}

pub struct Store {
    dir: PathBuf,
}

impl Store {
    pub fn new(dir: impl Into<PathBuf>) -> Self {
        Store { dir: dir.into() }
    }

    /// `$DEADBEAT_STATE_DIR`, else `$XDG_STATE_HOME/deadbeat`, else
    /// `~/.local/state/deadbeat`.
    pub fn open() -> Self {
        let dir = match std::env::var_os("DEADBEAT_STATE_DIR").filter(|d| !d.is_empty()) {
            Some(dir) => PathBuf::from(dir),
            None => {
                let state = std::env::var_os("XDG_STATE_HOME")
                    .filter(|d| !d.is_empty())
                    .map(PathBuf::from)
                    .unwrap_or_else(|| home().join(".local/state"));
                state.join("deadbeat")
            }
        };
        Store::new(dir)
    }

    pub fn path(&self) -> PathBuf {
        self.dir.join("jobs.json")
    }

    pub fn log_dir(&self) -> PathBuf {
        self.dir.join("logs")
    }

    /// The jobs as last saved. A missing or unreadable file is no jobs.
    pub fn load(&self) -> Vec<Job> {
        fs::read(self.path())
            .ok()
            .and_then(|bytes| serde_json::from_slice::<File_>(&bytes).ok())
            .map(|file| file.jobs)
            .unwrap_or_default()
    }

    /// Runs `change` on the stored jobs under the lock and saves the result.
    pub fn update<T>(&self, change: impl FnOnce(&mut Vec<Job>) -> T) -> Result<T, String> {
        fs::create_dir_all(self.log_dir())
            .map_err(|e| format!("cannot create {}: {e}", self.dir.display()))?;
        let lock_path = self.dir.join("jobs.lock");
        let lock = OpenOptions::new()
            .create(true)
            .truncate(false)
            .write(true)
            .open(&lock_path)
            .map_err(|e| format!("cannot open {}: {e}", lock_path.display()))?;
        acquire(&lock, &lock_path)?;
        let mut jobs = self.load();
        let result = change(&mut jobs);
        self.save(&jobs)?;
        Ok(result)
    }

    fn save(&self, jobs: &[Job]) -> Result<(), String> {
        // Written beside the target and renamed over it, so a crash mid-write
        // never leaves a half-written jobs.json.
        let path = self.path();
        let tmp = self.dir.join(format!(".jobs.json.{}", std::process::id()));
        let mut text = serde_json::to_string_pretty(&File_ { jobs: jobs.to_vec() })
            .map_err(|e| e.to_string())?;
        text.push('\n');
        fs::write(&tmp, text).map_err(|e| format!("cannot write {}: {e}", tmp.display()))?;
        fs::rename(&tmp, &path).map_err(|e| format!("cannot save {}: {e}", path.display()))
    }
}

/// An flock is dropped by the kernel when its holder dies, so unlike a lock
/// file it can never go stale.
fn acquire(lock: &File, path: &Path) -> Result<(), String> {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        match lock.try_lock() {
            Ok(()) => return Ok(()),
            Err(TryLockError::WouldBlock) if Instant::now() < deadline => {
                thread::sleep(Duration::from_millis(50))
            }
            Err(TryLockError::WouldBlock) => {
                return Err(format!("another deadbeat is holding {}", path.display()));
            }
            Err(TryLockError::Error(e)) => return Err(format!("cannot lock {}: {e}", path.display())),
        }
    }
}

pub fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/"))
}

/// Six hex digits, enough to tell a handful of jobs apart and short to type.
pub fn new_id() -> String {
    let random = RandomState::new().build_hasher().finish();
    format!("{:06x}", 0x100000 + random % 0xEFFFFF)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn round_trips_and_makes_the_log_directory() {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::new(dir.path());
        let job = Job {
            id: "abc123".into(),
            name: "train".into(),
            kind: "ssh".into(),
            spec: json!({"host": "box", "log": "~/run.log"}).as_object().unwrap().clone(),
            ..Job::default()
        };
        store.update(|jobs| jobs.push(job.clone())).unwrap();
        let loaded = store.load();
        assert_eq!(loaded, vec![job]);
        assert_eq!(loaded[0].source(), "box:~/run.log");
        assert!(store.log_dir().is_dir());
    }

    #[test]
    fn reads_the_files_the_qt_version_wrote() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("jobs.json"),
            r#"{"jobs": [{"id": "a", "name": "n", "kind": "git", "spec": {"repo": "o/r", "branch": "b"},
                "created": 1, "stallMinutes": 180, "state": "running", "detail": "",
                "activity": 2, "checked": 3, "notified": ""}]}"#,
        )
        .unwrap();
        let jobs = Store::new(dir.path()).load();
        assert_eq!(jobs[0].stall_minutes, 180);
        assert_eq!(jobs[0].source(), "o/r@b");
    }

    #[test]
    fn ids_are_six_hex_digits() {
        let id = new_id();
        assert_eq!(id.len(), 6);
        assert!(id.chars().all(|c| c.is_ascii_hexdigit()));
    }
}
