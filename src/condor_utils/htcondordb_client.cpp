/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "condor_common.h"
#include "condor_debug.h"
#include "condor_config.h"
#include "condor_io.h"
#include "htcondordb_client.h"

#include <cstring>
#include <vector>

#if !defined(WIN32)
#include <dlfcn.h>
#endif

namespace {

// The library's own status codes (see capi/queue.go).
constexpr int HCDB_OK      = 0;
constexpr int HCDB_ERR     = -1;
constexpr int HCDB_PENDING = 1;

// knob builds "<SUBSYS>_<NAME>", the per-daemon form of a knob.
std::string knob(const char * subsys, const char * name) {
	std::string s(subsys ? subsys : "");
	s += "_";
	s += name;
	return s;
}

// The default file name of the client library on this platform. It is dlopened,
// never linked, so a daemon with no library present simply has the feature off.
const char * defaultLibraryName() {
#if defined(DARWIN)
	return "libhtcondordb_client.dylib";
#else
	return "libhtcondordb_client.so";
#endif
}

} // anonymous namespace

HTCondorDBClient &
HTCondorDBClient::instance()
{
	static HTCondorDBClient self;
	return self;
}

bool
HTCondorDBClient::databaseConfigured() const
{
	// The question is not "is an address configured" -- an explicit address is the
	// unusual case -- but "does anything name a daemon", which is what the client
	// library goes on to ask. It resolves an address file first and a host second,
	// so an explicit address is only one of the ways to be configured.
	std::string v;
	if (param(v, "HTCONDORDB_ADDRESS") && ! v.empty()) { return true; }
	if (param(v, "HTCONDORDB_HOST") && ! v.empty()) { return true; }
	if (param(v, "HTCONDORDB_ADDRESS_FILE") && ! v.empty()) { return true; }

	// Running the database beside this schedd is the ordinary deployment, and the
	// knob that turns it on is set in the same configuration this schedd reads. Go
	// on that rather than on the daemon's address file, which may not exist yet:
	// the master starts both, and the schedd can be first.
	if (param_boolean("HTCONDORDB_SYNC_SCHEDD", false)) { return true; }

	// Last, a daemon that is already up and publishing where it is.
	std::string logdir;
	if (param(logdir, "LOG") && ! logdir.empty()) {
		struct stat st{};
		std::string file = logdir + "/.htcondordb_address";
		if (stat(file.c_str(), &st) == 0 && st.st_size > 0) { return true; }
	}
	return false;
}

