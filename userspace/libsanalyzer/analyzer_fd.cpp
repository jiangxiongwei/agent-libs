#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

#include "sinsp.h"
#include "sinsp_int.h"
#include "../../driver/ppm_ringbuffer.h"

#ifdef HAS_ANALYZER
#include "parsers.h"
#include "analyzer_int.h"
#include "analyzer.h"
#include "analyzer_thread.h"
#include "connectinfo.h"
#include "metrics.h"
#include "draios.pb.h"
#include "delays.h"
#include "scores.h"
#include "procfs_parser.h"
#include "sinsp_errno.h"
#include "sched_analyzer.h"
#include "analyzer_fd.h"

///////////////////////////////////////////////////////////////////////////////
// sinsp_proto_detector implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_proto_detector::sinsp_proto_detector()
{
	m_http_options_intval = (*(uint32_t*)HTTP_OPTIONS_STR);
	m_http_get_intval = (*(uint32_t*)HTTP_GET_STR);
	m_http_head_intval = (*(uint32_t*)HTTP_HEAD_STR);
	m_http_post_intval = (*(uint32_t*)HTTP_POST_STR);
	m_http_put_intval = (*(uint32_t*)HTTP_PUT_STR);
	m_http_delete_intval = (*(uint32_t*)HTTP_DELETE_STR);
	m_http_trace_intval = (*(uint32_t*)HTTP_TRACE_STR);
	m_http_connect_intval = (*(uint32_t*)HTTP_CONNECT_STR);
	m_http_resp_intval = (*(uint32_t*)HTTP_RESP_STR);
}

