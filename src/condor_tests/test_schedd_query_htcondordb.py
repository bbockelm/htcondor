#!/usr/bin/env pytest

# Verify SCHEDD_QUERY_USE_HTCONDORDB, the knob that lets the schedd answer
# condor_q from an htcondordb mirror of its job queue rather than by walking
# the queue itself.
#
# No htcondordb daemon or client library is present here, which is the case
# this test is for: a pool that turns the knob on without the database must
# get the condor_q it always had.  For each of NEVER (the default), AUTO,
# ALWAYS and a value that is none of those we check that
#
#   1. the schedd takes the setting and goes on answering queries,
#   2. the answers are identical to the ones NEVER gives -- whole ad,
#      projection and constraint alike,
#   3. the schedd's statistics count every query, and none of them went to a
#      mirror, and
#   4. ALWAYS with nothing to talk to, and a value that is not one of the
#      three, each say so in the schedd log rather than failing a query.
#
# Exercising the mirror itself needs the client library, which is not built
# with HTCondor, so that is not tested here.

import logging
import time

from ornithology import *

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)

NUM_JOBS = 5

# NEVER first: it is the default, and the baseline the rest are compared to.
SETTINGS = ["NEVER", "AUTO", "ALWAYS", "SOMETIMES"]

PROJECTION = ["ClusterId", "ProcId", "JobStatus", "Owner", "TestMarker"]
CONSTRAINT = "ProcId < 2"
QUERIES_PER_SETTING = 3

STAT_ATTRS = [
	"QueryJobAdsFromJobQueue",
	"QueryJobAdsFromMirror",
	"QueryJobAdsMirrorDeclined",
]


@standup
def condor(test_dir):
	with Condor(
		test_dir / "condor",
		config={
			"NUM_CPUS": "1",
			# Nothing here names an htcondordb daemon, which is the point.
		},
	) as condor:
		yield condor


def schedd_sinful(condor):
	"""The schedd's command socket, so its statistics can be read from the
	schedd itself rather than from a collector ad that lags behind it."""
	return (condor.spool_dir / ".schedd_address").read_text().splitlines()[0]


def read_stats(condor):
	result = condor.run_command(
		["condor_status", "-schedd", "-direct", schedd_sinful(condor), "-af"]
		+ STAT_ATTRS
	)
	assert result.returncode == 0, result.stderr
	values = result.stdout.split()
	assert len(values) == len(STAT_ATTRS), result.stdout
	return dict(zip(STAT_ATTRS, (int(v) for v in values)))


def set_knob(condor, setting):
	"""Put the knob at `setting` and wait for the running schedd to be the one
	answering for it."""
	with condor.config_file.open(mode="a") as f:
		f.write(f"\nSCHEDD_QUERY_USE_HTCONDORDB = {setting}\n")

	result = condor.run_command(["condor_reconfig", "-schedd"])
	assert result.returncode == 0, result.stderr

	# Asking the schedd for its own value of the knob is the one signal that
	# works for every setting: with no database to reach, none of them changes
	# what the schedd does.
	deadline = time.time() + 60
	while time.time() < deadline:
		result = condor.run_command(
			["condor_config_val", "-schedd", "SCHEDD_QUERY_USE_HTCONDORDB"]
		)
		if result.returncode == 0 and result.stdout.strip() == setting:
			return
		time.sleep(1)
	assert False, f"the schedd never picked up SCHEDD_QUERY_USE_HTCONDORDB = {setting}"


def rows(ads):
	"""Ads in a comparable form: ordered by job id, every value a string."""
	return [
		{k: str(v) for k, v in sorted(ad.items())}
		for ad in sorted(ads, key=lambda a: (a["ClusterId"], a["ProcId"]))
	]


def volatile_attrs(first, second):
	"""The attributes that differ between two answers to the same query, so
	comparing settings is not comparing clocks."""
	out = set()
	for a, b in zip(first, second):
		out |= {k for k in set(a) | set(b) if a.get(k) != b.get(k)}
	return out


