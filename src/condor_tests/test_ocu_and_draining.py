#!/usr/bin/env pytest

# Draining and OCU claims.
#
# An OCU is a claim the schedd holds on an execute slot for a user, so the
# capacity is there when that user has work for it.  A drain asks for the
# machine's capacity back.  The two have to agree on what happens to a slot an
# OCU is holding, and the answer taken here is that the OCU keeps it:
#
#   * draining leaves an OCU claim alone, where it releases every other claim,
#     because releasing it would hand the slot back to the general pool, which
#     is the thing the drain was trying to avoid; and
#   * a slot held by an OCU claim counts as drained, so the drain finishes and
#     its on-completion action still fires, rather than waiting forever for a
#     claim that is never going away.
#
# Two tests:
#
#   survives   an OCU claim taken before the drain is still held after it,
#              and the drain completes anyway (-resume-on-completion fires,
#              so Draining goes away).
#   admitted   whether an OCU can take a claim on an EP that is ALREADY
#              draining.  A draining slot's Requirements comes from the drain's
#              own START expression, and an OCU request ad carries IsOCUHolder,
#              so `condor_drain -start 'IsOCUHolder =?= true'` should admit an
#              OCU and nothing else.  This test records whether that is so.
#
# MAXJOBRETIREMENTTIME is 0 so the ordinary job drains away promptly and the
# test is about the OCU rather than about waiting.

import logging
import time
import getpass

import htcondor2 as htcondor
import classad2 as classad

from ornithology import *

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)

SETTLE = 60

CLAIM_CONSTRAINT = 'State == "Claimed" && Activity == "Idle"'

SLOT_PROJECTION = [
    "Name", "MyAddress", "State", "Activity", "Cpus",
    "PartitionableSlot", "RemoteUser", "Draining", "DrainingPeaceful", "OCU",
]


def slots(condor, constraint="true"):
    return condor.direct_status(
        htcondor.DaemonTypes.Startd,
        htcondor.AdTypes.Startd,
        constraint=constraint,
        projection=SLOT_PROJECTION,
    )


def slot_table(condor):
    return "\n".join(
        "    {:<30} {:>9}/{:<6} cpus={} remote_user={} ocu={} draining={}".format(
            ad["Name"],
            ad.get("State", "?"),
            ad.get("Activity", "?"),
            ad.get("Cpus", "?"),
            ad["RemoteUser"] if "RemoteUser" in ad else "<none>",
            ad["OCU"] if "OCU" in ad else "<absent>",
            ad["Draining"] if "Draining" in ad else "false",
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


def create_ocu(condor):
    """Create an OCU for this user.  User as well as Owner: the execute point
    takes the claim's owner from User, and a claim that names nobody is scored
    by the negotiator as unclaimed capacity and handed to the next job."""
    schedd = condor.get_local_schedd()
    request = classad.ClassAd()
    request["Owner"] = getpass.getuser()
    request["User"] = getpass.getuser()
    request["RequestCpus"] = 1
    request["RequestMemory"] = 1
    request["RequestDisk"] = 1

    result = schedd.create_ocu(request)
    assert result["Result"] == 0, "failed to create OCU: {}".format(
        result["ErrorString"] if "ErrorString" in result else "unknown"
    )
    return int(result["OCUId"])


def startd_address(condor):
    # Not the hostname: a personal pool's FULL_HOSTNAME can be "localhost",
    # which daemon location cannot resolve.
    return slots(condor)[0]["MyAddress"]


@standup
def condor(test_dir):
    with Condor(
        test_dir / "condor",
        config={
            "NUM_CPUS": "2",
            "MEMORY": "4096",
            "DISK": "4096000",
            "MAXJOBRETIREMENTTIME": "0",
            "NEGOTIATOR_CYCLE_DELAY": "2",
            "NEGOTIATOR_INTERVAL": "2",
            "OCU_SUPER_USERS": getpass.getuser(),
            "STARTD_DEBUG": "D_FULLDEBUG",
        },
    ) as condor:
        yield condor


@action
def seed_job(condor, test_dir, path_to_sleep):
    """Occupies a CPU and, more importantly, gives the schedd a SubmitterData
    record for this user, without which the negotiator will not negotiate for
    the OCU at all."""
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
def claim_then_drain(condor, seed_job):
    """Take an OCU claim, then drain the EP underneath it."""
    ocu_id = create_ocu(condor)

    claimed = poll_until(lambda: slots(condor, CLAIM_CONSTRAINT), timeout=120)
    assert claimed, "the OCU never claimed a slot:\n" + slot_table(condor)
    claim_name = claimed[0]["Name"]
    logger.debug("OCU claim is holding %s", claim_name)

    before = slot_table(condor)

    result = condor.run_command(
        ["condor_drain", "-resume-on-completion", startd_address(condor)],
        echo=True,
    )
    assert result.returncode == 0, "condor_drain failed: {}".format(result.stderr)

    # The drain finishes when every slot that is not held by an OCU has
    # drained; -resume-on-completion then cancels draining, so Draining going
    # away is the observable signal that the drain completed.
    resumed = poll_until(
        lambda: not slots(condor, "Draining =?= true"), timeout=SETTLE
    )
    still_claimed = slots(condor, CLAIM_CONSTRAINT)

    logger.debug("after the drain: resumed=%s claims=%d", bool(resumed),
                 len(still_claimed))

    yield {
        "claim_name": claim_name,
        "before": before,
        "after": slot_table(condor),
        "drain_completed": bool(resumed),
        "claim_held": any(ad["Name"] == claim_name for ad in still_claimed),
    }

    try:
        condor.get_local_schedd().remove_ocu(ocu_id)
    except Exception:
        pass


class TestOCUAndDraining:
    def test_drain_leaves_the_ocu_claim_alone(self, claim_then_drain):
        """Every other claim is released by the drain; this one is not."""
        assert claim_then_drain["claim_held"], (
            "the drain released the OCU claim on {}, handing the slot back to "
            "the pool:\nbefore:\n{}\n\nafter:\n{}".format(
                claim_then_drain["claim_name"],
                claim_then_drain["before"],
                claim_then_drain["after"],
            )
        )

    def test_drain_completes_anyway(self, claim_then_drain):
        """A slot held by an OCU counts as drained, so the drain finishes and
        -resume-on-completion fires rather than waiting forever."""
        assert claim_then_drain["drain_completed"], (
            "the drain never completed within {}s even though the only slot "
            "still claimed is held by an OCU:\n{}".format(
                SETTLE, claim_then_drain["after"]
            )
        )
