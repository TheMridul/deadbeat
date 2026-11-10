.pragma library

// Pure helpers for Panel.qml. jobs.json is written by the deadbeat binary,
// but anything on disk is untrusted here, so every field is typed and capped
// before the panel shows it.

var MAX_JOBS = 200
var MAX_TEXT = 300
var STATES = ["running", "stalled", "done", "failed", "ended", "unknown"]

function text(value, limit) {
  return typeof value === "string" ? value.slice(0, limit || MAX_TEXT) : ""
}

function number(value) {
  return typeof value === "number" && isFinite(value) ? Math.floor(value) : 0
}

function source(job) {
  var s = job.spec
  if (job.kind === "local") return text(s.command)
  if (job.kind === "ssh") return text(s.host) + ":" + text(s.log)
  if (job.kind === "ec2") return text(s.instance)
  if (job.kind === "git") return text(s.repo) + "@" + text(s.branch)
  return job.kind
}

// jobs.json text to a clean list. Bad JSON is no jobs, never an exception.
function parseJobs(raw) {
  var parsed
  try {
    parsed = JSON.parse(raw || "{}")
  } catch (e) {
    return []
  }
  var list = parsed && Array.isArray(parsed.jobs) ? parsed.jobs.slice(0, MAX_JOBS) : []
  var jobs = []
  for (var i = 0; i < list.length; i++) {
    var j = list[i]
    if (!j || typeof j !== "object") continue
    var spec = j.spec && typeof j.spec === "object" ? j.spec : {}
    var job = {
      id: text(j.id, 32),
      name: text(j.name, 120),
      kind: text(j.kind, 16),
      spec: {
        command: text(spec.command), host: text(spec.host, 255), log: text(spec.log, 1024),
        instance: text(spec.instance, 64), region: text(spec.region, 32),
        repo: text(spec.repo, 200), branch: text(spec.branch, 255)
      },
      state: STATES.indexOf(j.state) >= 0 ? j.state : "unknown",
      detail: text(j.detail),
      activity: number(j.activity),
      checked: number(j.checked),
      created: number(j.created)
    }
    if (job.id === "") continue
    job.source = source(job)
    jobs.push(job)
  }
  return jobs
}

// Counts per state, plus the two the bar cares about.
function summary(jobs) {
  var counts = { running: 0, stalled: 0, done: 0, failed: 0, ended: 0, unknown: 0 }
  var checked = 0
  for (var i = 0; i < jobs.length; i++) {
    counts[jobs[i].state]++
    checked = Math.max(checked, jobs[i].checked)
  }
  counts.attention = counts.stalled + counts.failed
  counts.total = jobs.length
  counts.checked = checked
  return counts
}

// Problems first, then live jobs, then finished ones; newest activity first.
function sorted(jobs) {
  var rank = { stalled: 0, failed: 1, unknown: 2, running: 3, done: 4, ended: 5 }
  return jobs.slice().sort(function(a, b) {
    return (rank[a.state] - rank[b.state]) || (b.activity - a.activity)
  })
}

// How long ago, as a catalog key and a count, so the caller translates it.
function ago(then, now) {
  if (then <= 0) return null
  var s = Math.max(0, now - then)
  if (s < 60) return { key: "time.justNow", count: 0 }
  if (s < 3600) return { key: "time.minutes", count: Math.floor(s / 60) }
  if (s < 86400) return { key: "time.hours", count: Math.floor(s / 3600) }
  return { key: "time.days", count: Math.floor(s / 86400) }
}

function shellQuote(value) {
  return "'" + String(value).replace(/'/g, "'\\''") + "'"
}

// A path for a remote shell, keeping a leading ~/ working.
function remotePath(path) {
  if (path === "~") return "\"$HOME\""
  if (path.indexOf("~/") === 0) return "\"$HOME\"/" + shellQuote(path.slice(2))
  return shellQuote(path)
}

// Where Enter takes a job: its log in a terminal, or its web page. An argv
// list, never a shell string, so a job's fields cannot run as commands here.
function followCommand(job) {
  var s = job.spec
  if (job.kind === "local" && s.log.charAt(0) === "/")
    return ["xdg-terminal-exec", "less", "+F", s.log]
  if (job.kind === "ssh" && s.host && s.host.charAt(0) !== "-")
    return ["xdg-terminal-exec", "ssh", "-t", s.host, "f=" + remotePath(s.log) + "; tail -n 50 -f \"$f\""]
  if (job.kind === "git" && /^[\w.-]+\/[\w.-]+$/.test(s.repo))
    return ["xdg-open", "https://github.com/" + s.repo + "/commits/" + encodeURIComponent(s.branch)]
  if (job.kind === "ec2" && /^i-[0-9a-f]+$/.test(s.instance)) {
    var region = /^[a-z0-9-]+$/.test(s.region) ? "?region=" + s.region : ""
    return ["xdg-open", "https://console.aws.amazon.com/ec2/home" + region + "#InstanceDetails:instanceId=" + s.instance]
  }
  return null
}

// aws-session --json to badge data, or null when there is nothing to show.
function parseAws(raw) {
  var o
  try {
    o = JSON.parse(raw || "")
  } catch (e) {
    return null
  }
  if (!o || typeof o !== "object") return null
  return { active: o.active === true, secondsLeft: number(o.seconds_left), error: text(o.error, 200) }
}
