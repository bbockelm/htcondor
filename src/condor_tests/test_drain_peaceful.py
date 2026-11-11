#!/usr/bin/env pytest

# condor_drain -peaceful does not kill the job it is draining.
#
# A peaceful drain is a graceful drain that additionally gives every retiring
# claim unlimited retirement time, so the slot comes back only when the job on
# it finishes on its own.  It is the same bargain `condor_off -peaceful` has
# always offered, reachable from the drain command.
#
# The pool here sets MAXJOBRETIREMENTTIME = 0, so a graceful drain evicts the
# running job immediately.  That makes the two modes separate cleanly:
#
#   graceful   the job is evicted and goes back to Idle
#   peaceful   the job keeps running, and the slot advertises
#              DrainingPeaceful = true
#
# Without the retirement override the peaceful case would be indistinguishable
# from a pool that simply drains slowly, which is why MAXJOBRETIREMENTTIME is
# pinned to 0 rather than left at whatever the default happens to be.

import logging
import time

import htcondor2 as htcondor

from ornithology import *

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)

# How long to watch after asking for the drain.  A graceful drain with no
# retirement time evicts within a poll or two; this is many times that.
WATCH = 40


def slots(condor, constraint="true"):
    return condor.direct_status(
        htcondor.DaemonTypes.Startd,
        htcondor.AdTypes.Startd,
        constraint=constraint,
        projection=["Name", "Machine", "MyAddress", "State", "Activity",
                    "Draining", "DrainingPeaceful", "MaxJobRetirementTime"],
    )


def job_status(handle):
    for ad in handle.query(projection=["JobStatus", "RemoteHost"]):
        return int(ad["JobStatus"])
    return None


@config(params={"peaceful": "-peaceful", "graceful": "-graceful"})
def drain_mode(request):
    return request.param


@standup
def condor(test_dir, drain_mode):
    with Condor(
        test_dir / "condor-{}".format(drain_mode.lstrip("-")),
        config={
            "NUM_CPUS": "2",
            "MEMORY": "4096",
            "DISK": "4096000",
            # A graceful drain evicts at once, so the two modes differ by the
            # retirement override alone and nothing else.
            "MAXJOBRETIREMENTTIME": "0",
            "NEGOTIATOR_CYCLE_DELAY": "2",
            "NEGOTIATOR_INTERVAL": "2",
        },
    ) as condor:
        yield condor


@action
def running_job(condor, test_dir, path_to_sleep, drain_mode):
    handle = condor.submit(
        description={
            "executable": path_to_sleep,
            "arguments": "600",
            "log": (test_dir / "job-{}.log".format(drain_mode.lstrip("-"))).as_posix(),
            "leave_in_queue": True,
            "should_transfer_files": False,
            "Request_Cpus": 1,
            "Request_Memory": 1,
            "Request_Disk": 1,
        },
        count=1,
    )
    assert handle.wait(
        condition=ClusterState.all_running,
        fail_condition=ClusterState.any_held,
        verbose=True,
        timeout=120,
    ), "the job never started, so there is nothing to drain around"

    yield handle
    handle.remove()


@action
def drained(condor, running_job, drain_mode):
    """Ask the startd to drain, then watch what happens to the job."""
    # Address the startd by its sinful rather than by hostname: a personal
    # pool's FULL_HOSTNAME can be "localhost", which daemon location cannot
    # resolve, and that has nothing to do with what is under test here.
    machine = slots(condor)[0]["MyAddress"]

    result = condor.run_command(["condor_drain", drain_mode, machine], echo=True)
    assert result.returncode == 0, "condor_drain {} failed: {}".format(
        drain_mode, result.stderr
    )

    # Watch for the whole window rather than sampling once: an eviction shows
    # up as the job going back to Idle, and we want to catch it whenever in
    # the window it happens, not only at the end.
    went_idle = False
    deadline = time.time() + WATCH
    while time.time() < deadline:
        if job_status(running_job) == 1:
            went_idle = True
            break
        time.sleep(2)

    ads = slots(condor, "Draining =?= true")
    peaceful = None
    if ads:
        peaceful = ads[0]["DrainingPeaceful"] if "DrainingPeaceful" in ads[0] else None

    logger.debug("%s: went_idle=%s DrainingPeaceful=%s", drain_mode, went_idle, peaceful)
    return {"went_idle": went_idle, "peaceful": peaceful, "machine": machine}


class TestDrainPeaceful:
    def test_slot_advertises_the_drain_mode(self, drained, drain_mode):
        """The slot says which kind of drain is in progress, so an admin (and
        this test) can tell them apart without inferring it from behavior."""
        assert drained["peaceful"] == (drain_mode == "-peaceful"), (
            "expected DrainingPeaceful={} for {}, got {}".format(
                drain_mode == "-peaceful", drain_mode, drained["peaceful"]
            )
        )

    def test_job_survives_only_a_peaceful_drain(self, drained, drain_mode):
        """With no retirement time, a graceful drain evicts the job and a
        peaceful one does not."""
        if drain_mode == "-peaceful":
            assert not drained["went_idle"], (
                "a peaceful drain evicted the job: it went back to Idle "
                "within {}s even though peaceful drains are supposed to wait "
                "for it to finish".format(WATCH)
            )
        else:
            assert drained["went_idle"], (
                "a graceful drain with MAXJOBRETIREMENTTIME=0 did not evict "
                "the job within {}s; if that is now expected, this test can "
                "no longer tell the two drain modes apart".format(WATCH)
            )
