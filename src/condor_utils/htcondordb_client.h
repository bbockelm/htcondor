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

#ifndef _CONDOR_HTCONDORDB_CLIENT_H
#define _CONDOR_HTCONDORDB_CLIENT_H

/*
 * A DaemonCore-friendly front end to the htcondordb C client library.
 *
 * htcondordb keeps a mirror of a schedd's job queue, and a daemon that can read
 * that mirror can answer a query without walking its own queue. The library that
 * talks to it is written in Go; this class is what makes it usable from a
 * single-threaded daemon:
 *
 *   - It is loaded with dlopen, never linked. A daemon built against a machine
 *     with the library runs unchanged on one without it: the feature turns itself
 *     off and every query takes the ordinary path.
 *   - Nothing it does blocks the event loop. Connecting, re-connecting and
 *     checking the mirror's freshness all happen on the library's own threads;
 *     the daemon reads a cached verdict.
 *   - A query's result arrives as bytes already in HTCondor's on-the-wire ad
 *     framing. The caller copies them to its client's socket. No ClassAd is
 *     parsed, built, or unparsed on the main thread, which is the whole point of
 *     reading from the mirror rather than from the queue.
 *
 * Completion is delivered through a DaemonCore pipe: the library writes a request
 * token to it, and the pipe's read handler runs the callback that request
 * registered, on the main thread.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "condor_daemon_core.h"

// How a daemon is configured to treat the database.
enum class HTCondorDBUse {
	Never,  // never read from it, and do not even load the library
	Auto,   // read from it when it is there and says it is current
	Always, // read from it whenever a session is up, current or not
};

// What hcdb_queue_state reports. Mirrors the HCDB_QUEUE_* codes in the library.
enum class HTCondorDBState {
	Down       = 0,
	Connecting = 1,
	Ready      = 2,
	Stale      = 3,
};

// Flags for HTCondorDBClient::begin, matching the library's HCDB_QUEUE_* flags.
constexpr int HTCONDORDB_Q_SERVER_TIME = 1 << 0; // append ServerTime to every ad
constexpr int HTCONDORDB_Q_COUNTS      = 1 << 1; // also return per-(universe,status) counts
constexpr int HTCONDORDB_Q_NO_ADS      = 1 << 2; // counts only; read no ads

// How far a response-writing query has got (HTCondorDBClient::result).
enum class HTCondorDBResult {
	Pending, // the response is still going out
	Done,    // every matching ad reached the client's socket
	Error,   // it did not; socket_clean says whether the socket may still be used
};

/*
 * The process-wide client. One instance, because the session, the liveness
 * probe and the notification pipe are all per-process.
 */
class HTCondorDBClient {
public:
	static HTCondorDBClient & instance();

	/*
	 * Read the configuration and bring the client into line with it: load the
	 * library and open the session when enabled, tear them down when not. Safe to
	 * call on every reconfig.
	 *
	 * subsys is the HTCondor subsystem the client authenticates as (e.g. "SCHEDD"),
	 * and job_queue_log the caller's own JOB_QUEUE_LOG path. A database mirroring a
	 * DIFFERENT schedd's log answers every query successfully and with the wrong
	 * jobs, so the caller names its log and the library refuses a mismatch.
	 */
	void config(const char * subsys, const char * job_queue_log);

	// Close the session and cancel outstanding queries. Called at shutdown.
	void shutdown();

	HTCondorDBUse mode() const { return m_mode; }

	/*
	 * Whether a query may be answered from the database right now. Fills why with
	 * the reason either way, which is what belongs in a D_FULLDEBUG line.
	 */
	bool usable(std::string & why);

	/*
	 * Start a query whose ads the LIBRARY writes to the client's socket, on its own
	 * thread, rather than this one. Returns a request handle, or 0 with the reason
	 * in err; it returns without reading a row.
	 *
	 * constraint is a ClassAd expression; projection a comma-separated attribute
	 * list (empty for every attribute); limit caps the ads returned (<= 0 for no
	 * cap); flags is a bitwise OR of the HTCONDORDB_Q_* values above.
	 *
	 * fd is the client's socket, positioned where nothing of the response has been
	 * written yet. Pass a descriptor of your own -- a dup -- because the library
	 * closes it when the response is finished, and because you still need yours:
	 * nothing but the ads goes out this way, and the response's final ad is still
	 * yours to write. The library takes the descriptor on every return, successful
	 * or not.
	 *
	 * crypto_state describes where an encrypted CEDAR session stands, as
	 * ReliSock::exportCryptoState writes it; empty for a session in the clear. Two
	 * writers cannot otherwise take turns on an encrypted session, because its
	 * counter and IV advance with every frame. When it is given, take the final
	 * position back with cryptoState() and import it before writing anything.
	 *
	 * on_ready runs once, on the main thread, when the response is finished; ask
	 * result() what happened.
	 */
	uintptr_t beginToSocket(const char * constraint,
	                        const char * projection,
	                        long long limit,
	                        int flags,
	                        int fd,
	                        const std::string & crypto_state,
	                        std::function<void()> on_ready,
	                        std::string & err);

