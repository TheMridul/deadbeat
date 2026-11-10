// Tests the plugin's pure helpers. Run with: node --test tests/
import { test } from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import vm from "node:vm"

const source = readFileSync(new URL("../Model.js", import.meta.url), "utf8").replace(".pragma library", "")
const inner = vm.runInNewContext(source + "; ({ parseJobs, summary, sorted, ago, followCommand, parseAws, remotePath })")
// Values from another context fail strict deep equality, so copy them out.
const plain = (value) => (value === null || value === undefined ? value : JSON.parse(JSON.stringify(value)))
const Model = Object.fromEntries(Object.entries(inner).map(([name, fn]) => [name, (...args) => plain(fn(...args))]))

const file = (jobs) => JSON.stringify({ jobs })

test("bad files are no jobs, never a crash", () => {
  assert.deepEqual(Model.parseJobs("not json"), [])
  assert.deepEqual(Model.parseJobs(""), [])
  assert.deepEqual(Model.parseJobs('{"jobs": "nope"}'), [])
  assert.deepEqual(Model.parseJobs(file([null, 3, { name: "no id" }])), [])
})

test("fields are typed and capped", () => {
  const [job] = Model.parseJobs(file([{
    id: "abc123", name: "x".repeat(500), kind: "local", state: "exploded",
    detail: 42, activity: "soon", spec: { command: "make", log: "/tmp/a.log" }
  }]))
  assert.equal(job.name.length, 120)
  assert.equal(job.state, "unknown")
  assert.equal(job.detail, "")
  assert.equal(job.activity, 0)
  assert.equal(job.source, "make")
})

test("markup in a job stays text for the plain-text labels", () => {
  const evil = '<img src="https://example.invalid/tracker">'
  const [job] = Model.parseJobs(file([{ id: "a", name: evil, kind: "local", detail: evil, spec: {} }]))
  assert.equal(job.name, evil)
  assert.equal(job.detail, evil)
})

test("problems sort first and the bar counts them", () => {
  const jobs = Model.parseJobs(file([
    { id: "a", state: "done", activity: 5 },
    { id: "b", state: "running", activity: 9 },
    { id: "c", state: "stalled", activity: 1 },
    { id: "d", state: "failed", activity: 2, checked: 70 }
  ]))
  assert.deepEqual(Model.sorted(jobs).map((j) => j.id), ["c", "d", "b", "a"])
  const s = Model.summary(jobs)
  assert.equal(s.attention, 2)
  assert.equal(s.running, 1)
  assert.equal(s.checked, 70)
})

test("ago picks a unit", () => {
  assert.equal(Model.ago(0, 100), null)
  assert.equal(Model.ago(100, 130).key, "time.justNow")
  assert.deepEqual({ ...Model.ago(0 + 1, 1 + 300) }, { key: "time.minutes", count: 5 })
  assert.deepEqual({ ...Model.ago(1, 1 + 7200) }, { key: "time.hours", count: 2 })
  assert.deepEqual({ ...Model.ago(1, 1 + 3 * 86400) }, { key: "time.days", count: 3 })
})

test("follow commands are argv lists that refuse odd input", () => {
  const job = (kind, spec) => Model.parseJobs(file([{ id: "a", kind, spec }]))[0]
  assert.deepEqual([...Model.followCommand(job("local", { log: "/s/a.log" }))], ["xdg-terminal-exec", "less", "+F", "/s/a.log"])
  assert.equal(Model.followCommand(job("local", { log: "relative.log" })), null)
  assert.equal(Model.followCommand(job("ssh", { host: "-oProxyCommand=evil", log: "x" })), null)
  const ssh = Model.followCommand(job("ssh", { host: "box", log: "~/a 'b'.log" }))
  assert.equal(ssh[4], `f="$HOME"/'a '\\''b'\\''.log'; tail -n 50 -f "$f"`)
  assert.equal(Model.followCommand(job("git", { repo: "o/r;rm -rf", branch: "main" })), null)
  assert.equal(Model.followCommand(job("git", { repo: "o/r", branch: "a/b" }))[1], "https://github.com/o/r/commits/a%2Fb")
  assert.equal(Model.followCommand(job("ec2", { instance: "i-0abc", region: "us-east-1" }))[1],
    "https://console.aws.amazon.com/ec2/home?region=us-east-1#InstanceDetails:instanceId=i-0abc")
  assert.equal(Model.followCommand(job("ec2", { instance: "i-0abc&x=1" })), null)
})

test("aws badge data", () => {
  assert.equal(Model.parseAws(""), null)
  const aws = Model.parseAws('{"active": true, "seconds_left": 600}')
  assert.equal(aws.active, true)
  assert.equal(aws.secondsLeft, 600)
})
