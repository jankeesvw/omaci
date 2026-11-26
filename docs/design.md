# Omaci, redesigned

The app was called CI Watch until this redesign.

This is the design behind the `fable-redesign` branch: what the window shows, why, and how the parts that guess (the time left, where a failure is) come to their answer.

## The three questions

The old window was a browser for runs: a list of workflow runs, the jobs of one, the log of one job. That answers "what does GitHub have", and it left the three questions people actually open the window with to the reader.

1. **How is the build going, and how long will it still take?** A row that says "running, 3m 10s" does not say whether that is early or late. Only earlier builds can say that.
2. **It failed. What failed?** The job is red and the step is red, but the reason is ninety lines above the last line of a six hundred line log, and the step that names it could not be clicked.
3. **How do I hand this to somebody else?** In practice to a coding agent, which wants a URL.

The redesign is organised around those three, in that order, top to bottom.

## A build is a commit

The unit in the list is no longer a workflow run but a build, defined the way the [GitHub Build Monitor](https://github.com/jankeesvw/omarchy-github-build-monitor) bar widget defines it: a commit, with every run that a push, a pull request or a dispatch started for it rolled into one. It is running while any of those runs is, red when any of them failed, and it lasts from the first run starting to the last one ending, which is the wait a person sits through. One push to main starts "Tests & Deploy" and "Linting & Security"; they are one row, and their jobs are one list.

Runs started by anything else (a schedule, an issue comment, Dependabot's own updater) also attach to the head of the branch. Rolling those in would stretch a four minute build to twelve hours because a nightly job ran on the same commit, so each of them stays an entry of its own.

`omaci <repo> --sha <sha>` (then `ciwatch`) used to filter the list down to that commit. Now it selects that build and leaves the others around it, because the others are what the estimate is made from. A commit too old to be in the list is fetched by its sha and added.

## The window

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ acme/storefront   [All branches]            2 running    Updated 3s ago   ?  │
├───────────────┬──────────────────────────────────────────────────────────────┤
│ BUILDS        │ ● Add gift cards to checkout                 about 3m 50s left│
│ ● Add gift…   │   main · 4f1c2ab · push · mira            usually 7m 10s      │
│   ▓▓▓▓░░ 3m50 │   ▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░[▒▒▒|▒▒]            ▁▃▂▅▃▂▃▄▃ ▌       │
│ ✕ Show order… ├──────────────────────────────────────────────────────────────┤
│ ✓ Bump rails  │ JOBS       0        2m        4m    now   6m        8m        │
│ ✓ Fix round…  │ ✓ changes  ▓                         │                        │
│ ✓ Send a re…  │ ● test       ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│▒▒▒▒▒▒▒▒     3m / ~4m 40s │
│               │ ● build      ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│▒▒▒         3m / ~3m 30s │
│               │ ○ deploy                             │      ░░░░░░░   ~1m 20s  │
│               ├───────────────┬──────────────────────────────────────────────┤
│               │ STEPS  test   │ test · step 5 of 6      Copy link  Open  …   │
│               │ ✓ Set up job  │ ┌ 1 failure in RSpec tests ────────────────┐ │
│               │ ✓ Check out   │ │ spec/models/order_total_spec.rb:18 …     │ │
│               │ ✕ RSpec tests │ └──────────────────────────────────────────┘ │
│               │ – Save runti… │ ── RSpec tests ──────────────────────────────│
│               │               │ 10:26:45  Failures:                          │
└───────────────┴───────────────┴──────────────────────────────────────────────┘
```

From the top:

- **Builds**, on the left, newest first, drawn like `git log --graph`: the builds of the default branch on a trunk line down the left with their branch name in the accent colour, the builds of every other branch set in off that line, with a dimmer title and their branch in the info colour, so what is on main reads at a glance. A running one carries a thin progress line and says how long it has left instead of how long it has run. A finished one says how long it took.
- **The build**, across the top of the rest. The answer to question one is the largest text in the window: "About 3m 50s left", or "Passed in 7m 2s", or "Failed after 1m 27s". While the build runs it is in the window's title as well, where the window switcher shows it. A build in which one job has already failed while others still run turns red at once and says so. Under it one bar: the filled part is the time gone, the whole bar is the time expected, a tick marks the usual finish and a soft band around it marks how far the earlier builds spread. Next to it the earlier builds themselves, one small bar each, as tall as the build was long and coloured by how it ended, so "usually seven minutes" has something to look at behind it and a build that has been getting slower shows as a slope.
- **Jobs**, as a timeline rather than a list. Every job is a row on the same time axis, starting where it started. Done is solid, the part still expected is drawn hollow, a job that has not started yet is hollow where it usually runs, and a line marks now. One look says which job the build is waiting for. Skipped jobs did nothing and share one line under the rows. Jobs that GitHub has not created yet (a `deploy` that needs `test` and `build`) are shown as expected rows, taken from the earlier builds, because a timeline that ends where `test` ends would promise a finish that is not coming.
- **Steps** of the picked job, each with how long it took, and for the running one how long it usually takes. Every step that wrote something is a button: it scrolls the log to where that step ends, with its last line at the bottom of the view, because the outcome of a step (a test summary, the error a command died on) is what it writes last. The step that is still running is not in the log yet, because GitHub has no log of it until it is done; it takes you to the end of the log, where it waits as a heading.
- **The log**, with a heading line where each step begins, so it reads as chapters rather than one stream. While you scroll, the step you are reading is the one highlighted in the list. Above it, when the job failed, a short panel with the failures by name.

`Z` hides the build and the timeline and gives the whole height to the log.

## How long it will still take

Everything here is computed in `src/estimate.cpp`, as functions of "the jobs as they are now, the earlier builds, and the clock", so the tests feed it numbers and check the answer.

**Which earlier builds count.** Builds that passed, older than this one, and like it: on the same branch when there are at least three of those, otherwise started by the same kind of event (so a pull request is compared with other pull requests, which do not deploy), otherwise all of them. Builds that failed or were cancelled stopped early and would make everything look fast.

**The first answer** needs nothing but the list of runs the app already has: the median wall clock of those builds. It is there the moment the window opens and is what the rows in the build list use.

**The better answer** uses the jobs of the last six like builds, which the app fetches once (a finished run's jobs never change) when the build you are looking at is still going. From them, for every job: how long it usually takes, and which jobs had always finished before it started. That second part is how the app knows `deploy` waits for `test` without reading the workflow file. The build is then played forward: a running job ends at its start plus its usual length, or now if it is already past that; a job that has not started begins when the jobs it waits for end; the build ends when the last job does. Only earlier builds in which the jobs running now also ran are used, which matters in a repository where a documentation change skips the tests: once `test` is running, the fifteen second builds stop counting.

**Uncertainty** is shown, not hidden. The play-forward is done three times, with every job's lower quartile, median and upper quartile. The median gives the number, the two quartiles give the band on the bar and the "usually 6m 50s to 7m 40s" under it. The number itself is rounded to how much it can be trusted: to five seconds under a minute, ten under five minutes, half a minute above that, and it always says "about". With fewer than three earlier builds to go on there is no estimate: the bar turns into a plain moving stripe and the text says how long the build has been running and why there is no guess. A build that has outlived its estimate says "taking longer than usual" in yellow with how far over it is, rather than sitting at "0s left".

## Finding what failed

Two separate things were wrong, and they are fixed separately.

**Steps are places in the log.** `GitHub::mapSteps` gives every step the line it starts on. The jobs API times a step to the second and several steps often start in the same second, so times alone cannot tell them apart; the log, though, has its own markers in step order. The walk goes through the steps that ran, in order, and gives each one the next marker of its kind that falls inside its time window: `##[group]Run …` for a step of the workflow, `Post job cleanup.` for a post step, `##[group]Checking docker version` for "Initialize containers", `Print service container logs` for "Stop containers", `Cleaning up orphan processes` or `Evaluate and set job outputs` for "Complete job", and line one for "Set up job". A step with no marker in its window falls back to the first line at or after its start time. Skipped steps get nothing and cannot be clicked. A step's window is its start second up to, but not including, the second after its end, because the API cuts times off rather than rounding them. Checked against the per-step files of GitHub's own log archive for the four jobs of a real run: all 42 steps that wrote something land on the same line. The run's log archive has exact per-step files, but it only exists once the run is over and has to be unzipped, and the markers get the same answer on a job that is still running.

**A failure is what the test runner printed, not what the runner said afterwards.** The only `##[error]` line in a failed RSpec job is the last one, "Process completed with exit code 1.", and that is where the old window landed. `src/failures.cpp` reads the log of the failed step for what test runners print: RSpec's `Failures:` section and its `rspec ./spec/…:107` lines, Cucumber's `Failing Scenarios:`, Minitest's `Failure:` and `Error:` blocks, pytest's `FAILED …`, Jest's `●` lines, Go's `--- FAIL:`, and `##[error]` annotations that carry a file. "Process completed with exit code" is kept as the last resort, for a job where nothing else matched.

A failed job opens with the first failure two lines below the top of the log, so the whole block is in view under it. The lines of a failure have a red edge, so they are findable while scrolling. The panel above the log names each failure and where it is (`spec/requests/admin/orders_spec.rb:107`); click one to go there, `E` and `Shift+E` walk them.

## Next to the build before it

"Is this slow, or is it always like this" and "did this break it, or was it already broken" are both questions about the build before. So every build is shown next to the one main had before it: the newest finished build on the default branch that started earlier. Whatever its result, because a main that was already red is exactly what you want to know before blaming a pull request. A pull request is compared with main too, not with its own previous push, since main is what it will be merged into.

It shows in three places. A row under the bar names that build, how it ended, how long it took and how long ago, then says what changed in a few words, the worst first ("test broke, build 1m 9s slower"), and is itself the way there: click it or press `B`. `B` again walks further back, and `Shift+B`, or the button that appears under the row, returns to the build the walk began on. On the timeline every job has a thin line under its bar where the same job ran last time, on the same axis, so slower and later are visible without reading a number. And a column on the right gives the job's time then, with an arrow up in yellow when it is slower now, an arrow down in green when faster, and "passed, 4m 20s" in red next to a job that fails now and did not then.

The rules are in `src/compare.cpp`. A job broke when it fails now and did not before, and is fixed the other way round. Two times differ when they are more than five seconds and more than fifteen percent apart; less than that is the runner's mood and gets no colour. A job that is still running says nothing until it has outlasted its last time. A job that only this build has is "new"; one that only the earlier build had is reported as not having run, but only once this build is done, because while it runs GitHub may simply not have created the job yet. The whole build is compared only when both passed.

Which branch is "main" is asked from GitHub once per repository (`default_branch`); until the answer is in, `main` or `master` is taken. The jobs of the previous build are one request per run, once, and often already there because the forecast uses the same builds.

## Handing it over

`C`, or the "Copy link" button, copies the URL of the picked job, `https://github.com/owner/repo/actions/runs/<run>/job/<job>`, or the run's when no job is picked. A toast shows what was copied.

`Y` copies the failure itself for an agent: one line saying what failed and where, the link, and the text of the failure blocks. `Shift+C` copies the whole log, which is what `C` used to do. `O` opens the job on GitHub, on the step you picked when you picked one (`#step:14:1`).

## Keyboard

Every one of these is also something to click.

| Key | Does |
| --- | --- |
| `J` / `↓`, `K` / `↑` | next, previous build |
| `L` / `→` / `Tab`, `H` / `←` / `Shift+Tab` | next, previous job |
| `N`, `P` | next, previous step: scrolls the log there |
| `E`, `Shift+E` | next, previous failure |
| `C` | copy the link of the job |
| `Y` | copy the failure, with the link, for an agent |
| `Shift+C` | copy the whole log |
| `O` / `Enter` | open the job on GitHub |
| `B`, `Shift+B` | the build main had before this one, and back to where you were |
| `Z` | give the whole height to the log, and back |
| `W` | wrap long log lines onto the next line, and back; on at the start |
| `M` | only the builds of main, and back to all |
| `F` | only the branch of this build, and back to all |
| `A` | only the builds that are running, and back to all |
| `S` | show or hide skipped builds |
| `PgUp` / `PgDn`, `Home`, `End` | scroll the log, top, follow the end |
| `Space` / `R` / `Ctrl+R` | refresh now |
| `G` | the menu of repositories, to watch another |
| `?` | every shortcut |
| `Esc` | close the help, or leave the tall log |
| `Q` | quit |

## What it costs

- **The list of runs**: one request a poll, a hundred runs at a time, which is about thirty-five builds of history. Sent with the `ETag` of the last answer, and GitHub does not count a "not modified" answer against the hourly budget, so an idle minute usually costs nothing.
- **The jobs** of the build you are looking at: one request per run that is still going (usually one or two), also conditional. A finished run's jobs are fetched once and kept for as long as the window is open.
- **The log** of the picked job. GitHub publishes the log of a step once the step is done and the log of a job once the job is, and nothing of either before that: `actions/jobs/{id}/logs` answers 404 for a job that is running, also when most of its steps are done, and the old `tail_logs=true` that once gave part of it stopped working in 2025. So while a job runs, every step that finishes is fetched once from `actions/jobs/{id}/steps/{n}/logs` and added at the bottom, in order, waiting for a step that is still on its way so the log only ever grows; steps that come back in the same poll are added in one go, because lines that slide in while others still do can be left halfway. Once the job is done, its whole log is fetched once, and it is exactly those steps one after the other, so it carries on from what was there. Logs are the requests that always count.
- **The build before**: the jobs of the previous build on main, a request per run, once per build you look at, and which branch is the default, one request per repository.
- **The menu of repositories**: `user/repos`, a hundred at a time, most recently pushed first, when the menu opens and again at most every five minutes. The first time the menu fills page by page; after that the old list stays until the new one is whole.
- **History for the estimate**: the jobs of the last six like builds, about a dozen requests, once, and only when the build you are looking at is running.

Polling is every 6 seconds while a build is running and once a minute otherwise. A running build costs a counted request per step of the job you watch, as the step finishes, plus the jobs requests that actually changed (a few per step), against 5000. At most four `gh` processes run at a time.

## The look

The window reads like a terminal program in the family of btop and lazygit, because that is what the rest of an Omarchy desktop looks like. The layout and what is on it did not change for this; only how it is drawn.

- **One font, one size.** The desktop's monospace font, which is what fontconfig's `monospace` resolves to and what Omarchy's font setting writes, at a size taken from the shell's `[font] base-size` (the user's `~/.config/omarchy/shell.toml` first, then the theme's, then 12). `Theme` measures one character of it and everything sits on that grid: widths in cells, rows a little over a line high. Text is hinted to whole pixels, as a terminal does it. The one exception is the headline, "About 3m 20s left", at one and a half times the size, because it is what the window is opened for.
- **Frames.** Every panel (`builds`, `build`, `jobs`, `steps`, `log`) is a one pixel line with its title set into the top edge, a quieter note after it (`jobs ─ 3 running, 1 to come`) and a count in the far corner. The frame of the panel the keys last acted on is in the accent. Corners take Hyprland's `decoration:rounding`, asked when the app starts and when the theme changes: 0 on a stock Omarchy, so sharp. No fills, shadows or ripples; the Material style is gone and the few controls left (scroll bars) are drawn by the app.
- **Bars are block characters, drawn as cells.** The progress bar is `███████░░░░▒▒┃▒▒`: full blocks for the time gone, light shade for what is left, medium shade for the band earlier builds ended in, a bar where one usually ends. The jobs are rows of blocks on a character-cell axis, shaded where still expected, with `╌` for the slow end and a `▁` line under each for where the job ran in the build before. They are drawn the way terminal emulators draw these characters, as the cells themselves rather than from the font, whose blocks are a hair narrower than a cell and would leave a seam between every two. The earlier builds are a real sparkline, `▅▆▅▆▃▅▆▁▆`.
- **Marks and keys are text.** `✓ ✗ ○ ◌ – ⊘` and a braille spinner for running. Actions are `[c] copy link`, `[y] copy failure`, `[b] go to it`: the key in the accent, the whole thing clickable and lit in the accent under the pointer. Capitals are the shifted keys: `[B]`, `[C]`, `[E]`.

## What stays the same

Nothing is written to disk but the name of the repository watched last, so a window started from the launcher opens on it. `gh` is the only way to GitHub. Log lines are plain text. The whole Omarchy palette is followed live now, not only the accent, so the window is light on a light theme. `--demo` shows invented builds for an invented repository, and the hidden `--screenshot <file>` draws the window offscreen into a PNG, which is how the screenshots in the README are made and how the layout was checked while building it.

## Known limits

- A run that starts on a commit long after its build ended (a workflow that fires when the pull request is merged, a re-run the next day) makes that build "running" again and stretches its length. The estimate uses medians, so one such build does not move it, and the history bars are capped at twice the usual so it does not flatten the others.
- The forecast learns the shape of a build from the last six that passed. For the first builds after a workflow gains or loses a job it is wrong about that job.
- A step of the workflow whose name starts with "Post " or "Pre " is taken for a post or pre step of an action.
- A composite action writes `##[group]Run …` lines of its own inside a step. They are only mistaken for the next step when that step starts in the same second.

## Left out

- Searching in the log.
- Reading the workflow file for the real `needs:` graph. The order is inferred from earlier builds instead, which costs nothing and is wrong only for the first builds after a workflow changes shape.
- The run's log archive as a source of exact step boundaries.
- Keeping the logs of finished jobs: going back to a job fetches its log again.
- Translations, which the Omarchy app template has and this app never did.
- Following a change of Hyprland's rounding or of the text size the moment it happens: both are read at start and again when the theme changes, not from Hyprland's event socket.