def strip(rs, volatile):
	return [{k: v for k, v in r.items() if k not in volatile} for r in rs]


def log_has(condor, text):
	return any(text in line.message for line in condor.schedd_log.open().read())


@action
def jobs(condor, test_dir, path_to_sleep):
	"""Jobs that will never match, so the queue holds still while it is
	queried."""
	handle = condor.submit(
		description={
			"executable": path_to_sleep,
			"arguments": "3600",
			"log": (test_dir / "job.log").as_posix(),
			"should_transfer_files": False,
			"requirements": "False",
			"My.TestMarker": '"htcondordb"',
		},
		count=NUM_JOBS,
	)
	yield handle
	handle.remove()


@action
def answers(condor, jobs):
	"""What the schedd answers, and what its counters say, under each setting
	of the knob."""
	out = {}
	for setting in SETTINGS:
		set_knob(condor, setting)
		schedd = condor.get_local_schedd()
		before = read_stats(condor)
		out[setting] = {
			"all": rows(schedd.query()),
			"projected": rows(schedd.query(projection=PROJECTION)),
			"constrained": rows(schedd.query(constraint=CONSTRAINT)),
			"before": before,
			"after": read_stats(condor),
		}

	# Two answers to the same query, from one schedd, with the setting left as
	# it is: whatever differs between them is the clock, not the knob.
	schedd = condor.get_local_schedd()
	out["volatile"] = volatile_attrs(rows(schedd.query()), rows(schedd.query()))
	logger.debug("ignoring volatile attributes %s", sorted(out["volatile"]))
	return out


class TestScheddQueryHTCondorDB:
	def test_jobs_are_queued(self, answers):
		for setting in SETTINGS:
			assert len(answers[setting]["all"]) == NUM_JOBS, setting

	def test_whole_ads_match(self, answers):
		volatile = answers["volatile"]
		baseline = strip(answers["NEVER"]["all"], volatile)
		for setting in SETTINGS[1:]:
			assert strip(answers[setting]["all"], volatile) == baseline, setting

	def test_projection_matches(self, answers):
		volatile = answers["volatile"]
		baseline = strip(answers["NEVER"]["projected"], volatile)
		for ad in baseline:
			# The projection was honored, custom attribute and all.
			assert ad["TestMarker"] == '"htcondordb"'
			assert "Cmd" not in ad
		for setting in SETTINGS[1:]:
			got = strip(answers[setting]["projected"], volatile)
			assert got == baseline, setting

	def test_constraint_matches(self, answers):
		volatile = answers["volatile"]
		baseline = strip(answers["NEVER"]["constrained"], volatile)
		assert len(baseline) == 2
		for setting in SETTINGS[1:]:
			got = strip(answers[setting]["constrained"], volatile)
			assert got == baseline, setting

	def test_every_query_is_counted(self, answers):
		"""Every query this test ran was counted, and all of them were
		answered from the job queue -- there is no database here."""
		for setting in SETTINGS:
			got = answers[setting]
			ran = (
				got["after"]["QueryJobAdsFromJobQueue"]
				- got["before"]["QueryJobAdsFromJobQueue"]
			)
			assert ran >= QUERIES_PER_SETTING, f"{setting}: {ran}"
			assert got["after"]["QueryJobAdsFromMirror"] == 0, setting
			assert got["after"]["QueryJobAdsMirrorDeclined"] == 0, setting

	def test_always_without_a_database_is_reported(self, condor, answers):
		assert log_has(
			condor,
			"SCHEDD_QUERY_USE_HTCONDORDB is ALWAYS but nothing names an "
			"htcondordb daemon",
		)

	def test_unrecognized_value_is_reported(self, condor, answers):
		assert log_has(condor, "expected ALWAYS, AUTO or NEVER")
