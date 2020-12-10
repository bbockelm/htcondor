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
#include "condor_config.h"
#include "condor_daemon_core.h"
#include "shared_port_server.h"

#include "daemon_command.h"

time_t SharedPortServer::m_start_time = time(NULL);

SharedPortServer::SharedPortServer():
	m_registered_handlers(false),
	m_publish_addr_timer(-1)
{
}

SharedPortServer::~SharedPortServer() {
	if( m_registered_handlers ) {
		daemonCore->Cancel_Command( SHARED_PORT_CONNECT );
	}

	if( !m_shared_port_server_ad_file.IsEmpty() ) {
		IGNORE_RETURN unlink( m_shared_port_server_ad_file.Value() );
	}

	if( m_publish_addr_timer != -1 ) {
		daemonCore->Cancel_Timer( m_publish_addr_timer );
	}
}

void
SharedPortServer::InitAndReconfig() {
	if( !m_registered_handlers ) {
		m_registered_handlers = true;

		int rc = daemonCore->Register_Command(
			SHARED_PORT_CONNECT,
			"SHARED_PORT_CONNECT",
			(CommandHandlercpp)&SharedPortServer::HandleConnectRequest,
			"SharedPortServer::HandleConnectRequest",
			this,
			ALLOW );
		ASSERT( rc >= 0 );

		rc = daemonCore->Register_Command(
			SHARED_PORT_LOADBALANCE,
			"SHARED_PORT_LOADBALANCE",
			(CommandHandlercpp)&SharedPortServer::HandleLoadBalance,
			"SharedPortServer::HandleLoadBalance",
			this,
			DAEMON );
		ASSERT( rc >= 0 );

		rc = daemonCore->Register_UnregisteredCommandHandler(
			(CommandHandlercpp)&SharedPortServer::HandleDefaultRequest,
			"SharedPortServer::HandleDefaultRequest",
			this,
			true);
		ASSERT( rc >= 0 );

                m_bookkeep_id = daemonCore->Register_Timer(10, 10,
                        (TimerHandlercpp)&SharedPortServer::BookkeepSessions,
			"SharedPortServer::BookkeepSessions", this);
		ASSERT( m_bookkeep_id >= 0 );
	}

	param(m_default_id, "SHARED_PORT_DEFAULT_ID");
	if (param_boolean("USE_SHARED_PORT", false) && param_boolean("COLLECTOR_USES_SHARED_PORT", true) && !m_default_id.size()) {
		m_default_id = "collector";
	}

	PublishAddress();

	if( m_publish_addr_timer == -1 ) {
			// We want to touch our address file periodically to
			// prevent problems with tmpwatch or anything else that
			// might remove it.  Rewriting it is also a way to
			// guarantee that changes in our contact information will
			// eventually be published.
			// We also use this timer to periodically write some 
			// operational metrics into the ad and into the log.

		const int publish_addr_period = 300;

		m_publish_addr_timer = daemonCore->Register_Timer(
			publish_addr_period,
			publish_addr_period,
			(TimerHandlercpp)&SharedPortServer::PublishAddress,
			"SharedPortServer::PublishAddress",
			this );
	}

	forker.Initialize();
	int max_workers = param_integer("SHARED_PORT_MAX_WORKERS",50,0);
	forker.setMaxWorkers( max_workers );
}

void
SharedPortServer::RemoveDeadAddressFile()
{
		// This function is called by condor_master on startup to make
		// sure no address file is still sitting around from an
		// ungraceful shutdown.
	MyString shared_port_server_ad_file;
	if( !param(shared_port_server_ad_file,"SHARED_PORT_DAEMON_AD_FILE") ) {
		dprintf( D_FULLDEBUG, "SHARED_PORT_DAEMON_AD_FILE not defined, not removing shared port daemon ad file.\n" );
		return;
	}

	int fd = open( shared_port_server_ad_file.Value(), O_RDONLY );
	if( fd != -1 ) {
		close( fd );
		if( unlink(shared_port_server_ad_file.Value()) == 0 ) {
			dprintf(D_ALWAYS,"Removed %s (assuming it is left over from previous run)\n",shared_port_server_ad_file.Value());
		} else {
			EXCEPT( "Failed to remove dead shared port address file '%s'!", shared_port_server_ad_file.Value() );
		}
	}
}