void
HTCondorDBClient::config(const char * subsys, const char * job_queue_log)
{

	HTCondorDBUse want = HTCondorDBUse::Never;
	std::string setting;
	if (param(setting, knob(subsys, "QUERY_USE_HTCONDORDB").c_str())) {
		if (strcasecmp(setting.c_str(), "ALWAYS") == 0) {
			want = HTCondorDBUse::Always;
		} else if (strcasecmp(setting.c_str(), "AUTO") == 0) {
			want = HTCondorDBUse::Auto;
		} else if (strcasecmp(setting.c_str(), "NEVER") == 0) {
			want = HTCondorDBUse::Never;
		} else {
			dprintf(D_ERROR, "%s_QUERY_USE_HTCONDORDB is \"%s\"; expected ALWAYS, AUTO or NEVER. "
				"Treating it as NEVER.\n", subsys, setting.c_str());
			want = HTCondorDBUse::Never;
		}
	}

	if (want == HTCondorDBUse::Never) {
		if (m_mode != HTCondorDBUse::Never) {
			dprintf(D_ALWAYS, "Queries will no longer be answered from htcondordb.\n");
		}
		m_mode = HTCondorDBUse::Never;
		closeSession();
		closePipe();
		return;
	}

	// AUTO is the default, so most schedds reach here having asked for nothing. With
	// no database to talk to there is nothing to load or report: stay off, quietly,
	// exactly as a schedd without this feature behaves.
	if ( ! databaseConfigured()) {
		if (want == HTCondorDBUse::Always) {
			dprintf(D_ERROR, "%s_QUERY_USE_HTCONDORDB is ALWAYS but nothing names an "
				"htcondordb daemon. Queries will be answered from the job queue.\n", subsys);
		}
		m_mode = HTCondorDBUse::Never;
		closeSession();
		closePipe();
		return;
	}

	m_mode = want;

	std::string path;
	param(path, "HTCONDORDB_CLIENT_LIBRARY");
	if (!loadLibrary(path)) {
		// Without the library there is nothing to ask. Under ALWAYS that defeats an
		// explicit instruction and is worth an error; under AUTO it just means the
		// package is not installed, which is not a misconfiguration.
		dprintf(want == HTCondorDBUse::Always ? D_ERROR : D_FULLDEBUG,
			"%s_QUERY_USE_HTCONDORDB is %s, but the htcondordb client library could not be "
			"loaded. Queries will be answered from the job queue.\n",
			subsys, want == HTCondorDBUse::Always ? "ALWAYS" : "AUTO");
		m_mode = HTCondorDBUse::Never;
		return;
	}
	if (!openPipe()) {
		m_mode = HTCondorDBUse::Never;
		return;
	}
	// Replace the session only when what it was built from has changed. Closing it
	// ends every query running over it, so a reconfig that touched something else
	// entirely must not disturb a condor_q that is halfway through its answer.
	std::string key = sessionKey(subsys, job_queue_log);
	if (m_handle && key == m_session_key) { return; }
	closeSession();
	m_session_key = key;
	openSession(subsys, job_queue_log);
}

std::string
HTCondorDBClient::sessionKey(const char * subsys, const char * job_queue_log) const
{
	std::string addr, table;
	param(addr, "HTCONDORDB_ADDRESS");
	if (!param(table, "HTCONDORDB_JOBS_TABLE")) { table = "jobs"; }
	std::string key;
	formatstr(key, "%s|%s|%s|%s|%lld|%d|%d",
		subsys ? subsys : "", job_queue_log ? job_queue_log : "", addr.c_str(), table.c_str(),
		(long long)param_integer(knob(subsys, "QUERY_HTCONDORDB_MAX_LAG").c_str(), 30),
		param_integer(knob(subsys, "QUERY_HTCONDORDB_PROBE_INTERVAL").c_str(), 15),
		(int)m_mode);
	return key;
}

bool
HTCondorDBClient::loadLibrary(const std::string & path_hint)
{
#if defined(WIN32)
	(void)path_hint;
	return false;
#else
	if (m_lib) { return true; }

	// A Go c-shared library brings its own runtime up at load time and cannot be
	// unloaded, so this happens once per process and the handle is never closed.
	std::vector<std::string> candidates;
	if (!path_hint.empty()) {
		candidates.push_back(path_hint);
	} else {
		std::string libexec;
		if (param(libexec, "LIBEXEC") && !libexec.empty()) {
			candidates.push_back(libexec + "/" + defaultLibraryName());
		}
		// Bare name last, so the dynamic loader's own search path can supply a
		// library the configuration did not name.
		candidates.push_back(defaultLibraryName());
	}

	for (const auto & cand : candidates) {
		void * lib = dlopen(cand.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!lib) {
			dprintf(D_FULLDEBUG, "htcondordb: dlopen(%s) failed: %s\n", cand.c_str(), dlerror());
			continue;
		}
		m_lib = lib;
		dprintf(D_ALWAYS, "htcondordb: loaded client library %s\n", cand.c_str());
		break;
	}
	if (!m_lib) { return false; }

	// Resolving every symbol up front means a library too old for this daemon is
	// rejected at load rather than halfway through a query.
	struct { const char * name; void ** slot; } wanted[] = {
		{ "hcdb_queue_open",   (void**)&m_fn.queue_open },
		{ "hcdb_queue_state",  (void**)&m_fn.queue_state },
		{ "hcdb_queue_probe",  (void**)&m_fn.queue_probe },
		{ "hcdb_queue_begin_fd",     (void**)&m_fn.queue_begin_fd },
		{ "hcdb_queue_result",       (void**)&m_fn.queue_result },
		{ "hcdb_queue_crypto_state", (void**)&m_fn.queue_crypto_state },
		{ "hcdb_queue_counts", (void**)&m_fn.queue_counts },
		{ "hcdb_queue_free",   (void**)&m_fn.queue_free },
		{ "hcdb_queue_close",  (void**)&m_fn.queue_close },
		{ "hcdb_free",         (void**)&m_fn.free },
	};
	for (auto & w : wanted) {
		*w.slot = dlsym(m_lib, w.name);
		if (!*w.slot) {
			dprintf(D_ERROR, "htcondordb: the client library has no %s; it is too old for this "
				"daemon.\n", w.name);
			m_fn = Symbols{};
			// The handle stays open (a Go runtime cannot be unloaded), but with the
			// symbol table cleared nothing will call into it.
			return false;
		}
	}

	return true;
#endif
}

