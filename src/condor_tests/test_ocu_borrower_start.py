#!/usr/bin/env pytest

# An OCU can say which other users' jobs may borrow it while it is idle.
#
# An idle OCU claim can be lent out.  Before, the rule was fixed: any job that
# set OCUWilling could take it.  OCUBorrowerStart on the OCU request lets the
# OCU's owner say more than that -- it is evaluated by the schedd against the
# same SLOT/OWNER/JOB scopes as START_VANILLA_UNIVERSE.
#
# Absent, the default is OCU_BORROWER_START_DEFAULT, and absent that,
# JOB.OCUWilling =?= true, which is what the code did unconditionally before,
# so an OCU that says nothing behaves as it always has.
#
# The OCU here will lend its claim only to a job that volunteers AND carries
# BorrowMe.  The pool has two CPUs, one running a seed job and one held by the
# claim, so a borrower that runs can only be running on the claim.
#
#   refused   OCUWilling, no BorrowMe -- stays idle
#   admitted  OCUWilling and BorrowMe -- runs, on the claim's slot

import logging
import time
import getpass

import htcondor2 as htcondor
import classad2 as classad

from ornithology import *

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)

# Long enough for many negotiation cycles at NEGOTIATOR_INTERVAL = 2.
WATCH = 45

CLAIM_CONSTRAINT = 'State == "Claimed" && Activity == "Idle"'

SLOT_PROJECTION = ["Name", "State", "Activity", "Cpus", "RemoteUser", "OCU"]


def slots(condor, constraint="true"):
    return condor.direct_status(
        htcondor.DaemonTypes.Startd,
        htcondor.AdTypes.Startd,
        constraint=constraint,
        projection=SLOT_PROJECTION,
    )