void
SharedPortServer::PublishAddress()
{
	if( !param(m_shared_port_server_ad_file,"SHARED_PORT_DAEMON_AD_FILE") ) {
		EXCEPT("SHARED_PORT_DAEMON_AD_FILE must be defined");
	}

	ClassAd ad;
	ad.Assign(ATTR_MY_ADDRESS,daemonCore->publicNetworkIpAddr());

	std::set<std::string> commandAddresses;
	const std::vector<Sinful> &mySinfuls = daemonCore->InfoCommandSinfulStringsMyself();
	for (std::vector<Sinful>::const_iterator it=mySinfuls.begin(); it!=mySinfuls.end(); it++)
	{
		commandAddresses.insert(it->getSinful());
	}
	StringList commandAddressesSL;
	for (std::set<std::string>::const_iterator it=commandAddresses.begin(); it!=commandAddresses.end(); it++)
	{
		commandAddressesSL.insert(it->c_str());
	}
	char *adAddresses = commandAddressesSL.print_to_string();
	if (adAddresses) {ad.InsertAttr(ATTR_SHARED_PORT_COMMAND_SINFULS, adAddresses);}
	free( adAddresses );

	// Place some operational metrics into the daemon ad
	ad.Assign("RequestsPendingCurrent",m_shared_port_client.get_currentPendingPassSocketCalls());
	ad.Assign("RequestsPendingPeak",m_shared_port_client.get_maxPendingPassSocketCalls());
	ad.Assign("RequestsSucceeded",m_shared_port_client.get_successPassSocketCalls());
	ad.Assign("RequestsFailed",m_shared_port_client.get_failPassSocketCalls());
	ad.Assign("RequestsBlocked",m_shared_port_client.get_wouldBlockPassSocketCalls());
	ad.Assign("ForkedChildrenCurrent",forker.getNumWorkers());
	ad.Assign("ForkedChildrenPeak",forker.getPeakWorkers());

	// print the ad to our log file as D_ALWAYS for now, as a) it contains
	// metrics that may be useful for debugging, and b) this method is 
	// only invoked every 5 minutes by default
	dprintf(D_ALWAYS, "About to update statistics in shared_port daemon ad file at %s :\n",
			m_shared_port_server_ad_file.Value());
	dPrintAd(D_ALWAYS|D_NOHEADER,ad);

	// and now save the ad to disk
	daemonCore->UpdateLocalAd(&ad,m_shared_port_server_ad_file.Value());
}

int
SharedPortServer::HandleLoadBalance(int, Stream *sock)
{
	sock->decode();

	ClassAd msg;
	if (!getClassAd(sock, msg) || !sock->end_of_message()) {
		dprintf(D_ALWAYS,
			"SharedPortServer: failed to receive load balancer registration"
			" from %s.\n", sock->peer_description());
		return false;
	}

	ClassAd resp;

	std::string requested_id;
	if (!msg.EvaluateAttrString("LoadBalancerId", requested_id)) {
		resp.InsertAttr(ATTR_ERROR_CODE, 1);
		resp.InsertAttr(ATTR_ERROR_STRING, "Load balance request was missing a requested ID");
		sock->encode();
		if (!putClassAd(sock, resp) || !sock->end_of_message()) {
			dprintf(D_FULLDEBUG,
				"SharedPortServer: failed to send response to load balance client"
				" at %s.\n", sock->peer_description());
			return false;
		}
		return true;
	}
	int cookie;
	if (!msg.EvaluateAttrInt("Cookie", cookie)) {
		resp.InsertAttr(ATTR_ERROR_CODE, 2);
		resp.InsertAttr(ATTR_ERROR_STRING, "Load balance request was missing a cookie");
		sock->encode();
		if (!putClassAd(sock, resp) || !sock->end_of_message()) {
			dprintf(D_FULLDEBUG,
				"SharedPortServer: failed to send response to load balance client"
				" at %s.\n", sock->peer_description());
			return false;
		}
	}

	//auto iter = m_dest_map.find(requested_id);
	//if (iter == m_dest_map.end())
	dprintf(D_FULLDEBUG, "SharedPortServer: TODO - finish registration of LB");

	return false;
}

