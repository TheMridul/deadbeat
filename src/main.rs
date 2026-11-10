//! deadbeat: watches long jobs and says when they stall, finish or fail.

mod check;
mod probe;
mod store;

use std::collections::HashMap;
use std::fs;
use std::os::unix::process::CommandExt;
use std::process::{Command, ExitCode, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};

use serde_json::{Map, Value, json};

use store::{Job, Store};

const USAGE: &str = "\
deadbeat watches long jobs and tells you when they stall, finish or fail.

  deadbeat run --name N [--stall MIN] -- COMMAND...
                                        start COMMAND in the background and watch it
  deadbeat add ssh --name N --host H --log PATH [--pid PID] [--stall MIN]
                                        watch a log (and process) on an ssh host
  deadbeat add ec2 --name N --instance ID [--region R]
                                        watch an EC2 instance until it stops
  deadbeat add git --name N --repo OWNER/REPO [--branch B] [--stall MIN]
                                        watch a branch that a loop pushes to
  deadbeat list                         the jobs as last checked
  deadbeat check                        check every job now and notify
  deadbeat remove ID                    stop watching a job (it keeps running)
  deadbeat skill install                install the agent skill

Every command takes --json. A job is stalled when its log, or the branch,
has been quiet longer than --stall minutes (20, or 180 for git).
";

const SKILL: &str = include_str!("../skill/SKILL.md");

const VALUED: [&str; 9] = ["--name", "--stall", "--host", "--log", "--pid", "--instance", "--region", "--repo", "--branch"];

pub fn now() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

#[derive(Default)]
struct Options {
    positional: Vec<String>,
    values: HashMap<String, String>,
    /// everything after --
    command: Vec<String>,
    json: bool,
}

impl Options {
    fn value(&self, key: &str) -> &str {
        self.values.get(key).map(String::as_str).unwrap_or("")
    }

    fn stall(&self) -> Result<Option<i64>, String> {
        match self.values.get("stall") {
            None => Ok(None),
            Some(text) => match text.parse::<i64>() {
                Ok(minutes) if minutes >= 1 => Ok(Some(minutes)),
                _ => Err("--stall takes a number of minutes".into()),
            },
        }
    }
}

fn parse(args: &[String]) -> Result<Options, String> {
    let mut o = Options::default();
    let mut i = 0;
    while i < args.len() {
        let arg = args[i].as_str();
        if arg == "--" {
            o.command = args[i + 1..].to_vec();
            break;
        } else if arg == "--json" {
            o.json = true;
        } else if VALUED.contains(&arg) {
            let value = args.get(i + 1).ok_or(format!("{arg} needs a value"))?;
            o.values.insert(arg[2..].to_string(), value.clone());
            i += 1;
        } else if arg.starts_with("--") {
            return Err(format!("unknown option {arg}"));
        } else {
            o.positional.push(arg.to_string());
        }
        i += 1;
    }
    Ok(o)
}

fn print_json(value: &Value) {
    println!("{value}");
}

fn job_json(job: &Job) -> Value {
    let mut value = serde_json::to_value(job).unwrap_or(Value::Null);
    if let Value::Object(map) = &mut value {
        map.insert("source".into(), json!(job.source()));
    }
    value
}

fn print_jobs(jobs: &[Job], as_json: bool) {
    if as_json {
        return print_json(&Value::Array(jobs.iter().map(job_json).collect()));
    }
    if jobs.is_empty() {
        println!("No jobs. Add one with deadbeat run or deadbeat add.");
    }
    for job in jobs {
        println!("{}  {:<8}{}  ({})", job.id, job.state, job.name, job.source());
        if !job.detail.is_empty() {
            println!("        {}", job.detail);
        }
    }
}

fn spec(value: Value) -> Map<String, Value> {
    value.as_object().cloned().unwrap_or_default()
}

fn add(o: &Options) -> Result<(), String> {
    let mut job = Job { kind: o.positional.get(1).cloned().unwrap_or_default(), ..Job::default() };
    match job.kind.as_str() {
        "ssh" => {
            if o.value("host").is_empty() || o.value("log").is_empty() {
                return Err("add ssh needs --host and --log".into());
            }
            job.spec = spec(json!({"host": o.value("host"), "log": o.value("log")}));
            if let Some(pid) = o.values.get("pid") {
                let pid: i64 = pid.parse().map_err(|_| "--pid takes a number")?;
                job.spec.insert("pid".into(), json!(pid));
            }
        }
        "ec2" => {
            if o.value("instance").is_empty() {
                return Err("add ec2 needs --instance".into());
            }
            job.spec = spec(json!({"instance": o.value("instance")}));
            if let Some(region) = o.values.get("region") {
                job.spec.insert("region".into(), json!(region));
            }
        }
        "git" => {
            if !o.value("repo").contains('/') {
                return Err("add git needs --repo OWNER/REPO".into());
            }
            let branch = o.values.get("branch").map(String::as_str).unwrap_or("main");
            job.spec = spec(json!({"repo": o.value("repo"), "branch": branch}));
            job.stall_minutes = 180;
        }
        _ => return Err("add takes ssh, ec2 or git".into()),
    }
    job.name = o.value("name").to_string();
    if job.name.is_empty() {
        return Err("--name is required".into());
    }
    if let Some(minutes) = o.stall()? {
        job.stall_minutes = minutes;
    }
    job.id = store::new_id();
    job.created = now();
    Store::open().update(|jobs| jobs.push(job.clone()))?;
    if o.json {
        print_json(&job_json(&job));
    } else {
        println!("Watching {} as {}", job.name, job.id);
    }
    Ok(())
}

