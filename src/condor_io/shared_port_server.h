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

#ifndef __SHARED_PORT_SERVER_H__
#define __SHARED_PORT_SERVER_H__

#include "shared_port_client.h"
#include "forkwork.h"

// SharedPortServer forwards connections received on this daemon's
// command port to other daemons on the same machine through their
// SharedPortEndpoint.  This enables Condor daemons to share a single
// network port.   For security reasons, the target daemons must
// all use the same DAEMON_SOCKET_DIR as configured in this daemon.

class SharedPortServer: Service {
 public:
	SharedPortServer();
	~SharedPortServer();

	void InitAndReconfig();

		// Remove address file left over from a previous run that exited
		// without proper cleanup.
	static void RemoveDeadAddressFile();

 private:
	bool m_registered_handlers;
	MyString m_shared_port_server_ad_file;
	int m_publish_addr_timer;
	SharedPortClient m_shared_port_client;
	std::string m_default_id;
	ForkWork forker;

		// Our model of a session; we must always send a given
		// client to the same load-balanced server for the same
		// session.
		//
		// To keep the m_sessions map efficient, we aim to keep
		// this structure to 8 bytes.
	struct SessionInfo {
		uint16_t m_dest_id;
		uint16_t m_lease_remaining;
		uint32_t m_lease_expiry;
	};
		// Map from client ID -> session info;
		// Note the client ID is just a hash of the real client
		// ID; we don't care if colliding clients are sent to the
		// same backend.
	std::unordered_map<std::size_t, SessionInfo> m_sessions;
	time_t m_last_session_clean;
	int m_bookkeep_id{-1};
	static time_t m_start_time;

		// Information about a particular destination.
	struct DestInfo {
		bool m_active{true};
		std::string m_shared_port_id;
	};
	struct BalancerInfo {
		std::vector<DestInfo> m_dests;
	};
	std::unordered_map<std::string, BalancerInfo> m_dest_map;

		// Number of load balancers we manage.
	unsigned m_load_balancer_count{0};

		// Iterate through the internal state and get the shared_port_id of the
		// load balancer relevant to the provided request
	bool GetLoadBalancerId(const char *requested_id, const char *client_name, const char *sess_id, char *dest_id);
	int HandleConnectRequest(int cmd,Stream *sock);

		// Handle the registration of a load balancer with the shared port server.
	int HandleLoadBalance(int cmd, Stream *sock);
	int HandleDefaultRequest(int cmd,Stream *sock);
	int PassRequest(Sock *sock, const char *shared_port_id);
	void PublishAddress();

		// Periodically update (and expire) sessions for the load-balancer.
	void BookkeepSessions();
};

#endif
