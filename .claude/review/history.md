# Review lessons from Botty+ history

These are patterns extracted from 125 historical Cubic inline comments on PRs
#1, #3, #7, #8, #11, #14 and #16. They describe past bugs and test gaps, not
claims that those bugs remain. Use only patterns relevant to the current diff;
verify any finding in the current implementation before posting it.

## Follow production paths beyond the changed line

Accepting root ownership did not make `fchmod(dir, 0700)` work for UID 1.
Trace every later syscall under the actual effective identity, not just the new
predicate. A predicate-only unit test did not exercise runtime publication.
[PR #14](https://github.com/Portablelle/Botty-Plus/pull/14#discussion_r4223803011)

A file-open wrapper logged before returning; logging could overwrite `errno`,
so an `EMFILE` retry checked the wrong error. Inspect pinned upstream callees,
cleanup and logging between failure and recovery; preserve the original error.
[PR #16](https://github.com/Portablelle/Botty-Plus/pull/16#discussion_r4224622634)

## Prove the test reaches the claimed behavior

An artwork negative test passed because its source fixture lacked the query key;
the identity guard never ran. Assert the provider/production path was reached,
then demonstrate a failing test when the guard is removed or the bug restored.
[PR #1](https://github.com/Portablelle/Botty-Plus/pull/1#discussion_r4206308991)

Package-revision tests checked only rejection: a verifier rejecting every binary
would still pass. Pair acceptance with rejection, and cover missing fields as
well as malformed ones. Never silently pass when a required manifest is absent.
[PR #11](https://github.com/Portablelle/Botty-Plus/pull/11#discussion_r4213611517)

Preview update modes never reached POST because their state fixture omitted the
availability field. Follow state -> enabled action -> encoded endpoint -> result;
assert the request and terminal behavior, not only a helper's output.
[PR #11](https://github.com/Portablelle/Botty-Plus/pull/11#discussion_r4212487417)

## Compare independent producer and consumer contracts

Package verification trusted the manifest as its entire file set, so omitted
required files and unlisted extras escaped checks. Compare against an independent
runtime inventory; reject symlinked parents and special files before opening.
[PR #7](https://github.com/Portablelle/Botty-Plus/pull/7#discussion_r4209219570)

Source revision strings did not prove the shipped ELF matched them. Verify the
compiled marker and UI bytes, not just self-consistent manifests and hashes.
[PR #11](https://github.com/Portablelle/Botty-Plus/pull/11#discussion_r4213469444)

Installer/package limits disagreed at exactly 16 MiB, version parsers disagreed
on component widths, and compressed-payload bounds differed from uncompressed
bounds. Check equality boundaries, fixed-buffer capacities, missing contract
fields and both directions of API/version compatibility.
[PR #11](https://github.com/Portablelle/Botty-Plus/pull/11#discussion_r4213141645)

## Walk startup, concurrent shutdown and partial recovery

An updater started before its own HTTP endpoint was bound and postponed the
next automatic check for six hours. Follow dependency readiness and failure
scheduling across startup, not just the initializer.
[PR #11](https://github.com/Portablelle/Botty-Plus/pull/11#discussion_r4213141715)

A bounded exit-poll loop still used a blocking connect. A detached thread outlived
its stack-owned server; a check-then-assign of `std::thread` raced under concurrent
requests. Account for the time spent inside each operation, object lifetime,
locks, joins and cancellation of in-flight network work.
[PR #11](https://github.com/Portablelle/Botty-Plus/pull/11#discussion_r4213469458)

Failed staging left an immutable partial release that retries could not complete;
a killed publisher left a PID-named temporary causing EEXIST after PID reuse.
Trace every failure point, what survives a crash, and what the next attempt does.
[PR #11](https://github.com/Portablelle/Botty-Plus/pull/11#discussion_r4213141738)

## Trace CI trust boundaries and real command execution

Public PR jobs could read persistent runner credentials even with mode 0600,
because the job UID owned them. Follow identity, credential lifetime, filesystem
mounts, network reachability and permissions through the actual container.
[PR #8](https://github.com/Portablelle/Botty-Plus/pull/8#discussion_r4209771639)

A child `bash -c` did not inherit `set -e`; a successful final command masked a
failed build. A JIT config in an environment variable was never passed to run.sh.
Trace exit codes through each shell, subshell, pipeline and command invocation.
[PR #8](https://github.com/Portablelle/Botty-Plus/pull/8#discussion_r4210039905)

## Check supported inputs and operator documentation

Normalization depended on suffix order and erased bracketed ID evidence before
detecting release names. Exercise reversed order, delimiters and unmarked
canonical names; prove normalization does not erase identity information.
[PR #3](https://github.com/Portablelle/Botty-Plus/pull/3#discussion_r4206565089)

Documented build commands omitted a compressor prerequisite; validation docs
attributed tests from Portal+ to Botty+. Follow commands from the stated working
directory and platform. Check Python/library requirements, compiler wrappers,
transitive includes and what each claimed test actually runs.
[PR #7](https://github.com/Portablelle/Botty-Plus/pull/7#discussion_r4209418198)