int
SharedPortServer::HandleConnectRequest(int,Stream *sock)
{
	sock->decode();

		// to avoid possible D-O-S attacks, we read into fixed-length buffers
	char shared_port_id[512];
	char client_name[512];
		// Session ID provides a hint to the load-balancer as connections
		// belonging to the same security session must go to the same backend.
	char client_session_id[512];
	client_session_id[0] = '\0';
	int deadline = 0;
	int more_args = 0;

	if( !sock->get(shared_port_id,sizeof(shared_port_id)) ||
		!sock->get(client_name,sizeof(client_name)) ||
		!sock->get(deadline) ||
		!sock->get(more_args) )
	{
		dprintf(D_ALWAYS,
				"SharedPortServer: failed to receive request from %s.\n",
				sock->peer_description() );
		return FALSE;
	}

	if( more_args > 100 || more_args < 0 ) {
		dprintf(D_ALWAYS,
				"SharedPortServer: got invalid more_args=%d.\n", more_args);
		return FALSE;
	}

		// Client has sent a hint about their session ID.
	if (more_args == 1) {
		more_args--;
		if (!sock->get(client_session_id, sizeof(client_session_id))) {
			dprintf(D_ALWAYS, "SharedPortServer: failed to receive client ID from %s.\n", sock->peer_description());
			return false;
		}
	}

		// for possible future use
	while( more_args-- > 0 ) {
		char junk[512];
		if( !sock->get(junk,sizeof(junk)) ) {
			dprintf(D_ALWAYS,
					"SharedPortServer: failed to receive extra args in request from %s.\n",
					sock->peer_description() );
			return FALSE;
		}
		dprintf(D_FULLDEBUG,
			"SharedPortServer: ignoring trailing argument in request from "
			"%s.\n", sock->peer_description());
	}

	if( !sock->end_of_message() ) {
		dprintf(D_ALWAYS,
				"SharedPortServer: failed to receive end of request from %s.\n",
				sock->peer_description() );
		return FALSE;
	}

	if( client_name[0] ) {
		MyString client_buf(client_name);
			// client name is purely for debugging purposes
		client_buf.formatstr_cat(" on %s",sock->peer_description());
		sock->set_peer_description(client_buf.Value());
	}

	MyString deadline_desc;
	if( deadline >= 0 ) {
		sock->set_deadline_timeout( deadline );

		if( IsDebugLevel( D_NETWORK ) ) {
			deadline_desc.formatstr(" (deadline %ds)", deadline);
		}
	}

		// Only execute this if a load balancer exists to avoid a hash
		// table lookup in the non-load-balancer case.
	if (m_load_balancer_count && client_session_id[0]) {
		if (!GetLoadBalancerId(shared_port_id, client_name, client_session_id, shared_port_id)) {
			dprintf(D_ALWAYS, "SharedPortServer: failed to assign client %s to a load balancer.\n",
				sock->peer_description());
			return false;
		}
	}

	dprintf( D_FULLDEBUG,
			"SharedPortServer: request from %s to connect to %s%s. (CurPending=%u PeakPending=%u)\n",
			sock->peer_description(), shared_port_id, deadline_desc.Value(),
			m_shared_port_client.get_currentPendingPassSocketCalls(),
			m_shared_port_client.get_maxPendingPassSocketCalls() );

	if( strcmp( shared_port_id, "self" ) == 0 ) {
		// The last 'true' flags this protocol as being "loopback," so
		// we won't ever end up back here and pass off the request.
		classy_counted_ptr< DaemonCommandProtocol > r = new DaemonCommandProtocol( sock, true, true );
		return r->doProtocol();
	}

	// Technically optional, but since HTCondor code always sets it to
	// its own Sinful string, check to see if it's a daemon trying to
	// connect to itself and refuse.
	if( client_name[0] ) {
		//dprintf( D_FULLDEBUG, "Found client name '%s'.\n", client_name );
		char * client_sinful = strstr( client_name, "<" );
		Sinful s( client_sinful );
		if( s.valid() ) {
			//dprintf( D_FULLDEBUG, "Client name '%s' contains a valid Sinful.\n", client_name );
			char const * sourceSharedPortID = s.getSharedPortID();
			if( sourceSharedPortID && strcmp( sourceSharedPortID, shared_port_id ) == 0 ) {
				dprintf( D_FULLDEBUG, "Client name '%s' has same shared port ID as its target (%s).\n", client_name, shared_port_id );
				s.setSharedPortID( NULL );
				Sinful t( global_dc_sinful() );
				if( t.valid() ) {
					//dprintf( D_FULLDEBUG, "Daemon core Sinful (%s) is valid.\n", global_dc_sinful() );
					t.setSharedPortID( NULL );
					if( t.addressPointsToMe( s ) ) {
						dprintf( D_ALWAYS, "Rejected request from %s to connect to itself.\n", sock->peer_description() );
						return FALSE;
					}
				}
			}
		}
	}

	return PassRequest(static_cast<Sock*>(sock), shared_port_id);
}

