<!-- SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com> -->
<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Weekly stable-update publisher

The minihost user timer proposes updates from the stable branch recorded in
`.github/upstream.json`. The independent `upstream-preview.yml` workflow checks
development master. Neither advances `plasma-fusion`.

## Publication contract

`publisher.py`, `upstream.py` and `publisher_report.py` are installed from a reviewed,
checksum-verified revision into `~/.local/lib/kwin-upstream-publisher/`. They are
not loaded from a fetched candidate. Updating that installation is a manual,
reviewed operation.

Each invocation uses a fresh bare repository with empty templates and isolated
Git configuration. It fetches the fork and stable branch, prepares the explicit
baseline integration tree, and advances the manifest inside that tree. There is
no source checkout or build on the signer. Changes to `.github`, `.gitattributes`
or `.gitmodules`, apart from the generated manifest, require manual integration.

The branch name contains both full input commits. A new proposal is one signed
commit whose only parent is the fetched fork head. It uses the existing owner
identity, DCO and signing key. Branch creation uses Git's empty-expectation lease;
it cannot replace even an existing fast-forwardable branch.

An existing branch must have the exact expected tree, parent, identity, message
and approved signature. Its original commit is reused. Pull requests are queried
in all states: an existing open PR must match, while closed or mismatched PRs stop
the run. A lost response after branch or PR creation can therefore be reconciled
by a later run without generating another signed candidate.

New PRs are drafts. The required PR checks and source review still apply before
the owner approves a merge. The publisher checks for a moved fork head before
and after publication; these checks are not a cross-service transaction. A
concurrent movement can leave a draft branch/PR requiring inspection, never an
automatic main-branch update.

## Commands and records

Run as the existing minihost user, with its current GitHub and GPG access:

```sh
python3 -Es ~/.local/lib/kwin-upstream-publisher/publisher.py --check
python3 -Es ~/.local/lib/kwin-upstream-publisher/publisher.py --publish
python3 -Es ~/.local/lib/kwin-upstream-publisher/publisher.py --test-alert
```

`--check` fetches and prepares only. `--publish` creates or reconciles a proposal.
`--test-alert` sends one clearly marked notification through the existing private
channel. No channel value or credential is included in logs or repository files.

The state directory defaults to `~/.local/state/kwin-upstream-publisher/` and must
be private to the current user. `latest.json` describes the latest run, and
`runs/<run-id>/` retains integration diagnostics and the result. A nonblocking
`flock` serializes local invocations; a busy invocation leaves the active result
alone. Git and GitHub commands have 120-second limits. Failure alerts contain a
run identifier and local inspection instructions, not raw remote diagnostics.
`alert_accepted` means the notification server acknowledged the message; it is
not proof that a device displayed it.

Status records are bounded to 128 KiB of serialized JSON; the reporter uses the
same limit. Larger integration diagnostics remain in `integration.json`, outside
the status/acknowledgement record. Existing unreadable or oversized status files
are never treated as missing and overwritten by a delayed failure handler.

The independent `kwin-upstream-publisher-failure.service` is activated by
`OnFailure=`. It handles timeout, forced termination, startup failure and OOM,
including cases where the publisher cannot execute its own error handler. The
scheduled service explicitly opts into systemd invocation IDs; ordinary manual
runs use fresh IDs even when their shell inherits a service environment.

The failure service preserves the original per-run result and writes
`supervisor.json` alongside it, plus `last-service-failure.json`. It replaces a
matching stale `latest.json` but does not overwrite a newer run. An already
acknowledged alert is not sent again. A lost notification acknowledgement may
result in another alert; this is not an exactly-once delivery protocol.
Interrupted temporary repositories are retained with the run for inspection;
remove them only after identifying the finished run and preserving its evidence.
Early supervised storage errors leave notification to the independent handler
instead of sending an unrecorded alert from the publisher first.

## Install and rollback

Before enabling the timer, verify the installed files against the reviewed
revision, run the tests on minihost, validate the unit files, exercise the real
read-only path, and verify a bounded publication and its repeated invocation.
Test notification acceptance separately.

The supplied user timer runs Mondays at 03:10 UTC with up to five minutes of
jitter. `Persistent=true` catches a missed firing after the user manager returns.
The master preview and current-fork CI have their own schedules and results.

After copying both verified services and the timer into `~/.config/systemd/user/`:

```sh
systemd-analyze --user verify ~/.config/systemd/user/kwin-upstream-publisher*.service ~/.config/systemd/user/kwin-upstream-publisher.timer
systemctl --user daemon-reload
systemctl --user enable --now kwin-upstream-publisher.timer
systemctl --user list-timers kwin-upstream-publisher.timer
```

To stop future runs:

```sh
systemctl --user disable --now kwin-upstream-publisher.timer
```

If an invocation is active, inspect it before stopping the service. Retain the
state and published refs for reconciliation. Roll back the installed scripts to
the previous verified revision with the timer disabled. There is no automatic
branch deletion, force-replacement, merge, package installation or key change.
Do not manually start or restart the scheduled publisher while its failure
service is still activating. Wait for that handler to finish first; the fixed
`OnFailure` unit is not a queue for rapid repeated failures.

## Validation and command references

```sh
python3 -m unittest discover -s .github/ci -p 'test_*.py'
ruff check .github/ci/publisher*.py .github/ci/test_publisher*.py
ruff format --check .github/ci/publisher*.py .github/ci/test_publisher*.py
```

The publication tests use disposable local Git repositories and an isolated
temporary GPG key. GitHub responses are simulated there; live qualification
separately checks the installed GitHub CLI and existing user-service identity.

Relevant command contracts (verify against the installed versions):

- [git push](https://git-scm.com/docs/git-push): `--force-with-lease=<ref>:`
  requires the remote ref to be absent.
- [git commit-tree](https://git-scm.com/docs/git-commit-tree): explicit parent,
  tree and signing key.
- [git configuration](https://git-scm.com/docs/git): `GIT_CONFIG_GLOBAL` and
  `GIT_CONFIG_NOSYSTEM`; the source repository's local config is not copied.
- [gh pr list](https://cli.github.com/manual/gh_pr_list): all-state lookup and
  explicit JSON fields.
- [gh pr create](https://cli.github.com/manual/gh_pr_create): explicit `--head`
  skips implicit push/fork behavior.
- [systemd.timer](https://www.freedesktop.org/software/systemd/man/latest/systemd.timer.html):
  calendar, randomized delay and persistence.