bool
HTCondorDBClient::openPipe()
{
#if defined(WIN32)
	return false;
#else
	if (m_pipe_registered) { return true; }

	// can_register_read so DaemonCore will select on it; a blocking write end so a
	// burst of completions queues in the pipe instead of being dropped.
	if (!daemonCore->Create_Pipe(m_pipe, true /*can_register_read*/, false /*can_register_write*/,
			true /*nonblocking_read*/, false /*nonblocking_write*/)) {
		dprintf(D_ERROR, "htcondordb: could not create the completion pipe.\n");
		return false;
	}
	if (!daemonCore->Get_Pipe_FD(m_pipe[1], &m_notify_fd)) {
		dprintf(D_ERROR, "htcondordb: could not get the completion pipe's descriptor.\n");
		daemonCore->Close_Pipe(m_pipe[0]);
		daemonCore->Close_Pipe(m_pipe[1]);
		m_pipe[0] = m_pipe[1] = -1;
		return false;
	}
	int rc = daemonCore->Register_Pipe(m_pipe[0], "htcondordb completions",
		[this](int pipe_end) { return this->pipeReady(pipe_end); },
		"HTCondorDBClient::pipeReady");
	if (rc < 0) {
		dprintf(D_ERROR, "htcondordb: could not register the completion pipe.\n");
		daemonCore->Close_Pipe(m_pipe[0]);
		daemonCore->Close_Pipe(m_pipe[1]);
		m_pipe[0] = m_pipe[1] = -1;
		m_notify_fd = -1;
		return false;
	}
	m_pipe_registered = true;
	return true;
#endif
}

void
HTCondorDBClient::closePipe()
{
	if (m_pipe[0] >= 0) {
		daemonCore->Close_Pipe(m_pipe[0]); // also cancels the registration
		m_pipe[0] = -1;
	}
	if (m_pipe[1] >= 0) {
		daemonCore->Close_Pipe(m_pipe[1]);
		m_pipe[1] = -1;
	}
	m_notify_fd = -1;
	m_pipe_registered = false;
	m_partial.clear();
}

void
HTCondorDBClient::openSession(const char * subsys, const char * job_queue_log)
{
	if (!m_fn.queue_open) { return; }

	std::string addr, table;
	param(addr, "HTCONDORDB_ADDRESS");
	if (!param(table, "HTCONDORDB_JOBS_TABLE")) { table = "jobs"; }

	long long max_lag = param_integer(knob(subsys, "QUERY_HTCONDORDB_MAX_LAG").c_str(), 30);
	int probe = param_integer(knob(subsys, "QUERY_HTCONDORDB_PROBE_INTERVAL").c_str(), 15);

	// ALWAYS means "do not let freshness stop you": pass no bound, so the library
	// only insists that the database is mirroring a job queue at all.
	if (m_mode == HTCondorDBUse::Always) { max_lag = 0; }

	m_handle = (*m_fn.queue_open)(addr.c_str(), subsys, table.c_str(),
		job_queue_log ? job_queue_log : "", max_lag, probe, m_notify_fd);
	if (!m_handle) {
		dprintf(D_ERROR, "htcondordb: could not create a client session.\n");
		return;
	}
	dprintf(D_ALWAYS, "htcondordb: reading queries from table \"%s\" at %s (%s)\n",
		table.c_str(), addr.empty() ? "the configured address" : addr.c_str(),
		m_mode == HTCondorDBUse::Always ? "ALWAYS" : "AUTO");
}