sinsp_partial_transaction::type sinsp_proto_detector::detect_proto(sinsp_evt *evt, 
	sinsp_partial_transaction *trinfo, 
	sinsp_partial_transaction::direction trdir,
	uint8_t* buf, uint32_t buflen)
{
	uint16_t serverport = evt->m_fdinfo->get_serverport();

	//
	// Make sure there are at least 4 bytes
	//
	if(buflen >= MIN_VALID_PROTO_BUF_SIZE)
	{
		//
		// Detect HTTP
		//
		if(*(uint32_t*)buf == m_http_get_intval ||
				*(uint32_t*)buf == m_http_post_intval ||
				*(uint32_t*)buf == m_http_put_intval ||
				*(uint32_t*)buf == m_http_delete_intval ||
				*(uint32_t*)buf == m_http_trace_intval ||
				*(uint32_t*)buf == m_http_connect_intval ||
				*(uint32_t*)buf == m_http_options_intval ||
				(*(uint32_t*)buf == m_http_resp_intval && buf[4] == '/'))
		{
			//ASSERT(trinfo->m_protoparser == NULL);
			sinsp_http_parser* st = new sinsp_http_parser;
			trinfo->m_protoparser = (sinsp_protocol_parser*)st;

			return sinsp_partial_transaction::TYPE_HTTP;
		}
		//
		// Detect mysql
		//
		else if(serverport == SRV_PORT_MYSQL)
		{
			uint8_t* tbuf;
			uint32_t tbuflen;
			uint32_t stsize = trinfo->m_reassembly_buffer.get_size();

			if(stsize != 0)
			{
				trinfo->m_reassembly_buffer.copy((char*)buf, buflen);
				tbuf = (uint8_t*)trinfo->m_reassembly_buffer.get_buf();
				tbuflen = stsize + buflen;
			}
			else
			{
				tbuf = buf;
				tbuflen = buflen;
			}

			if(tbuflen > 5	// min length
				&& *(uint16_t*)tbuf == tbuflen - 4 // first 3 bytes are length
				&& tbuf[2] == 0x00 // 3rd byte of packet length
				&& tbuf[3] == 0) // Sequence number is zero for the beginning of a query
			{
				sinsp_mysql_parser* st = new sinsp_mysql_parser;
				trinfo->m_protoparser = (sinsp_protocol_parser*)st;
				return sinsp_partial_transaction::TYPE_MYSQL;
			}
		}
		else
		{
			//ASSERT(trinfo->m_protoparser == NULL);
			trinfo->m_protoparser = NULL;
			return sinsp_partial_transaction::TYPE_IP;
		}
	}

	if(serverport == SRV_PORT_MYSQL)
	{
		//
		// This transaction has not been recognized yet, and the port is
		// the mysql one. Sometimes mysql splits the receive into multiple
		// reads, so we try to buffer this data and try again later
		//
		if((evt->m_fdinfo->is_role_server() && trdir == sinsp_partial_transaction::DIR_IN )||
			(evt->m_fdinfo->is_role_client() && trdir == sinsp_partial_transaction::DIR_OUT))
		{
			if(trdir !=	trinfo->m_direction)
			{
				trinfo->m_reassembly_buffer.clear();
			}

			trinfo->m_reassembly_buffer.copy((char*)buf, buflen);
		}
	}

	//ASSERT(trinfo->m_protoparser == NULL);
	trinfo->m_protoparser = NULL;
	return sinsp_partial_transaction::TYPE_IP;		
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_analyzer_fd_listener implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_analyzer_fd_listener::sinsp_analyzer_fd_listener(sinsp* inspector, sinsp_analyzer* analyzer)
{
	m_inspector = inspector; 
	m_analyzer = analyzer;
}

bool sinsp_analyzer_fd_listener::patch_network_role(sinsp_threadinfo* ptinfo, 
										  sinsp_fdinfo_t* pfdinfo,
										  bool incoming)
{
	//
	// This should be disabled for the moment
	//
	ASSERT(false);

	bool is_sip_local = 
		m_inspector->m_network_interfaces->is_ipv4addr_in_local_machine(pfdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip);
	bool is_dip_local = 
		m_inspector->m_network_interfaces->is_ipv4addr_in_local_machine(pfdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip);

	//
	// If only the client is local, mark the role as client.
	// If only the server is local, mark the role as server.
	//
	if(is_sip_local)
	{
		if(!is_dip_local)
		{
			pfdinfo->set_role_client();
			return true;
		}
	}
	else if(is_dip_local)
	{
		if(!is_sip_local)
		{
			pfdinfo->set_role_server();
			return true;
		}
	}

	//
	// Both addresses are local
	//
	ASSERT(is_sip_local && is_dip_local);

	//
	// If this process owns the port, mark it as server, otherwise mark it as client
	//
	if(ptinfo->is_bound_to_port(pfdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport))
	{
		if(ptinfo->uses_client_port(pfdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport))
		{
			goto wildass_guess;
		}

		pfdinfo->set_role_server();
		return true;
	}
	else
	{
		pfdinfo->set_role_client();
		return true;
	}

wildass_guess:
	if(!(pfdinfo->m_flags & (sinsp_fdinfo_t::FLAGS_ROLE_CLIENT | sinsp_fdinfo_t::FLAGS_ROLE_SERVER)))
	{
		//
		// We just assume that a server usually starts with a read and a client with a write
		//
		if(incoming)
		{
			pfdinfo->set_role_server();
		}
		else
		{
			pfdinfo->set_role_client();
		}
	}

	return true;
}

void sinsp_analyzer_fd_listener::on_read(sinsp_evt *evt, int64_t tid, int64_t fd, char *data, uint32_t original_len, uint32_t len)
{
	if(evt->m_fdinfo->is_file())
	{
		analyzer_file_stat* file_stat = get_file_stat(evt->m_fdinfo->m_name);
		ASSERT(file_stat);
		if(file_stat)
		{
			file_stat->m_bytes += original_len;
			file_stat->m_time_ns += evt->m_tinfo->m_latency;
		}
	}

	evt->set_iosize(original_len);

	if(evt->m_fdinfo->is_ipv4_socket() || evt->m_fdinfo->is_unix_socket())
	{
		sinsp_connection *connection = NULL;

		/////////////////////////////////////////////////////////////////////////////
		// Handle the connection
		/////////////////////////////////////////////////////////////////////////////
		if(evt->m_fdinfo->is_unix_socket())
		{
#ifdef HAS_UNIX_CONNECTIONS
			// ignore invalid destination addresses
			if(0 == evt->m_fdinfo->m_sockinfo.m_unixinfo.m_fields.m_dest)
			{
//				return;
			}

			connection = m_analyzer->get_connection(evt->m_fdinfo->m_sockinfo.m_unixinfo, evt->get_ts());
			if(connection == NULL)
			{
				//
				// We dropped the accept() or connect()
				// Create a connection entry here and make an assumption this is the server FD.
				// (we assume that a server usually starts with a read).
				//
				evt->m_fdinfo->set_role_server();
				string scomm = evt->m_tinfo->get_comm();
				connection = m_analyzer->m_unix_connections->add_connection(evt->m_fdinfo->m_sockinfo.m_unixinfo,
					&scomm,
					evt->m_tinfo->m_pid,
					tid,
					fd,
					evt->m_fdinfo->is_role_client(),
					evt->get_ts());
			}
			else if((!(evt->m_tinfo->m_pid == connection->m_spid && fd == connection->m_sfd) &&
				!(evt->m_tinfo->m_pid == connection->m_dpid && fd == connection->m_dfd)) ||
				(connection->m_analysis_flags & sinsp_connection::AF_CLOSED))
			{
				//
				// We dropped both accept() and connect(), and the connection has already been established
				// when handling a read on the other side.
				//
				if(connection->m_analysis_flags == sinsp_connection::AF_CLOSED)
				{
					//
					// There is a closed connection with the same key. We drop its content and reuse it.
					// We also mark it as reused so that the analyzer is aware of it
					//
					connection->reset();
					connection->m_analysis_flags = sinsp_connection::AF_REUSED;
					evt->m_fdinfo->set_role_server();
				}
				else
				{
					if(connection->is_server_only())
					{
						if(evt->m_fdinfo->is_role_none())
						{
							evt->m_fdinfo->set_role_client();
						}
					}
					else if(connection->is_client_only())
					{
						if(evt->m_fdinfo->is_role_none())
						{
							evt->m_fdinfo->set_role_server();
						}
					}
					else
					{
						//
						// FDs don't match but the connection has not been closed yet.
						// This seem to heppen with unix sockets, whose addresses are reused when 
						// just on of the endpoints has been closed.
						// Jusr recycle the connection.
						//
						if(evt->m_fdinfo->is_role_server())
						{
							connection->reset_server();
						}
						else if(evt->m_fdinfo->is_role_client())
						{
							connection->reset_client();
						}
						else
						{
							connection->reset();
						}

						connection->m_analysis_flags = sinsp_connection::AF_REUSED;
						evt->m_fdinfo->set_role_server();
					}
				}

				string scomm = evt->m_tinfo->get_comm();
				connection = m_analyzer->m_unix_connections->add_connection(evt->m_fdinfo->m_sockinfo.m_unixinfo,
					&scomm,
					evt->m_tinfo->m_pid,
					tid,
					fd,
					evt->m_fdinfo->is_role_client(),
					evt->get_ts());
			}
#else
			return;
#endif // HAS_UNIX_CONNECTIONS

		}
		else if(evt->m_fdinfo->is_ipv4_socket())
		{
			connection = m_analyzer->get_connection(evt->m_fdinfo->m_sockinfo.m_ipv4info, evt->get_ts());
			
			if(connection == NULL)
			{
				//
				// This is either:
				//  - the first read of a UDP socket
				//  - a TCP socket for which we dropped the accept() or connect()
				// Create a connection entry here and try to automatically detect if this is the client or the server.
				//
				if(evt->m_fdinfo->is_role_none())
				{
					if(patch_network_role(evt->m_tinfo, evt->m_fdinfo, true) == false)
					{
						goto r_conn_creation_done;
					}
				}

				string scomm = evt->m_tinfo->get_comm();
				
				connection = m_analyzer->m_ipv4_connections->add_connection(evt->m_fdinfo->m_sockinfo.m_ipv4info,
					&scomm,
					evt->m_tinfo->m_pid,
					tid,
					fd,
					evt->m_fdinfo->is_role_client(),
					evt->get_ts());
			}
			else if((!(evt->m_tinfo->m_pid == connection->m_spid && fd == connection->m_sfd) &&
				!(evt->m_tinfo->m_pid == connection->m_dpid && fd == connection->m_dfd)) ||
				(connection->m_analysis_flags & sinsp_connection::AF_CLOSED))
			{
				//
				// We dropped both accept() and connect(), and the connection has already been established
				// when handling a read on the other side.
				//
				if(connection->m_analysis_flags == sinsp_connection::AF_CLOSED)
				{
					//
					// There is a closed connection with the same key. We drop its content and reuse it.
					// We also mark it as reused so that the analyzer is aware of it
					//
					connection->reset();
					connection->m_analysis_flags = sinsp_connection::AF_REUSED;

					if(evt->m_fdinfo->is_role_none())
					{
						if(patch_network_role(evt->m_tinfo, evt->m_fdinfo, true) == false)
						{
							goto r_conn_creation_done;
						}
					}
				}
				else
				{
					if(connection->is_server_only())
					{
						if(evt->m_fdinfo->is_role_none())
						{
							evt->m_fdinfo->set_role_client();
						}
					}
					else if(connection->is_client_only())
					{
						if(evt->m_fdinfo->is_role_none())
						{
							evt->m_fdinfo->set_role_server();
						}
					}
					else
					{
						//
						// FDs don't match but the connection has not been closed yet.
						// This can happen in case of event drops, or when a connection
						// is accepted by a process and served by another one.
						//
						if(evt->m_fdinfo->is_role_server())
						{
							connection->reset_server();
						}
						else if(evt->m_fdinfo->is_role_client())
						{
							connection->reset_client();
						}
						else
						{
							connection->reset();
						}

						connection->m_analysis_flags = sinsp_connection::AF_REUSED;

						if(evt->m_fdinfo->is_role_none())
						{
							if(patch_network_role(evt->m_tinfo, evt->m_fdinfo, true) == false)
							{
								goto r_conn_creation_done;
							}
						}
					}
				}

				string scomm = evt->m_tinfo->get_comm();
				connection = m_analyzer->m_ipv4_connections->add_connection(evt->m_fdinfo->m_sockinfo.m_ipv4info,
					&scomm,
					evt->m_tinfo->m_pid,
					tid,
					fd,
					evt->m_fdinfo->is_role_client(),
					evt->get_ts());
			}
		}

r_conn_creation_done:
	
		//
		// Attribute the read bytes to the proper connection side
		//
		if(connection == NULL)
		{
			//
			// This happens when the connection table is full
			//
			return;
		}

		if(evt->m_fdinfo->is_role_server())
		{
			connection->m_metrics.m_server.add_in(1, original_len);
		}
		else if (evt->m_fdinfo->is_role_client())
		{
			connection->m_metrics.m_client.add_in(1, original_len);
		}
		else
		{
			ASSERT(false);
		}

		/////////////////////////////////////////////////////////////////////////////
		// Handle the transaction
		/////////////////////////////////////////////////////////////////////////////
/*
		if(evt->m_fdinfo->is_role_server())
		{
			//
			// See if there's already a transaction
			//
 			sinsp_partial_transaction *trinfo = &(evt->m_fdinfo->m_usrstate);
			if(trinfo->m_type == sinsp_partial_transaction::TYPE_UNKNOWN)
			{
				//
				// Try to parse this as HTTP
				//
				if(m_http_parser.is_msg_http(data, len) && m_http_parser.parse_request(data, len))
				{
					//
					// Success. Add an HTTP entry to the transaction table for this fd
					//
					trinfo->m_type = sinsp_partial_transaction::TYPE_HTTP;
					trinfo->m_protoinfo.push_back(m_http_parser.m_url);
					trinfo->m_protoinfo.push_back(m_http_parser.m_agent);
				}
				else
				{
					//
					// The message has not been recognized as HTTP.
					// Add an IP entry to the transaction table for this fd
					//
					trinfo->m_type = sinsp_partial_transaction::TYPE_IP;
				}
			}

			//
			// Update the transaction state.
			//
			ASSERT(connection != NULL);
			trinfo->update(m_analyzer,
				evt->m_tinfo,
				connection,
				evt->m_tinfo->m_lastevent_ts, 
				evt->get_ts(), 
				sinsp_partial_transaction::DIR_IN, 
				len);
		}
*/
		//
		// Determine the transaction direction.
		// recv(), recvfrom() and recvmsg() return 0 if the connection has been closed by the other side.
		//
		sinsp_partial_transaction::direction trdir;

		uint16_t etype = evt->get_type();
		if(len == 0 && (etype == PPME_SOCKET_RECVFROM_X || etype == PPME_SOCKET_RECV_X || etype == PPME_SOCKET_RECVMSG_X))
		{
			trdir = sinsp_partial_transaction::DIR_CLOSE;
		}
		else
		{
			trdir = sinsp_partial_transaction::DIR_IN;
		}

		//
		// Check if this is a new transaction that needs to be initialized, and whose
		// protocol needs to be discovered.
		// NOTE: after two turns, we give up discovering the protocol and we consider this
		//       to be just IP.
		//
		sinsp_partial_transaction *trinfo = evt->m_fdinfo->m_usrstate;

		if(trinfo == NULL)
		{
			evt->m_fdinfo->m_usrstate = new sinsp_partial_transaction();
			trinfo = evt->m_fdinfo->m_usrstate;
		}

		if(!trinfo->is_active() ||
			(trinfo->m_n_direction_switches < 2 && trinfo->m_type <= sinsp_partial_transaction::TYPE_IP))
		{
			//
			// New or just detected transaction. Detect the protocol and initialize the transaction.
			// Note: m_type can be bigger than TYPE_IP if the connection has been reset by something 
			//       like a shutdown().
			//
			if(trinfo->m_type <= sinsp_partial_transaction::TYPE_IP)
			{
				sinsp_partial_transaction::type type = 
					m_proto_detector.detect_proto(evt, trinfo, trdir, 
					(uint8_t*)data, len);

				trinfo->mark_active_and_reset(type);
			}
			else
			{
				trinfo->mark_active_and_reset(trinfo->m_type);
			}
		}

		//
		// Update the transaction state.
		//
		trinfo->update(m_analyzer,
			evt->m_tinfo,
			evt->m_fdinfo,
			connection,
			evt->m_tinfo->m_lastevent_ts, 
			evt->get_ts(), 
			evt->get_cpuid(),
			trdir,
#if _DEBUG
			evt,
			fd,
#endif
			data,
			original_len,
			len);
	}
#ifdef HAS_PIPE_CONNECTIONS
	else if(evt->m_fdinfo->is_pipe())
	{
		sinsp_connection *connection = m_analyzer->get_connection(evt->m_fdinfo->m_ino, evt->get_ts());
		if(NULL == connection || connection->is_server_only())
		{
			string scomm = evt->m_tinfo->get_comm();
			m_analyzer->m_pipe_connections->add_connection(evt->m_fdinfo->m_ino,
				&scomm,
				evt->m_tinfo->m_pid,
			    tid,
			    fd,
			    true,
			    evt->get_ts());
		}
	}
#endif
}

void sinsp_analyzer_fd_listener::on_write(sinsp_evt *evt, int64_t tid, int64_t fd, char *data, uint32_t original_len, uint32_t len)
{
	if(evt->m_fdinfo->is_file())
	{
		analyzer_file_stat* file_stat = get_file_stat(evt->m_fdinfo->m_name);
		ASSERT(file_stat);
		if(file_stat)
		{
			file_stat->m_bytes += original_len;
			file_stat->m_time_ns += evt->m_tinfo->m_latency;
		}
	}
	
	evt->set_iosize(original_len);

	if(evt->m_fdinfo->is_ipv4_socket() || evt->m_fdinfo->is_unix_socket())
	{
		/////////////////////////////////////////////////////////////////////////////
		// Handle the connection
		/////////////////////////////////////////////////////////////////////////////
		sinsp_connection* connection = NULL; 

		if(evt->m_fdinfo->is_unix_socket())
		{
#ifdef HAS_UNIX_CONNECTIONS
			// ignore invalid destination addresses
			if(0 == evt->m_fdinfo->m_sockinfo.m_unixinfo.m_fields.m_dest)
			{
//				return;
			}

			connection = m_analyzer->get_connection(evt->m_fdinfo->m_sockinfo.m_unixinfo, evt->get_ts());
			if(connection == NULL)
			{
				//
				// We dropped the accept() or connect()
				// Create a connection entry here and make an assumption this is the client FD
				// (we assume that a client usually starts with a write)
				//
				evt->m_fdinfo->set_role_client();
				string scomm = evt->m_tinfo->get_comm();
				connection = m_analyzer->m_unix_connections->add_connection(evt->m_fdinfo->m_sockinfo.m_unixinfo,
					&scomm,
					evt->m_tinfo->m_pid,
				    tid,
				    fd,
				    evt->m_fdinfo->is_role_client(),
				    evt->get_ts());
			}
			else if(!(evt->m_tinfo->m_pid == connection->m_spid && fd == connection->m_sfd) &&
				!(evt->m_tinfo->m_pid == connection->m_dpid && fd == connection->m_dfd))
			{
				//
				// We dropped both accept() and connect(), and the connection has already been established
				// when handling a read on the other side.
				//
				if(connection->m_analysis_flags == sinsp_connection::AF_CLOSED)
				{
					//
					// There is a closed connection with the same key. We drop its content and reuse it.
					// We also mark it as reused so that the analyzer is aware of it
					//
					connection->reset();
					connection->m_analysis_flags = sinsp_connection::AF_REUSED;
					evt->m_fdinfo->set_role_client();
				}
				else
				{
					if(connection->is_server_only())
					{
						if(evt->m_fdinfo->is_role_none())
						{
							evt->m_fdinfo->set_role_client();
						}
					}
					else if(connection->is_client_only())
					{
						if(evt->m_fdinfo->is_role_none())
						{
							evt->m_fdinfo->set_role_server();
						}
					}
					else
					{
						//
						// FDs don't match but the connection has not been closed yet.
						// This seem to heppen with unix sockets, whose addresses are reused when 
						// just on of the endpoints has been closed.
						// Jusr recycle the connection.
						//
						if(evt->m_fdinfo->is_role_server())
						{
							connection->reset_server();
						}
						else if(evt->m_fdinfo->is_role_client())
						{
							connection->reset_client();
						}
						else
						{
							connection->reset();
						}

						connection->m_analysis_flags = sinsp_connection::AF_REUSED;
						evt->m_fdinfo->set_role_client();
					}
				}

				string scomm = evt->m_tinfo->get_comm();
				connection = m_analyzer->m_unix_connections->add_connection(evt->m_fdinfo->m_sockinfo.m_unixinfo,
					&scomm,
					evt->m_tinfo->m_pid,
					tid,
					fd,
					evt->m_fdinfo->is_role_client(),
					evt->get_ts());
			}
#else
			return;
#endif // HAS_UNIX_CONNECTIONS
		}
		else if(evt->m_fdinfo->is_ipv4_socket())
		{
			connection = m_analyzer->get_connection(evt->m_fdinfo->m_sockinfo.m_ipv4info, evt->get_ts());

			if(connection == NULL)
			{
				//
				// This is either:
				//  - the first write of a UDP socket
				//  - a TCP socket for which we dropped the accept() or connect()
				// Create a connection entry here and try to detect if this is the client or the server by lookig
				// at the ports.
				// (we assume that a client usually starts with a write)
				//
				if(evt->m_fdinfo->is_role_none())
				{
					if(patch_network_role(evt->m_tinfo, evt->m_fdinfo, false) == false)
					{
						goto w_conn_creation_done;
					}
				}

				string scomm = evt->m_tinfo->get_comm();
				connection = m_analyzer->m_ipv4_connections->add_connection(evt->m_fdinfo->m_sockinfo.m_ipv4info,
					&scomm,
					evt->m_tinfo->m_pid,
					tid,
					fd,
					evt->m_fdinfo->is_role_client(),
					evt->get_ts());
			}
			else if(!(evt->m_tinfo->m_pid == connection->m_spid && fd == connection->m_sfd) &&
				!(evt->m_tinfo->m_pid == connection->m_dpid && fd == connection->m_dfd))
			{
				//
				// We dropped both accept() and connect(), and the connection has already been established
				// when handling a read on the other side.
				//
				if(connection->m_analysis_flags == sinsp_connection::AF_CLOSED)
				{
					//
					// There is a closed connection with the same key. We drop its content and reuse it.
					// We also mark it as reused so that the analyzer is aware of it
					//
					connection->reset();
					connection->m_analysis_flags = sinsp_connection::AF_REUSED;

					if(evt->m_fdinfo->is_role_none())
					{
						if(patch_network_role(evt->m_tinfo, evt->m_fdinfo, false) == false)
						{
							goto w_conn_creation_done;
						}
					}
				}
				else
				{
					if(connection->is_server_only())
					{
						if(evt->m_fdinfo->is_role_none())
						{
							evt->m_fdinfo->set_role_client();
						}
					}
					else if(connection->is_client_only())
					{
						if(evt->m_fdinfo->is_role_none())
						{
							evt->m_fdinfo->set_role_server();
						}
					}
					else
					{
						//
						// FDs don't match but the connection has not been closed yet.
						// This can happen in case of event drops, or when a commection
						// is accepted by a process and served by another one.
						//
						if(evt->m_fdinfo->is_role_server())
						{
							connection->reset_server();
						}
						else if(evt->m_fdinfo->is_role_client())
						{
							connection->reset_client();
						}
						else
						{
							connection->reset();
						}

						connection->m_analysis_flags = sinsp_connection::AF_REUSED;

						if(evt->m_fdinfo->is_role_none())
						{
							if(patch_network_role(evt->m_tinfo, evt->m_fdinfo, false) == false)
							{
								goto w_conn_creation_done;
							}
						}
					}
				}

				string scomm = evt->m_tinfo->get_comm();
				connection = m_analyzer->m_ipv4_connections->add_connection(evt->m_fdinfo->m_sockinfo.m_ipv4info,
					&scomm,
					evt->m_tinfo->m_pid,
					tid,
					fd,
					evt->m_fdinfo->is_role_client(),
					evt->get_ts());
			}
		}

w_conn_creation_done:
		
		//
		// Attribute the read bytes to the proper connection side
		//
		if(connection == NULL)
		{
			//
			// This happens when the connection table is full
			//
			return;
		}

		if(evt->m_fdinfo->is_role_server())
		{
			connection->m_metrics.m_server.add_out(1, original_len);
		}
		else if(evt->m_fdinfo->is_role_client())
		{
			connection->m_metrics.m_client.add_out(1, original_len);
		}
		else
		{
			ASSERT(false);
		}

		/////////////////////////////////////////////////////////////////////////////
		// Handle the transaction
		/////////////////////////////////////////////////////////////////////////////
		//
		// Check if this is a new transaction that needs to be initialized, and whose
		// protocol needs to be discovered.
		// NOTE: after two turns, we give up discovering the protocol and we consider this
		//       to be just IP.
		//
		sinsp_partial_transaction *trinfo = evt->m_fdinfo->m_usrstate;

		if(trinfo == NULL)
		{
			evt->m_fdinfo->m_usrstate = new sinsp_partial_transaction();
			trinfo = evt->m_fdinfo->m_usrstate;
		}

		if(!trinfo->is_active() ||
			(trinfo->m_n_direction_switches < 2 && trinfo->m_type <= sinsp_partial_transaction::TYPE_IP))
		{
			//
			// New or just detected transaction. Detect the protocol and initialize the transaction.
			// Note: m_type can be bigger than TYPE_IP if the connection has been reset by something 
			//       like a shutdown().
			//
			if(trinfo->m_type <= sinsp_partial_transaction::TYPE_IP)
			{
				sinsp_partial_transaction::type type = 
					m_proto_detector.detect_proto(evt, trinfo, sinsp_partial_transaction::DIR_OUT,
						(uint8_t*)data, len);

				trinfo->mark_active_and_reset(type);
			}
			else
			{
				trinfo->mark_active_and_reset(trinfo->m_type);
			}
		}

		//
		// Update the transaction state.
		//
		trinfo->update(m_analyzer,
			evt->m_tinfo,
			evt->m_fdinfo,
			connection,
			evt->m_tinfo->m_lastevent_ts, 
			evt->get_ts(), 
			evt->get_cpuid(),
			sinsp_partial_transaction::DIR_OUT, 
#if _DEBUG
			evt,
			fd,
#endif
			data,
			original_len,
			len);
	}
#ifdef HAS_PIPE_CONNECTIONS
	else if(evt->m_fdinfo->is_pipe())
	{
		sinsp_connection *connection = m_analyzer->get_connection(evt->m_fdinfo->m_ino, evt->get_ts());

		if(NULL == connection || connection->is_client_only())
		{
			string scomm = evt->m_tinfo->get_comm();
			m_analyzer->m_pipe_connections->add_connection(evt->m_fdinfo->m_ino,
				&scomm,
				evt->m_tinfo->m_pid,
			    tid,
			    fd,
			    false,
			    evt->get_ts());
		}
	}
#endif
}

void sinsp_analyzer_fd_listener::on_connect(sinsp_evt *evt, uint8_t* packed_data)
{
	int64_t tid = evt->get_tid();

	uint8_t family = *packed_data;

	if(family == PPM_AF_INET || family == PPM_AF_INET6)
	{
		//
		// Mark this fd as a transaction
		//
		ASSERT(evt->m_fdinfo->m_usrstate == NULL);
		evt->m_fdinfo->m_usrstate = new sinsp_partial_transaction();

		//
		// Lookup the connection
		//
		sinsp_connection* conn = m_analyzer->m_ipv4_connections->get_connection(
			evt->m_fdinfo->m_sockinfo.m_ipv4info,
			evt->get_ts());

		//
		// If a connection for this tuple is already there, drop it and replace it with a new one.
		// Note that remove_connection just decreases the connection reference counter, since connections
		// are destroyed by the analyzer at the end of the sample.
		// Note that UDP sockets can have an arbitrary number of connects, and each new one overrides
		// the previous one.
		//
		if(conn)
		{
			if(conn->m_analysis_flags == sinsp_connection::AF_CLOSED)
			{
				//
				// There is a closed connection with the same key. We drop its content and reuse it.
				// We also mark it as reused so that the analyzer is aware of it
				//
				conn->reset();
				conn->m_analysis_flags = sinsp_connection::AF_REUSED;
				conn->m_refcount = 1;
			}

			m_analyzer->m_ipv4_connections->remove_connection(evt->m_fdinfo->m_sockinfo.m_ipv4info);
		}

		//
		// Update the FD info with this tuple
		//
		if(family == PPM_AF_INET)
		{
			m_inspector->m_parser->set_ipv4_addresses_and_ports(evt->m_fdinfo, packed_data);
		}
		else
		{
			m_inspector->m_parser->set_ipv4_mapped_ipv6_addresses_and_ports(evt->m_fdinfo, 
				packed_data);
		}

		//
		// Add the tuple to the connection table
		//
		string scomm = evt->m_tinfo->get_comm();

		m_analyzer->m_ipv4_connections->add_connection(evt->m_fdinfo->m_sockinfo.m_ipv4info,
			&scomm,
			evt->m_tinfo->m_pid,
		    tid,
		    evt->m_tinfo->m_lastevent_fd,
		    true,
		    evt->get_ts());
	}
	else
	{
		m_inspector->m_parser->set_unix_info(evt->m_fdinfo, packed_data);

#ifdef HAS_UNIX_CONNECTIONS
		//
		// Mark this fd as a transaction
		//
		evt->m_fdinfo->set_is_transaction();

		string scomm = evt->m_tinfo->get_comm();
		m_analyzer->m_unix_connections->add_connection(evt->m_fdinfo->m_sockinfo.m_unixinfo,
			&scomm,
			evt->m_tinfo->m_pid,
		    tid,
		    evt->m_tinfo->m_lastevent_fd,
		    true,
		    evt->get_ts());
#endif // HAS_UNIX_CONNECTIONS
	}
}

void sinsp_analyzer_fd_listener::on_accept(sinsp_evt *evt, int64_t newfd, uint8_t* packed_data, sinsp_fdinfo_t* new_fdinfo)
{
	string scomm = evt->m_tinfo->get_comm();
	int64_t tid = evt->get_tid();

	if(new_fdinfo->m_type == SCAP_FD_IPV4_SOCK)
	{
		//
		// Add the tuple to the connection table
		//
		m_analyzer->m_ipv4_connections->add_connection(new_fdinfo->m_sockinfo.m_ipv4info,
			&scomm,
			evt->m_tinfo->m_pid,
		    tid,
		    newfd,
		    false,
		    evt->get_ts());
	}
	else if(new_fdinfo->m_type == SCAP_FD_UNIX_SOCK)
	{
#ifdef HAS_UNIX_CONNECTIONS
		m_analyzer->m_unix_connections->add_connection(new_fdinfo->m_sockinfo.m_unixinfo,
			&scomm,
			evt->m_tinfo->m_pid,
		    tid,
		    newfd,
		    false,
		    evt->get_ts());
#else
		return;
#endif
	}
	else
	{
		//
		// This should be checked by parse_accept_exit()
		//
		ASSERT(false);
	}

	//
	// Mark this fd as a transaction
	//
	ASSERT(new_fdinfo->m_usrstate == NULL);
	new_fdinfo->m_usrstate = new sinsp_partial_transaction();
}

void sinsp_analyzer_fd_listener::on_erase_fd(erase_fd_params* params)
{
	//
	// If this fd has an active transaction transaction table, mark it as unititialized
	//
	if(params->m_fdinfo->is_transaction())
	{
		sinsp_connection *connection;
		bool do_remove_transaction = params->m_fdinfo->m_usrstate->is_active();

		if(do_remove_transaction)
		{
			if(params->m_fdinfo->is_ipv4_socket())
			{
				connection = params->m_inspector->m_analyzer->get_connection(params->m_fdinfo->m_sockinfo.m_ipv4info, 
					params->m_ts);
			}
#ifdef HAS_UNIX_CONNECTIONS
			else if(params->m_fdinfo->is_unix_socket())
			{
				connection = params->m_inspector->m_analyzer->get_connection(params->m_fdinfo->m_sockinfo.m_unixinfo, 
					params->m_ts);
			}
#endif
			else
			{
				ASSERT(false);
				do_remove_transaction = false;
			}
		}

		if(do_remove_transaction)
		{
			params->m_fdinfo->m_usrstate->update(params->m_inspector->m_analyzer,
				params->m_tinfo,
				params->m_fdinfo,
				connection,
				params->m_ts, 
				params->m_ts, 
				-1,
				sinsp_partial_transaction::DIR_CLOSE,
#if _DEBUG
				NULL,
				params->m_fd,
#endif
				NULL,
				0,
				0);
		}

		params->m_fdinfo->m_usrstate->mark_inactive();			
	}

	//
	// If the fd is in the connection table, schedule the connection for removal
	//
	if(params->m_fdinfo->is_ipv4_socket() && 
		!params->m_fdinfo->has_no_role())
	{
		params->m_inspector->m_analyzer->m_ipv4_connections->remove_connection(params->m_fdinfo->m_sockinfo.m_ipv4info, false);
	}
#ifdef HAS_UNIX_CONNECTIONS
	else if(params->m_fdinfo->is_unix_socket() && 
		!params->m_fdinfo->has_no_role())
	{
		params->m_inspector->m_analyzer->m_unix_connections->remove_connection(params->m_fdinfo->m_sockinfo.m_unixinfo, false);
	}
#endif
}

void sinsp_analyzer_fd_listener::on_socket_shutdown(sinsp_evt *evt)
{
	//
	// If this fd has an active transaction, update it and then mark it as unititialized
	//
	if(evt->m_fdinfo->is_transaction() && evt->m_fdinfo->m_usrstate->is_active())
	{
		sinsp_connection* connection = NULL;

		if(evt->m_fdinfo->is_ipv4_socket())
		{
			connection = m_analyzer->get_connection(evt->m_fdinfo->m_sockinfo.m_ipv4info, evt->get_ts());
		}
#ifdef HAS_UNIX_CONNECTIONS
		else
		{
			connection = m_analyzer->get_connection(evt->m_fdinfo->m_sockinfo.m_unixinfo, evt->get_ts());
		}
#endif

		evt->m_fdinfo->m_usrstate->update(m_inspector->m_analyzer,
			evt->m_tinfo,
			evt->m_fdinfo,
			connection,
			evt->get_ts(), 
			evt->get_ts(), 
			evt->get_cpuid(),
			sinsp_partial_transaction::DIR_CLOSE,
#if _DEBUG
			evt,
			evt->m_tinfo->m_lastevent_fd,
#endif
			NULL,
			0,
			0);

		evt->m_fdinfo->m_usrstate->mark_inactive();
	}
}

void sinsp_analyzer_fd_listener::on_file_create(sinsp_evt* evt, const string& fullpath)
{
	analyzer_file_stat* file_stat = get_file_stat(fullpath);
	ASSERT(file_stat);

	if(evt->m_fdinfo)
	{
		ASSERT(evt->m_fdinfo->is_file());
		ASSERT(evt->m_fdinfo->m_name == fullpath);
		if(evt->m_fdinfo->is_file())
		{
			if(file_stat)
			{
				++file_stat->m_open_count;			
			}
		}
	}
	else
	{
		if(file_stat)
		{
			++file_stat->m_errors;
		}		
	}
}

void sinsp_analyzer_fd_listener::on_error(sinsp_evt* evt)
{
	ASSERT(evt->m_fdinfo);
	ASSERT(evt->m_errorcode != 0);
	if(evt->m_fdinfo && evt->m_fdinfo->is_file())
	{
		analyzer_file_stat* file_stat = get_file_stat(evt->m_fdinfo->m_name);
		ASSERT(file_stat);
		if(file_stat)
		{
			++file_stat->m_errors;
		}
	}
}

analyzer_file_stat* sinsp_analyzer_fd_listener::get_file_stat(const string& name)
{
	unordered_map<string, analyzer_file_stat>::iterator it = 
		m_files_stat.find(name);

	if(it == m_files_stat.end())
	{
		analyzer_file_stat file_stat;
		file_stat.m_name = name;
		m_files_stat.insert(pair<string, analyzer_file_stat>(string(name), file_stat));
		it = m_files_stat.find(name);
	}

	return &it->second;
}

#endif // HAS_ANALYZER