	/*
	 * Where a finished response-writing query left an encrypted session, in the
	 * form ReliSock::importCryptoState takes. False means the session cannot be
	 * continued, and the connection has to be dropped rather than written to.
	 */
	bool cryptoState(uintptr_t req, std::string & out);

	/*
	 * What became of such a query. ads is how many reached the socket.
	 *
	 * socket_clean says whether the socket is still at an ad boundary. False means
	 * a write stopped part way through one, and the caller must close the
	 * connection rather than write anything more: a client that loses its
	 * connection reports a failure, where one handed half an ad followed by a
	 * well-formed summary reports a short queue and no error at all.
	 */
	HTCondorDBResult result(uintptr_t req, long long & ads, bool & socket_clean, std::string & err);

	/*
	 * A finished query's job counts, as one "<universe> <status> <count>" line per
	 * group -- the triples IncrementLiveJobCounter takes, with the count as the
	 * increment. Empty unless the query asked for HTCONDORDB_Q_COUNTS.
	 */
	bool counts(uintptr_t req, std::string & out);

	// Cancel the query if it is still running, and free its handle.
	void release(uintptr_t req);

private:
	HTCondorDBClient() = default;
	~HTCondorDBClient() = default;
	HTCondorDBClient(const HTCondorDBClient &) = delete;
	HTCondorDBClient & operator=(const HTCondorDBClient &) = delete;

	bool loadLibrary(const std::string & path_hint);
	bool openPipe();
	void closePipe();
	int pipeReady(int pipe_end);
	void openSession(const char * subsys, const char * job_queue_log);
	void closeSession();
	// Everything the session is built from, so a reconfig that changes none of it
	// leaves the session -- and the queries running over it -- alone.
	std::string sessionKey(const char * subsys, const char * job_queue_log) const;
	// Whether anything names an htcondordb daemon, by any of the means the client
	// library would use to find one.
	bool databaseConfigured() const;

	HTCondorDBUse m_mode{HTCondorDBUse::Never};
	void * m_lib{nullptr};
	uintptr_t m_handle{0};
	std::string m_session_key;

	// The DaemonCore pipe the library signals completion on. m_pipe[0] is the end
	// we read and register; m_pipe[1] the raw descriptor the library writes.
	int m_pipe[2]{-1, -1};
	int m_notify_fd{-1};
	bool m_pipe_registered{false};
	// Bytes of a token that arrived split across two reads. A write of eight bytes
	// to a pipe is atomic, so this stays empty in practice; carrying it costs
	// nothing and a lost token would hang a query.
	std::string m_partial;

	std::unordered_map<uintptr_t, std::function<void()>> m_watchers;

	// The library's entry points, resolved once at load.
	struct Symbols {
		uintptr_t (*queue_open)(const char * addr, const char * subsys, const char * table,
		                        const char * job_queue_log, long long max_lag_seconds,
		                        int probe_seconds, int notify_fd);
		int  (*queue_state)(uintptr_t h, char ** detail);
		void (*queue_probe)(uintptr_t h);
		uintptr_t (*queue_begin_fd)(uintptr_t h, const char * constraint, const char * projection,
		                            long long limit, int flags, int fd,
		                            const unsigned char * state, size_t state_len, char ** err);
		int  (*queue_result)(uintptr_t rh, long long * ads, int * clean, char ** err);
		int  (*queue_crypto_state)(uintptr_t rh, void ** out, size_t * len, char ** err);
		int  (*queue_counts)(uintptr_t rh, char ** out);
		void (*queue_free)(uintptr_t rh);
		void (*queue_close)(uintptr_t h);
		void (*free)(char * p);
	};
	Symbols m_fn{};
};

#endif /* _CONDOR_HTCONDORDB_CLIENT_H */