void
HTCondorDBClient::closeSession()
{
	// Finish the outstanding queries rather than abandoning them. Each is a client
	// socket somebody is waiting on, and a watcher dropped on the floor is a
	// condor_q that never returns.
	//
	// Freeing each request first makes the wake-up deterministic: a freed request
	// answers the next collection with an error immediately, where a merely
	// cancelled one might still answer "nothing yet" and go back to waiting for a
	// notification that will never come.
	auto watchers = std::move(m_watchers);
	m_watchers.clear();
	if (m_fn.queue_free) {
		for (const auto & [req, cb] : watchers) {
			(void)cb;
			(*m_fn.queue_free)(req);
		}
	}
	if (m_handle && m_fn.queue_close) {
		(*m_fn.queue_close)(m_handle);
	}
	m_handle = 0;
	m_session_key.clear();
	for (const auto & [req, cb] : watchers) {
		(void)req;
		cb();
	}
}

void
HTCondorDBClient::shutdown()
{
	closeSession();
	closePipe();
	m_mode = HTCondorDBUse::Never;
}

int
HTCondorDBClient::pipeReady(int pipe_end)
{
	// Each completion is an eight-byte request token. Reading them in bulk keeps
	// one wake-up from costing one syscall per finished chunk.
	char buf[8 * 64];
	for (;;) {
		int n = daemonCore->Read_Pipe(pipe_end, buf, (int)sizeof(buf));
		if (n <= 0) { break; }
		m_partial.append(buf, n);
		// Eight bytes, little endian, whatever this platform's uintptr_t is: the
		// library writes a fixed-width token so the two sides cannot disagree.
		while (m_partial.size() >= 8) {
			uint64_t raw = 0;
			const unsigned char * p = (const unsigned char *)m_partial.data();
			for (int i = 0; i < 8; i++) {
				raw |= ((uint64_t)p[i]) << (8 * i);
			}
			uintptr_t token = (uintptr_t)raw;
			m_partial.erase(0, 8);

			auto it = m_watchers.find(token);
			if (it == m_watchers.end()) {
				// A completion for a request the caller has already released. The
				// library may have had a chunk in flight when it was cancelled; there
				// is nothing left to deliver it to.
				continue;
			}
			// Copy the callback before running it: a handler that releases its own
			// request erases the map entry under us.
			auto cb = it->second;
			cb();
		}
		if (n < (int)sizeof(buf)) { break; }
	}
	return KEEP_STREAM;
}

bool
HTCondorDBClient::usable(std::string & why)
{
	if (m_mode == HTCondorDBUse::Never) {
		why = "reading from htcondordb is disabled";
		return false;
	}
	if (!m_handle || !m_fn.queue_state) {
		why = "no htcondordb session";
		return false;
	}
	char * detail = nullptr;
	int state = (*m_fn.queue_state)(m_handle, &detail);
	why = detail ? detail : "";
	if (detail) { (*m_fn.free)(detail); }

	switch ((HTCondorDBState)state) {
	case HTCondorDBState::Ready:
		return true;
	case HTCondorDBState::Stale:
		// ALWAYS is the administrator overriding the freshness check, which is what
		// it is for: a pool that would rather have a fast answer than a current one,
		// or one whose mirror cannot advertise its lag.
		return m_mode == HTCondorDBUse::Always;
	case HTCondorDBState::Down:
		// Ask for a check now rather than waiting out the interval, so a database
		// that has come back is picked up on the next query instead of up to
		// <SUBSYS>_QUERY_HTCONDORDB_PROBE_INTERVAL seconds later. The library paces
		// its own dialling against the clock, so asking on every query costs at
		// most one connection attempt per backoff period.
		(*m_fn.queue_probe)(m_handle);
		return false;
	default:
		return false;
	}
}