def slot_table(condor):
    return "\n".join(
        "    {:<30} {:>9}/{:<6} cpus={} remote_user={} ocu={}".format(
            ad["Name"],
            ad.get("State", "?"),
            ad.get("Activity", "?"),
            ad.get("Cpus", "?"),
            ad["RemoteUser"] if "RemoteUser" in ad else "<none>",
            ad["OCU"] if "OCU" in ad else "<absent>",
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


def ran_on(handle):
    for ad in handle.query(projection=["JobStatus", "RemoteHost"]):
        if "RemoteHost" in ad:
            return ad["RemoteHost"]
    return None


@config(params={"custom": True, "default": False})
def ocu_sets_expression(request):
    return request.param


@standup
def condor(test_dir):
    with Condor(
        test_dir / "condor",
        config={
            "NUM_CPUS": "2",
            "MEMORY": "4096",
            "DISK": "4096000",
            "NEGOTIATOR_CYCLE_DELAY": "2",
            "NEGOTIATOR_INTERVAL": "2",
            "OCU_SUPER_USERS": getpass.getuser(),
        },
    ) as condor:
        yield condor


@action
def seed_job(condor, test_dir, path_to_sleep):
    handle = condor.submit(
        description={
            "executable": path_to_sleep,
            "arguments": "600",
            "log": (test_dir / "seed.log").as_posix(),
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
def the_claim(condor, seed_job, ocu_sets_expression):
    """With "custom", an OCU that lends its claim only to volunteers carrying
    BorrowMe.  With "default", an OCU that says nothing, which must behave as
    OCUs always have and lend to any volunteer."""
    schedd = condor.get_local_schedd()
    request = classad.ClassAd()
    request["Owner"] = getpass.getuser()
    request["User"] = getpass.getuser()
    request["RequestCpus"] = 1
    request["RequestMemory"] = 1
    request["RequestDisk"] = 1
    if ocu_sets_expression:
        request["OCUBorrowerStart"] = classad.ExprTree(
            "JOB.OCUWilling =?= true && JOB.BorrowMe =?= true"
        )

    result = schedd.create_ocu(request)
    assert result["Result"] == 0, "failed to create OCU: {}".format(
        result["ErrorString"] if "ErrorString" in result else "unknown"
    )

    claimed = poll_until(lambda: slots(condor, CLAIM_CONSTRAINT), timeout=120)
    assert claimed, "the OCU never claimed a slot:\n" + slot_table(condor)

    # The pool must be full, or a borrower could run somewhere else and tell
    # us nothing about whether it was let onto the claim.
    for ad in slots(condor, "PartitionableSlot =?= true"):
        assert ad["Cpus"] == 0, (
            "the pool still has free CPUs, so this test cannot tell a borrowed "
            "claim from spare capacity:\n" + slot_table(condor)
        )

    claim_name = claimed[0]["Name"]
    logger.debug("OCU claim is holding %s", claim_name)

    yield claim_name

    try:
        schedd.remove_ocu(int(result["OCUId"]))
    except Exception:
        pass


def submit_borrower(condor, test_dir, path_to_sleep, name, borrow_me):
    description = {
        "executable": path_to_sleep,
        "arguments": "600",
        "log": (test_dir / "{}.log".format(name)).as_posix(),
        "leave_in_queue": True,
        "should_transfer_files": False,
        "Request_Cpus": 1,
        "Request_Memory": 1,
        "Request_Disk": 1,
        "+OCUWilling": "true",
    }
    if borrow_me:
        description["+BorrowMe"] = "true"
    return condor.submit(description=description, count=1)


@action
def plain_volunteer(condor, test_dir, path_to_sleep, the_claim):
    """Volunteers with OCUWilling, but does not carry BorrowMe."""
    handle = submit_borrower(condor, test_dir, path_to_sleep, "volunteer", False)
    host = poll_until(lambda: ran_on(handle), timeout=WATCH)
    logger.debug("plain volunteer after %ss: %s", WATCH, host)

    # Take it away again, so the claim is idle for the next borrower whether
    # or not this one was let onto it.
    handle.remove()
    poll_until(lambda: slots(condor, CLAIM_CONSTRAINT), timeout=60)
    return host


@action
def admitted(condor, test_dir, path_to_sleep, the_claim, plain_volunteer):
    """Volunteers and carries BorrowMe.  Submitted after the plain volunteer
    so that one has already had its chance at the claim."""
    handle = submit_borrower(condor, test_dir, path_to_sleep, "admitted", True)
    host = poll_until(lambda: ran_on(handle), timeout=WATCH)
    logger.debug("admitted borrower after %ss: %s", WATCH, host)
    yield host
    handle.remove()


class TestOCUBorrowerStart:
    def test_plain_volunteer_obeys_the_expression(
        self, condor, plain_volunteer, the_claim, ocu_sets_expression
    ):
        """OCUWilling alone is enough for an OCU that asked for nothing more,
        and not enough for one that did.  The same job either way, so the
        expression is the only thing deciding."""
        if ocu_sets_expression:
            assert plain_volunteer is None, (
                "a volunteer the OCU's borrower expression refuses ran anyway, "
                "on {} (the claim is holding {}):\n{}".format(
                    plain_volunteer, the_claim, slot_table(condor)
                )
            )
        else:
            assert plain_volunteer == the_claim, (
                "an OCU that set no borrower expression refused a plain "
                "OCUWilling volunteer, which is how OCUs behaved before the "
                "expression existed; it ran on {} rather than the claim's slot "
                "{}:\n{}".format(plain_volunteer, the_claim, slot_table(condor))
            )

    def test_borrowme_volunteer_runs_on_the_claim(
        self, condor, admitted, the_claim
    ):
        """A job both expressions admit runs on the claim's own slot, which is
        the only place it could be running, the pool being full."""
        assert admitted == the_claim, (
            "expected the admitted borrower to run on the claim's slot {}, "
            "but it ran on {}:\n{}".format(
                the_claim, admitted, slot_table(condor)
            )
        )
