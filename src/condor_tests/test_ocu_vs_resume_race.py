#!/usr/bin/env pytest

# How bad is the negotiator race when a drain resumes?
#
# `condor_drain -resume-on-completion` cancels the drain once the machine is
# drained, restoring the normal START expression.  That is the simple way to
# hand freed capacity to a reservation: drain, let it resume, let the OCU
# claim the machine like any other.  The cost is that the restored machine is
# offered to the whole pool in the same negotiation cycle, so the reservation
# has to win a race against whatever else is idle.
#
# This measures that race.  The machine has two CPUs, both busy.  Competing
# with one OCU for them are COMPETITORS ordinary idle jobs, which is the
# situation that makes draining worth doing in the first place -- on an empty
# pool there is nothing to race.
#
# Nothing here is configured to let OCUs onto draining slots: the point is to
# measure the path that needs no new configuration at all.

import logging
import time
import getpass

import htcondor2 as htcondor
import classad2 as classad

from ornithology import *

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)

COMPETITORS = 4
WATCH = 90


def slots(condor, constraint="true"):
    return condor.direct_status(
        htcondor.DaemonTypes.Startd, htcondor.AdTypes.Startd,
        constraint=constraint,
        projection=["Name", "MyAddress", "State", "Activity", "Cpus", "OCU",
                    "Draining", "RemoteUser"],
    )


def slot_table(condor):
    return "\n".join(
        "    {:<28} {:>9}/{:<6} cpus={} ocu={} draining={}".format(
            ad["Name"], ad.get("State", "?"), ad.get("Activity", "?"),
            ad.get("Cpus", "?"),
            ad["OCU"] if "OCU" in ad else "<absent>",
            ad["Draining"] if "Draining" in ad else "false")
        for ad in sorted(slots(condor), key=lambda a: a["Name"]))


def poll_until(predicate, timeout, interval=2):
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = predicate()
        if r:
            return r
        time.sleep(interval)
    return None


@config(params={"same_submitter": None, "rival_submitter": "rivals"})
def rival(request):
    """Competitors under a different accounting group are a separate submitter,
    so the negotiator divides the machine between them and the OCU's owner by
    user priority rather than by queue order."""
    return request.param


@standup
def condor(test_dir, rival):
    with Condor(
        test_dir / "condor-{}".format(rival or "same"),
        config={
            "NUM_CPUS": "2",
            "MEMORY": "4096",
            "DISK": "4096000",
            # A fast drain, which is the case where the OCU cannot get in
            # while draining and must take its chances after the resume.
            "MAXJOBRETIREMENTTIME": "0",
            "NEGOTIATOR_CYCLE_DELAY": "2",
            "NEGOTIATOR_INTERVAL": "2",
            "OCU_SUPER_USERS": getpass.getuser(),
        },
    ) as condor:
        yield condor


def submit_sleepers(condor, test_dir, path_to_sleep, name, count, group=None):
    description = {
        "executable": path_to_sleep,
        "arguments": "600",
        "log": (test_dir / "{}.log".format(name)).as_posix(),
        "should_transfer_files": False,
        "Request_Cpus": 1, "Request_Memory": 1, "Request_Disk": 1,
    }
    if group:
        description["accounting_group"] = group
        description["accounting_group_user"] = "stranger"
    return condor.submit(description=description, count=count)


@action
def race(condor, test_dir, path_to_sleep, rival):
    # Fill the machine.
    busy = submit_sleepers(condor, test_dir, path_to_sleep, "busy", 2)
    assert busy.wait(condition=ClusterState.all_running,
                     fail_condition=ClusterState.any_held,
                     verbose=True, timeout=120), "the machine never filled"

    # A deep idle queue, which is what the OCU has to beat.
    competitors = submit_sleepers(condor, test_dir, path_to_sleep,
                                  "competitors", COMPETITORS, group=rival)

    schedd = condor.get_local_schedd()
    ad = classad.ClassAd()
    ad["Owner"] = getpass.getuser()
    ad["User"] = getpass.getuser()
    ad["RequestCpus"] = 1
    ad["RequestMemory"] = 1
    ad["RequestDisk"] = 1
    created = schedd.create_ocu(ad)
    assert created["Result"] == 0

    address = slots(condor)[0]["MyAddress"]
    r = condor.run_command(
        ["condor_drain", "-resume-on-completion", address], echo=True)
    assert r.returncode == 0, "condor_drain failed: {}".format(r.stderr)

    # Three explicit phases.  Without waiting for draining to BEGIN, an early
    # poll sees "not draining" and reports a resume that never happened.
    began = poll_until(lambda: slots(condor, "Draining =?= true"), timeout=60)
    logger.debug("phase 1: draining began=%s\n%s", bool(began),
                 slot_table(condor))

    resumed = poll_until(lambda: not slots(condor, "Draining =?= true"),
                         timeout=WATCH)
    logger.debug("phase 2: resumed=%s\n%s", bool(resumed), slot_table(condor))

    got_it = poll_until(
        lambda: slots(condor, 'OCU =?= true && State == "Claimed"'),
        timeout=WATCH)
    logger.debug("phase 3: ocu_claimed=%s\n%s", bool(got_it),
                 slot_table(condor))

    yield {"began": bool(began), "resumed": bool(resumed),
           "ocu_claimed": bool(got_it),
           "table": slot_table(condor)}

    busy.remove()
    competitors.remove()
    try:
        schedd.remove_ocu(int(created["OCUId"]))
    except Exception:
        pass


class TestOCUVsResumeRace:
    def test_drain_resumed(self, race):
        """The drain must actually complete and resume, or the race never
        happens and the rest of this measures nothing."""
        assert race["resumed"], (
            "the drain never completed and resumed:\n" + race["table"])

    def test_ocu_wins_the_race(self, race):
        """Does the reservation get capacity the drain freed, with a deep
        idle queue competing for it?  A failure here is the cost of relying
        on -resume-on-completion rather than handing the slot over while the
        machine is still draining."""
        assert race["ocu_claimed"], (
            "after the drain resumed, {} competing idle jobs took the freed "
            "capacity and the OCU got none of it within {}s:\n{}".format(
                COMPETITORS, WATCH, race["table"]))