uintptr_t
HTCondorDBClient::beginToSocket(const char * constraint, const char * projection, long long limit,
                                int flags, int fd, const std::string & crypto_state,
                                std::function<void()> on_ready, std::string & err)
{
	if (!m_handle || !m_fn.queue_begin_fd) {
		err = "no htcondordb session that can write a response";
		// The library owns the descriptor on every return of the entry point below,
		// but we never reached it, so this one is still ours.
		if (fd >= 0) { close(fd); }
		return 0;
	}
	char * cerr = nullptr;
	uintptr_t req = (*m_fn.queue_begin_fd)(m_handle, constraint ? constraint : "true",
		projection ? projection : "", limit, flags, fd,
		crypto_state.empty() ? nullptr : (const unsigned char *)crypto_state.data(),
		crypto_state.size(), &cerr);
	if (!req) {
		err = cerr ? cerr : "starting the query failed";
		if (cerr) { (*m_fn.free)(cerr); }
		return 0;
	}
	if (cerr) { (*m_fn.free)(cerr); }

	// Registered before returning to the event loop, so the pipe cannot be read --
	// and a token for this request dropped as unknown -- before the watcher is in
	// place.
	m_watchers[req] = std::move(on_ready);
	return req;
}

HTCondorDBResult
HTCondorDBClient::result(uintptr_t req, long long & ads, bool & socket_clean, std::string & err)
{
	ads = 0;
	// A query we cannot ask about is one whose outcome we do not know, and an
	// unknown outcome means an unknown socket: say it is not safe to write to.
	socket_clean = false;
	if (!m_fn.queue_result) {
		err = "no htcondordb session";
		return HTCondorDBResult::Error;
	}
	long long n = 0;
	int clean = 0;
	char * cerr = nullptr;
	int rc = (*m_fn.queue_result)(req, &n, &clean, &cerr);
	if (cerr) {
		err = cerr;
		(*m_fn.free)(cerr);
	}
	ads = n;
	socket_clean = (clean != 0);
	switch (rc) {
	case HCDB_PENDING: return HTCondorDBResult::Pending;
	case HCDB_OK:      return HTCondorDBResult::Done;
	case HCDB_ERR:
	default:
		if (err.empty()) { err = "the query failed"; }
		return HTCondorDBResult::Error;
	}
}

bool
HTCondorDBClient::cryptoState(uintptr_t req, std::string & out)
{
	out.clear();
	if (!m_fn.queue_crypto_state) { return false; }
	void * buf = nullptr;
	size_t len = 0;
	char * cerr = nullptr;
	int rc = (*m_fn.queue_crypto_state)(req, &buf, &len, &cerr);
	if (buf && len) { out.assign((const char *)buf, len); }
	if (buf) { (*m_fn.free)((char*)buf); }
	if (cerr) {
		dprintf(D_FULLDEBUG, "htcondordb: no session to resume: %s\n", cerr);
		(*m_fn.free)(cerr);
	}
	return rc == HCDB_OK && !out.empty();
}

bool
HTCondorDBClient::counts(uintptr_t req, std::string & out)
{
	out.clear();
	if (!m_fn.queue_counts) { return false; }
	char * c = nullptr;
	int rc = (*m_fn.queue_counts)(req, &c);
	if (c) {
		out = c;
		(*m_fn.free)(c);
	}
	return rc == HCDB_OK;
}

void
HTCondorDBClient::release(uintptr_t req)
{
	m_watchers.erase(req);
	if (m_fn.queue_free) {
		(*m_fn.queue_free)(req);
	}
}