int
SharedPortServer::PassRequest(Sock *sock, const char *shared_port_id)
{
	int result = TRUE;
#if HAVE_SCM_RIGHTS_PASSFD
		// Note: the HAVE_SCM_RIGHTS_PASSFD implementation of PassSocket()
		// is nonblocking.  See gt #4094.
		// Note: returns TRUE, FALSE, or KEEP_STREAM if operation is still pending...
	result = m_shared_port_client.PassSocket((Sock *)sock, shared_port_id, NULL, true);
#else
		// Because of an ACK in the PassSocket protocol, this may block
		// while waiting for the requested endpoint to respond.
		// Therefore, we fork to try to stay responsive.  It is likely
		// that this command could also simply be enabled for threading.

	ForkStatus fork_status = forker.NewJob();
	if( fork_status != FORK_PARENT ) {
		if( fork_status == FORK_CHILD ) {
			dprintf(D_FULLDEBUG,
					"SharedPortServer: forked worker for request from %s to connect to %s.\n",
					sock->peer_description(), shared_port_id);
		}

		m_shared_port_client.PassSocket((Sock *)sock,shared_port_id);

		if( fork_status == FORK_CHILD ) {
			dprintf(D_FULLDEBUG,
					"SharedPortServer: worker finished for request from %s to connect to %s.\n",
					sock->peer_description(), shared_port_id);
			forker.WorkerDone();
		}
	}
#endif

	return result;
}

bool
SharedPortServer::GetLoadBalancerId(const char *requested_id, const char *client_name, const char *sess_id, char *dest_shared_id)
{
	std::string requested_str;
	BalancerInfo *bal;
	{
		const auto iter = m_dest_map.find(std::string(requested_id));
		if (iter == m_dest_map.end()) {
			strcpy(dest_shared_id, requested_id);
			return true;
		}
		bal = &(iter->second);
	}

	std::size_t id = std::hash<std::string>{}(client_name) ^ (std::hash<std::string>{}(sess_id) << 1);
	const auto iter = m_sessions.find(id);
	if (iter == m_dests.end()) {
		static const uint16_t lease_duration = 3600;
		static const uint32_t session_duration = 86400;
			// Note this will overflow if the shared_port server is alive for more than 136 years.
		uint32_t expiry = static_cast<uint32_t>(time(NULL) - m_start_time) + session_duration;

		uint16_t dest_id;
		int idx;
		for (idx=0; idx<10; idx++) {
			int selection = get_random_int_insecure();
			dest_id = static_cast<uint16_t>(selection % bal->size());
			const auto &dest = bal->m_dests[dest_id];
			if (dest.m_active) {
				strcpy(dest_shared_id, dest.m_shared_port_id.c_str());
				break;
			}
		}
			// We give up on random selection - just take the next active ID.
			// This will bias the resulting load balancer selection but we don't
			// care because this seems statistically unlikely.
		if (idx == 10) {
			uint16_t orig_dest_id;
			while (true) {
				dest_id++;
				dest_id = dest_id % bal->size();
				if (orig_dest_id == dest_id) {
					dprintf(D_ALWAYS, "SharedPortServer: Logic error - invalid load balancer setup.\n");
					return false;
				}
				const auto &dest = bal->m_dests[dest_id];
				if (dest.m_active) {
					strcpy(dest_shared_id, dest.m_shared_port_id.c_str());
					break;
				}
			}
		}

		m_dests.emplace(std::make_pair<std::size_t, BalancerInfo>(id, {dest_id, lease_duration, expiry}))
        } else {
		// Existing session; use the same load balancer.
		const auto &dest_info = bal->m_dests[dest_id];
		static const uint32_t lease_duration = 3600;
		iter->second.m_lease_remaining = lease_duration;
		strcpy(dest_shared_id, dest.m_shared_port_id.c_str());
	}
	return true;
}

int
SharedPortServer::HandleDefaultRequest(int cmd,Stream *sock)
{
	if (!m_default_id.size()) {
		dprintf(D_FULLDEBUG, "SharedPortServer: Got request for command %d from %s, but no default client specified.\n",
			cmd, sock->peer_description());
		return 0;
	}
	dprintf(D_FULLDEBUG, "SharedPortServer: Passing a request from %s for command %d to ID %s.\n",
		sock->peer_description(), cmd, m_default_id.c_str()); 
	return PassRequest(static_cast<Sock*>(sock), m_default_id.c_str());
}

void
SharedPortServer::BookkeepSessions()
{
}
