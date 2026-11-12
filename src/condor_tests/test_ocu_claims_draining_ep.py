#!/usr/bin/env pytest

# An OCU can take a claim on an EP that is already draining.
#
# The companion test, test_ocu_and_draining.py, covers an OCU claim that was
# taken before the drain started.  This one is the other order: the EP is
# already draining and an OCU wants capacity on it, which is what a
# reservation wants when a drain is being run in order to free the machine
# for it.
#
# No new mechanism is needed for this.  A draining slot's Requirements comes
# from the drain's own START expression -- `condor_drain -start <expr>`, which
# the startd holds as soft state for the life of the drain -- and an OCU's
# request ad carries IsOCUHolder.  So
#
#     condor_drain -start 'IsOCUHolder =?= true'
#
# drains the machine and admits an OCU, and nothing else, onto what it frees.
# A drain with no -start sets Requirements to false and admits nothing, which
# is the existing behavior and is what the "closed" case below checks.

import logging
import time
import getpass

import htcondor2 as htcondor
import classad2 as classad

from ornithology import *

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)

WATCH = 60

OCU_CLAIM_CONSTRAINT = 'State == "Claimed" && Activity == "Idle" && OCU =?= true'

SLOT_PROJECTION = ["Name", "MyAddress", "State", "Activity", "Cpus",
                   "PartitionableSlot", "RemoteUser", "Draining", "OCU",
                   "Requirements", "Start"]


def slots(condor, constraint="true"):
    return condor.direct_status(
        htcondor.DaemonTypes.Startd,
        htcondor.AdTypes.Startd,
        constraint=constraint,
        projection=SLOT_PROJECTION,
    )


def slot_table(condor):
    return "\n".join(
        "    {:<30} {:>9}/{:<6} cpus={} ocu={} draining={}\n"
        "        Start        = {}\n"
        "        Requirements = {}".format(
            ad["Name"],
            ad.get("State", "?"),
            ad.get("Activity", "?"),
            ad.get("Cpus", "?"),
            ad["OCU"] if "OCU" in ad else "<absent>",
            ad["Draining"] if "Draining" in ad else "false",
            ad["Start"] if "Start" in ad else "<absent>",
            ad["Requirements"] if "Requirements" in ad else "<absent>",
        )
        for ad in sorted(slots(condor), key=lambda a: a["Name"])
    )


def poll_until(predicate, timeout, interval=2):
    deadline = time.time() + timeout
    while time.time() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(interval)
    return None


OCU_ONLY = "IsOCUHolder =?= true"


@config(
    params={
        # -start governs while the drain is running; DRAIN_FINAL_START_EXPR
        # governs once the machine is drained, which is when a reservation
        # actually wants the capacity.
        # Retirement of 0 means the drain reaches its final phase at once, so
        # these exercise a machine that is already fully drained.
        "admits_ocus": {"id": "admits", "start": OCU_ONLY, "final": OCU_ONLY,
                        "retirement": "0", "expect": True},
        # A control: if an OCU cannot claim a drained EP even when it admits
        # everything, the problem is not the expression.
        "admits_anything": {"id": "anything", "start": "true", "final": "true",
                            "retirement": "0", "expect": True},
        "closed": {"id": "closed", "start": None, "final": None,
                   "retirement": "0", "expect": False},
        # The realistic case: a long retirement time keeps the drain in
        # progress, so the drain's own -start expression is still in force and
        # slots are claimable as they free.  No DRAIN_FINAL_START_EXPR.
        "mid_drain": {"id": "middrain", "start": OCU_ONLY, "final": None,
                      "retirement": "3600", "expect": True},
    }
)
def drain_case(request):
    return request.param


@standup
def condor(test_dir, drain_case):
    extra = {}
    if drain_case["final"]:
        extra["DRAIN_FINAL_START_EXPR"] = drain_case["final"]
    if drain_case["start"]:
        # The schedd stamps "Draining =!= true" onto every OCU request unless
        # the pool says otherwise, so without this no OCU can match a draining
        # slot no matter what the slot is willing to admit.
        extra["OCU_MATCH_DRAINING_SLOTS"] = "true"
    with Condor(
        test_dir / "condor-{}".format(drain_case["id"]),
        config={
            "NUM_CPUS": "2",
            "MEMORY": "4096",
            "DISK": "4096000",
            "MAXJOBRETIREMENTTIME": drain_case["retirement"],
            "NEGOTIATOR_CYCLE_DELAY": "2",
            "NEGOTIATOR_INTERVAL": "2",
            "OCU_SUPER_USERS": getpass.getuser(),
            **extra,
        },
    ) as condor:
        yield condor


@action
def seed_job(condor, test_dir, path_to_sleep, drain_case):
    """Runs before the drain so the schedd has a SubmitterData record for this
    user; without one the negotiator never negotiates for the OCU."""
    handle = condor.submit(
        description={
            "executable": path_to_sleep,
            "arguments": "600",
            "log": (test_dir / "seed-{}.log".format(
                drain_case["id"])).as_posix(),
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
    ), "the seed job never started, so the pool is not working"
    yield handle
    handle.remove()


@action
def ocu_on_draining_ep(condor, seed_job, drain_case):
    """Drain the EP first, then ask for an OCU on it."""
    address = slots(condor)[0]["MyAddress"]

    command = ["condor_drain"]
    if drain_case["start"]:
        command += ["-start", drain_case["start"]]
    command += [address]

    result = condor.run_command(command, echo=True)
    assert result.returncode == 0, "condor_drain failed: {}".format(result.stderr)

    draining = poll_until(lambda: slots(condor, "Draining =?= true"), timeout=60)
    assert draining, "the EP never started draining:\n" + slot_table(condor)

    schedd = condor.get_local_schedd()
    request = classad.ClassAd()
    request["Owner"] = getpass.getuser()
    request["User"] = getpass.getuser()
    request["RequestCpus"] = 1
    request["RequestMemory"] = 1
    request["RequestDisk"] = 1

    create = schedd.create_ocu(request)
    assert create["Result"] == 0, "failed to create OCU: {}".format(
        create["ErrorString"] if "ErrorString" in create else "unknown"
    )

    claimed = poll_until(
        lambda: slots(condor, OCU_CLAIM_CONSTRAINT), timeout=WATCH
    )
    logger.debug("drain case %s: OCU claimed %s",
                 drain_case["id"], claimed[0]["Name"] if claimed else None)

    yield {
        "claim": claimed[0]["Name"] if claimed else None,
        "table": slot_table(condor),
    }

    try:
        schedd.remove_ocu(int(create["OCUId"]))
    except Exception:
        pass


class TestOCUClaimsDrainingEP:
    def test_ocu_claims_the_draining_ep_only_when_admitted(
        self, ocu_on_draining_ep, drain_case
    ):
        """A drain that admits OCUs gets one; a drain that admits nothing --
        the default, where Requirements becomes false -- does not."""
        if drain_case["expect"]:
            assert ocu_on_draining_ep["claim"] is not None, (
                "the EP was draining with DRAIN_FINAL_START_EXPR = {!r}, "
                "which an OCU request satisfies, but no OCU claim was made "
                "within {}s:\n{}".format(
                    drain_case["final"], WATCH, ocu_on_draining_ep["table"]
                )
            )
        else:
            assert ocu_on_draining_ep["claim"] is None, (
                "an OCU claimed a slot on an EP drained with no "
                "DRAIN_FINAL_START_EXPR, where Requirements is false and "
                "nothing should match:\n{}".format(ocu_on_draining_ep["table"])
            )