/// Quotes only the words that need it, so the command reads naturally.
fn display_command(command: &[String]) -> String {
    let plain = |word: &str| {
        !word.is_empty() && word.chars().all(|c| c.is_ascii_alphanumeric() || "_./:=@%+,-".contains(c))
    };
    command
        .iter()
        .map(|word| if plain(word) { word.clone() } else { probe::shell_quote(word) })
        .collect::<Vec<_>>()
        .join(" ")
}

fn run(o: &Options) -> Result<(), String> {
    let first = o.command.first().ok_or("nothing to run; put the command after --")?;
    let stall = o.stall()?.unwrap_or(20);
    let store = Store::open();
    store.update(|_| ())?; // creates the directories

    let id = store::new_id();
    let log = store.log_dir().join(format!("{id}.log"));
    let exit = store.log_dir().join(format!("{id}.exit"));
    let cwd = std::env::current_dir().map_err(|e| format!("no working directory: {e}"))?;

    // A small shell owns the command so its exit code lands in a file, which
    // is how a later `deadbeat check` tells done from failed. Its own session
    // keeps it alive when the terminal or agent that started it goes away.
    let mut shell = Command::new("sh");
    shell
        .args(["-c", r#"log=$1 exitf=$2; shift 2; "$@" >"$log" 2>&1 </dev/null; echo $? >"$exitf""#, "deadbeat-run"])
        .arg(&log)
        .arg(&exit)
        .args(&o.command)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    // SAFETY: setsid is async-signal-safe, which is all pre_exec requires.
    unsafe {
        shell.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    let child = shell.spawn().map_err(|e| format!("could not start sh: {e}"))?;
    let pid = child.id() as i64;

    let started = now();
    let job = Job {
        id: id.clone(),
        name: o.values.get("name").cloned().unwrap_or_else(|| first.clone()),
        kind: "local".into(),
        spec: spec(json!({
            "command": display_command(&o.command),
            "cwd": cwd,
            "pid": pid,
            "ticks": probe::start_ticks(pid),
            "log": log,
            "exit": exit,
        })),
        created: started,
        stall_minutes: stall,
        state: "running".into(),
        activity: started,
        ..Job::default()
    };
    store.update(|jobs| jobs.push(job.clone()))?;
    if o.json {
        print_json(&job_json(&job));
    } else {
        println!("Started {} as {} (pid {pid})\nLog: {}", job.name, job.id, log.display());
    }
    Ok(())
}

fn check(o: &Options) -> Result<(), String> {
    let store = Store::open();
    let results = check::probe_all(store.load(), now());
    check::record(&store, &results)?;
    print_jobs(&store.load(), o.json);
    Ok(())
}

fn remove(o: &Options) -> Result<(), String> {
    let id = o.positional.get(1).ok_or("which job? deadbeat remove ID")?;
    let found = Store::open().update(|jobs| {
        let before = jobs.len();
        jobs.retain(|job| &job.id != id);
        jobs.len() < before
    })?;
    if !found {
        return Err(format!("no job {id}"));
    }
    if o.json {
        print_json(&json!({"removed": id}));
    } else {
        println!("Stopped watching {id}");
    }
    Ok(())
}

fn install_skill(o: &Options) -> Result<(), String> {
    let root = std::env::var_os("DEADBEAT_SKILL_DIR")
        .map(Into::into)
        .unwrap_or_else(|| store::home().join(".agents/skills"));
    let dir = root.join("deadbeat");
    let path = dir.join("SKILL.md");
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    fs::write(&path, SKILL).map_err(|e| format!("cannot write {}: {e}", path.display()))?;
    if o.json {
        print_json(&json!({"installed": path}));
    } else {
        println!("Installed {}", path.display());
    }
    Ok(())
}

fn dispatch(args: &[String]) -> Result<(), String> {
    if args.iter().any(|a| a == "--help" || a == "-h") {
        print!("{USAGE}");
        return Ok(());
    }
    let o = parse(args)?;
    match o.positional.first().map(String::as_str) {
        None => {
            eprint!("{USAGE}");
            Err(String::new())
        }
        Some("help") => {
            print!("{USAGE}");
            Ok(())
        }
        Some("version" | "--version") => {
            println!("deadbeat {}", env!("CARGO_PKG_VERSION"));
            Ok(())
        }
        Some("run") => run(&o),
        Some("add") => add(&o),
        Some("list") => {
            print_jobs(&Store::open().load(), o.json);
            Ok(())
        }
        Some("check") => check(&o),
        Some("remove") => remove(&o),
        Some("skill") if o.positional.get(1).map(String::as_str) == Some("install") => install_skill(&o),
        Some(other) => Err(format!("unknown command {other}; see deadbeat help")),
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match dispatch(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            if !message.is_empty() {
                eprintln!("deadbeat: {message}");
            }
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn strings(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn options_split_at_the_double_dash() {
        let o = parse(&strings(&["run", "--name", "x", "--json", "--", "echo", "--name"])).unwrap();
        assert_eq!(o.positional, ["run"]);
        assert_eq!(o.value("name"), "x");
        assert!(o.json);
        assert_eq!(o.command, ["echo", "--name"]);
        assert!(parse(&strings(&["--bogus"])).is_err());
        assert!(parse(&strings(&["add", "--name"])).is_err());
    }

    #[test]
    fn commands_are_quoted_only_where_needed() {
        assert_eq!(display_command(&strings(&["python3", "train.py", "--lr", "a b"])), "python3 train.py --lr 'a b'");
    }
}
